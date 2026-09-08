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

#include <pw_unit_test/framework.h>

#include <lib/core/CHIPError.h>
#include <lib/support/CHIPMem.h>
#include <system/SystemLayer.h>

#include "../VirtualBlePlatform.h"

namespace chip {
namespace DeviceLayer {
namespace Internal {

class VirtualBlePlatformTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ASSERT_EQ(Platform::MemoryInit(), CHIP_NO_ERROR);
        ASSERT_EQ(DeviceLayer::SystemLayer().Init(), CHIP_NO_ERROR);
    }

    static void TearDownTestSuite()
    {
        DeviceLayer::SystemLayer().Shutdown();
        Platform::MemoryShutdown();
    }
};

TEST_F(VirtualBlePlatformTest, InitializeAndConnect)
{
    VirtualBlePlatformDelegate serverDelegate;
    VirtualBlePlatformDelegate clientDelegate;

    // 1. Initialize Virtual BLE Server (Accessory) on port 16405
    EXPECT_EQ(serverDelegate.Init(nullptr, 16405), CHIP_NO_ERROR);

    // 2. Initialize Virtual BLE Client (Commissioner) on port 16405
    EXPECT_EQ(clientDelegate.Init(nullptr, 16405), CHIP_NO_ERROR);

    // 3. Connect Client to Server over Virtual BLE TCP Socket
    SetupDiscriminator discriminator;
    discriminator.SetLongValue(3840);

    bool connectionComplete = false;
    clientDelegate.OnConnectionComplete = [](void * appState, BLE_CONNECTION_OBJECT connObj) {
        bool * flag = reinterpret_cast<bool *>(appState);
        if (flag) *flag = true;
    };

    clientDelegate.NewConnection(nullptr, &connectionComplete, discriminator);

    // Allow background thread to process handshake
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(connectionComplete);

    clientDelegate.Shutdown();
    serverDelegate.Shutdown();
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
