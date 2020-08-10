/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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
 *          Provides an implementation of the BLEManager singleton object
 *          for the Linux platforms.
 */

#ifndef BLE_MANAGER_IMPL_H
#define BLE_MANAGER_IMPL_H

#include <ble/BleError.h>
#include <ble/BleLayer.h>
#include <platform/internal/BLEManager.h>

#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE

namespace chip {
namespace DeviceLayer {
namespace Internal {

using namespace chip::Ble;

typedef bool (*SendIndicationCallback)(void * data, chip::System::PacketBuffer * msgBuf);

typedef uint16_t (*GetMTUCallback)(void * connObj);

struct InEventParam
{
    enum EventTypeEnum
    {
        kEvent_IndicationConfirmation,
        kEvent_SubscribeReceived,
        kEvent_UnsubscribeReceived,
        kEvent_ConnectionError,
        kEvent_WriteReceived
    };

    EventTypeEnum EventType;
    void * ConnectionObject;
    Ble::BleLayer * Ble;

    union
    {
        struct
        {
            const chip::Ble::ChipBleUUID * SvcId;
            const chip::Ble::ChipBleUUID * CharId;
        } IndicationConfirmation;

        struct
        {
            const chip::Ble::ChipBleUUID * SvcId;
            const chip::Ble::ChipBleUUID * CharId;
        } SubscriptionChange;

        struct
        {
            BLE_ERROR mErr;
        } ConnectionError;

        struct
        {
            const chip::Ble::ChipBleUUID * SvcId;
            const chip::Ble::ChipBleUUID * CharId;
            chip::System::PacketBuffer * MsgBuf;
        } WriteReceived;
    };
};

void HandleIncomingBleConnection(BLEEndPoint *bleEP);

/**
 * Concrete implementation of the BLEManagerImpl singleton object for the Linux platforms.
 */
class BLEManagerImpl final : public BLEManager, private BleLayer, private BlePlatformDelegate,  private BleApplicationDelegate
{
    // Allow the BLEManager interface class to delegate method calls to
    // the implementation methods provided by this class.
    friend BLEManager;

public:
    // ===== Platform-specific members available for use by the application.

    // Enum to represent BLE activities
    typedef enum
    {
        kBleConnect = 0,
        kBleDisconnect,
        kWoBlePktTx,
        kWoBlePktRx
    } BleActivity;

    uint8_t GetAdvertisingHandle(void);
    void SetAdvertisingHandle(uint8_t handle);

    // Application can use this callback to get notification regarding
    // BLE activity
    virtual void NotifyBleActivity(BleActivity bleActivity) {};

    // BleApplicationDelegate function overrides
    // Application can use this callback to get notification regarding
    // Chip connection closure
    virtual void NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT connObj);

public:
    // ===== Members that implement the BLEManager internal interface.

    CHIP_ERROR _Init(void);
    CHIPoBLEServiceMode _GetCHIPoBLEServiceMode(void);
    CHIP_ERROR _SetCHIPoBLEServiceMode(CHIPoBLEServiceMode val);
    bool _IsAdvertisingEnabled(void);
    CHIP_ERROR _SetAdvertisingEnabled(bool val);
    bool _IsFastAdvertisingEnabled(void);
    CHIP_ERROR _SetFastAdvertisingEnabled(bool val);
    bool _IsAdvertising(void);
    CHIP_ERROR _GetDeviceName(char * buf, size_t bufSize);
    CHIP_ERROR _SetDeviceName(const char * deviceName);
    uint16_t _NumConnections(void);
    void _OnPlatformEvent(const ChipDeviceEvent * event);
    BleLayer * _GetBleLayer(void) const;
/*
    // ===== Members that implement virtual methods on BlePlatformDelegate.



    // ===== Members that implement virtual methods on BleApplicationDelegate.

    void NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT conId) override;
*/
    // ===== Members for internal use by the following friends.

    friend BLEManager & BLEMgr(void){
            return BLEManagerImpl::sInstance;
    };
    friend BLEManagerImpl & BLEMgrImpl(void){
        return BLEManagerImpl::sInstance;
    };

    static BLEManagerImpl sInstance;

    // ===== Private members reserved for use by this class only.



    enum
    {
        kMaxConnections             = 1,
        kMaxDeviceNameLength        = 20, // TODO: right-size this
        kMaxAdvertismentDataSetSize = 31  // TODO: verify this
    };

//    ble_gatts_char_handles_t mCHIPoBLECharHandle_RX;
//    ble_gatts_char_handles_t mCHIPoBLECharHandle_TX;
    CHIPoBLEServiceMode mServiceMode;
    uint16_t mFlags;
    uint16_t mNumGAPCons;
    uint16_t mSubscribedConIds[kMaxConnections];
    uint8_t mAdvHandle;
    uint8_t mAdvDataBuf[kMaxAdvertismentDataSetSize];
    uint8_t mScanRespDataBuf[kMaxAdvertismentDataSetSize];

    CHIP_ERROR StartAdvertising(void);
    CHIP_ERROR StopAdvertising(void);
    void DriveBLEState(void);

    enum
    {
        kFlag_AsyncInitCompleted       = 0x0001, /**< One-time asynchronous initialization actions have been performed. */
        kFlag_BluezBLELayerInitialized   = 0x0002, /**< The ESP BLE layer has been initialized. */
        kFlag_AppRegistered            = 0x0004, /**< The CHIPoBLE application has been registered with the ESP BLE layer. */
        kFlag_AttrsRegistered          = 0x0008, /**< The CHIPoBLE GATT attributes have been registered with the ESP BLE layer. */
        kFlag_GATTServiceStarted       = 0x0010, /**< The CHIPoBLE GATT service has been started. */
        kFlag_AdvertisingConfigured    = 0x0020, /**< CHIPoBLE advertising has been configured in the ESP BLE layer. */
        kFlag_Advertising              = 0x0040, /**< The system is currently CHIPoBLE advertising. */
        kFlag_ControlOpInProgress      = 0x0080, /**< An async control operation has been issued to the ESP BLE layer. */
        kFlag_AdvertisingEnabled       = 0x0100, /**< The application has enabled CHIPoBLE advertising. */
        kFlag_FastAdvertisingEnabled   = 0x0200, /**< The application has enabled fast advertising. */
        kFlag_UseCustomDeviceName      = 0x0400, /**< The application has configured a custom BLE device name. */
        kFlag_AdvertisingRefreshNeeded = 0x0800, /**< The advertising configuration/state in ESP BLE layer needs to be updated. */
    };
/*
    CHIP_ERROR ConfigureAdvertising(void);
//    CHIP_ERROR EncodeAdvertisingData(ble_gap_adv_data_t & gapAdvData);
    CHIP_ERROR StartAdvertising(void);
    CHIP_ERROR StopAdvertising(void);
    void HandleSoftDeviceBLEEvent(const ChipDeviceEvent * event);
    CHIP_ERROR HandleGAPConnect(const ChipDeviceEvent * event);
    CHIP_ERROR HandleGAPDisconnect(const ChipDeviceEvent * event);
    CHIP_ERROR HandleRXCharWrite(const ChipDeviceEvent * event);
    CHIP_ERROR HandleTXCharCCCDWrite(const ChipDeviceEvent * event);
    CHIP_ERROR HandleTXComplete(const ChipDeviceEvent * event);
    CHIP_ERROR SetSubscribed(uint16_t conId);
    bool UnsetSubscribed(uint16_t conId);
    bool IsSubscribed(uint16_t conId);


    //
    //
    //
    // static void SoftDeviceBLEEventCallback(const ble_evt_t * bleEvent, void * context);
    */
    static void DriveBLEState(intptr_t arg);

    Ble::BleLayer * Ble;
    SendIndicationCallback SendIndicationCb;


    bool SubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId, const ChipBleUUID * charId) override;
    bool UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId, const ChipBleUUID * charId) override;
    bool CloseConnection(BLE_CONNECTION_OBJECT conId) override;
    uint16_t GetMTU(BLE_CONNECTION_OBJECT conId) const override;

    bool SendIndication(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId, const ChipBleUUID * charId,
                        PacketBuffer * pBuf) override;
    bool SendWriteRequest(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId, const ChipBleUUID * charId,
                          PacketBuffer * pBuf) override;
    bool SendReadRequest(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId, const ChipBleUUID * charId,
                         PacketBuffer * pBuf) override;
    bool SendReadResponse(BLE_CONNECTION_OBJECT conId, BLE_READ_REQUEST_CONTEXT requestContext, const ChipBleUUID * svcId,
                          const ChipBleUUID * charId) override;


    // Driven by BlueZ IO, calling into BleLayer:
    static void WoBLEz_NewConnection(void * user_data);
    static void WoBLEz_WriteReceived(void * user_data, const uint8_t * value, size_t len);
    static void WoBLEz_ConnectionClosed(void * user_data);
    static void WoBLEz_SubscriptionChange(void * user_data);
    static void WoBLEz_IndicationConfirmation(void * user_data);
    static bool WoBLEz_TimerCb(void * user_data);

    char mDeviceName[kMaxDeviceNameLength + 1];

};

/**
 * Returns a reference to the public interface of the BLEManager singleton object.
 *
 * Internal components should use this to access features of the BLEManager object
 * that are common to all platforms.
 */

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

#endif // CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE

#endif // BLE_MANAGER_IMPL_H
