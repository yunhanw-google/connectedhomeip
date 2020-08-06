/*
 *
 *    Copyright (c) 2017-2018 Nest Labs, Inc.
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
/**
 *    @file
 *      This file provides the implementation of BluezBleApplicationDelegate class
 *
 *      BluezBleApplicationDelegate provides the interface for Chip
 *      to inform the application regarding activity within the WoBluez
 *      layer
 */
#include <support/CodeUtils.h>
#include "BluezBleApplicationDelegate.h"
#include "BluezHelperCode.h"

#if CONFIG_BLE_PLATFORM_BLUEZ

using namespace ::nl;

namespace chip {
namespace DeviceLayer {
namespace Internal {

static int CloseBleconnectionCB(void *aArg);

void BluezBleApplicationDelegate::NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT connObj)
{
    bool status = true;

    ChipLogRetain(Ble, "Got notification regarding chip connection closure");

    status = BluezRunOnBluezThread(CloseBleconnectionCB, NULL);
    if (!status)
    {
        ChipLogError(Ble, "Failed to schedule CloseBleconnection() on wobluez thread");
    }
};

static int CloseBleconnectionCB(void *aArg)
{
    CloseBleconnection();

    return G_SOURCE_REMOVE;
}

} // namespace BlueZ
} // namespace Platform
} // namespace Ble

#endif /* CONFIG_BLE_PLATFORM_BLUEZ */
