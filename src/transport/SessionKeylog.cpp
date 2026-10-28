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

#include <transport/SessionKeylog.h>

#if CHIP_CONFIG_SESSION_KEYLOG

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <transport/SecureSession.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace chip {
namespace Transport {

namespace {

constexpr char kKeylogFileEnvVar[] = "CHIP_SESSION_KEYLOG_FILE";

void WriteHex(FILE * file, const Crypto::Symmetric128BitsKeyByteArray & key)
{
    for (uint8_t byte : key)
    {
        fprintf(file, "%02x", byte);
    }
}

} // namespace

void LogSessionKeys(const SecureSession & session)
{
    const char * path = getenv(kKeylogFileEnvVar);
    if (path == nullptr || path[0] == '\0')
    {
        return;
    }

    Crypto::Symmetric128BitsKeyByteArray i2rKey;
    Crypto::Symmetric128BitsKeyByteArray r2iKey;
    CHIP_ERROR err = session.GetCryptoContext().ExportSessionKeysForKeylog(i2rKey, r2iKey);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(SecureChannel, "Session keylog: cannot export keys of LSID:%u: %" CHIP_ERROR_FORMAT,
                     session.GetLocalSessionId(), err.Format());
        return;
    }

    FILE * file = fopen(path, "a");
    if (file == nullptr)
    {
        ChipLogError(SecureChannel, "Session keylog: cannot open %s", path);
        Crypto::ClearSecretData(i2rKey);
        Crypto::ClearSecretData(r2iKey);
        return;
    }

    fprintf(file,
            "MATTER_SESSION_KEYS v=1 type=%s role=%s local_sid=%u peer_sid=%u local_node=%016" PRIX64 " peer_node=%016" PRIX64
            " i2r=",
            session.IsPASESession() ? "PASE" : "CASE", session.GetCryptoContext().IsInitiator() ? "initiator" : "responder",
            session.GetLocalSessionId(), session.GetPeerSessionId(), session.GetLocalScopedNodeId().GetNodeId(),
            session.GetPeer().GetNodeId());
    WriteHex(file, i2rKey);
    fprintf(file, " r2i=");
    WriteHex(file, r2iKey);
    fprintf(file, "\n");
    fclose(file);

    Crypto::ClearSecretData(i2rKey);
    Crypto::ClearSecretData(r2iKey);
    ChipLogProgress(SecureChannel, "Session keylog: wrote keys of LSID:%u PSID:%u (TEST-ONLY)", session.GetLocalSessionId(),
                    session.GetPeerSessionId());
}

} // namespace Transport
} // namespace chip

#endif // CHIP_CONFIG_SESSION_KEYLOG
