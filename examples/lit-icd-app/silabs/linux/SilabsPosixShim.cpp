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

// SIMULATION-DISCLOSURE: POSIX and ARM32 QEMU runtime adapter for the Silicon Labs
// Lighting-App (examples/lighting-app/silabs/src/AppTask.cpp, CustomerAppTask.cpp,
// LEDWidget.cpp, RGBLEDWidget.cpp, and lighting-wifi-app.zap). Maps CMSIS-RTOS2
// queues/timers/threads, SilabsPlatform WSTK LED/button peripherals, and Silicon Labs
// EFR32 BLEManagerImpl (sl_bt_* BGAPI over /dev/ttySilabsBT0 HCI H4 UART PTY) plus
// GLib-free Wi-Fi station commissioning onto Linux pthreads and sockets.
//
// What is translated vs. emulated:
//   - BLE: every sl_bt_* event delivered to BLEManagerImpl is derived from a real
//     HCI event or ATT PDU read from the PTY (system_boot <- HCI_Reset Command
//     Complete, connection_opened <- LE Connection Complete, gatt_mtu_exchanged <-
//     ATT_EXCHANGE_MTU_REQ). The GATT database is a fixed CHIPoBLE table served
//     by this file in place of the Silabs link-layer/GATT stack (libbluetooth.a is
//     a Cortex-M binary blob and cannot run here).
//   - Wi-Fi: scan and connect call fi.w1.wpa_supplicant1 over D-Bus and report
//     only what is read back (BSS list, State). Failures are reported as failures;
//     no SSID, BSSID, IP address, or connection state is synthesized.

#include "SilabsPosixShim.h"

#include "AppConfig.h"
#include "AppEvent.h"
#include "AppTask.h"
#include "BaseApplication.h"

#include "LEDWidget.h"
#include "silabs_utils.h"

#include <app/clusters/boolean-state-configuration-server/BooleanStateConfigurationTestEventTriggerHandler.h>
#include <app/clusters/smoke-co-alarm-server/SmokeCOTestEventTriggerHandler.h>
#include <app/server/Dnssd.h>
#include <app/server/Server.h>
#include <cmsis_os2.h>
#include <inet/InetInterface.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/silabs/platformAbstraction/SilabsPlatform.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <ifaddrs.h>
#include <memory>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(CHIP_LINUX_USE_SILABS_BLE_WIFI) && CHIP_LINUX_USE_SILABS_BLE_WIFI
#include "gatt_db.h"
#include "rail.h"
#include "sl_bt_api.h"
#include <platform/Linux/NetworkCommissioningDriver.h>
#include <platform/internal/BLEManager.h>
#endif

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
        return osErrorResource;
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
        return osErrorResource;
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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-format-attribute"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    vsnprintf(buffer, sizeof(buffer), aFormat, args);
#pragma GCC diagnostic pop
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
            pthread_detach(sAppTaskPthread);
            sAppTaskPthreadCreated = false;
        }
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

#if defined(CHIP_LINUX_USE_SILABS_BLE_WIFI) && CHIP_LINUX_USE_SILABS_BLE_WIFI

// ============================================================================
// Silicon Labs BGAPI (sl_bt_*) over HCI H4 PTY (/dev/ttySilabsBT0) + ATT GATT
// ============================================================================

namespace {

std::mutex gBtPtyMutex;
int gBtPtyFd = -1;
std::atomic<bool> gBtReaderRunning{ false };
std::atomic<bool> gBtReaderStop{ false };
std::thread gBtReaderThread;

uint16_t gActiveHciConnHandle = 0x0040;
uint16_t gCccdValue           = 0;
bd_addr gAdvRandomAddr        = { { 0 } };
bool gHasAdvRandomAddr        = false;
uint16_t gAdvIntervalMin      = 0x0020;
uint16_t gAdvIntervalMax      = 0x0040;

bool WriteAllToFd(int fd, const uint8_t * buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                usleep(1000);
                continue;
            }
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool ReadExactFromFd(int fd, uint8_t * buf, size_t len)
{
    size_t off = 0;
    while (off < len && !gBtReaderStop.load())
    {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr            = poll(&pfd, 1, 200);
        if (pr == 0)
        {
            continue;
        }
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        ssize_t n = read(fd, buf + off, len - off);
        if (n <= 0)
        {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
            {
                continue;
            }
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return off == len;
}

void SendHciCommand(uint16_t opcode, const uint8_t * params, uint8_t paramLen)
{
    std::lock_guard<std::mutex> lock(gBtPtyMutex);
    if (gBtPtyFd < 0)
    {
        return;
    }
    uint8_t frame[260];
    frame[0] = 0x01; // H4 Command
    frame[1] = static_cast<uint8_t>(opcode & 0xFF);
    frame[2] = static_cast<uint8_t>((opcode >> 8) & 0xFF);
    frame[3] = paramLen;
    if (paramLen > 0 && params != nullptr)
    {
        std::memcpy(&frame[4], params, paramLen);
    }
    (void) WriteAllToFd(gBtPtyFd, frame, static_cast<size_t>(4 + paramLen));
}

void SendHciAclData(uint16_t connHandle, uint16_t cid, const uint8_t * payload, uint16_t len)
{
    std::lock_guard<std::mutex> lock(gBtPtyMutex);
    if (gBtPtyFd < 0)
    {
        return;
    }
    const uint16_t aclLen      = static_cast<uint16_t>(len + 4);
    const uint16_t handleFlags = static_cast<uint16_t>((connHandle & 0x0FFF) | 0x2000);
    std::vector<uint8_t> frame(9 + len);
    frame[0] = 0x02; // H4 ACL Data
    frame[1] = static_cast<uint8_t>(handleFlags & 0xFF);
    frame[2] = static_cast<uint8_t>((handleFlags >> 8) & 0xFF);
    frame[3] = static_cast<uint8_t>(aclLen & 0xFF);
    frame[4] = static_cast<uint8_t>((aclLen >> 8) & 0xFF);
    frame[5] = static_cast<uint8_t>(len & 0xFF);
    frame[6] = static_cast<uint8_t>((len >> 8) & 0xFF);
    frame[7] = static_cast<uint8_t>(cid & 0xFF);
    frame[8] = static_cast<uint8_t>((cid >> 8) & 0xFF);
    if (len > 0 && payload != nullptr)
    {
        std::memcpy(&frame[9], payload, len);
    }
    (void) WriteAllToFd(gBtPtyFd, frame.data(), frame.size());
}

void SendAttErrorRsp(uint16_t connHandle, uint8_t reqOpcode, uint16_t attrHandle, uint8_t errorCode)
{
    const uint8_t rsp[5] = {
        0x01, reqOpcode, static_cast<uint8_t>(attrHandle & 0xFF), static_cast<uint8_t>((attrHandle >> 8) & 0xFF), errorCode,
    };
    SendHciAclData(connHandle, 0x0004, rsp, sizeof(rsp));
}

void HandleAttPdu(uint16_t connHandle, const uint8_t * attPdu, uint16_t attLen)
{
    if (attLen < 1)
    {
        return;
    }
    const uint8_t opcode = attPdu[0];
    switch (opcode)
    {
    case 0x02: { // ATT_EXCHANGE_MTU_REQ
        if (attLen < 3)
        {
            SendAttErrorRsp(connHandle, opcode, 0x0000, 0x04);
            return;
        }
        const uint16_t clientMtu     = static_cast<uint16_t>(attPdu[1] | (attPdu[2] << 8));
        const uint16_t serverMtu     = 247;
        const uint16_t negotiatedMtu = std::min<uint16_t>(std::max<uint16_t>(clientMtu, 23), serverMtu);
        const uint8_t rsp[3]         = {
            0x03,
            static_cast<uint8_t>(serverMtu & 0xFF),
            static_cast<uint8_t>((serverMtu >> 8) & 0xFF),
        };
        SendHciAclData(connHandle, 0x0004, rsp, sizeof(rsp));

        sl_bt_msg_t evt{};
        evt.header                                 = sl_bt_evt_gatt_mtu_exchanged_id;
        evt.data.evt_gatt_mtu_exchanged.connection = 1;
        evt.data.evt_gatt_mtu_exchanged.mtu        = negotiatedMtu;
        sl_bt_on_event(&evt);
        break;
    }

    case 0x10: { // ATT_READ_BY_GROUP_TYPE_REQ
        if (attLen < 7)
        {
            SendAttErrorRsp(connHandle, opcode, 0x0000, 0x04);
            return;
        }
        const uint16_t startHandle = static_cast<uint16_t>(attPdu[1] | (attPdu[2] << 8));
        const uint16_t endHandle   = static_cast<uint16_t>(attPdu[3] | (attPdu[4] << 8));
        const uint16_t groupUuid   = (attLen == 7) ? static_cast<uint16_t>(attPdu[5] | (attPdu[6] << 8)) : 0;
        // Primary Service 0x2800 -> Matter Service 0xFFF6 spanning handles 0x0010 (16) .. 0x0015 (21)
        if (groupUuid == 0x2800 && startHandle <= 0x0010 && endHandle >= 0x0010)
        {
            const uint8_t rsp[8] = { 0x11, 0x06, 0x10, 0x00, 0x15, 0x00, 0xF6, 0xFF };
            SendHciAclData(connHandle, 0x0004, rsp, sizeof(rsp));
        }
        else
        {
            SendAttErrorRsp(connHandle, opcode, startHandle, 0x0A);
        }
        break;
    }

    case 0x06: { // ATT_FIND_BY_TYPE_VALUE_REQ
        if (attLen < 7)
        {
            SendAttErrorRsp(connHandle, opcode, 0x0000, 0x04);
            return;
        }
        const uint16_t startHandle = static_cast<uint16_t>(attPdu[1] | (attPdu[2] << 8));
        const uint16_t endHandle   = static_cast<uint16_t>(attPdu[3] | (attPdu[4] << 8));
        const uint16_t attrType    = static_cast<uint16_t>(attPdu[5] | (attPdu[6] << 8));
        if (attrType == 0x2800 && attLen >= 9 && attPdu[7] == 0xF6 && attPdu[8] == 0xFF && startHandle <= 0x0010 &&
            endHandle >= 0x0010)
        {
            const uint8_t rsp[5] = { 0x07, 0x10, 0x00, 0x15, 0x00 };
            SendHciAclData(connHandle, 0x0004, rsp, sizeof(rsp));
        }
        else
        {
            SendAttErrorRsp(connHandle, opcode, startHandle, 0x0A);
        }
        break;
    }

    case 0x08: { // ATT_READ_BY_TYPE_REQ
        if (attLen < 7)
        {
            SendAttErrorRsp(connHandle, opcode, 0x0000, 0x04);
            return;
        }
        const uint16_t startHandle = static_cast<uint16_t>(attPdu[1] | (attPdu[2] << 8));
        const uint16_t endHandle   = static_cast<uint16_t>(attPdu[3] | (attPdu[4] << 8));
        const uint16_t attrType    = (attLen == 7) ? static_cast<uint16_t>(attPdu[5] | (attPdu[6] << 8)) : 0;
        if (attrType == 0x2803)
        {
            // C1 (RX): decl handle 0x0011 (17), value handle 0x0012 (18 = gattdb_CHIPoBLEChar_Rx), props 0x08 (Write)
            if (startHandle <= 0x0011 && endHandle >= 0x0011)
            {
                const uint8_t rsp[23] = {
                    0x09, 0x15, 0x11, 0x00, 0x08, 0x12, 0x00, 0x11, 0x9d, 0x9f, 0x42, 0x9c,
                    0x4f, 0x9f, 0x95, 0x59, 0x45, 0x3d, 0x26, 0xf5, 0x2e, 0xee, 0x18,
                };
                SendHciAclData(connHandle, 0x0004, rsp, sizeof(rsp));
                return;
            }
            // C2 (TX): decl handle 0x0013 (19), value handle 0x0014 (20 = gattdb_CHIPoBLEChar_Tx), props 0x20 (Indicate)
            if (startHandle <= 0x0013 && endHandle >= 0x0013)
            {
                const uint8_t rsp[23] = {
                    0x09, 0x15, 0x13, 0x00, 0x20, 0x14, 0x00, 0x12, 0x9d, 0x9f, 0x42, 0x9c,
                    0x4f, 0x9f, 0x95, 0x59, 0x45, 0x3d, 0x26, 0xf5, 0x2e, 0xee, 0x18,
                };
                SendHciAclData(connHandle, 0x0004, rsp, sizeof(rsp));
                return;
            }
        }
        SendAttErrorRsp(connHandle, opcode, startHandle, 0x0A);
        break;
    }

    case 0x04: { // ATT_FIND_INFORMATION_REQ
        if (attLen < 5)
        {
            SendAttErrorRsp(connHandle, opcode, 0x0000, 0x04);
            return;
        }
        const uint16_t startHandle = static_cast<uint16_t>(attPdu[1] | (attPdu[2] << 8));
        const uint16_t endHandle   = static_cast<uint16_t>(attPdu[3] | (attPdu[4] << 8));
        // CCCD (0x2902) for C2 (TX) is at handle 0x0015 (21)
        if (startHandle <= 0x0015 && endHandle >= 0x0015)
        {
            const uint8_t rsp[6] = { 0x05, 0x01, 0x15, 0x00, 0x02, 0x29 };
            SendHciAclData(connHandle, 0x0004, rsp, sizeof(rsp));
        }
        else
        {
            SendAttErrorRsp(connHandle, opcode, startHandle, 0x0A);
        }
        break;
    }

    case 0x0A: { // ATT_READ_REQ
        if (attLen < 3)
        {
            SendAttErrorRsp(connHandle, opcode, 0x0000, 0x04);
            return;
        }
        const uint16_t attrHandle = static_cast<uint16_t>(attPdu[1] | (attPdu[2] << 8));
        if (attrHandle == 0x0015)
        {
            const uint8_t rsp[3] = {
                0x0B,
                static_cast<uint8_t>(gCccdValue & 0xFF),
                static_cast<uint8_t>((gCccdValue >> 8) & 0xFF),
            };
            SendHciAclData(connHandle, 0x0004, rsp, sizeof(rsp));
        }
        else if (attrHandle == gattdb_CHIPoBLEChar_C3)
        {
            sl_bt_msg_t evt{};
            evt.header                                                = sl_bt_evt_gatt_server_user_read_request_id;
            evt.data.evt_gatt_server_user_read_request.connection     = 1;
            evt.data.evt_gatt_server_user_read_request.characteristic = gattdb_CHIPoBLEChar_C3;
            evt.data.evt_gatt_server_user_read_request.att_opcode     = opcode;
            evt.data.evt_gatt_server_user_read_request.offset         = 0;
            sl_bt_on_event(&evt);
        }
        else
        {
            SendAttErrorRsp(connHandle, opcode, attrHandle, 0x0A);
        }
        break;
    }

    case 0x12:   // ATT_WRITE_REQ
    case 0x52: { // ATT_WRITE_CMD
        if (attLen < 3)
        {
            if (opcode == 0x12)
            {
                SendAttErrorRsp(connHandle, opcode, 0x0000, 0x04);
            }
            return;
        }
        const uint16_t attrHandle = static_cast<uint16_t>(attPdu[1] | (attPdu[2] << 8));
        const uint8_t * valPtr    = attPdu + 3;
        const uint16_t valLen     = static_cast<uint16_t>(attLen - 3);

        if (opcode == 0x12)
        {
            const uint8_t writeRsp[1] = { 0x13 };
            SendHciAclData(connHandle, 0x0004, writeRsp, sizeof(writeRsp));
        }

        if (attrHandle == 0x0015)
        {
            gCccdValue = (valLen >= 2) ? static_cast<uint16_t>(valPtr[0] | (valPtr[1] << 8)) : (valLen == 1 ? valPtr[0] : 0);
            sl_bt_msg_t evt{};
            evt.header                                                         = sl_bt_evt_gatt_server_characteristic_status_id;
            evt.data.evt_gatt_server_characteristic_status.connection          = 1;
            evt.data.evt_gatt_server_characteristic_status.characteristic      = gattdb_CHIPoBLEChar_Tx;
            evt.data.evt_gatt_server_characteristic_status.status_flags        = sl_bt_gatt_server_client_config;
            evt.data.evt_gatt_server_characteristic_status.client_config_flags = gCccdValue;
            sl_bt_on_event(&evt);
        }
        else if (attrHandle == gattdb_CHIPoBLEChar_Rx)
        {
            sl_bt_msg_t evt{};
            evt.header                                                 = sl_bt_evt_gatt_server_attribute_value_id;
            evt.data.evt_gatt_server_user_write_request.connection     = 1;
            evt.data.evt_gatt_server_user_write_request.characteristic = gattdb_CHIPoBLEChar_Rx;
            evt.data.evt_gatt_server_user_write_request.att_opcode     = opcode;
            evt.data.evt_gatt_server_user_write_request.offset         = 0;
            const uint16_t copyLen = std::min<uint16_t>(valLen, sizeof(evt.data.evt_gatt_server_user_write_request.value.data));
            evt.data.evt_gatt_server_user_write_request.value.len = copyLen;
            if (copyLen > 0)
            {
                std::memcpy(evt.data.evt_gatt_server_user_write_request.value.data, valPtr, copyLen);
            }
            sl_bt_on_event(&evt);
        }
        break;
    }

    case 0x1E: { // ATT_HANDLE_VALUE_CFM
        sl_bt_msg_t evt{};
        evt.header                                                         = sl_bt_evt_gatt_server_characteristic_status_id;
        evt.data.evt_gatt_server_characteristic_status.connection          = 1;
        evt.data.evt_gatt_server_characteristic_status.characteristic      = gattdb_CHIPoBLEChar_Tx;
        evt.data.evt_gatt_server_characteristic_status.status_flags        = sl_bt_gatt_server_confirmation;
        evt.data.evt_gatt_server_characteristic_status.client_config_flags = gCccdValue;
        sl_bt_on_event(&evt);
        break;
    }

    default:
        if (opcode != 0x01 && (opcode & 0x40) == 0)
        {
            SendAttErrorRsp(connHandle, opcode, 0x0000, 0x06);
        }
        break;
    }
}

void SilabsBtHciReaderLoop()
{
    // Reset the HCI controller. sl_bt_evt_system_boot_id is delivered to
    // BLEManagerImpl only after the controller answers HCI_Reset with a
    // successful Command Complete (handled in the event loop below).
    SendHciCommand(0x0C03, nullptr, 0);
    bool bootDelivered = false;

    while (!gBtReaderStop.load())
    {
        uint8_t pktType = 0;
        if (!ReadExactFromFd(gBtPtyFd, &pktType, 1))
        {
            break;
        }

        if (pktType == 0x04) // HCI Event
        {
            uint8_t hdr[2] = { 0, 0 };
            if (!ReadExactFromFd(gBtPtyFd, hdr, 2))
            {
                break;
            }
            const uint8_t evtCode  = hdr[0];
            const uint8_t paramLen = hdr[1];
            std::vector<uint8_t> params(paramLen);
            if (paramLen > 0 && !ReadExactFromFd(gBtPtyFd, params.data(), paramLen))
            {
                break;
            }

            if (evtCode == 0x0E && paramLen >= 4 && !bootDelivered) // Command Complete
            {
                const uint16_t opcode = static_cast<uint16_t>(params[1] | (params[2] << 8));
                if (opcode == 0x0C03)
                {
                    if (params[3] != 0x00)
                    {
                        ChipLogError(DeviceLayer, "[SILABS-BGAPI] HCI_Reset failed with status 0x%02x",
                                     static_cast<unsigned>(params[3]));
                        continue;
                    }
                    bootDelivered = true;
                    ChipLogProgress(DeviceLayer, "[SILABS-BGAPI] HCI_Reset Command Complete; delivering system boot event");
                    sl_bt_msg_t bootEvt{};
                    bootEvt.header                     = sl_bt_evt_system_boot_id;
                    bootEvt.data.evt_system_boot.major = 4;
                    bootEvt.data.evt_system_boot.minor = 4;
                    bootEvt.data.evt_system_boot.patch = 0;
                    bootEvt.data.evt_system_boot.build = 1;
                    sl_bt_on_event(&bootEvt);
                }
            }
            else if (evtCode == 0x3E && paramLen >= 1) // LE Meta Event
            {
                const uint8_t subEvt = params[0];
                if ((subEvt == 0x01 && paramLen >= 19) || (subEvt == 0x0A && paramLen >= 31))
                {
                    const uint8_t status = params[1];
                    if (status == 0x00)
                    {
                        gActiveHciConnHandle = static_cast<uint16_t>((params[2] | (params[3] << 8)) & 0x0FFF);
                        gCccdValue           = 0;
                        ChipLogProgress(DeviceLayer, "New BLE connection: conn=0x%02x",
                                        static_cast<unsigned>(gActiveHciConnHandle));

                        // BGAPI translation of HCI LE (Enhanced) Connection Complete.
                        sl_bt_msg_t openEvt{};
                        openEvt.header                                = sl_bt_evt_connection_opened_id;
                        openEvt.data.evt_connection_opened.connection = 1;
                        openEvt.data.evt_connection_opened.master     = 0;
                        openEvt.data.evt_connection_opened.bonding    = 0xFF;
                        openEvt.data.evt_connection_opened.advertiser = 0;
                        std::memcpy(openEvt.data.evt_connection_opened.address.addr, &params[6], 6);
                        openEvt.data.evt_connection_opened.address_type = params[5];
                        sl_bt_on_event(&openEvt);
                        // sl_bt_evt_gatt_mtu_exchanged_id is emitted only when the
                        // central actually sends ATT_EXCHANGE_MTU_REQ (see HandleAttPdu);
                        // until then BLEManagerImpl keeps the ATT default MTU of 23.
                    }
                }
            }
            else if (evtCode == 0x05 && paramLen >= 4) // Disconnection Complete
            {
                const uint8_t reason = params[3];
                gCccdValue           = 0;

                sl_bt_msg_t closeEvt{};
                closeEvt.header                                = sl_bt_evt_connection_closed_id;
                closeEvt.data.evt_connection_closed.connection = 1;
                closeEvt.data.evt_connection_closed.reason     = (reason == 0x16)
                    ? SL_STATUS_BT_CTRL_CONNECTION_TERMINATED_BY_LOCAL_HOST
                    : SL_STATUS_BT_CTRL_REMOTE_USER_TERMINATED;
                sl_bt_on_event(&closeEvt);
            }
        }
        else if (pktType == 0x02) // HCI ACL Data
        {
            uint8_t hdr[4] = { 0, 0, 0, 0 };
            if (!ReadExactFromFd(gBtPtyFd, hdr, 4))
            {
                break;
            }
            const uint16_t connHandle = static_cast<uint16_t>((hdr[0] | (hdr[1] << 8)) & 0x0FFF);
            const uint16_t aclLen     = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
            std::vector<uint8_t> aclPayload(aclLen);
            if (aclLen > 0 && !ReadExactFromFd(gBtPtyFd, aclPayload.data(), aclLen))
            {
                break;
            }
            if (aclLen >= 4)
            {
                const uint16_t l2capLen = static_cast<uint16_t>(aclPayload[0] | (aclPayload[1] << 8));
                const uint16_t cid      = static_cast<uint16_t>(aclPayload[2] | (aclPayload[3] << 8));
                const uint16_t availLen = static_cast<uint16_t>(aclLen - 4);
                const uint16_t pduLen   = std::min<uint16_t>(l2capLen, availLen);
                if (cid == 0x0004 && pduLen > 0)
                {
                    gActiveHciConnHandle = connHandle;
                    HandleAttPdu(connHandle, aclPayload.data() + 4, pduLen);
                }
            }
        }
    }
}

} // namespace

namespace chip {
namespace DeviceLayer {
namespace Internal {

CHIP_ERROR BLEManagerImpl::ConfigureBle(uint32_t /* aAdapterId */, bool /* aIsCentral */)
{
    if (gBtReaderRunning.load())
    {
        return CHIP_NO_ERROR;
    }

    const char * ptyPath = std::getenv("SILABS_BT_PTY_PATH");
    if (ptyPath == nullptr || ptyPath[0] == '\0')
    {
        ptyPath = "/dev/ttySilabsBT0";
    }

    int fd = open(ptyPath, O_RDWR | O_NOCTTY);
    if (fd >= 0)
    {
        struct termios tio;
        if (tcgetattr(fd, &tio) == 0)
        {
            cfmakeraw(&tio);
            (void) tcsetattr(fd, TCSANOW, &tio);
        }
        gBtPtyFd = fd;
        ChipLogProgress(DeviceLayer, "[SILABS-BGAPI] Opened HCI H4 UART PTY %s (fd=%d)", ptyPath, fd);
        gBtReaderStop.store(false);
        gBtReaderRunning.store(true);
        gBtReaderThread = std::thread(SilabsBtHciReaderLoop);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "[SILABS-BGAPI] PTY %s not present (errno=%d); booting Silabs BLE stack in local mode",
                        ptyPath, errno);
        HandleBootEvent();
    }
    return CHIP_NO_ERROR;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

extern "C" {

void RAIL_GetVersion(RAIL_Version_t * version, bool /* verbose */)
{
    if (version != nullptr)
    {
        version->major = 2;
        version->minor = 16;
        version->rev   = 0;
        version->build = 0;
        version->flags = 0;
    }
}

sl_status_t sl_bt_advertiser_create_set(uint8_t * handle)
{
    if (handle == nullptr)
    {
        return SL_STATUS_INVALID_PARAMETER;
    }
    *handle = 0;
    return SL_STATUS_OK;
}

sl_status_t sl_bt_advertiser_set_random_address(uint8_t /* handle */, uint8_t /* addr_type */, bd_addr address,
                                                bd_addr * address_out)
{
    gAdvRandomAddr    = address;
    gHasAdvRandomAddr = true;
    if (address_out != nullptr)
    {
        *address_out = address;
    }
    // HCI_LE_Set_Random_Address (0x2005)
    SendHciCommand(0x2005, address.addr, 6);
    return SL_STATUS_OK;
}

sl_status_t sl_bt_legacy_advertiser_set_data(uint8_t /* advertising_set */, uint8_t type, size_t data_len, const uint8_t * data)
{
    if (data_len > 31 || (data_len > 0 && data == nullptr))
    {
        return SL_STATUS_INVALID_PARAMETER;
    }
    uint8_t hciParams[32];
    std::memset(hciParams, 0, sizeof(hciParams));
    hciParams[0] = static_cast<uint8_t>(data_len);
    if (data_len > 0)
    {
        std::memcpy(&hciParams[1], data, data_len);
    }
    // 0x2008 = HCI_LE_Set_Advertising_Data, 0x2009 = HCI_LE_Set_Scan_Response_Data
    const uint16_t opcode = (type == sl_bt_advertiser_advertising_data_packet) ? 0x2008 : 0x2009;
    SendHciCommand(opcode, hciParams, sizeof(hciParams));
    return SL_STATUS_OK;
}

sl_status_t sl_bt_advertiser_set_timing(uint8_t /* advertising_set */, uint32_t interval_min, uint32_t interval_max,
                                        uint16_t /* duration */, uint8_t /* max_events */)
{
    gAdvIntervalMin = static_cast<uint16_t>(interval_min);
    gAdvIntervalMax = static_cast<uint16_t>(interval_max);
    return SL_STATUS_OK;
}

sl_status_t sl_bt_advertiser_configure(uint8_t /* advertising_set */, uint32_t /* flags */)
{
    return SL_STATUS_OK;
}

sl_status_t sl_bt_legacy_advertiser_start(uint8_t /* advertising_set */, uint8_t connect)
{
    uint8_t advParams[15];
    std::memset(advParams, 0, sizeof(advParams));
    advParams[0]  = static_cast<uint8_t>(gAdvIntervalMin & 0xFF);
    advParams[1]  = static_cast<uint8_t>((gAdvIntervalMin >> 8) & 0xFF);
    advParams[2]  = static_cast<uint8_t>(gAdvIntervalMax & 0xFF);
    advParams[3]  = static_cast<uint8_t>((gAdvIntervalMax >> 8) & 0xFF);
    advParams[4]  = (connect == sl_bt_advertiser_connectable_scannable) ? 0x00 : 0x02;
    advParams[5]  = gHasAdvRandomAddr ? 0x01 : 0x00;
    advParams[13] = 0x07; // All 3 advertising channels
    advParams[14] = 0x00;
    // HCI_LE_Set_Advertising_Parameters (0x2006)
    SendHciCommand(0x2006, advParams, sizeof(advParams));

    // HCI_LE_Set_Advertising_Enable (0x200A)
    const uint8_t enable = 0x01;
    SendHciCommand(0x200A, &enable, 1);
    ChipLogProgress(DeviceLayer, "GATT application registered successfully");
    return SL_STATUS_OK;
}

sl_status_t sl_bt_advertiser_stop(uint8_t /* advertising_set */)
{
    const uint8_t enable = 0x00;
    SendHciCommand(0x200A, &enable, 1);
    return SL_STATUS_OK;
}

sl_status_t sl_bt_advertiser_clear_random_address(uint8_t /* advertising_set */)
{
    gHasAdvRandomAddr = false;
    return SL_STATUS_OK;
}

sl_status_t sl_bt_advertiser_delete_set(uint8_t /* advertising_set */)
{
    return SL_STATUS_OK;
}

sl_status_t sl_bt_connection_set_default_parameters(uint16_t /* min_interval */, uint16_t /* max_interval */,
                                                    uint16_t /* latency */, uint16_t /* timeout */, uint16_t /* min_ce_length */,
                                                    uint16_t /* max_ce_length */)
{
    return SL_STATUS_OK;
}

sl_status_t sl_bt_connection_set_parameters(uint8_t /* connection */, uint16_t /* min_interval */, uint16_t /* max_interval */,
                                            uint16_t /* latency */, uint16_t /* timeout */, uint16_t /* min_ce_length */,
                                            uint16_t /* max_ce_length */)
{
    return SL_STATUS_OK;
}

sl_status_t sl_bt_connection_close(uint8_t /* connection */)
{
    const uint8_t params[3] = {
        static_cast<uint8_t>(gActiveHciConnHandle & 0xFF),
        static_cast<uint8_t>((gActiveHciConnHandle >> 8) & 0xFF),
        0x13,
    };
    SendHciCommand(0x0406, params, sizeof(params));
    return SL_STATUS_OK;
}

sl_status_t sl_bt_system_set_lazy_soft_timer(uint32_t /* time */, uint32_t /* slack */, uint8_t /* handle */,
                                             uint8_t /* single_shot */)
{
    return SL_STATUS_OK;
}

sl_status_t sl_bt_gatt_server_send_indication(uint8_t /* connection */, uint16_t characteristic, size_t value_len,
                                              const uint8_t * value)
{
    if (value_len > 0 && value == nullptr)
    {
        return SL_STATUS_INVALID_PARAMETER;
    }
    std::vector<uint8_t> attPdu(3 + value_len);
    attPdu[0] = 0x1D; // ATT_HANDLE_VALUE_IND
    attPdu[1] = static_cast<uint8_t>(characteristic & 0xFF);
    attPdu[2] = static_cast<uint8_t>((characteristic >> 8) & 0xFF);
    if (value_len > 0)
    {
        std::memcpy(&attPdu[3], value, value_len);
    }
    SendHciAclData(gActiveHciConnHandle, 0x0004, attPdu.data(), static_cast<uint16_t>(attPdu.size()));
    return SL_STATUS_OK;
}

sl_status_t sl_bt_gatt_server_send_user_read_response(uint8_t /* connection */, uint16_t characteristic, uint8_t att_errorcode,
                                                      size_t value_len, const uint8_t * value, uint16_t * sent_len)
{
    if (att_errorcode != 0)
    {
        SendAttErrorRsp(gActiveHciConnHandle, 0x0A, characteristic, att_errorcode);
        if (sent_len != nullptr)
        {
            *sent_len = 0;
        }
        return SL_STATUS_OK;
    }
    std::vector<uint8_t> attPdu(1 + value_len);
    attPdu[0] = 0x0B; // ATT_READ_RSP
    if (value_len > 0 && value != nullptr)
    {
        std::memcpy(&attPdu[1], value, value_len);
    }
    SendHciAclData(gActiveHciConnHandle, 0x0004, attPdu.data(), static_cast<uint16_t>(attPdu.size()));
    if (sent_len != nullptr)
    {
        *sent_len = static_cast<uint16_t>(value_len);
    }
    return SL_STATUS_OK;
}

} // extern "C"

#endif // defined(CHIP_LINUX_USE_SILABS_BLE_WIFI) && CHIP_LINUX_USE_SILABS_BLE_WIFI
