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

/*
 *  Copyright (c) 2016-2019, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef CHIP_BLUEZ_HELPER_H
#define CHIP_BLUEZ_HELPER_H

#include <stdbool.h>
#include <stdint.h>
#include "Linux/gen/DbusBluez.h"

namespace chip {
namespace DeviceLayer {
namespace Internal {

    /**
* This enumeration type represents BLE advertisement types.
*
*/

    /* Weave Identification Information */
    struct CHIPIdInfo
    {
        uint8_t major;
        uint8_t minor;
        uint16_t vendorId;
        uint16_t productId;
        uint64_t deviceId;
        uint8_t pairingStatus;
    } __attribute__((packed));

/* Weave Service Data*/
    struct CHIPServiceData
    {
        uint8_t dataBlock0Len;
        uint8_t dataBlock0Type;
        CHIPIdInfo idInfo;
    } __attribute__((packed));

    typedef enum BluezAdvType
    {
        BLUEZ_ADV_TYPE_CONNECTABLE = 0x01,
        BLUEZ_ADV_TYPE_SCANNABLE   = 0x02,
        BLUEZ_ADV_TYPE_DIRECTED    = 0x04,

        BLUEZ_ADV_TYPE_UNDIRECTED_NONCONNECTABLE_NONSCANNABLE = 0,
        BLUEZ_ADV_TYPE_UNDIRECTED_CONNECTABLE_NONSCANNABLE    = BLUEZ_ADV_TYPE_CONNECTABLE,
        BLUEZ_ADV_TYPE_UNDIRECTED_NONCONNECTABLE_SCANNABLE    = BLUEZ_ADV_TYPE_SCANNABLE,
        BLUEZ_ADV_TYPE_UNDIRECTED_CONNECTABLE_SCANNABLE = BLUEZ_ADV_TYPE_CONNECTABLE | BLUEZ_ADV_TYPE_SCANNABLE,

        BLUEZ_ADV_TYPE_DIRECTED_NONCONNECTABLE_NONSCANNABLE = BLUEZ_ADV_TYPE_DIRECTED,
        BLUEZ_ADV_TYPE_DIRECTED_CONNECTABLE_NONSCANNABLE    = BLUEZ_ADV_TYPE_DIRECTED | BLUEZ_ADV_TYPE_CONNECTABLE,
        BLUEZ_ADV_TYPE_DIRECTED_NONCONNECTABLE_SCANNABLE    = BLUEZ_ADV_TYPE_DIRECTED | BLUEZ_ADV_TYPE_SCANNABLE,
        BLUEZ_ADV_TYPE_DIRECTED_CONNECTABLE_SCANNABLE =
        BLUEZ_ADV_TYPE_DIRECTED | BLUEZ_ADV_TYPE_CONNECTABLE | BLUEZ_ADV_TYPE_SCANNABLE,
    } ChipAdvType;

    typedef struct BluezServerEndpoint {
        char *owningName;  // Bus owning name

        // Adapter properties
        char *adapterName;
        char *adapterAddr;

        // Paths for objects published by this service
        char *rootPath;
        char *advPath;
        char *servicePath;

        // Objects (interfaces) subscibed to by this service
        GDBusObjectManager *objMgr;
        BluezAdapter1 *adapter;
        BluezDevice1 *device;

        // Objects (interfaces) published by this service
        GDBusObjectManagerServer *root;
        BluezGattService1 *service;
        BluezGattCharacteristic1 *c1;
        BluezGattCharacteristic1 *c2;

        // map device path to the connection
        GHashTable *connMap;

        uint32_t nodeId;
        uint16_t mtu;
        bool isCentral;
        char * advertisingUUID;
        CHIPServiceData * chipServiceData;
    } BluezServerEndpoint;

    /**
 * This type represents advertisement configuration.
 *
 */
    typedef struct ChipAdvConfig
    {
        ChipAdvType mType;     ///< Advertisement type.
        uint16_t       mInterval; ///< Advertisement interval (in ms).
        const uint8_t *mData;     ///< Advertisement data - Formatted as sequence of "<len, type, data>" structures.
        uint16_t       mLength;   ///< Advertisement data length (number of bytes).
    } ChipAdvConfig;

#define BLUEZ_PATH                    "/org/bluez"
#define ADVERTISING_PATH              BLUEZ_PATH "/advertising"

#define BLUEZ_INTERFACE               "org.bluez"
#define ADAPTER_INTERFACE             BLUEZ_INTERFACE ".Adapter1"
#define PROFILE_INTERFACE             BLUEZ_INTERFACE ".GattManager1"
#define ADVERTISING_MANAGER_INTERFACE BLUEZ_INTERFACE ".LEAdvertisingManager1"
#define SERVICE_INTERFACE             BLUEZ_INTERFACE ".GattService1"
#define CHARACTERISTIC_INTERFACE      BLUEZ_INTERFACE ".GattCharacteristic1"
#define ADVERTISING_INTERFACE         BLUEZ_INTERFACE ".LEAdvertisement1"
#define DEVICE_INTERFACE              BLUEZ_INTERFACE ".Device1"
#define BLE_ADDRESS_STRING_LENGTH 18

#define CHAR_TO_NIBBLE(c) (((c) <= '9') ? (c) - '0' : tolower((c)) - 'a' + 10)

        void PlatformBlueZInit(bool aIsCentral, char *aBleAddr, char *aBleName, uint32_t aNodeId);

// IPC primitives

        void bluezRunOnCHIPThread(void (*)(void), ...);

// platform hooks
        void platformBluezProcess(fd_set *aReadFds, fd_set *aWriteFds, int *aMaxFd);

        void platformBluezUpdateFdSet(fd_set *aReadFds, fd_set *aWriteFds, int *aMaxFd);

// static DBusConnection * gBluezDbusConn;
// static Adapter * gDefaultAdapter;

#define CHIP_PLAT_BLE_UUID_C1_STRING "18ee2ef5-263d-4559-959f-4f9c429f9d11"
#define CHIP_PLAT_BLE_UUID_C2_STRING "18ee2ef5-263d-4559-959f-4f9c429f9d12"

#define CHIP_BLE_BASE_SERVICE_UUID_STRING "-0000-1000-8000-00805f9b34fb"
#define CHIP_BLE_SERVICE_PREFIX_LENGTH 8
#define CHIP_BLE_BASE_SERVICE_PREFIX "0000"
#define CHIP_BLE_UUID_SERVICE_SHORT_STRING "fffb"

#define CHIP_BLE_UUID_SERVICE_STRING CHIP_BLE_BASE_SERVICE_PREFIX CHIP_BLE_UUID_SERVICE_SHORT_STRING CHIP_BLE_BASE_SERVICE_UUID_STRING

#define BLUEZ_BLE_PATH "/org/bluez/chipble"

#define BLUEZ_ADV_TYPE_FLAGS                0x01
#define BLUEZ_ADV_TYPE_INC_16_SERVICE_UUID  0x02
#define BLUEZ_ADV_TYPE_FIN_16_SERVICE_UUID  0x03
#define BLUEZ_ADV_TYPE_INC_32_SERVICE_UUID  0x04
#define BLUEZ_ADV_TYPE_FIN_32_SERVICE_UUID  0x05
#define BLUEZ_ADV_TYPE_INC_128_SERVICE_UUID 0x06
#define BLUEZ_ADV_TYPE_FIN_128_SERVICE_UUID 0x07
#define BLUEZ_ADV_TYPE_SHORT_LOCAL_NAME     0x08
#define BLUEZ_ADV_TYPE_FULL_LOCAL_NAME      0x09
#define BLUEZ_ADV_TYPE_TX_POWER             0x0A

#define BLUEZ_ADV_TYPE_TK_VALUE             0x10

#define BLUEZ_ADV_TYPE_CONN_INTERVAL        0x12
#define BLUEZ_ADV_TYPE_SOLICIT_16_UUID      0x14
#define BLUEZ_ADV_TYPE_SOLICIT_128_UUID     0x15
#define BLUEZ_ADV_TYPE_SERVICE_DATA         0x16

#define BLUEZ_ADV_FLAGS_LE_LIMITED          (1 << 0)
#define BLUEZ_ADV_FLAGS_LE_DISCOVERABLE     (1 << 1)
#define BLUEZ_ADV_FLAGS_EDR_UNSUPPORTED     (1 << 2)
#define BLUEZ_ADV_FLAGS_LE_EDR_CONTROLLER   (1 << 3)
#define BLUEZ_ADV_FLAGS_LE_EDR_HOST         (1 << 4)
// TODO: FIX for central if relevant.
#define FLAGS_C1 "write"
#define FLAGS_C2 "read,indicate"
#define HCI_MAX_MTU (104)
#define TO_GENERIC_FUN(fn) (void (*)(void))(fn)
#define PIPE_READ 0
#define PIPE_WRITE 1

}
}
}

#endif // CHIP_BLUEZ_HELPER_H
