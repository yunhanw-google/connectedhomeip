/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
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

#pragma once

#include "sl_bgapi.h"
#include "sl_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SIMULATION-DISCLOSURE: Silicon Labs BGAPI (sl_bt_*) C interface header used by
// src/platform/silabs/efr32/BLEManagerImpl.cpp when compiled for ARM32 Linux
// and executed over /dev/ttySilabsBT0 (HCI H4 PTY bridge).

typedef struct
{
    uint8_t addr[6];
} bd_addr;

typedef struct
{
    uint8_t len;
    uint8_t data[255];
} sl_bt_uint8array_t;

typedef enum
{
    sl_bt_gap_public_address               = 0x0,
    sl_bt_gap_static_address               = 0x1,
    sl_bt_gap_random_resolvable_address    = 0x2,
    sl_bt_gap_random_nonresolvable_address = 0x3,
} sl_bt_gap_address_type_t;

typedef enum
{
    sl_bt_advertiser_advertising_data_packet = 0x0,
    sl_bt_advertiser_scan_response_packet    = 0x1,
} sl_bt_advertiser_packet_type_t;

typedef enum
{
    sl_bt_advertiser_non_connectable           = 0x0,
    sl_bt_advertiser_connectable_scannable     = 0x2,
    sl_bt_advertiser_scannable_non_connectable = 0x3,
    sl_bt_advertiser_connectable_non_scannable = 0x4,
} sl_bt_advertiser_connection_mode_t;

typedef enum
{
    sl_bt_gatt_disable      = 0x0,
    sl_bt_gatt_notification = 0x1,
    sl_bt_gatt_indication   = 0x2,
} sl_bt_gatt_client_config_flag_t;

typedef enum
{
    sl_bt_gatt_server_client_config = 0x1,
    sl_bt_gatt_server_confirmation  = 0x2,
} sl_bt_gatt_server_characteristic_status_flag_t;

enum
{
    sl_bt_evt_system_boot_id                       = 0x000100a0,
    sl_bt_evt_system_soft_timer_id                 = 0x000102a0,
    sl_bt_evt_connection_opened_id                 = 0x000600a0,
    sl_bt_evt_connection_closed_id                 = 0x000601a0,
    sl_bt_evt_connection_parameters_id             = 0x000602a0,
    sl_bt_evt_connection_phy_status_id             = 0x000604a0,
    sl_bt_evt_connection_remote_used_features_id   = 0x000605a0,
    sl_bt_evt_connection_data_length_id            = 0x000606a0,
    sl_bt_evt_gatt_mtu_exchanged_id                = 0x000900a0,
    sl_bt_evt_gatt_server_attribute_value_id       = 0x000a00a0,
    sl_bt_evt_gatt_server_user_read_request_id     = 0x000a01a0,
    sl_bt_evt_gatt_server_user_write_request_id    = 0x000a02a0,
    sl_bt_evt_gatt_server_characteristic_status_id = 0x000a03a0,
};

typedef struct
{
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint16_t build;
    uint32_t bootloader;
    uint16_t hw;
    uint32_t hash;
} sl_bt_evt_system_boot_t;

typedef struct
{
    uint8_t handle;
} sl_bt_evt_system_soft_timer_t;

typedef struct
{
    bd_addr address;
    uint8_t address_type;
    uint8_t master;
    uint8_t connection;
    uint8_t bonding;
    uint8_t advertiser;
} sl_bt_evt_connection_opened_t;

typedef struct
{
    uint8_t connection;
    uint16_t reason;
} sl_bt_evt_connection_closed_t;

typedef struct
{
    uint8_t connection;
    uint16_t interval;
    uint16_t latency;
    uint16_t timeout;
    uint8_t security_mode;
    uint16_t txsize;
} sl_bt_evt_connection_parameters_t;

typedef struct
{
    uint8_t connection;
    uint8_t phy;
} sl_bt_evt_connection_phy_status_t;

typedef struct
{
    uint8_t connection;
    uint16_t tx_data_len;
    uint16_t tx_time_us;
    uint16_t rx_data_len;
    uint16_t rx_time_us;
} sl_bt_evt_connection_data_length_t;

typedef struct
{
    uint8_t connection;
    uint16_t mtu;
} sl_bt_evt_gatt_mtu_exchanged_t;

typedef struct
{
    uint8_t connection;
    uint16_t characteristic;
    uint8_t att_opcode;
    uint16_t offset;
    sl_bt_uint8array_t value;
} sl_bt_evt_gatt_server_user_write_request_t;

typedef struct
{
    uint8_t connection;
    uint16_t characteristic;
    uint8_t att_opcode;
    uint16_t offset;
} sl_bt_evt_gatt_server_user_read_request_t;

typedef struct
{
    uint8_t connection;
    uint16_t characteristic;
    uint8_t status_flags;
    uint16_t client_config_flags;
    uint16_t client_config;
} sl_bt_evt_gatt_server_characteristic_status_t;

typedef struct
{
    uint32_t header;
    union
    {
        uint8_t payload[264];
        sl_bt_evt_system_boot_t evt_system_boot;
        sl_bt_evt_system_soft_timer_t evt_system_soft_timer;
        sl_bt_evt_connection_opened_t evt_connection_opened;
        sl_bt_evt_connection_closed_t evt_connection_closed;
        sl_bt_evt_connection_parameters_t evt_connection_parameters;
        sl_bt_evt_connection_phy_status_t evt_connection_phy_status;
        sl_bt_evt_connection_data_length_t evt_connection_data_length;
        sl_bt_evt_gatt_mtu_exchanged_t evt_gatt_mtu_exchanged;
        sl_bt_evt_gatt_server_user_write_request_t evt_gatt_server_user_write_request;
        sl_bt_evt_gatt_server_user_read_request_t evt_gatt_server_user_read_request;
        sl_bt_evt_gatt_server_characteristic_status_t evt_gatt_server_characteristic_status;
    } data;
} sl_bt_msg_t;

void sl_bt_on_event(sl_bt_msg_t * evt);

sl_status_t sl_bt_connection_close(uint8_t connection);
sl_status_t sl_bt_connection_set_default_parameters(uint16_t min_interval, uint16_t max_interval, uint16_t latency,
                                                    uint16_t timeout, uint16_t min_ce_length, uint16_t max_ce_length);
sl_status_t sl_bt_connection_set_parameters(uint8_t connection, uint16_t min_interval, uint16_t max_interval, uint16_t latency,
                                            uint16_t timeout, uint16_t min_ce_length, uint16_t max_ce_length);
sl_status_t sl_bt_system_set_lazy_soft_timer(uint32_t time, uint32_t slack, uint8_t handle, uint8_t single_shot);
sl_status_t sl_bt_gatt_server_send_indication(uint8_t connection, uint16_t characteristic, size_t value_len, const uint8_t * value);
sl_status_t sl_bt_gatt_server_send_user_read_response(uint8_t connection, uint16_t characteristic, uint8_t att_errorcode,
                                                      size_t value_len, const uint8_t * value, uint16_t * sent_len);
sl_status_t sl_bt_advertiser_create_set(uint8_t * handle);
sl_status_t sl_bt_advertiser_set_random_address(uint8_t handle, uint8_t addr_type, bd_addr address, bd_addr * address_out);
sl_status_t sl_bt_advertiser_clear_random_address(uint8_t handle);
sl_status_t sl_bt_advertiser_delete_set(uint8_t handle);
sl_status_t sl_bt_advertiser_set_timing(uint8_t handle, uint32_t interval_min, uint32_t interval_max, uint16_t duration,
                                        uint8_t maxevents);
sl_status_t sl_bt_advertiser_configure(uint8_t handle, uint32_t flags);
sl_status_t sl_bt_advertiser_stop(uint8_t handle);
sl_status_t sl_bt_legacy_advertiser_set_data(uint8_t advertising_set, uint8_t type, size_t data_len, const uint8_t * data);
sl_status_t sl_bt_legacy_advertiser_start(uint8_t advertising_set, uint8_t connect);

#ifdef __cplusplus
}
#endif
