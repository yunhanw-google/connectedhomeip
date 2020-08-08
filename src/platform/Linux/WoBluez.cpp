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
 *      This file defines WoBluez peripheral interface implementation that hands the wrapper talking with BleLayer via the
 *      corresponding platform interface function when the application passively receives an incoming BLE connection.
 *
 */

#include "WoBluez.h"

#include <inet/InetLayer.h>
#include <system/SystemPacketBuffer.h>
#include "BluezBlePlatformDelegate.h"
#include "BluezBleApplicationDelegate.h"
#include "CHIPBluezHelper.h"

#if CONFIG_BLE_PLATFORM_BLUEZ


namespace chip {
namespace DeviceLayer {
namespace Internal {

const chip::Ble::ChipBleUUID CHIP_BLE_CHAR_1_ID = { { // 18EE2EF5-263D-4559-959F-4F9C429F9D11
                                                     0x18, 0xEE, 0x2E, 0xF5, 0x26, 0x3D, 0x45, 0x59, 0x95, 0x9F, 0x4F, 0x9C, 0x42,
                                                     0x9F, 0x9D, 0x11 } };

const chip::Ble::ChipBleUUID CHIP_BLE_CHAR_2_ID = { { // 18EE2EF5-263D-4559-959F-4F9C429F9D12
                                                     0x18, 0xEE, 0x2E, 0xF5, 0x26, 0x3D, 0x45, 0x59, 0x95, 0x9F, 0x4F, 0x9C, 0x42,
                                                     0x9F, 0x9D, 0x12 } };

void WoBLEz_NewConnection(void * data)
{
    ChipLogProgress(Ble, "WoBLEz_NewConnection: %p", data);
}

void WoBLEz_WriteReceived(void * data, const uint8_t * value, size_t len)
{
    // Peripheral behaviour
    CHIP_ERROR err                          = CHIP_NO_ERROR;
    chip::System::PacketBuffer * msgBuf = NULL;
    chip::System::Error syserr          = CHIP_SYSTEM_NO_ERROR;
    InEventParam * params                    = NULL;

    msgBuf = chip::System::PacketBuffer::New();

    VerifyOrExit(msgBuf != NULL, err = CHIP_ERROR_NO_MEMORY);
    VerifyOrExit(msgBuf->AvailableDataLength() >= len, err = CHIP_ERROR_BUFFER_TOO_SMALL);

    syserr = gBluezBlePlatformDelegate->NewEventParams(&params);
    SuccessOrExit(syserr);

    memcpy(msgBuf->Start(), value, len);
    msgBuf->SetDataLength(len);

    params->EventType            = InEventParam::EventTypeEnum::kEvent_WriteReceived;
    params->ConnectionObject     = data;
    params->WriteReceived.SvcId  = &(chip::Ble::CHIP_BLE_SVC_ID);
    params->WriteReceived.CharId = &(CHIP_BLE_CHAR_1_ID);
    params->WriteReceived.MsgBuf = msgBuf;

    syserr = gBluezBlePlatformDelegate->SendToWeaveThread(params);
    SuccessOrExit(syserr);

    if (gBluezBleApplicationDelegate != NULL)
    {
        gBluezBleApplicationDelegate->NotifyBleActivity(kWoBlePktRx);
    }
    params = NULL;
    msgBuf = NULL;

exit:
    if (syserr != CHIP_SYSTEM_NO_ERROR)
    {
        ChipLogError(Ble, "WoBLEz_WriteReceived syserr: %d", syserr);
    }

    if (params != NULL)
    {
        gBluezBlePlatformDelegate->ReleaseEventParams(params);
    }

    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Ble, "WoBLEz_WriteReceived failed: %d", err);
    }

    if (NULL != msgBuf)
    {
        chip::System::PacketBuffer::Free(msgBuf);
    }
}

void WoBLEz_ConnectionClosed(void * data)
{
    InEventParam * params        = NULL;
    chip::System::Error err = CHIP_SYSTEM_NO_ERROR;

    if (data == NULL)
    {
        ChipLogProgress(Ble, "WoBLEz connection has closed");
        ExitNow();
    }
    ChipLogProgress(Ble, "WoBLEz_ConnectionClosed: %p", data);

    err = gBluezBlePlatformDelegate->NewEventParams(&params);
    SuccessOrExit(err);

    params->EventType            = InEventParam::EventTypeEnum::kEvent_ConnectionError;
    params->ConnectionObject     = data;
    params->ConnectionError.mErr = BLE_ERROR_REMOTE_DEVICE_DISCONNECTED;

    err = gBluezBlePlatformDelegate->SendToWeaveThread(params);
    SuccessOrExit(err);

    params = NULL;

exit:
    if (err != CHIP_SYSTEM_NO_ERROR)
    {
        ChipLogError(Ble, "WoBLEz_ConnectionClosed err: %d", err);
    }

    if (params != NULL)
    {
        gBluezBlePlatformDelegate->ReleaseEventParams(params);
    }
}

void WoBLEz_SubscriptionChange(void * data)
{
    InEventParam * params          = NULL;
    chip::System::Error err   = CHIP_SYSTEM_NO_ERROR;
    BluezServerEndpoint * endpoint = static_cast<BluezServerEndpoint *>(data);
    const char * msg               = NULL;

    VerifyOrExit(endpoint != NULL, msg = "endpoint is NULL in WoBLEz_SubscriptionChange");
    //VerifyOrExit(endpoint == gBluezServerEndpoint, msg = "Unexpected endpoint in WoBLEz_SubscriptionChange");
    VerifyOrExit(endpoint->c2 != NULL, msg = "weaveC2 is NULL in WoBLEz_SubscriptionChange");

    err = gBluezBlePlatformDelegate->NewEventParams(&params);
    SuccessOrExit(err);

    params->EventType = endpoint->isNotify ? InEventParam::EventTypeEnum::kEvent_SubscribeReceived
                                                       : InEventParam::EventTypeEnum::kEvent_UnsubscribeReceived;
    params->ConnectionObject          = data;
    params->SubscriptionChange.SvcId  = &(chip::Ble::CHIP_BLE_SVC_ID);
    params->SubscriptionChange.CharId = &(CHIP_BLE_CHAR_2_ID);

    err = gBluezBlePlatformDelegate->SendToWeaveThread(params);
    SuccessOrExit(err);

    params = NULL;

exit:

    if (err != CHIP_SYSTEM_NO_ERROR)
    {
        ChipLogError(Ble, "WoBLEz_ConnectionClosed err: %d", err);
    }

    if (params != NULL)
    {
        gBluezBlePlatformDelegate->ReleaseEventParams(params);
    }

    if (NULL != msg)
    {
        ChipLogError(Ble, msg);
    }
}

void WoBLEz_IndicationConfirmation(void * data)
{
    InEventParam * params        = NULL;
    chip::System::Error err = CHIP_SYSTEM_NO_ERROR;

    err = gBluezBlePlatformDelegate->NewEventParams(&params);
    SuccessOrExit(err);

    params->EventType                     = InEventParam::EventTypeEnum::kEvent_IndicationConfirmation;
    params->ConnectionObject              = data;
    params->IndicationConfirmation.SvcId  = &(chip::Ble::CHIP_BLE_SVC_ID);
    params->IndicationConfirmation.CharId = &(CHIP_BLE_CHAR_2_ID);

    err = gBluezBlePlatformDelegate->SendToWeaveThread(params);
    SuccessOrExit(err);

    params = NULL;

exit:
    if (err != CHIP_SYSTEM_NO_ERROR)
    {
        ChipLogError(Ble, "WoBLEz_IndicationConfirmation err: %d", err);
    }

    if (params != NULL)
    {
        gBluezBlePlatformDelegate->ReleaseEventParams(params);
    }
}

} // namespace BlueZ
} /* namespace Platform */
} /* namespace Ble */


#endif /* CONFIG_BLE_PLATFORM_BLUEZ */
