/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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
 *      This file defines read handler for a CHIP Interaction Data model
 *
 */

#include <app/AppBuildConfig.h>
#include <app/InteractionModelEngine.h>
#include <app/MessageDef/EventPath.h>
#include <app/ReadHandler.h>
#include <app/reporting/Engine.h>
#include <app/MessageDef/SubscribeRequest.h>
#include <app/MessageDef/SubscribeResponse.h>
#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <lib/support/TypeTraits.h>

namespace chip {
namespace app {
CHIP_ERROR SubscribeResponder::Init(Messaging::ExchangeManager * apExchangeMgr, InteractionModelDelegate * apDelegate, Messaging::ExchangeContext * apExchangeContext)
{
    VerifyOrReturnError(apDelegate != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    mpExchangeMgr = apExchangeMgr,
    mSubscriptionId = 0;
    mFinalSyncIntervalSeconds = 0;
    return ReadHandler::Init(apDelegate, apExchangeContext);
}

void SubscribeResponder::Shutdown()
{
    mSubscriptionId = 0;
    mFinalSyncIntervalSeconds = 0;
    InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->CancelTimer(OnSubscribeTimerCallback, this);
    ReadHandler::Shutdown();
}

CHIP_ERROR SubscribeResponder::OnSubscribeRequest(Messaging::ExchangeContext * apExchangeContext, System::PacketBufferHandle && aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    System::PacketBufferHandle response;

    mpExchangeCtx = apExchangeContext;
    err           = ProcessSubscribeRequest(std::move(aPayload));

    if (err != CHIP_NO_ERROR)
    {
        ChipLogFunctError(err);
        mpExchangeCtx = nullptr;
        Shutdown();
    }

    return err;
}

CHIP_ERROR SubscribeResponder::SendSubscribeResponse()
{
    SubscribeResponse::Builder response;
    System::PacketBufferTLVWriter writer;
    System::PacketBufferHandle packet = System::PacketBufferHandle::New(chip::app::kMaxSecureSduLengthBytes);
    VerifyOrReturnLogError(!packet.IsNull(), CHIP_ERROR_NO_MEMORY);

    writer.Init(std::move(packet));
    ReturnLogErrorOnFailure(response.Init(&writer));
    response.SubscriptionId(mSubscriptionId).FinalSyncIntervalSeconds(mFinalSyncIntervalSeconds).EndOfSubscribeResponse();
    ReturnLogErrorOnFailure(response.GetError());

    ReturnLogErrorOnFailure(writer.Finalize(&packet));
    VerifyOrReturnLogError(mpExchangeCtx != nullptr, CHIP_ERROR_INCORRECT_STATE);

    ReturnLogErrorOnFailure(mpExchangeCtx->SendMessage(Protocols::InteractionModel::MsgType::SubscribeResponse, std::move(packet), Messaging::SendFlags(Messaging::SendMessageFlags::kExpectResponse)));
    MoveToState(HandlerState::Subscribing);
    return CHIP_NO_ERROR;
}

CHIP_ERROR SubscribeResponder::ProcessSubscribeRequest(System::PacketBufferHandle && aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    System::PacketBufferTLVReader reader;
    uint16_t minIntervalSeconds = 0;
    uint16_t maxIntervalSeconds = 0;
    SubscribeRequest::Parser subscribeRequestParser;
    EventPathList::Parser eventPathListParser;
    AttributePathList::Parser attributePathListParser;

    reader.Init(std::move(aPayload));

    err = reader.Next();
    SuccessOrExit(err);

    err = subscribeRequestParser.Init(reader);
    SuccessOrExit(err);
#if CHIP_CONFIG_IM_ENABLE_SCHEMA_CHECK
    err = subscribeRequestParser.CheckSchemaValidity();
SuccessOrExit(err);
#endif

    err = subscribeRequestParser.GetAttributePathList(&attributePathListParser);
    if (err == CHIP_END_OF_TLV)
    {
        err = CHIP_NO_ERROR;
    }
    else
    {
        SuccessOrExit(err);
        err = ProcessAttributePathList(attributePathListParser);
    }
    SuccessOrExit(err);

    err = subscribeRequestParser.GetEventPathList(&eventPathListParser);
    if (err == CHIP_END_OF_TLV)
    {
        err = CHIP_NO_ERROR;
    }
    else
    {
        SuccessOrExit(err);
        err = ProcessEventPathList(eventPathListParser);
    }
    SuccessOrExit(err);

    err = subscribeRequestParser.GetMinIntervalSeconds(&minIntervalSeconds);
    SuccessOrExit(err);

    err = subscribeRequestParser.GetMaxIntervalSeconds(&maxIntervalSeconds);
    SuccessOrExit(err);

    err = mpDelegate->GetSubscribeFinalSyncInterval(minIntervalSeconds, maxIntervalSeconds, mFinalSyncIntervalSeconds);
    SuccessOrExit(err);

    // TODO: Use GetSecureRandomData to generate subscription id
    //err = Platform::Security::GetSecureRandomData((uint8_t *) &mSubscriptionId, sizeof(mSubscriptionId));
    //SuccessOrExit(err);
    mSubscriptionId = (uint64_t)rand();

    err = SendSubscribeResponse();
    SuccessOrExit(err);

    // mpExchangeCtx can be null here due to
    // https://github.com/project-chip/connectedhomeip/issues/8031
    if (mpExchangeCtx)
    {
        mpExchangeCtx->WillSendMessage();
    }

    // There must be no code after the WillSendMessage() call that can cause
    // this method to return a failure.

exit:
    ChipLogFunctError(err);
    return err;
}

void SubscribeResponder::OnSubscribeTimerCallback(System::Layer * apSystemLayer, void * apAppState)
{
    InteractionModelEngine::GetInstance()->GetReportingEngine().ScheduleRun();
}

CHIP_ERROR SubscribeResponder::RefreshSubscribeSyncTimer(void)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    VerifyOrReturnLogError(IsReporting(), CHIP_ERROR_INCORRECT_STATE);
    // Calculate margin to reserve for MRP activity, so we send out sync report earlier
    // TODO: calculate the margin Sec for MRP retry activity
    uint16_t marginSeconds = (3 + 1) * 2; //(CRMP Retry+1)*CRMP RETRANS_TIMEOUT
    uint16_t timeoutSeconds = mFinalSyncIntervalSeconds;
    if (marginSeconds < timeoutSeconds)
    {
        timeoutSeconds = mFinalSyncIntervalSeconds - marginSeconds;
    }
    else
    {
        timeoutSeconds = mFinalSyncIntervalSeconds - 1; // Make sure client's sync internal is larger than the time for sync report
    }
    InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->CancelTimer(OnSubscribeTimerCallback, this);

    err = InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->StartTimer(
            timeoutSeconds * 1000, OnSubscribeTimerCallback, this);
    ReturnLogErrorOnFailure(err);

    return err;
}

CHIP_ERROR SubscribeResponder::SendReportData(System::PacketBufferHandle && aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    VerifyOrReturnLogError(IsReportable(), CHIP_ERROR_INCORRECT_STATE);
    if (IsInitialReport())
    {
        mSecureSession = mpExchangeCtx->GetSecureSession();
    }
    else
    {
        mpExchangeCtx = mpExchangeMgr->NewContext(mSecureSession, this);
        mpExchangeCtx->SetResponseTimeout(kImMessageTimeoutMsec);
    }

    err = ReadHandler::SendReportData(std::move(aPayload));
    if (err == CHIP_NO_ERROR)
    {
        RefreshSubscribeSyncTimer();
    }
    return err;
}
} // namespace app
} // namespace chip
