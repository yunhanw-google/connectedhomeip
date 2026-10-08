/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

#include "AppMain.h"
#include "NamedPipeCommands.h"
#include "Options.h"
#include "lit-icd-common/Identify.h"
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/icd/server/ICDManager.h>
#include <app/icd/server/ICDNotifier.h>
#include <app/icd/server/ICDStateObserver.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include "system/SystemClock.h"

#include <cstdlib>
#include <cstring>

using namespace chip;
using namespace chip::app;
using namespace chip::System::Clock::Literals;

class SleepyLitIcdObserver : public ICDStateObserver
{
public:
    static SleepyLitIcdObserver & Instance()
    {
        static SleepyLitIcdObserver sInstance;
        return sInstance;
    }

    void SetRadioGated(bool gated)
    {
        if (gated)
        {
            GateRadio();
        }
        else
        {
            UngateRadio();
        }
    }

    void GateRadio()
    {
        if (mIsGated)
        {
            return;
        }
        // CIRQUE_ICD_RADIO_GATE: drop non-loopback UDP 5540 traffic during IdleMode
        int r1 = system("iptables -I INPUT -p udp --dport 5540 -j DROP >/dev/null 2>&1");
        int r2 = system("ip6tables -I INPUT -p udp --dport 5540 -j DROP >/dev/null 2>&1");
        (void) r1;
        (void) r2;
        mIsGated = true;
        ChipLogProgress(AppServer, "CIRQUE_ICD_RADIO_GATE: Entered IdleMode -> Radio GATED (RX-OFF)");
    }

    void UngateRadio()
    {
        if (!mIsGated)
        {
            return;
        }
        // CIRQUE_ICD_RADIO_GATE: remove drop rules during ActiveMode
        while (system("iptables -D INPUT -p udp --dport 5540 -j DROP >/dev/null 2>&1") == 0)
        {
        }
        while (system("ip6tables -D INPUT -p udp --dport 5540 -j DROP >/dev/null 2>&1") == 0)
        {
        }
        mIsGated = false;
        ChipLogProgress(AppServer, "CIRQUE_ICD_RADIO_GATE: Entered ActiveMode -> Radio UNGATED (RX-ON)");
    }

    // ICDStateObserver
    void OnEnterActiveMode() override
    {
        SetRadioGated(false);
    }

    void OnEnterIdleMode() override
    {
        SetRadioGated(true);
    }

    void OnTransitionToIdle() override
    {
        SetRadioGated(false);
    }

    void OnICDModeChange() override {}

    bool IsGated() const { return mIsGated; }

private:
    bool mIsGated = false;
};

using LinuxICDRadioGater = SleepyLitIcdObserver;

class IcdAppNamedPipeCommandDelegate : public NamedPipeCommandDelegate
{
public:
    void OnEventCommandReceived(const char * json) override
    {
        ChipLogProgress(AppServer, "IcdAppNamedPipeCommandDelegate: Received named pipe command: %s", json ? json : "");
        // Synchronously ungate radio BEFORE scheduling stack work to prevent Check-In Black Hole!
        SleepyLitIcdObserver::Instance().SetRadioGated(false);

        TEMPORARY_RETURN_IGNORED DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
            ChipLogProgress(AppServer, "IcdAppNamedPipeCommandDelegate: Triggering ICD NetworkActivityNotification");
            ICDNotifier::GetInstance().NotifyNetworkActivityNotification();
        });
    }
};

static IcdAppNamedPipeCommandDelegate sIcdAppNamedPipeCommandDelegate;
static NamedPipeCommands sChipNamedPipeCommands;

static bool IsSleepyRadioGateEnabled()
{
    const char * envGate = getenv("CHIP_CIRQUE_SLEEPY_RADIO_GATE");
    if (envGate != nullptr && (strcmp(envGate, "1") == 0 || strcmp(envGate, "true") == 0))
    {
        return true;
    }
    if (getenv("CHIP_ICD_SLEEPY_NODE") != nullptr)
    {
        return true;
    }
    const char * pipe = LinuxDeviceOptions::GetInstance().app_pipe;
    return (pipe != nullptr && strlen(pipe) > 0);
}

void ApplicationInit()
{
    if (IsSleepyRadioGateEnabled())
    {
        Server::GetInstance().GetICDManager().RegisterObserver(&SleepyLitIcdObserver::Instance());

        if (Server::GetInstance().GetICDManager().GetOperaionalState() == ICDManager::OperationalState::IdleMode)
        {
            SleepyLitIcdObserver::Instance().OnEnterIdleMode();
        }

        const char * pipePath = LinuxDeviceOptions::GetInstance().app_pipe;
        if (pipePath == nullptr || strlen(pipePath) == 0)
        {
            pipePath = "/tmp/chip_icd_fifo";
        }

        ChipLogProgress(AppServer, "SleepyLitIcdObserver: Starting NamedPipeCommands on %s", pipePath);
        CHIP_ERROR err = sChipNamedPipeCommands.Start(pipePath, &sIcdAppNamedPipeCommandDelegate);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(AppServer, "Failed to start NamedPipeCommands: %" CHIP_ERROR_FORMAT, err.Format());
        }
    }
}

void ApplicationShutdown()
{
    TEMPORARY_RETURN_IGNORED sChipNamedPipeCommands.Stop();
    SleepyLitIcdObserver::Instance().SetRadioGated(false);
}

void notifyIcdActive(System::Layer * layer, void *)
{
    ICDNotifier::GetInstance().NotifyNetworkActivityNotification();
    TEMPORARY_RETURN_IGNORED DeviceLayer::SystemLayer().StartTimer(10000_ms32, notifyIcdActive, nullptr);
}

int main(int argc, char * argv[])
{
    VerifyOrDie(ChipLinuxAppInit(argc, argv) == 0);

    if (!IsSleepyRadioGateEnabled())
    {
        SuccessOrDie(DeviceLayer::SystemLayer().StartTimer(10000_ms32, notifyIcdActive, nullptr));
    }
    else
    {
        ChipLogProgress(AppServer, "Sleepy LIT ICD mode active: disabling 10s timer, relying on radio-gating & FIFO trigger.");
    }

    SuccessOrDie(IdentifyInit());
    ChipLinuxAppMainLoop();
    return 0;
}
