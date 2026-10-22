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

#include <stdint.h>

typedef uint32_t sl_status_t;

#ifndef SL_STATUS_OK
#define SL_STATUS_OK ((sl_status_t) 0x0000)
#endif
#ifndef SL_STATUS_INVALID_STATE
#define SL_STATUS_INVALID_STATE ((sl_status_t) 0x0002)
#endif
#ifndef SL_STATUS_NOT_SUPPORTED
#define SL_STATUS_NOT_SUPPORTED ((sl_status_t) 0x000F)
#endif
#ifndef SL_STATUS_INVALID_PARAMETER
#define SL_STATUS_INVALID_PARAMETER ((sl_status_t) 0x0021)
#endif
#ifndef SL_STATUS_BT_ATT_INVALID_ATT_LENGTH
#define SL_STATUS_BT_ATT_INVALID_ATT_LENGTH ((sl_status_t) 0x040D)
#endif
#ifndef SL_STATUS_BT_CTRL_REMOTE_USER_TERMINATED
#define SL_STATUS_BT_CTRL_REMOTE_USER_TERMINATED ((sl_status_t) 0x1013)
#endif
#ifndef SL_STATUS_BT_CTRL_REMOTE_DEVICE_TERMINATED_CONNECTION_DUE_TO_LOW_RESOURCES
#define SL_STATUS_BT_CTRL_REMOTE_DEVICE_TERMINATED_CONNECTION_DUE_TO_LOW_RESOURCES ((sl_status_t) 0x1014)
#endif
#ifndef SL_STATUS_BT_CTRL_REMOTE_POWERING_OFF
#define SL_STATUS_BT_CTRL_REMOTE_POWERING_OFF ((sl_status_t) 0x1015)
#endif
#ifndef SL_STATUS_BT_CTRL_CONNECTION_TERMINATED_BY_LOCAL_HOST
#define SL_STATUS_BT_CTRL_CONNECTION_TERMINATED_BY_LOCAL_HOST ((sl_status_t) 0x1016)
#endif
