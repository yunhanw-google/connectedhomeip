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

// SIMULATION-DISCLOSURE: Silicon Labs WSTK board component catalog macros
// enabled for the POSIX-hosted Silabs Lighting-App simulation node.

#ifndef ENABLE_WSTK_LEDS
#define ENABLE_WSTK_LEDS 1
#endif

#ifndef SL_CATALOG_SIMPLE_LED_LED1_PRESENT
#define SL_CATALOG_SIMPLE_LED_LED1_PRESENT 1
#endif

#ifndef SL_CATALOG_SIMPLE_BUTTON_PRESENT
#define SL_CATALOG_SIMPLE_BUTTON_PRESENT 1
#endif

#ifndef SL_MATTER_RGB_LED_ENABLED
#define SL_MATTER_RGB_LED_ENABLED 1
#endif
