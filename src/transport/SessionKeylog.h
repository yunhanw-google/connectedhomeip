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

/**
 *    @file
 *      TEST-ONLY session key log, compiled in only when the chip_enable_session_keylog GN arg
 *      (CHIP_CONFIG_SESSION_KEYLOG) is set. It exists so that test harnesses (for example the
 *      Cirque flow visualizer) can decrypt captured Matter messages. Never enable it in a product.
 *
 *      Nothing is written unless the CHIP_SESSION_KEYLOG_FILE environment variable names a file.
 *      When it does, every secure session this node activates (PASE or CASE, either role) appends
 *      one line to that file:
 *
 *        MATTER_SESSION_KEYS v=1 type=<PASE|CASE> role=<initiator|responder>
 *            local_sid=<u16> peer_sid=<u16> local_node=<16 hex> peer_node=<16 hex>
 *            i2r=<32 hex> r2i=<32 hex>
 *
 *      (one line, fields separated by single spaces). Session IDs are decimal; local_node /
 *      peer_node are the session's local and peer node IDs as stored in SecureSession (for CASE
 *      the operational node IDs; for PASE the local node ID is 0000000000000000 and the peer is a
 *      PAKE key ID such as FFFFFFFB00000000); i2r / r2i are the AES-128-CCM keys of the
 *      initiator-to-responder and responder-to-initiator directions (Matter Core spec, session
 *      key derivation: HKDF-SHA256 output = I2RKey || R2IKey || AttestationChallenge).
 *
 *      A message carries the session ID chosen by its receiver, so a message whose session ID is
 *      the responder's session ID was sent by the initiator and is decrypted with i2r, and vice
 *      versa. The nonce source node ID is the sender's operational node ID for CASE and 0 for
 *      PASE. Only the raw session keystore exposes key material; with other keystores no line is
 *      written and an error is logged.
 */

#pragma once

#include <lib/core/CHIPConfig.h>

#if CHIP_CONFIG_SESSION_KEYLOG

namespace chip {
namespace Transport {

class SecureSession;

/**
 * Appends the keys of an activated secure session to $CHIP_SESSION_KEYLOG_FILE (see file comment).
 * Does nothing when the environment variable is unset or empty. Never changes session state.
 */
void LogSessionKeys(const SecureSession & session);

} // namespace Transport
} // namespace chip

#endif // CHIP_CONFIG_SESSION_KEYLOG
