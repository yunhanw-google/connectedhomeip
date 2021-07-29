/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
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
 *      This file defines the initiator side of a CHIP Read Interaction.
 *
 */

#include <app/AppBuildConfig.h>
#include <app/InteractionModelEngine.h>
#include <app/SubscribeInitiator.h>
#include <app/MessageDef/SubscribeRequest.h>
#include <app/MessageDef/SubscribeResponse.h>
#include <lib/support/TypeTraits.h>

namespace chip {
namespace app {
CHIP_ERROR SubscribeInitiator::Init(Messaging::ExchangeManager * apExchangeMgr, InteractionModelDelegate * apDelegate,
                                 uint64_t aAppIdentifier)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    err = ReadClient::Init(apExchangeMgr, apDelegate, aAppIdentifier);
    mRetryCounter = 0;
    mFinalSyncIntervalSeconds = 0;
    mSubscriptionId = 0;
    mEnableResubscribe = false;
    return err;
}

void SubscribeInitiator::ShutdownInternal()
{
    mRetryCounter = 0;
    mFinalSyncIntervalSeconds = 0;
    mSubscriptionId = 0;
    mEnableResubscribe = false;
    CancelLivenessCheckTimer();

    if (mSubscribePrepareParams.mpAttributePathParamsList != nullptr)
    {
        delete mSubscribePrepareParams.mpAttributePathParamsList;
        mSubscribePrepareParams.mpAttributePathParamsList = nullptr;
    }

    if (mSubscribePrepareParams.mpEventPathParamsList != nullptr)
    {
        delete mSubscribePrepareParams.mpEventPathParamsList;
        mSubscribePrepareParams.mpEventPathParamsList = nullptr;
    }

    ReadClient::ShutdownInternal();
}

CHIP_ERROR SubscribeInitiator::SendSubscribeRequest(SubscribePrepareParams & aSubscribePrepareParams)
{
    mSubscribePrepareParams = aSubscribePrepareParams;
    aSubscribePrepareParams.mpEventPathParamsList = nullptr;
    aSubscribePrepareParams.mEventPathParamsListSize   = 0;
    aSubscribePrepareParams.mpAttributePathParamsList  = nullptr;
    aSubscribePrepareParams.mAttributePathParamsListSize = 0;
    return SendSubscribeRequestInternal();
}

CHIP_ERROR SubscribeInitiator::SendSubscribeRequestInternal()
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    System::PacketBufferHandle msgBuf;
    System::PacketBufferTLVWriter writer;
    SubscribeRequest::Builder request;
    VerifyOrExit(ClientState::Initialized == mState, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpDelegate != nullptr, err = CHIP_ERROR_INCORRECT_STATE);
    msgBuf = System::PacketBufferHandle::New(kMaxSecureSduLengthBytes);
    VerifyOrExit(!msgBuf.IsNull(), err = CHIP_ERROR_NO_MEMORY);

    AbortExistingExchangeContext();

    writer.Init(std::move(msgBuf));

    err = request.Init(&writer);
    SuccessOrExit(err);

    if (mSubscribePrepareParams.mEventPathParamsListSize != 0 && mSubscribePrepareParams.mpEventPathParamsList != nullptr)
    {
        EventPathList::Builder & eventPathListBuilder = request.CreateEventPathListBuilder();
        SuccessOrExit(err = eventPathListBuilder.GetError());
        err = GenerateEventPathList(eventPathListBuilder, mSubscribePrepareParams.mpEventPathParamsList, mSubscribePrepareParams.mEventPathParamsListSize);
        SuccessOrExit(err);

        if (mSubscribePrepareParams.mEventNumber != 0)
        {
            // EventNumber is optional
            request.EventNumber(mSubscribePrepareParams.mEventNumber);
        }
    }

    if (mSubscribePrepareParams.mAttributePathParamsListSize != 0 && mSubscribePrepareParams.mpAttributePathParamsList != nullptr)
    {
        AttributePathList::Builder & attributePathListBuilder = request.CreateAttributePathListBuilder();
        SuccessOrExit(err = attributePathListBuilder.GetError());
        err = GenerateAttributePathList(attributePathListBuilder, mSubscribePrepareParams.mpAttributePathParamsList, mSubscribePrepareParams.mAttributePathParamsListSize);
        SuccessOrExit(err);
    }

    request.MinIntervalSeconds(mSubscribePrepareParams.mMinIntervalSeconds).MaxIntervalSeconds(mSubscribePrepareParams.mMaxIntervalSeconds).EndOfSubscribeRequest();
    SuccessOrExit(err = request.GetError());

    err = writer.Finalize(&msgBuf);
    SuccessOrExit(err);

    mpExchangeCtx = mpExchangeMgr->NewContext(mSubscribePrepareParams.mSecureSession, this);
    VerifyOrExit(mpExchangeCtx != nullptr, err = CHIP_ERROR_NO_MEMORY);
    mpExchangeCtx->SetResponseTimeout(kImMessageTimeoutMsec);

    err = mpExchangeCtx->SendMessage(Protocols::InteractionModel::MsgType::SubscribeRequest, std::move(msgBuf),
                                     Messaging::SendFlags(Messaging::SendMessageFlags::kExpectResponse));
    SuccessOrExit(err);
    MoveToState(ClientState::Subscribing);

exit:
    ChipLogFunctError(err);

    if (err != CHIP_NO_ERROR)
    {
        AbortExistingExchangeContext();
    }

    return err;
}

CHIP_ERROR SubscribeInitiator::OnMessageReceived(Messaging::ExchangeContext * apExchangeContext, const PacketHeader & aPacketHeader,
                                         const PayloadHeader & aPayloadHeader, System::PacketBufferHandle && aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    VerifyOrExit(!IsFree(), err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpDelegate != nullptr, err = CHIP_ERROR_INCORRECT_STATE);
    if (aPayloadHeader.HasMessageType(Protocols::InteractionModel::MsgType::SubscribeResponse))
    {
        VerifyOrExit(apExchangeContext == mpExchangeCtx, err = CHIP_ERROR_INCORRECT_STATE);
        err = ProcessSubscribeResponse(std::move(aPayload));
        if (err != CHIP_NO_ERROR)
        {
            mpDelegate->SubscribeError(this, err);
        }
        else
        {
            mpDelegate->SubscribeResponseProcessed(this);
        }
    }
    else if (aPayloadHeader.HasMessageType(Protocols::InteractionModel::MsgType::ReportData))
    {
        SetExchangeContext(apExchangeContext);
        err = ProcessReportData(std::move(aPayload));
        if (err != CHIP_NO_ERROR)
        {
            mpDelegate->ReportError(this, err);
        }
        else
        {
            mpDelegate->ReportProcessed(this);
        }
        ClearExchangeContext();
    }
    else
    {
        err = CHIP_ERROR_INVALID_MESSAGE_TYPE;
    }

exit:
    ChipLogFunctError(err);
    if (err != CHIP_NO_ERROR)
    {
        ShutdownInternal();
    }
    return err;
}

CHIP_ERROR SubscribeInitiator::ProcessSubscribeResponse(System::PacketBufferHandle && aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    SubscribeResponse::Parser subscribeResponse;
    System::PacketBufferTLVReader reader;
    reader.Init(std::move(aPayload));
    reader.Next();

    err = subscribeResponse.Init(reader);
    SuccessOrExit(err);

#if CHIP_CONFIG_IM_ENABLE_SCHEMA_CHECK
    err = subscribeResponse.CheckSchemaValidity();
    SuccessOrExit(err);
#endif

    err = subscribeResponse.GetSubscriptionId(&mSubscriptionId);
    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);

    err = subscribeResponse.GetFinalSyncIntervalSeconds(&mFinalSyncIntervalSeconds);
    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }

exit:
    ChipLogFunctError(err);
    err = SendStatusReport(err, true);
    return err;
}

void SubscribeInitiator::OnResponseTimeout(Messaging::ExchangeContext * apExchangeContext)
{
    ChipLogProgress(DataManagement, "Time out! failed to receive report data from Exchange: %d",
                    apExchangeContext->GetExchangeId());
    if (nullptr != mpDelegate)
    {
        if (ClientState::Subscribing == mState)
        {
            mpDelegate->SubscribeError(this, CHIP_ERROR_TIMEOUT);
        }
        else if (ClientState::SubscriptionIdle == mState)
        {
            mpDelegate->ReportError(this, CHIP_ERROR_TIMEOUT);
        }
    }

    InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->ScheduleWork(OnRetryCallback, this);
}

CHIP_ERROR SubscribeInitiator::SendStatusReport(CHIP_ERROR aError, bool aExpectResponse)
{
    ReturnLogErrorOnFailure(ReadClient::SendStatusReport(aError, aExpectResponse));
    if (IsSubscribing())
    {
        MoveToState(ClientState::SubscriptionIdle);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR SubscribeInitiator::ProcessReportData(System::PacketBufferHandle && aPayload)
{
    ReturnLogErrorOnFailure(ReadClient::ProcessReportData(std::move(aPayload)));
    ReturnLogErrorOnFailure(RefreshLivenessCheckTimer());
    return CHIP_NO_ERROR;
}

CHIP_ERROR SubscribeInitiator::RefreshLivenessCheckTimer()
{
    CHIP_ERROR err                   = CHIP_NO_ERROR;
    CancelLivenessCheckTimer();
    ChipLogProgress(DataManagement, "SubscribeInitiator::RefreshLivenessCheckTime timer %d", mFinalSyncIntervalSeconds);
    err = InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->StartTimer(
                mFinalSyncIntervalSeconds*1000, OnRetryCallback, this);

    if (err != CHIP_NO_ERROR)
    {
        ChipLogFunctError(err);
        ShutdownInternal();
    }
    return err;
}

void SubscribeInitiator::CancelLivenessCheckTimer()
{
   InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->CancelTimer(OnRetryCallback, this);
}

void SubscribeInitiator::OnRetryCallback(System::Layer * apSystemLayer, void * apAppState)
{
    CHIP_ERROR err                   = CHIP_NO_ERROR;
    uint16_t retryTimeSec = 0;
    SubscribeInitiator * const client = reinterpret_cast<SubscribeInitiator *>(apAppState);
    if ((nullptr != client->mpDelegate) && (ClientState::Uninitialized != client->mState))
    {
        client->mpDelegate->ApplyResubscribePolicy(client->mRetryCounter, retryTimeSec, client->mEnableResubscribe);
        if (client->mEnableResubscribe)
        {
            err = InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->StartTimer(
                    retryTimeSec * 1000, OnReSubscribeTimerCallback, apAppState);
        }
        else
        {
            client->ShutdownInternal();
        }
    }
}

void SubscribeInitiator::OnReSubscribeTimerCallback(System::Layer * apSystemLayer, void * apAppState)
{
    SubscribeInitiator * const client = reinterpret_cast<SubscribeInitiator *>(apAppState);
    if (client != nullptr && client->mEnableResubscribe && (client->mState != ClientState::Uninitialized))
    {
        client->mRetryCounter ++;
        client->MoveToState(ClientState::Initialized);
        client->SendSubscribeRequestInternal();
    }
}

/**
 * @brief Kick the resubscribe mechanism. This will initiate an immediate retry if resubscribe is enabled
 */
void SubscribeInitiator::ResetResubscribe()
{
    mRetryCounter = 0;
    mSubscriptionId = 0;
    mFinalSyncIntervalSeconds = 0;
    if (mState != ClientState::Uninitialized)
    {
        CancelLivenessCheckTimer();
        CancelResubscribe();
        if (mEnableResubscribe)
        {
            InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->StartTimer(
                    0, OnReSubscribeTimerCallback, this);
        }
    }
}

void SubscribeInitiator::CancelResubscribe()
{
    InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->CancelTimer(OnReSubscribeTimerCallback, this);
}
} // namespace app
} // namespace chip
