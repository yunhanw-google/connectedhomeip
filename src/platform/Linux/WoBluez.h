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
 *      This file defines WoBluez peripheral interfaces that hands the wrapper talking with BleLayer via the
 *      corresponding platform interface function when the application passively receives an incoming BLE connection.
 *
 */

#ifndef WOBLEZ_H_
#define WOBLEZ_H_

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <stdbool.h>
#include <stdint.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <support/CodeUtils.h>
#include <inet/InetLayer.h>
#include <system/SystemPacketBuffer.h>

namespace chip {
    namespace DeviceLayer {
        namespace Internal {

// Driven by BlueZ IO, calling into BleLayer:
void WoBLEz_NewConnection(void * user_data);
void WoBLEz_WriteReceived(void * user_data, const uint8_t * value, size_t len);
void WoBLEz_ConnectionClosed(void * user_data);
void WoBLEz_SubscriptionChange(void * user_data);
void WoBLEz_IndicationConfirmation(void * user_data);
bool WoBLEz_TimerCb(void * user_data);

// Called by BlePlatformDelegate:
bool WoBLEz_ScheduleSendIndication(void * user_data, chip::System::PacketBuffer * msgBuf);

} // namespace BlueZ
} // namespace Platform
} // namespace Ble


#endif /* WOBLEZ_H_ */
