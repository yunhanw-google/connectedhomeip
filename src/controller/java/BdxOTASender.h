/*
 *   Copyright (c) 2023 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */
#pragma once

#include <jni.h>
#include <map>
#include <memory>
#include <utility>

#include <messaging/ExchangeMgr.h>
#include <protocols/bdx/BdxUri.h>
#include <protocols/bdx/TransferFacilitator.h>

constexpr uint32_t kMaxBDXURILen = 256;

class BdxOTASender;

class BdxOTASession : public chip::bdx::Responder
{
public:
    BdxOTASession(BdxOTASender * owner, jobject otaDelegate, chip::System::Layer * systemLayer, chip::FabricIndex fabricIndex,
                  chip::NodeId nodeId);
    ~BdxOTASession() override;

    CHIP_ERROR PrepareForTransfer();
    void ResetState();

    chip::FabricIndex GetFabricIndex() const { return mFabricIndex; }
    chip::NodeId GetNodeId() const { return mNodeId; }

private:
    static void HandleBdxInitReceivedTimeoutExpired(chip::System::Layer * systemLayer, void * state);

    CHIP_ERROR OnMessageToSend(chip::bdx::TransferSession::OutputEvent & event);
    CHIP_ERROR OnTransferSessionBegin(chip::bdx::TransferSession::OutputEvent & event);
    CHIP_ERROR OnTransferSessionEnd(chip::bdx::TransferSession::OutputEvent & event);
    CHIP_ERROR OnBlockQuery(chip::bdx::TransferSession::OutputEvent & event);
    void HandleTransferSessionOutput(chip::bdx::TransferSession::OutputEvent & event) override;

    BdxOTASender * mOwner                           = nullptr;
    jobject mOtaDelegate                            = nullptr;
    chip::System::Layer * mSystemLayer              = nullptr;
    bool mInitialized                               = false;
    chip::FabricIndex mFabricIndex                  = chip::kUndefinedFabricIndex;
    chip::NodeId mNodeId                            = chip::kUndefinedNodeId;
    uint64_t mTransferGeneration                    = 0;
};

class BdxOTASender : public chip::Messaging::UnsolicitedMessageHandler
{
public:
    BdxOTASender(jobject otaDelegate) : mOtaDelegate(otaDelegate) {}
    ~BdxOTASender() override { static_cast<void>(Shutdown()); }

    CHIP_ERROR PrepareForTransfer(chip::FabricIndex fabricIndex, chip::NodeId nodeId);

    CHIP_ERROR Init(chip::System::Layer * systemLayer, chip::Messaging::ExchangeManager * exchangeMgr);

    CHIP_ERROR Shutdown();

    void ResetState();

    void RemoveSession(chip::FabricIndex fabricIndex, chip::NodeId nodeId);

    CHIP_ERROR OnUnsolicitedMessageReceived(const chip::PayloadHeader & payloadHeader, const chip::SessionHandle & session,
                                            chip::Messaging::ExchangeDelegate *& newDelegate) override;

private:
    jobject mOtaDelegate                            = nullptr;
    chip::System::Layer * mSystemLayer              = nullptr;
    chip::Messaging::ExchangeManager * mExchangeMgr = nullptr;

    std::map<std::pair<chip::FabricIndex, chip::NodeId>, std::unique_ptr<BdxOTASession>> mSessions;
};
