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

namespace chip {
namespace DeviceLayer {
namespace Silabs {

/**
 * Shut down the POSIX-backed CMSIS-RTOS2 AppTask thread and timers cleanly
 * before the Linux Matter event loop exits.
 */
void ShutdownSilabsPosixRuntime();

/**
 * Inject a simulated WSTK button action (e.g. from NamedPipeCommands).
 * button: 0 = APP_FUNCTION_BUTTON (PB0), 1 = APP_LIGHT_SWITCH (PB1)
 * action: 0 = ButtonReleased, 1 = ButtonPressed
 */
void TriggerSilabsButtonAction(uint8_t button, uint8_t action);

} // namespace Silabs
} // namespace DeviceLayer
} // namespace chip
