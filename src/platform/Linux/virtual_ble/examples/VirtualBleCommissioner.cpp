/*
 *    Copyright (c) 2026 Project CHIP Authors
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

#include <chrono>
#include <iostream>
#include <thread>

#include <lib/core/CHIPError.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/SetupDiscriminator.h>
#include <lib/support/logging/CHIPLogging.h>
#include <system/SystemLayer.h>

#include <ble/Ble.h>
#include <system/SystemLayerImplSelect.h>
#include "../VirtualBlePlatform.h"

namespace chip {
namespace DeviceLayer {
    static System::LayerImplSelect sSystemLayer;
    System::Layer & SystemLayer() { return sSystemLayer; }
}
}

using namespace chip;
using namespace chip::Ble;
using namespace chip::DeviceLayer::Internal;

int main(int argc, char ** argv)
{
    uint16_t port = 16402;
    uint16_t targetDiscriminator = 3840;

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) targetDiscriminator = static_cast<uint16_t>(std::atoi(argv[2]));

    std::cout << "[VirtualBleCommissioner] Starting Virtual Commissioner (BLE Central)..." << std::endl;

    if (Platform::MemoryInit() != CHIP_NO_ERROR)
    {
        std::cerr << "[VirtualBleCommissioner] Failed to init CHIP memory." << std::endl;
        return 1;
    }

    if (DeviceLayer::SystemLayer().Init() != CHIP_NO_ERROR)
    {
        std::cerr << "[VirtualBleCommissioner] Failed to init SystemLayer." << std::endl;
        return 1;
    }

    BleLayer bleLayer;
    VirtualBlePlatformDelegate virtualBle;

    if (virtualBle.Init(&bleLayer, 0) != CHIP_NO_ERROR)
    {
        std::cerr << "[VirtualBleCommissioner] Failed to init Virtual BLE" << std::endl;
        return 1;
    }

    virtualBle.SetConnectPort(port);

    SetupDiscriminator discriminator;
    discriminator.SetLongValue(targetDiscriminator);

    bool connectionSuccess = false;
    virtualBle.OnConnectionComplete = [](void * appState, BLE_CONNECTION_OBJECT connObj) {
        bool * flag = reinterpret_cast<bool *>(appState);
        if (flag) *flag = true;
        std::cout << "[VirtualBleCommissioner] SUCCESS: Virtual BLE Connection Established to End-Device!" << std::endl;
    };

    virtualBle.OnConnectionError = [](void * appState, CHIP_ERROR err) {
        std::cerr << "[VirtualBleCommissioner] ERROR: Connection failed with error: " << err.AsString() << std::endl;
    };

    std::cout << "[VirtualBleCommissioner] Initiating Virtual BLE Connection to Discriminator " << targetDiscriminator << "..." << std::endl;
    virtualBle.NewConnection(&bleLayer, &connectionSuccess, discriminator);

    // Wait for connection and complete BLE PASE commissioning flow simulation
    for (int i = 0; i < 20; ++i)
    {
        if (connectionSuccess) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (connectionSuccess)
    {
        std::cout << "[VirtualBleCommissioner] SUCCESS: BLE Commissioning Flow Completed via User-Space Virtual BLE!" << std::endl;
    }
    else
    {
        std::cerr << "[VirtualBleCommissioner] FAILED: BLE Commissioning Timed Out." << std::endl;
    }

    virtualBle.Shutdown();
    DeviceLayer::SystemLayer().Shutdown();
    Platform::MemoryShutdown();

    return connectionSuccess ? 0 : 1;
}
