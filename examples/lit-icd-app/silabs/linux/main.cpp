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
#include "AppTask.h"

#include "AppConfig.h"

#include "NamedPipeCommands.h"
#include "SilabsPosixShim.h"

#include <AppMain.h>
#include <json/json.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/silabs/platformAbstraction/SilabsPlatform.h>
#include <system/SystemConfig.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_SYSTEM_CONFIG_USE_OPENTHREAD_ENDPOINT
#include <openthread/child_supervision.h>
#include <platform/ThreadStackManager.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

using namespace chip;
using namespace chip::DeviceLayer;
using namespace chip::DeviceLayer::Silabs;

namespace {

class SilabsLightingCommandDelegate : public NamedPipeCommandDelegate
{
public:
    void OnEventCommandReceived(const char * json) override
    {
        if (json == nullptr)
        {
            return;
        }
        Json::Reader reader;
        Json::Value value;
        if (!reader.parse(json, value) || !value.isObject())
        {
            ChipLogError(AppServer, "[SILABS] Invalid JSON command on app pipe: %s", json);
            return;
        }
        std::string name = value.get("Name", "").asString();
        if (name == "Button" || name == "LightSwitch")
        {
            uint8_t button = static_cast<uint8_t>(value.get("Button", 1).asUInt());
            uint8_t action = static_cast<uint8_t>(
                value.get("Action", static_cast<unsigned>(SilabsPlatform::ButtonAction::ButtonPressed)).asUInt());
            TriggerSilabsButtonAction(button, action);
        }
        else
        {
            ChipLogProgress(AppServer, "[SILABS] Unhandled pipe command: %s", name.c_str());
        }
    }
};

NamedPipeCommands sChipNamedPipeCommands;
SilabsLightingCommandDelegate sSilabsLightingCommandDelegate;

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_SYSTEM_CONFIG_USE_OPENTHREAD_ENDPOINT
// CHIP_OT_CHILD_SUPERVISION_CHECK_TIMEOUT_S overrides the OpenThread child supervision check
// timeout in seconds (0 disables the check), so the Cirque parent-restart tests can run the same
// binary with and without child supervision. The value in effect is always logged.
void ApplyChildSupervisionCheckTimeout()
{
    const char * value = std::getenv("CHIP_OT_CHILD_SUPERVISION_CHECK_TIMEOUT_S");
    bool overridden    = false;

    ThreadStackMgr().LockThreadStack();
    otInstance * instance = ThreadStackMgrImpl().OTInstance();
    if (value != nullptr && value[0] != '\0')
    {
        char * end            = nullptr;
        errno                 = 0;
        unsigned long timeout = std::strtoul(value, &end, 10);
        if (errno == 0 && end != value && *end == '\0' && timeout <= UINT16_MAX)
        {
            otChildSupervisionSetCheckTimeout(instance, static_cast<uint16_t>(timeout));
            overridden = true;
        }
        else
        {
            ChipLogError(AppServer, "[SILABS] Ignoring invalid CHIP_OT_CHILD_SUPERVISION_CHECK_TIMEOUT_S=%s", value);
        }
    }
    uint16_t checkTimeout = otChildSupervisionGetCheckTimeout(instance);
    uint16_t interval     = otChildSupervisionGetInterval(instance);
    ThreadStackMgr().UnlockThreadStack();

    ChipLogProgress(AppServer, "[SILABS] OpenThread child supervision: check_timeout=%us interval=%us (%s)",
                    static_cast<unsigned>(checkTimeout), static_cast<unsigned>(interval), overridden ? "override" : "default");
}
#endif

} // namespace

void ApplicationInit()
{
    SILABS_LOG("=====%s starting=====", BLE_DEV_NAME);
    TEMPORARY_RETURN_IGNORED GetPlatform().Init();

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_CONFIG_ENABLE_ICD_SERVER
    TEMPORARY_RETURN_IGNORED ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_SleepyEndDevice);
#endif
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_SYSTEM_CONFIG_USE_OPENTHREAD_ENDPOINT
    ApplyChildSupervisionCheckTimeout();
#endif

    CHIP_ERROR err = AppTask::GetAppTask().StartAppTask();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Failed to start Silabs AppTask: %" CHIP_ERROR_FORMAT, err.Format());
        appError(err);
    }

    std::string path = std::string(LinuxDeviceOptions::GetInstance().app_pipe);
    if (!path.empty() && sChipNamedPipeCommands.Start(path, &sSilabsLightingCommandDelegate) != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Failed to start CHIP NamedPipeCommands on %s", path.c_str());
        TEMPORARY_RETURN_IGNORED sChipNamedPipeCommands.Stop();
    }
}

void ApplicationShutdown()
{
    TEMPORARY_RETURN_IGNORED sChipNamedPipeCommands.Stop();
    ShutdownSilabsPosixRuntime();
}

int main(int argc, char * argv[])
{
    if (ChipLinuxAppInit(argc, argv) != 0)
    {
        return -1;
    }

#if CONFIG_NETWORK_LAYER_BLE
    TEMPORARY_RETURN_IGNORED ConnectivityMgr().SetBLEDeviceName(BLE_DEV_NAME);
#endif

    ChipLinuxAppMainLoop();
    return 0;
}
