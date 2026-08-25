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

#include "BdxOTASender.h"

#include <lib/support/CHIPJNIError.h>
#include <lib/support/JniReferences.h>
#include <lib/support/JniTypeWrappers.h>
#include <platform/LockTracker.h>
#include <protocols/interaction_model/Constants.h>

using namespace chip;
using namespace chip::app;
using namespace chip::bdx;
using Protocols::InteractionModel::Status;

constexpr uint32_t kMaxBdxBlockSize = 1024;

// Since the BDX timeout is 5 minutes and we are starting this after query image is available and before the BDX init comes,
// we just double the timeout to give enough time for the BDX init to come in a reasonable amount of time.
constexpr System::Clock::Timeout kBdxInitReceivedTimeout = System::Clock::Seconds16(10 * 60);

constexpr System::Clock::Timeout kBdxTimeout        = System::Clock::Seconds16(5 * 60); // OTA Spec mandates >= 5 minutes
constexpr System::Clock::Timeout kBdxPollIntervalMs = System::Clock::Milliseconds32(50);
constexpr bdx::TransferRole kBdxRole                = bdx::TransferRole::kSender;

// ================= BdxOTASession Implementation =================

BdxOTASession::BdxOTASession(BdxOTASender * owner, jobject otaDelegate, System::Layer * systemLayer, FabricIndex fabricIndex,
                             NodeId nodeId) :
    mOwner(owner), mOtaDelegate(otaDelegate), mSystemLayer(systemLayer), mFabricIndex(fabricIndex), mNodeId(nodeId)
{}

BdxOTASession::~BdxOTASession()
{
    ResetState();
}

void BdxOTASession::HandleBdxInitReceivedTimeoutExpired(System::Layer * systemLayer, void * state)
{
    VerifyOrReturn(state != nullptr);
    static_cast<BdxOTASession *>(state)->ResetState();
}

CHIP_ERROR BdxOTASession::PrepareForTransfer()
{
    assertChipStackLockedByCurrentThread();
    VerifyOrReturnError(mSystemLayer != nullptr, CHIP_ERROR_INCORRECT_STATE);

    if (mInitialized)
    {
        ResetState();
    }

    CHIP_ERROR err = mSystemLayer->StartTimer(kBdxInitReceivedTimeout, HandleBdxInitReceivedTimeoutExpired, this);
    LogErrorOnFailure(err);

    BitFlags<bdx::TransferControlFlags> flags(bdx::TransferControlFlags::kReceiverDrive);
    err = Responder::PrepareForTransfer(mSystemLayer, kBdxRole, flags, kMaxBdxBlockSize, kBdxTimeout, kBdxPollIntervalMs);
    if (err == CHIP_NO_ERROR)
    {
        mInitialized = true;
    }
    return err;
}

void BdxOTASession::ResetState()
{
    assertChipStackLockedByCurrentThread();
    if (mSystemLayer)
    {
        mSystemLayer->CancelTimer(HandleBdxInitReceivedTimeoutExpired, this);
    }
    if (!mInitialized)
    {
        return;
    }
    mInitialized = false;
    Responder::ResetTransfer();
    ++mTransferGeneration;

    if (mExchangeCtx != nullptr)
    {
        mExchangeCtx->Close();
        mExchangeCtx = nullptr;
    }

    FabricIndex fabricIndex = mFabricIndex;
    NodeId nodeId           = mNodeId;
    BdxOTASender * owner    = mOwner;
    mOwner                  = nullptr;

    if (owner != nullptr)
    {
        owner->RemoveSession(fabricIndex, nodeId);
    }
}

CHIP_ERROR BdxOTASession::OnMessageToSend(TransferSession::OutputEvent & event)
{
    assertChipStackLockedByCurrentThread();

    VerifyOrReturnError(mExchangeCtx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mOtaDelegate != nullptr, CHIP_ERROR_INCORRECT_STATE);

    Messaging::SendFlags sendFlags;

    // All messages sent from the Sender expect a response, except for a StatusReport which would indicate an error and
    // the end of the transfer.
    if (!event.msgTypeData.HasMessageType(Protocols::SecureChannel::MsgType::StatusReport))
    {
        sendFlags.Set(Messaging::SendMessageFlags::kExpectResponse);
    }

    auto & msgTypeData = event.msgTypeData;
    CHIP_ERROR err =
        mExchangeCtx->SendMessage(msgTypeData.ProtocolId, msgTypeData.MessageType, std::move(event.MsgData), sendFlags);
    if (err != CHIP_NO_ERROR)
    {
        mExchangeCtx->Close();
        mExchangeCtx = nullptr;
        ResetState();
    }
    else if (event.msgTypeData.HasMessageType(Protocols::SecureChannel::MsgType::StatusReport))
    {
        mExchangeCtx = nullptr;
        ResetState();
    }
    return err;
}

CHIP_ERROR BdxOTASession::OnTransferSessionBegin(TransferSession::OutputEvent & event)
{
    assertChipStackLockedByCurrentThread();
    if (mSystemLayer)
    {
        mSystemLayer->CancelTimer(HandleBdxInitReceivedTimeoutExpired, this);
    }

    VerifyOrReturnError(mFabricIndex != kUndefinedFabricIndex, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mNodeId != kUndefinedNodeId, CHIP_ERROR_INCORRECT_STATE);
    uint16_t fdl = 0;

    const uint8_t * fd = mTransfer.GetFileDesignator(fdl);
    VerifyOrReturnError(fdl <= bdx::kMaxFileDesignatorLen, CHIP_ERROR_INVALID_ARGUMENT);
    CharSpan fileDesignatorSpan(Uint8::to_const_char(fd), fdl);

    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();

    JniLocalReferenceScope scope(env);
    UtfString fileDesignator(env, fileDesignatorSpan);

    uint64_t offset = mTransfer.GetStartOffset();

    jmethodID handleBDXTransferSessionBeginMethod;
    CHIP_ERROR err = JniReferences::GetInstance().FindMethod(env, mOtaDelegate, "handleBDXTransferSessionBegin",
                                                             "(JLjava/lang/String;J)V", &handleBDXTransferSessionBeginMethod);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ChipLogError(Controller, "Could not find handleBDXTransferSessionBegin method"));

    env->CallVoidMethod(mOtaDelegate, handleBDXTransferSessionBeginMethod, static_cast<jlong>(mNodeId), fileDesignator.jniValue(),
                        static_cast<jlong>(offset));
    if (env->ExceptionCheck())
    {
        ChipLogError(Support, "Exception in call java method");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    TransferSession::TransferAcceptData acceptData;
    acceptData.ControlMode  = bdx::TransferControlFlags::kReceiverDrive;
    acceptData.MaxBlockSize = mTransfer.GetTransferBlockSize();
    acceptData.StartOffset  = mTransfer.GetStartOffset();
    acceptData.Length       = mTransfer.GetTransferLength();

    LogErrorOnFailure(mTransfer.AcceptTransfer(acceptData));

    return CHIP_NO_ERROR;
}

CHIP_ERROR BdxOTASession::OnTransferSessionEnd(TransferSession::OutputEvent & event)
{
    assertChipStackLockedByCurrentThread();

    VerifyOrReturnError(mFabricIndex != kUndefinedFabricIndex, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mNodeId != kUndefinedNodeId, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mOtaDelegate != nullptr, CHIP_ERROR_INCORRECT_STATE);

    CHIP_ERROR error = CHIP_NO_ERROR;
    if (event.EventType == TransferSession::OutputEventType::kTransferTimeout)
    {
        error = CHIP_ERROR_TIMEOUT;
    }
    else if (event.EventType != TransferSession::OutputEventType::kAckEOFReceived)
    {
        error = CHIP_ERROR_INTERNAL;
    }

    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();

    jmethodID handleBDXTransferSessionEndMethod;
    CHIP_ERROR err = JniReferences::GetInstance().FindMethod(env, mOtaDelegate, "handleBDXTransferSessionEnd", "(JJ)V",
                                                             &handleBDXTransferSessionEndMethod);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ChipLogError(Controller, "Could not find handleBDXTransferSessionEnd method"));

    env->CallVoidMethod(mOtaDelegate, handleBDXTransferSessionEndMethod, static_cast<jlong>(error.AsInteger()),
                        static_cast<jlong>(mNodeId));
    if (env->ExceptionCheck())
    {
        ChipLogError(Support, "Exception in call java method");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    ResetState();
    return CHIP_NO_ERROR;
}

CHIP_ERROR BdxOTASession::OnBlockQuery(TransferSession::OutputEvent & event)
{
    assertChipStackLockedByCurrentThread();

    VerifyOrReturnError(mFabricIndex != kUndefinedFabricIndex, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mNodeId != kUndefinedNodeId, CHIP_ERROR_INCORRECT_STATE);

    uint16_t blockSize  = mTransfer.GetTransferBlockSize();
    uint32_t blockIndex = mTransfer.GetNextBlockNum();

    uint64_t bytesToSkip = 0;
    if (event.EventType == TransferSession::OutputEventType::kQueryWithSkipReceived)
    {
        bytesToSkip = event.bytesToSkip.BytesToSkip;
    }

    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();

    JniLocalReferenceScope scope(env);
    jmethodID handleBDXQueryMethod;
    CHIP_ERROR err = JniReferences::GetInstance().FindMethod(
        env, mOtaDelegate, "handleBDXQuery", "(JIJJ)Lchip/devicecontroller/OTAProviderDelegate$BDXData;", &handleBDXQueryMethod);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, ChipLogError(Controller, "Could not find handleBDXQuery method"));

    jobject bdxData =
        env->CallObjectMethod(mOtaDelegate, handleBDXQueryMethod, static_cast<jlong>(mNodeId), static_cast<jint>(blockSize),
                              static_cast<jlong>(blockIndex), static_cast<jlong>(bytesToSkip));
    if (env->ExceptionCheck())
    {
        ChipLogError(Support, "Exception in call java method");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    if (bdxData == nullptr)
    {
        LogErrorOnFailure(mTransfer.AbortTransfer(bdx::StatusCode::kUnknown));
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    jmethodID getDataMethod;
    err = JniReferences::GetInstance().FindMethod(env, bdxData, "getData", "()[B", &getDataMethod);
    if (env->ExceptionCheck())
    {
        ChipLogError(Support, "Exception in call java method");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    jmethodID isEOFMethod;
    err = JniReferences::GetInstance().FindMethod(env, bdxData, "isEOF", "()Z", &isEOFMethod);
    if (env->ExceptionCheck())
    {
        ChipLogError(Support, "Exception in call java method");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }
    jbyteArray jData = (jbyteArray) env->CallObjectMethod(bdxData, getDataMethod);
    jboolean jIsEOF  = env->CallBooleanMethod(bdxData, isEOFMethod);

    JniByteArray data(env, jData);

    TransferSession::BlockData blockData;
    blockData.Data   = static_cast<const uint8_t *>(data.byteSpan().data());
    blockData.Length = static_cast<size_t>(data.byteSpan().size());
    blockData.IsEof  = jIsEOF == JNI_TRUE;

    err = mTransfer.PrepareBlock(blockData);
    if (CHIP_NO_ERROR != err)
    {
        LogErrorOnFailure(err);
        LogErrorOnFailure(mTransfer.AbortTransfer(bdx::StatusCode::kUnknown));
    }

    return CHIP_NO_ERROR;
}

void BdxOTASession::HandleTransferSessionOutput(TransferSession::OutputEvent & event)
{
    VerifyOrReturn(mOtaDelegate != nullptr);

    CHIP_ERROR err = CHIP_NO_ERROR;
    switch (event.EventType)
    {
    case TransferSession::OutputEventType::kInitReceived:
        err = OnTransferSessionBegin(event);
        if (err != CHIP_NO_ERROR)
        {
            LogErrorOnFailure(mTransfer.AbortTransfer(GetBdxStatusCodeFromChipError(err)));
        }
        break;
    case TransferSession::OutputEventType::kStatusReceived:
        ChipLogError(BDX, "Got StatusReport %x", static_cast<uint16_t>(event.statusData.statusCode));
        FALLTHROUGH;
    case TransferSession::OutputEventType::kAckEOFReceived:
    case TransferSession::OutputEventType::kInternalError:
    case TransferSession::OutputEventType::kTransferTimeout:
        err = OnTransferSessionEnd(event);
        break;
    case TransferSession::OutputEventType::kQueryWithSkipReceived:
    case TransferSession::OutputEventType::kQueryReceived:
        err = OnBlockQuery(event);
        break;
    case TransferSession::OutputEventType::kMsgToSend:
        err = OnMessageToSend(event);
        break;
    case TransferSession::OutputEventType::kNone:
    case TransferSession::OutputEventType::kAckReceived:
        break;
    case TransferSession::OutputEventType::kAcceptReceived:
    case TransferSession::OutputEventType::kBlockReceived:
    default:
        chipDie();
        break;
    }
    LogErrorOnFailure(err);
}

// ================= BdxOTASender Implementation =================

CHIP_ERROR BdxOTASender::Init(System::Layer * systemLayer, Messaging::ExchangeManager * exchangeMgr)
{
    assertChipStackLockedByCurrentThread();

    VerifyOrReturnError(mSystemLayer == nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mExchangeMgr == nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(systemLayer != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(exchangeMgr != nullptr, CHIP_ERROR_INCORRECT_STATE);

    TEMPORARY_RETURN_IGNORED exchangeMgr->RegisterUnsolicitedMessageHandlerForProtocol(Protocols::BDX::Id, this);

    mSystemLayer = systemLayer;
    mExchangeMgr = exchangeMgr;

    return CHIP_NO_ERROR;
}

CHIP_ERROR BdxOTASender::Shutdown()
{
    assertChipStackLockedByCurrentThread();
    if (mExchangeMgr != nullptr)
    {
        TEMPORARY_RETURN_IGNORED mExchangeMgr->UnregisterUnsolicitedMessageHandlerForProtocol(Protocols::BDX::Id);
        mExchangeMgr = nullptr;
    }
    ResetState();
    mSystemLayer = nullptr;
    return CHIP_NO_ERROR;
}

void BdxOTASender::ResetState()
{
    assertChipStackLockedByCurrentThread();
    mSessions.clear();
}

void BdxOTASender::RemoveSession(FabricIndex fabricIndex, NodeId nodeId)
{
    assertChipStackLockedByCurrentThread();
    auto key = std::make_pair(fabricIndex, nodeId);
    mSessions.erase(key);
}

CHIP_ERROR BdxOTASender::PrepareForTransfer(FabricIndex fabricIndex, NodeId nodeId)
{
    assertChipStackLockedByCurrentThread();
    VerifyOrReturnError(mExchangeMgr != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mSystemLayer != nullptr, CHIP_ERROR_INCORRECT_STATE);

    auto key = std::make_pair(fabricIndex, nodeId);
    auto it  = mSessions.find(key);
    if (it != mSessions.end())
    {
        it->second->ResetState();
    }

    auto session   = std::make_unique<BdxOTASession>(this, mOtaDelegate, mSystemLayer, fabricIndex, nodeId);
    CHIP_ERROR err = session->PrepareForTransfer();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Controller, "Failed to prepare BDX transfer for node 0x" ChipLogFormatX64 ": %" CHIP_ERROR_FORMAT,
                     ChipLogValueX64(nodeId), err.Format());
        return err;
    }

    mSessions[key] = std::move(session);
    return CHIP_NO_ERROR;
}

CHIP_ERROR BdxOTASender::OnUnsolicitedMessageReceived(const PayloadHeader & payloadHeader, const SessionHandle & session,
                                                      Messaging::ExchangeDelegate *& newDelegate)
{
    ScopedNodeId peer       = session->GetPeer();
    FabricIndex fabricIndex = peer.GetFabricIndex();
    NodeId peerNodeId       = peer.GetNodeId();

    auto key = std::make_pair(fabricIndex, peerNodeId);
    auto it  = mSessions.find(key);
    if (it != mSessions.end())
    {
        newDelegate = it->second.get();
        return CHIP_NO_ERROR;
    }

    ChipLogError(Controller, "No BDX session registered for node 0x" ChipLogFormatX64 ", fabric %u", ChipLogValueX64(peerNodeId),
                 fabricIndex);
    return CHIP_ERROR_NOT_FOUND;
}
