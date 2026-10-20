/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

// SIMULATION-DISCLOSURE: POSIX runtime adapter for the Silicon Labs Lighting-App
// (examples/lighting-app/silabs/src/AppTask.cpp, CustomerAppTask.cpp, LEDWidget.cpp,
// RGBLEDWidget.cpp, and lighting-wifi-app.zap). Maps CMSIS-RTOS2 queues/timers/threads
// and SilabsPlatform WSTK LED/button peripherals onto Linux pthreads and CHIP logging
// while running the real Matter BLE (BlueZ) + Wi-Fi (wpa_supplicant) DeviceLayer stack.

#include "SilabsPosixShim.h"

#include "AppConfig.h"
#include "AppEvent.h"
#include "AppTask.h"
#include "BaseApplication.h"
#include "CustomerAppTask.h"
#include "LEDWidget.h"
#include "silabs_utils.h"

#include <app/clusters/boolean-state-configuration-server/BooleanStateConfigurationTestEventTriggerHandler.h>
#include <app/clusters/smoke-co-alarm-server/SmokeCOTestEventTriggerHandler.h>
#include <app/server/Dnssd.h>
#include <app/server/Server.h>
#include <cmsis_os2.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/silabs/platformAbstraction/SilabsPlatform.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

using namespace chip;
using namespace chip::app;
using namespace chip::DeviceLayer;
using namespace chip::DeviceLayer::Silabs;

namespace {

struct PosixMessageQueue
{
    uint32_t msgCount = 0;
    uint32_t msgSize  = 0;
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> queue;
    bool shutdown        = false;
    bool firstWaitSignal = false;
    std::condition_variable readyCv;
};

struct PosixTimer
{
    osTimerFunc_t func = nullptr;
    osTimerType_t type = osTimerOnce;
    void * arg         = nullptr;
    std::atomic<bool> running{ false };
    std::atomic<bool> deleted{ false };
    uint32_t periodMs = 0;
    std::chrono::steady_clock::time_point nextExpiry{};
};

std::mutex gTimerMutex;
std::condition_variable gTimerCv;
std::vector<std::shared_ptr<PosixTimer>> gTimers;
bool gTimerWorkerRunning = false;
bool gTimerWorkerStop    = false;
std::thread gTimerWorkerThread;

void TimerWorkerLoop()
{
    std::unique_lock<std::mutex> lock(gTimerMutex);
    while (!gTimerWorkerStop)
    {
        auto now      = std::chrono::steady_clock::now();
        auto nextWake = now + std::chrono::milliseconds(500);
        std::vector<std::pair<osTimerFunc_t, void *>> readyCallbacks;

        for (auto & timer : gTimers)
        {
            if (!timer || timer->deleted.load() || !timer->running.load())
            {
                continue;
            }
            if (now >= timer->nextExpiry)
            {
                readyCallbacks.emplace_back(timer->func, timer->arg);
                if (timer->type == osTimerPeriodic && timer->periodMs > 0)
                {
                    timer->nextExpiry = now + std::chrono::milliseconds(timer->periodMs);
                    if (timer->nextExpiry < nextWake)
                    {
                        nextWake = timer->nextExpiry;
                    }
                }
                else
                {
                    timer->running.store(false);
                }
            }
            else if (timer->nextExpiry < nextWake)
            {
                nextWake = timer->nextExpiry;
            }
        }

        if (!readyCallbacks.empty())
        {
            lock.unlock();
            for (auto & cb : readyCallbacks)
            {
                if (cb.first != nullptr)
                {
                    cb.first(cb.second);
                }
            }
            lock.lock();
            continue;
        }

        gTimerCv.wait_until(lock, nextWake);
    }
}

void EnsureTimerWorkerStartedLocked()
{
    if (!gTimerWorkerRunning)
    {
        gTimerWorkerStop    = false;
        gTimerWorkerRunning = true;
        gTimerWorkerThread  = std::thread(TimerWorkerLoop);
    }
}

struct PosixThreadStartCtx
{
    osThreadFunc_t func = nullptr;
    void * arg          = nullptr;
};

std::mutex gQueueRegistryMutex;
std::vector<std::unique_ptr<PosixMessageQueue>> gMessageQueues;

void * PosixThreadEntry(void * rawCtx)
{
    std::unique_ptr<PosixThreadStartCtx> ctx(static_cast<PosixThreadStartCtx *>(rawCtx));
    if (ctx && ctx->func != nullptr)
    {
        ctx->func(ctx->arg);
    }
    return nullptr;
}

#ifndef APP_EVENT_QUEUE_SIZE
#define APP_EVENT_QUEUE_SIZE 16
#endif

#ifndef FACTORY_RESET_TRIGGER_TIMEOUT
#define FACTORY_RESET_TRIGGER_TIMEOUT 3000
#endif

#ifndef FACTORY_RESET_CANCEL_WINDOW_TIMEOUT
#define FACTORY_RESET_CANCEL_WINDOW_TIMEOUT 3000
#endif

#define SYSTEM_STATE_LED 0
#define LIGHT_ACTUATOR_LED 1

osTimerId_t sFunctionTimer        = nullptr;
osTimerId_t sStatusLightTimer     = nullptr;
osMessageQueueId_t sAppEventQueue = nullptr;
pthread_t sAppTaskPthread         = 0;
bool sAppTaskPthreadCreated       = false;
LEDWidget sStatusLED;

constexpr uint32_t kStatusLightTimerPeriodMs = 200;

std::mutex gLedMutex;
bool gLedStates[4]       = { false, false, false, false };
uint8_t gButtonStates[4] = { 0, 0, 0, 0 };
uint8_t gRgbRed          = 0;
uint8_t gRgbGreen        = 0;
uint8_t gRgbBlue         = 0;

#ifdef MATTER_DM_PLUGIN_IDENTIFY_SERVER
Clusters::Identify::EffectIdentifierEnum sIdentifyEffect = Clusters::Identify::EffectIdentifierEnum::kStopEffect;
ObjectPool<Identify, MATTER_DM_IDENTIFY_CLUSTER_SERVER_ENDPOINT_COUNT + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT> IdentifyPool;
#endif // MATTER_DM_PLUGIN_IDENTIFY_SERVER

} // namespace

// ============================================================================
// CMSIS-RTOS2 POSIX Implementation
// ============================================================================

extern "C" {

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type, void * argument, const osTimerAttr_t * /* attr */)
{
    if (func == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(gTimerMutex);
    EnsureTimerWorkerStartedLocked();
    auto timer  = std::make_shared<PosixTimer>();
    timer->func = func;
    timer->type = type;
    timer->arg  = argument;
    gTimers.push_back(timer);
    return static_cast<osTimerId_t>(timer.get());
}

osStatus_t osTimerStart(osTimerId_t timer_id, uint32_t ticks)
{
    if (timer_id == nullptr)
    {
        return osErrorParameter;
    }
    auto * timer = static_cast<PosixTimer *>(timer_id);
    std::lock_guard<std::mutex> lock(gTimerMutex);
    if (timer->deleted.load())
    {
        return osErrorResource;
    }
    timer->periodMs   = (ticks == 0) ? 1 : ticks;
    timer->nextExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(timer->periodMs);
    timer->running.store(true);
    gTimerCv.notify_all();
    return osOK;
}

osStatus_t osTimerStop(osTimerId_t timer_id)
{
    if (timer_id == nullptr)
    {
        return osErrorParameter;
    }
    auto * timer = static_cast<PosixTimer *>(timer_id);
    std::lock_guard<std::mutex> lock(gTimerMutex);
    timer->running.store(false);
    gTimerCv.notify_all();
    return osOK;
}

uint32_t osTimerIsRunning(osTimerId_t timer_id)
{
    if (timer_id == nullptr)
    {
        return 0;
    }
    auto * timer = static_cast<PosixTimer *>(timer_id);
    return timer->running.load() ? 1U : 0U;
}

osStatus_t osTimerDelete(osTimerId_t timer_id)
{
    if (timer_id == nullptr)
    {
        return osErrorParameter;
    }
    auto * timer = static_cast<PosixTimer *>(timer_id);
    std::lock_guard<std::mutex> lock(gTimerMutex);
    timer->running.store(false);
    timer->deleted.store(true);
    gTimerCv.notify_all();
    return osOK;
}

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size, const osMessageQueueAttr_t * /* attr */)
{
    if (msg_count == 0 || msg_size == 0)
    {
        return nullptr;
    }
    auto mq      = std::make_unique<PosixMessageQueue>();
    mq->msgCount = msg_count;
    mq->msgSize  = msg_size;
    auto * raw   = mq.get();
    {
        std::lock_guard<std::mutex> lock(gQueueRegistryMutex);
        gMessageQueues.push_back(std::move(mq));
    }
    return static_cast<osMessageQueueId_t>(raw);
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void * msg_ptr, uint8_t /* msg_prio */, uint32_t /* timeout */)
{
    if (mq_id == nullptr || msg_ptr == nullptr)
    {
        return osErrorParameter;
    }
    auto * mq = static_cast<PosixMessageQueue *>(mq_id);
    std::lock_guard<std::mutex> lock(mq->mtx);
    if (mq->shutdown)
    {
        return osErrorResource;
    }
    if (mq->queue.size() >= mq->msgCount)
    {
        return osErrorResource;
    }
    const auto * bytes = static_cast<const uint8_t *>(msg_ptr);
    mq->queue.emplace_back(bytes, bytes + mq->msgSize);
    mq->cv.notify_one();
    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void * msg_ptr, uint8_t * /* msg_prio */, uint32_t timeout)
{
    if (mq_id == nullptr || msg_ptr == nullptr)
    {
        return osErrorParameter;
    }
    auto * mq = static_cast<PosixMessageQueue *>(mq_id);
    std::unique_lock<std::mutex> lock(mq->mtx);
    if (!mq->firstWaitSignal)
    {
        mq->firstWaitSignal = true;
        mq->readyCv.notify_all();
    }
    if (mq->shutdown)
    {
        lock.unlock();
        pthread_exit(nullptr);
    }

    if (mq->queue.empty())
    {
        if (timeout == 0)
        {
            return osErrorResource;
        }
        if (timeout == osWaitForever)
        {
            mq->cv.wait(lock, [mq]() { return mq->shutdown || !mq->queue.empty(); });
        }
        else
        {
            mq->cv.wait_for(lock, std::chrono::milliseconds(timeout), [mq]() { return mq->shutdown || !mq->queue.empty(); });
        }
    }

    if (mq->shutdown)
    {
        lock.unlock();
        pthread_exit(nullptr);
    }

    if (mq->queue.empty())
    {
        return osErrorTimeout;
    }

    auto front = std::move(mq->queue.front());
    mq->queue.pop_front();
    std::memcpy(msg_ptr, front.data(), mq->msgSize);
    return osOK;
}

osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
    {
        return osErrorParameter;
    }
    auto * mq = static_cast<PosixMessageQueue *>(mq_id);
    std::lock_guard<std::mutex> lock(gQueueRegistryMutex);
    gMessageQueues.erase(std::remove_if(gMessageQueues.begin(), gMessageQueues.end(),
                                        [mq](const std::unique_ptr<PosixMessageQueue> & item) { return item.get() == mq; }),
                         gMessageQueues.end());
    return osOK;
}

osThreadId_t osThreadNew(osThreadFunc_t func, void * argument, const osThreadAttr_t * /* attr */)
{
    if (func == nullptr)
    {
        return nullptr;
    }
    auto ctx      = std::make_unique<PosixThreadStartCtx>();
    ctx->func     = func;
    ctx->arg      = argument;
    pthread_t tid = 0;
    if (pthread_create(&tid, nullptr, PosixThreadEntry, ctx.get()) != 0)
    {
        return nullptr;
    }
    (void) ctx.release();
    sAppTaskPthread        = tid;
    sAppTaskPthreadCreated = true;
    return reinterpret_cast<osThreadId_t>(tid);
}

void silabsInitLog(void) {}

void silabsLog(const char * aFormat, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, aFormat);
    vsnprintf(buffer, sizeof(buffer), aFormat, args);
    va_end(args);
    ChipLogProgress(AppServer, "[SILABS] %s", buffer);
    fflush(stdout);
}

void appError(int err)
{
    ChipLogError(AppServer, "[SILABS] App Critical Error: %d", err);
    fflush(stdout);
    chipAbort();
}

} // extern "C"

void appError(CHIP_ERROR error)
{
    appError(static_cast<int>(error.AsInteger()));
}

// ============================================================================
// SilabsPlatform POSIX Implementation
// ============================================================================

namespace chip {
namespace DeviceLayer {
namespace Silabs {

SilabsPlatform SilabsPlatform::sSilabsPlatformAbstractionManager;
SilabsPlatform::SilabsButtonCb SilabsPlatform::mButtonCallback = nullptr;

CHIP_ERROR SilabsPlatform::Init(void)
{
    std::lock_guard<std::mutex> lock(gLedMutex);
    std::memset(gLedStates, 0, sizeof(gLedStates));
    std::memset(gButtonStates, 0, sizeof(gButtonStates));
    gRgbRed   = 0;
    gRgbGreen = 0;
    gRgbBlue  = 0;
    return CHIP_NO_ERROR;
}

void SilabsPlatform::InitLed(void)
{
    ChipLogProgress(AppServer, "[SILABS-LIGHT] WSTK LEDs initialized");
}

CHIP_ERROR SilabsPlatform::SetLed(bool state, uint8_t led)
{
    {
        std::lock_guard<std::mutex> lock(gLedMutex);
        if (led < sizeof(gLedStates))
        {
            gLedStates[led] = state;
        }
    }
    if (led == LIGHT_ACTUATOR_LED)
    {
        ChipLogProgress(AppServer, "[SILABS-LIGHT] LED %u state=%s", static_cast<unsigned>(led), state ? "ON" : "OFF");
        fflush(stdout);
    }
    return CHIP_NO_ERROR;
}

bool SilabsPlatform::GetLedState(uint8_t led)
{
    std::lock_guard<std::mutex> lock(gLedMutex);
    return (led < sizeof(gLedStates)) ? gLedStates[led] : false;
}

CHIP_ERROR SilabsPlatform::ToggleLed(uint8_t led)
{
    bool nextState = false;
    {
        std::lock_guard<std::mutex> lock(gLedMutex);
        if (led < sizeof(gLedStates))
        {
            gLedStates[led] = !gLedStates[led];
            nextState       = gLedStates[led];
        }
    }
    if (led == LIGHT_ACTUATOR_LED)
    {
        ChipLogProgress(AppServer, "[SILABS-LIGHT] LED %u state=%s", static_cast<unsigned>(led), nextState ? "ON" : "OFF");
        fflush(stdout);
    }
    return CHIP_NO_ERROR;
}

uint8_t SilabsPlatform::GetButtonState(uint8_t button)
{
    std::lock_guard<std::mutex> lock(gLedMutex);
    return (button < sizeof(gButtonStates)) ? gButtonStates[button] : 0;
}

#if (defined(SL_MATTER_RGB_LED_ENABLED) && SL_MATTER_RGB_LED_ENABLED == 1)
bool SilabsPlatform::GetRGBLedState(uint8_t /* led */)
{
    return true;
}

CHIP_ERROR SilabsPlatform::SetLedColor(uint8_t led, uint8_t r, uint8_t g, uint8_t b)
{
    {
        std::lock_guard<std::mutex> lock(gLedMutex);
        gRgbRed   = r;
        gRgbGreen = g;
        gRgbBlue  = b;
    }
    ChipLogProgress(AppServer, "[SILABS-LIGHT] RGB LED %u color=(%u,%u,%u)", static_cast<unsigned>(led), static_cast<unsigned>(r),
                    static_cast<unsigned>(g), static_cast<unsigned>(b));
    fflush(stdout);
    return CHIP_NO_ERROR;
}

CHIP_ERROR SilabsPlatform::GetLedColor(uint8_t /* led */, uint16_t & r, uint16_t & g, uint16_t & b)
{
    std::lock_guard<std::mutex> lock(gLedMutex);
    r = static_cast<uint16_t>(gRgbRed * 2);
    g = static_cast<uint16_t>(gRgbGreen * 2);
    b = static_cast<uint16_t>(gRgbBlue * 2);
    return CHIP_NO_ERROR;
}
#endif // SL_MATTER_RGB_LED_ENABLED

CHIP_ERROR SilabsPlatform::FlashInit()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR SilabsPlatform::FlashErasePage(uint32_t /* addr */)
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR SilabsPlatform::FlashWritePage(uint32_t /* addr */, const uint8_t * /* data */, size_t /* size */)
{
    return CHIP_NO_ERROR;
}

void SilabsPlatform::SoftwareReset(void)
{
    ChipLogProgress(AppServer, "[SILABS] SoftwareReset requested");
}

CHIP_ERROR SilabsPlatform::VerifyIfUpdated()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR SilabsPlatform::NvmInit()
{
    return CHIP_NO_ERROR;
}

void TriggerSilabsButtonAction(uint8_t button, uint8_t action)
{
    {
        std::lock_guard<std::mutex> lock(gLedMutex);
        if (button < sizeof(gButtonStates))
        {
            gButtonStates[button] = action;
        }
    }
    ChipLogProgress(AppServer, "[SILABS-LIGHT] Button %u action=%u", static_cast<unsigned>(button), static_cast<unsigned>(action));
    if (SilabsPlatform::mButtonCallback != nullptr)
    {
        SilabsPlatform::mButtonCallback(button, action);
    }
}

void ShutdownSilabsPosixRuntime()
{
    chip::Server::GetInstance().GetFabricTable().RemoveFabricDelegate(&BaseApplication::sAppDelegate);

    if (sAppEventQueue != nullptr)
    {
        auto * mq = static_cast<PosixMessageQueue *>(sAppEventQueue);
        {
            std::lock_guard<std::mutex> lock(mq->mtx);
            mq->shutdown = true;
            mq->cv.notify_all();
        }
        if (sAppTaskPthreadCreated)
        {
            pthread_join(sAppTaskPthread, nullptr);
            sAppTaskPthreadCreated = false;
        }
        osMessageQueueDelete(sAppEventQueue);
        sAppEventQueue = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(gTimerMutex);
        for (auto & timer : gTimers)
        {
            if (timer)
            {
                timer->running.store(false);
                timer->deleted.store(true);
            }
        }
        gTimers.clear();
        gTimerWorkerStop = true;
        gTimerCv.notify_all();
    }

    if (gTimerWorkerThread.joinable())
    {
        gTimerWorkerThread.join();
    }
    gTimerWorkerRunning = false;
}

} // namespace Silabs
} // namespace DeviceLayer
} // namespace chip

// ============================================================================
// BaseApplication & Delegate POSIX Implementation
// ============================================================================

bool BaseApplication::sIsProvisioned                  = false;
bool BaseApplication::sIsFactoryResetTriggered        = false;
LEDWidget * BaseApplication::sAppActionLed            = nullptr;
BaseApplicationDelegate BaseApplication::sAppDelegate = BaseApplicationDelegate();

void BaseApplicationDelegate::OnCommissioningSessionStarted()
{
    isComissioningStarted = true;
}

void BaseApplicationDelegate::OnCommissioningSessionStopped()
{
    isComissioningStarted = false;
}

void BaseApplicationDelegate::OnCommissioningSessionEstablishmentError(CHIP_ERROR /* err */)
{
    isComissioningStarted = false;
}

void BaseApplicationDelegate::OnCommissioningWindowClosed()
{
    if (BaseApplication::GetProvisionStatus())
    {
        isComissioningStarted = false;
    }
}

void BaseApplicationDelegate::OnFabricCommitted(const FabricTable & fabricTable, FabricIndex /* fabricIndex */)
{
    if (fabricTable.FabricCount() == 1)
    {
        BaseApplication::UpdateCommissioningStatus(true);
    }
}

void BaseApplicationDelegate::OnFabricRemoved(const FabricTable & fabricTable, FabricIndex /* fabricIndex */)
{
    if (fabricTable.FabricCount() == 0)
    {
        BaseApplication::UpdateCommissioningStatus(false);
        BaseApplication::DoProvisioningReset();
    }
}

CHIP_ERROR BaseApplication::StartAppTask(osThreadFunc_t taskFunction)
{
    ReturnErrorOnFailure(PlatformMgr().AddEventHandler(OnPlatformEvent, 0));

    sAppEventQueue = osMessageQueueNew(APP_EVENT_QUEUE_SIZE, sizeof(AppEvent), nullptr);
    if (sAppEventQueue == nullptr)
    {
        ChipLogError(AppServer, "Failed to allocate app event queue");
        appError(APP_ERROR_EVENT_QUEUE_FAILED);
    }

    osThreadId_t tid = osThreadNew(taskFunction, &sAppEventQueue, nullptr);
    if (tid == nullptr)
    {
        ChipLogError(AppServer, "Failed to create app task");
        appError(APP_ERROR_CREATE_TASK_FAILED);
    }

    // Wait for AppTaskMain to complete BaseApplication::Init() + AppTask::AppInit()
    // before the main thread enters PlatformMgr().RunEventLoop(), eliminating any
    // startup lock race on mChipStackLock.
    auto * mq = static_cast<PosixMessageQueue *>(sAppEventQueue);
    std::unique_lock<std::mutex> lock(mq->mtx);
    mq->readyCv.wait_for(lock, std::chrono::seconds(5), [mq]() { return mq->firstWaitSignal; });

    return CHIP_NO_ERROR;
}

CHIP_ERROR BaseApplication::Init()
{
    CHIP_ERROR err = BaseInit();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "BaseInit() failed");
        appError(err);
        return err;
    }

    err = AppInit();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "AppInit() failed");
        appError(err);
        return err;
    }

    mIsApplicationInitialized = true;
    return CHIP_NO_ERROR;
}

CHIP_ERROR BaseApplication::BaseInit()
{
    sFunctionTimer = osTimerNew(FunctionTimerEventHandler, osTimerOnce, this, nullptr);
    if (sFunctionTimer == nullptr)
    {
        ChipLogError(AppServer, "funct timer create failed");
        appError(APP_ERROR_CREATE_TIMER_FAILED);
    }

    sStatusLightTimer = osTimerNew(LightTimerEventHandler, osTimerPeriodic, this, nullptr);
    if (sStatusLightTimer == nullptr)
    {
        ChipLogError(AppServer, "Light Timer create failed");
        appError(APP_ERROR_CREATE_TIMER_FAILED);
    }

    ChipLogProgress(AppServer, "Current Software Version String: %s", CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING);
    ChipLogProgress(AppServer, "Current Software Version: %d", CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION);

    LEDWidget::InitGpio();
    sStatusLED.Init(SYSTEM_STATE_LED);

    BaseApplication::sIsProvisioned = ConnectivityMgr().IsWiFiStationProvisioned();

    return chip::Server::GetInstance().GetFabricTable().AddFabricDelegate(&sAppDelegate);
}

void BaseApplication::InitCompleteCallback(CHIP_ERROR /* err */) {}

void BaseApplication::FunctionTimerEventHandler(void * timerCbArg)
{
    AppEvent event{};
    event.Type               = AppEvent::kEventType_Timer;
    event.TimerEvent.Context = timerCbArg;
    event.Handler            = FunctionEventHandler;
    PostEvent(&event);
}

void BaseApplication::FunctionEventHandler(AppEvent * aEvent)
{
    VerifyOrReturn(aEvent != nullptr && aEvent->Type == AppEvent::kEventType_Timer);
    if (!sIsFactoryResetTriggered)
    {
        StartFactoryResetSequence();
    }
    else
    {
        ScheduleFactoryReset();
    }
}

bool BaseApplication::ActivateStatusLedPatterns()
{
    sStatusLED.Set(BaseApplication::sIsProvisioned);
    return true;
}

void BaseApplication::UpdateCommissioningStatus(bool newState)
{
    BaseApplication::sIsProvisioned = newState;
    ActivateStatusLedPatterns();
}

void BaseApplication::LightEventHandler()
{
    if (!sIsFactoryResetTriggered)
    {
        ActivateStatusLedPatterns();
    }
    sStatusLED.Animate();
    if (sAppActionLed != nullptr)
    {
        sAppActionLed->Animate();
    }
}

void BaseApplication::LightTimerEventHandler(void * /* timerCbArg */)
{
    LightEventHandler();
}

void BaseApplication::ButtonHandler(AppEvent * aEvent)
{
    VerifyOrReturn(aEvent != nullptr);
    if (aEvent->ButtonEvent.Action == static_cast<uint8_t>(SilabsPlatform::ButtonAction::ButtonPressed))
    {
        StartFunctionTimer(FACTORY_RESET_TRIGGER_TIMEOUT);
    }
    else
    {
        if (sIsFactoryResetTriggered)
        {
            CancelFactoryResetSequence();
        }
        else
        {
            CancelFunctionTimer();
        }
    }
}

void BaseApplication::CancelFunctionTimer()
{
    if (sFunctionTimer != nullptr)
    {
        (void) osTimerStop(sFunctionTimer);
    }
}

void BaseApplication::StartFunctionTimer(uint32_t aTimeoutInMs)
{
    if (sFunctionTimer != nullptr)
    {
        (void) osTimerStart(sFunctionTimer, pdMS_TO_TICKS(aTimeoutInMs));
    }
}

void BaseApplication::StartFactoryResetSequence()
{
    ChipLogProgress(AppServer, "Factory Reset Triggered. Release button within %ums to cancel.",
                    FACTORY_RESET_CANCEL_WINDOW_TIMEOUT);
    StartFunctionTimer(FACTORY_RESET_CANCEL_WINDOW_TIMEOUT);
    sIsFactoryResetTriggered = true;
}

void BaseApplication::CancelFactoryResetSequence()
{
    CancelFunctionTimer();
    if (sIsFactoryResetTriggered)
    {
        sIsFactoryResetTriggered = false;
        ChipLogProgress(AppServer, "Factory Reset has been cancelled");
    }
}

void BaseApplication::StartStatusLEDTimer()
{
    if (sStatusLightTimer != nullptr)
    {
        (void) osTimerStart(sStatusLightTimer, kStatusLightTimerPeriodMs);
    }
}

void BaseApplication::StopStatusLEDTimer()
{
    sStatusLED.Set(false);
    if (sStatusLightTimer != nullptr)
    {
        (void) osTimerStop(sStatusLightTimer);
    }
}

#ifdef MATTER_DM_PLUGIN_IDENTIFY_SERVER
void BaseApplication::OnIdentifyStart(Identify * /* identify */)
{
    ChipLogDetail(Zcl, "onIdentifyStart");
}

void BaseApplication::OnIdentifyStop(Identify * /* identify */)
{
    ChipLogDetail(Zcl, "onIdentifyStop");
}

void BaseApplication::OnTriggerIdentifyEffectCompleted(chip::System::Layer * /* systemLayer */, void * /* appState */)
{
    sIdentifyEffect = Clusters::Identify::EffectIdentifierEnum::kStopEffect;
}

void BaseApplication::OnTriggerIdentifyEffect(Identify * identify)
{
    if (identify != nullptr)
    {
        sIdentifyEffect = identify->mCurrentEffectIdentifier;
    }
}

void emberAfIdentifyClusterInitCallback(chip::EndpointId endpoint)
{
    IdentifyPool.CreateObject(endpoint, BaseApplication::OnIdentifyStart, BaseApplication::OnIdentifyStop,
                              Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator, BaseApplication::OnTriggerIdentifyEffect);
}
#endif // MATTER_DM_PLUGIN_IDENTIFY_SERVER

void BaseApplication::NotifyCodeDrivenIdentifyStart() {}
void BaseApplication::NotifyCodeDrivenIdentifyStop() {}
void BaseApplication::NotifyCodeDrivenTriggerEffect(Clusters::Identify::EffectIdentifierEnum /* effect */,
                                                    Clusters::Identify::EffectVariantEnum /* variant */)
{}

void BaseApplication::PostEvent(const AppEvent * aEvent)
{
    if (sAppEventQueue != nullptr && aEvent != nullptr)
    {
        if (osMessageQueuePut(sAppEventQueue, aEvent, 0, 0) != osOK)
        {
            ChipLogError(AppServer, "Failed to post event to app task event queue");
        }
    }
}

void BaseApplication::DispatchEvent(AppEvent * aEvent)
{
    if (aEvent != nullptr && aEvent->Handler != nullptr)
    {
        aEvent->Handler(aEvent);
    }
}

void BaseApplication::ScheduleFactoryReset()
{
    TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork([](intptr_t) {
        PlatformMgr().HandleServerShuttingDown();
        ConfigurationMgr().InitiateFactoryReset();
    });
}

void BaseApplication::DoProvisioningReset()
{
    TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork([](intptr_t) {
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
        chip::DeviceLayer::ConnectivityMgr().ClearWiFiStationProvision();
#endif
        (void) Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow();
    });
}

void BaseApplication::OnPlatformEvent(const ChipDeviceEvent * event, intptr_t)
{
    if (event == nullptr)
    {
        return;
    }
    if (event->Type == DeviceEventType::kServiceProvisioningChange)
    {
        BaseApplication::sIsProvisioned = event->ServiceProvisioningChange.IsServiceProvisioned;
    }
    else if (event->Type == DeviceEventType::kCommissioningComplete)
    {
        BaseApplication::sIsProvisioned = true;
        ChipLogProgress(AppServer, "[SILABS] Commissioning complete event received");
    }
}

void BaseApplication::OutputQrCode(bool /* refreshLCD */) {}

bool BaseApplication::GetProvisionStatus()
{
    return BaseApplication::sIsProvisioned;
}

// ============================================================================
// Matter Data Model Callbacks & Linux AppMain TestEventTrigger Hooks
// ============================================================================

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath & attributePath, uint8_t type, uint16_t size,
                                       uint8_t * value)
{
    VerifyOrReturn(CustomerAppTask::GetAppTask().IsApplicationInitialized());
    CustomerAppTask::GetAppTask().DMPostAttributeChangeCallback(attributePath, type, size, value);
}

void emberAfOnOffClusterInitCallback(chip::EndpointId /* endpoint */) {}

bool HandleSmokeCOTestEventTrigger(uint64_t /* eventTrigger */)
{
    return false;
}

bool HandleBooleanStateConfigurationTestEventTrigger(uint64_t /* eventTrigger */)
{
    return false;
}

namespace chip {

CHIP_ERROR BooleanStateConfigurationTestEventTriggerHandler::HandleEventTrigger(uint64_t eventTrigger)
{
    eventTrigger = clearEndpointInEventTrigger(eventTrigger);
    return HandleBooleanStateConfigurationTestEventTrigger(eventTrigger) ? CHIP_NO_ERROR : CHIP_ERROR_INVALID_ARGUMENT;
}

} // namespace chip
