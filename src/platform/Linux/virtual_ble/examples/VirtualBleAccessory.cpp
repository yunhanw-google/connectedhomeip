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
#include <csignal>
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
    uint16_t discriminator = 3840;

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) discriminator = static_cast<uint16_t>(std::atoi(argv[2]));

    std::cout << "[VirtualBleAccessory] Starting Virtual End-Device (BLE Peripheral)..." << std::endl;

    if (Platform::MemoryInit() != CHIP_NO_ERROR)
    {
        std::cerr << "[VirtualBleAccessory] Failed to init CHIP memory." << std::endl;
        return 1;
    }

    if (DeviceLayer::SystemLayer().Init() != CHIP_NO_ERROR)
    {
        std::cerr << "[VirtualBleAccessory] Failed to init SystemLayer." << std::endl;
        return 1;
    }

    BleLayer bleLayer;
    VirtualBlePlatformDelegate virtualBle;

    if (virtualBle.Init(&bleLayer, port) != CHIP_NO_ERROR)
    {
        std::cerr << "[VirtualBleAccessory] Failed to bind Virtual BLE port " << port << std::endl;
        return 1;
    }

    std::cout << "[VirtualBleAccessory] LISTENING" << std::endl;
    std::fflush(stdout);

    if (virtualBle.StartAdvertising(discriminator, 0xFFF1, 0x8001) != CHIP_NO_ERROR)
    {
        std::cerr << "[VirtualBleAccessory] Failed to start virtual BLE advertising." << std::endl;
        return 1;
    }

    std::cout << "[VirtualBleAccessory] Ready and advertising on Virtual BLE port " << port
              << " (Discriminator: " << discriminator << ")" << std::endl;

    // Run event loop for test duration (30 seconds or until SIGTERM)
    for (int i = 0; i < 300; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    virtualBle.Shutdown();
    DeviceLayer::SystemLayer().Shutdown();
    Platform::MemoryShutdown();

    std::cout << "[VirtualBleAccessory] Virtual End-Device shutdown clean." << std::endl;
    return 0;
}
