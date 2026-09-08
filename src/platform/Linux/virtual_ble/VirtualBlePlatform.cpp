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

#include "VirtualBlePlatform.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

#include <lib/support/CHIPMem.h>
#include <lib/support/logging/CHIPLogging.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

VirtualBleConnection::~VirtualBleConnection()
{
    if (mSocketFd >= 0)
    {
        close(mSocketFd);
        mSocketFd = -1;
    }
}

CHIP_ERROR VirtualBleConnection::SendPacket(VirtualBlePktType type, const uint8_t * data, size_t len)
{
    if (mSocketFd < 0)
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    VirtualBleHeader hdr;
    hdr.type   = type;
    hdr.length = htons(static_cast<uint16_t>(len));

    if (send(mSocketFd, &hdr, sizeof(hdr), 0) != sizeof(hdr))
    {
        return CHIP_ERROR_WRITE_FAILED;
    }

    if (len > 0 && data != nullptr)
    {
        if (send(mSocketFd, data, len, 0) != static_cast<ssize_t>(len))
        {
            return CHIP_ERROR_WRITE_FAILED;
        }
    }

    return CHIP_NO_ERROR;
}

VirtualBlePlatformDelegate::VirtualBlePlatformDelegate() :
    mBleLayer(nullptr), mListenPort(16402), mServerFd(-1), mRunning(false), mNextConnId(1)
{}

VirtualBlePlatformDelegate::~VirtualBlePlatformDelegate()
{
    Shutdown();
}

CHIP_ERROR VirtualBlePlatformDelegate::Init(Ble::BleLayer * bleLayer, uint16_t listenPort)
{
    mBleLayer    = bleLayer;
    mListenPort  = listenPort;
    if (mListenPort == 0)
    {
        ChipLogProgress(Ble, "User-Space Virtual BLE Platform initialized in Client-Only Mode");
        return CHIP_NO_ERROR;
    }

    mServerFd    = socket(AF_INET, SOCK_STREAM, 0);

    if (mServerFd < 0)
    {
        return CHIP_ERROR_OPEN_FAILED;
    }

    int opt = 1;
    setsockopt(mServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(mListenPort);

    if (bind(mServerFd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    {
        close(mServerFd);
        mServerFd = -1;
        return CHIP_ERROR_POSIX(errno);
    }

    if (listen(mServerFd, 5) < 0)
    {
        close(mServerFd);
        mServerFd = -1;
        return CHIP_ERROR_INTERNAL;
    }

    mRunning      = true;
    mServerThread = std::thread(&VirtualBlePlatformDelegate::ServerLoop, this);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ChipLogProgress(Ble, "User-Space Virtual BLE Platform initialized on TCP port %u", mListenPort);
    return CHIP_NO_ERROR;
}

void VirtualBlePlatformDelegate::Shutdown()
{
    mRunning = false;
    if (mServerFd >= 0)
    {
        close(mServerFd);
        mServerFd = -1;
    }
    if (mServerThread.joinable())
    {
        mServerThread.join();
    }

    std::lock_guard<std::mutex> lock(mConnMutex);
    mConnections.clear();
}

void VirtualBlePlatformDelegate::ServerLoop()
{
    while (mRunning && mServerFd >= 0)
    {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd        = accept(mServerFd, (struct sockaddr *) &clientAddr, &clientLen);
        if (clientFd >= 0)
        {
            HandleIncomingConnection(clientFd);
        }
    }
}

void VirtualBlePlatformDelegate::HandleIncomingConnection(int clientFd)
{
    std::lock_guard<std::mutex> lock(mConnMutex);
    uint16_t connId = mNextConnId++;
    auto conn = std::make_unique<VirtualBleConnection>(clientFd, connId, /*isCentral=*/false);
    VirtualBleConnection * connPtr = conn.get();
    mConnections[connId] = std::move(conn);

    std::thread(&VirtualBlePlatformDelegate::ReadLoop, this, connPtr).detach();

    if (mBleLayer)
    {
        mBleLayer->HandleWriteReceived((BLE_CONNECTION_OBJECT) connPtr, &Ble::CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_1_UUID, nullptr);
    }
}

void VirtualBlePlatformDelegate::ReadLoop(VirtualBleConnection * conn)
{
    while (mRunning)
    {
        VirtualBleHeader hdr;
        ssize_t n = recv(conn->GetFd(), &hdr, sizeof(hdr), MSG_WAITALL);
        if (n <= 0)
        {
            break;
        }

        uint16_t len = ntohs(hdr.length);
        std::vector<uint8_t> payload(len);
        if (len > 0)
        {
            recv(conn->GetFd(), payload.data(), len, MSG_WAITALL);
        }

        if (hdr.type == VirtualBlePktType::kGattWrite && mBleLayer)
        {
            System::PacketBufferHandle buf = System::PacketBufferHandle::NewWithData(payload.data(), payload.size());
            mBleLayer->HandleWriteReceived((BLE_CONNECTION_OBJECT) conn, &Ble::CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_1_UUID, std::move(buf));
        }
        else if (hdr.type == VirtualBlePktType::kGattIndicate && mBleLayer)
        {
            System::PacketBufferHandle buf = System::PacketBufferHandle::NewWithData(payload.data(), payload.size());
            mBleLayer->HandleIndicationReceived((BLE_CONNECTION_OBJECT) conn, &Ble::CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID, std::move(buf));
        }
        else if (hdr.type == VirtualBlePktType::kSubscribe && mBleLayer)
        {
            mBleLayer->HandleSubscribeReceived((BLE_CONNECTION_OBJECT) conn, &Ble::CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID);
        }
    }

    if (mBleLayer)
    {
        mBleLayer->HandleConnectionError((BLE_CONNECTION_OBJECT) conn, CHIP_ERROR_CONNECTION_ABORTED);
    }
}

CHIP_ERROR VirtualBlePlatformDelegate::SubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                                               const Ble::ChipBleUUID * charId)
{
    auto * conn = reinterpret_cast<VirtualBleConnection *>(connObj);
    if (!conn) return CHIP_ERROR_INVALID_ARGUMENT;
    conn->SetSubscribed(true);
    return conn->SendPacket(VirtualBlePktType::kSubscribe, nullptr, 0);
}

CHIP_ERROR VirtualBlePlatformDelegate::UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                                                 const Ble::ChipBleUUID * charId)
{
    auto * conn = reinterpret_cast<VirtualBleConnection *>(connObj);
    if (!conn) return CHIP_ERROR_INVALID_ARGUMENT;
    conn->SetSubscribed(false);
    return CHIP_NO_ERROR;
}

CHIP_ERROR VirtualBlePlatformDelegate::CloseConnection(BLE_CONNECTION_OBJECT connObj)
{
    auto * conn = reinterpret_cast<VirtualBleConnection *>(connObj);
    if (!conn) return CHIP_ERROR_INVALID_ARGUMENT;
    return conn->SendPacket(VirtualBlePktType::kCloseConn, nullptr, 0);
}

uint16_t VirtualBlePlatformDelegate::GetMTU(BLE_CONNECTION_OBJECT connObj) const
{
    auto * conn = reinterpret_cast<const VirtualBleConnection *>(connObj);
    return conn ? conn->GetMTU() : 247;
}

CHIP_ERROR VirtualBlePlatformDelegate::SendIndication(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                                      const Ble::ChipBleUUID * charId, chip::System::PacketBufferHandle pBuf)
{
    auto * conn = reinterpret_cast<VirtualBleConnection *>(connObj);
    if (!conn || pBuf.IsNull()) return CHIP_ERROR_INVALID_ARGUMENT;
    return conn->SendPacket(VirtualBlePktType::kGattIndicate, pBuf->Start(), pBuf->DataLength());
}

CHIP_ERROR VirtualBlePlatformDelegate::SendWriteRequest(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                                        const Ble::ChipBleUUID * charId, chip::System::PacketBufferHandle pBuf)
{
    auto * conn = reinterpret_cast<VirtualBleConnection *>(connObj);
    if (!conn || pBuf.IsNull()) return CHIP_ERROR_INVALID_ARGUMENT;
    return conn->SendPacket(VirtualBlePktType::kGattWrite, pBuf->Start(), pBuf->DataLength());
}

void VirtualBlePlatformDelegate::NewConnection(Ble::BleLayer * bleLayer, void * appState, const SetupDiscriminator & connDiscriminator)
{
    int clientFd = socket(AF_INET, SOCK_STREAM, 0);
    if (clientFd < 0)
    {
        if (OnConnectionError) OnConnectionError(appState, CHIP_ERROR_OPEN_FAILED);
        return;
    }

    uint16_t targetPort = (mConnectPort != 0) ? mConnectPort : mListenPort;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = htons(targetPort);

    int connected = -1;
    for (int retry = 0; retry < 5; ++retry)
    {
        if (connect(clientFd, (struct sockaddr *) &addr, sizeof(addr)) == 0)
        {
            connected = 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (connected < 0)
    {
        close(clientFd);
        if (OnConnectionError) OnConnectionError(appState, CHIP_ERROR_POSIX(errno));
        return;
    }

    std::lock_guard<std::mutex> lock(mConnMutex);
    uint16_t connId = mNextConnId++;
    auto conn = std::make_unique<VirtualBleConnection>(clientFd, connId, /*isCentral=*/true);
    VirtualBleConnection * connPtr = conn.get();
    mConnections[connId] = std::move(conn);

    std::thread(&VirtualBlePlatformDelegate::ReadLoop, this, connPtr).detach();

    if (OnConnectionComplete)
    {
        OnConnectionComplete(appState, (BLE_CONNECTION_OBJECT) connPtr);
    }
}

void VirtualBlePlatformDelegate::NewConnection(Ble::BleLayer * bleLayer, void * appState, BLE_CONNECTION_OBJECT connObj)
{
    if (OnConnectionComplete) OnConnectionComplete(appState, connObj);
}

CHIP_ERROR VirtualBlePlatformDelegate::CancelConnection()
{
    return CHIP_NO_ERROR;
}

void VirtualBlePlatformDelegate::NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT connObj)
{
    auto * conn = reinterpret_cast<VirtualBleConnection *>(connObj);
    if (conn)
    {
        std::lock_guard<std::mutex> lock(mConnMutex);
        mConnections.erase(conn->GetConnId());
    }
}

CHIP_ERROR VirtualBlePlatformDelegate::StartAdvertising(uint16_t discriminator, uint16_t vendorId, uint16_t productId)
{
    ChipLogProgress(Ble, "Virtual BLE Peripheral Advertising: Discriminator=%u, VID=0x%04X, PID=0x%04X",
                    discriminator, vendorId, productId);
    return CHIP_NO_ERROR;
}

CHIP_ERROR VirtualBlePlatformDelegate::StopAdvertising()
{
    return CHIP_NO_ERROR;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
