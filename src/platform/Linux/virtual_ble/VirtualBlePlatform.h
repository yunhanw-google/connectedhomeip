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

/**
 *    @file
 *          User-Space Virtual BLE Foundation & Platform Delegate for Matter SDK.
 *          Does NOT rely on Linux kernel vhci/btvirt or BlueZ DBus.
 *          Operates entirely in user-space over TCP/Socket IPC bus.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ble/Ble.h>
#include <ble/BleApplicationDelegate.h>
#include <ble/BleConnectionDelegate.h>
#include <ble/BleLayer.h>
#include <ble/BlePlatformDelegate.h>
#include <lib/core/CHIPError.h>
#include <lib/support/SetupDiscriminator.h>
#include <system/SystemPacketBuffer.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

// Packet Types for User-Space Virtual BLE IPC/TCP protocol
enum class VirtualBlePktType : uint8_t
{
    kAdvReport    = 0x01,
    kConnectReq   = 0x02,
    kConnectResp  = 0x03,
    kGattWrite    = 0x04, // Write to RX Characteristic
    kGattIndicate = 0x05, // Indication from TX Characteristic
    kSubscribe    = 0x06, // CCCD Subscribe
    kCloseConn    = 0x07,
};

#pragma pack(push, 1)
struct VirtualBleHeader
{
    VirtualBlePktType type;
    uint16_t length;
};
#pragma pack(pop)

/**
 * User-Space Virtual BLE Connection handle.
 */
class VirtualBleConnection
{
public:
    VirtualBleConnection(int socketFd, uint16_t connId, bool isCentral) :
        mSocketFd(socketFd), mConnId(connId), mIsCentral(isCentral), mMtu(247), mSubscribed(false)
    {}

    ~VirtualBleConnection();

    int GetFd() const { return mSocketFd; }
    uint16_t GetConnId() const { return mConnId; }
    uint16_t GetMTU() const { return mMtu; }
    void SetMTU(uint16_t mtu) { mMtu = mtu; }
    bool IsSubscribed() const { return mSubscribed; }
    void SetSubscribed(bool sub) { mSubscribed = sub; }

    CHIP_ERROR SendPacket(VirtualBlePktType type, const uint8_t * data, size_t len);

private:
    int mSocketFd;
    uint16_t mConnId;
    bool mIsCentral;
    uint16_t mMtu;
    bool mSubscribed;
};

/**
 * User-Space Virtual BLE Platform & Connection Delegate implementation.
 */
class VirtualBlePlatformDelegate : public Ble::BlePlatformDelegate,
                                   public Ble::BleConnectionDelegate,
                                   public Ble::BleApplicationDelegate
{
public:
    VirtualBlePlatformDelegate();
    ~VirtualBlePlatformDelegate() override;

    // Initialize Virtual BLE Bus (starts TCP server for server/accessory role or client for commissioner)
    CHIP_ERROR Init(Ble::BleLayer * bleLayer, uint16_t listenPort = 16402);
    void SetConnectPort(uint16_t port) { mConnectPort = port; }
    void Shutdown();

    // BlePlatformDelegate API:
    CHIP_ERROR SubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                       const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                         const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR CloseConnection(BLE_CONNECTION_OBJECT connObj) override;
    uint16_t GetMTU(BLE_CONNECTION_OBJECT connObj) const override;
    CHIP_ERROR SendIndication(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                              const Ble::ChipBleUUID * charId, chip::System::PacketBufferHandle pBuf) override;
    CHIP_ERROR SendWriteRequest(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                const Ble::ChipBleUUID * charId, chip::System::PacketBufferHandle pBuf) override;

    // BleConnectionDelegate API:
    void NewConnection(Ble::BleLayer * bleLayer, void * appState, const SetupDiscriminator & connDiscriminator) override;
    void NewConnection(Ble::BleLayer * bleLayer, void * appState, BLE_CONNECTION_OBJECT connObj) override;
    CHIP_ERROR CancelConnection() override;

    // BleApplicationDelegate API:
    void NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT connObj) override;

    // Virtual BLE Peripheral Advertising
    CHIP_ERROR StartAdvertising(uint16_t discriminator, uint16_t vendorId, uint16_t productId);
    CHIP_ERROR StopAdvertising();

private:
    void ServerLoop();
    void HandleIncomingConnection(int clientFd);
    void ReadLoop(VirtualBleConnection * conn);

    Ble::BleLayer * mBleLayer;
    uint16_t mListenPort;
    uint16_t mConnectPort = 0;
    int mServerFd;
    bool mRunning;
    std::thread mServerThread;

    mutable std::mutex mConnMutex;
    uint16_t mNextConnId;
    std::unordered_map<uint16_t, std::unique_ptr<VirtualBleConnection>> mConnections;
};

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
