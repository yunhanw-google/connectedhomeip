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
#include <app/ReadClient.h>
#include <app/MessageDef/SubscribeRequest.h>
#include <app/MessageDef/SubscribeResponse.h>
#include <protocols/secure_channel/StatusReport.h>

namespace chip {
namespace app {

CHIP_ERROR ReadClient::Init(Messaging::ExchangeManager * apExchangeMgr, InteractionModelDelegate * apDelegate,
                            uint64_t aAppIdentifier)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    // Error if already initialized.
    VerifyOrExit(apExchangeMgr != nullptr, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpExchangeMgr == nullptr, err = CHIP_ERROR_INCORRECT_STATE);

    mpExchangeMgr  = apExchangeMgr;
    mpDelegate     = apDelegate;
    mState         = ClientState::Initialized;
    mAppIdentifier = aAppIdentifier;

    mRetryCounter = 0;
    mFinalSyncIntervalSeconds = 0;
    mSubscriptionId = 0;
    mEnableResubscribe = false;

    AbortExistingExchangeContext();

exit:
    ChipLogFunctError(err);
    return err;
}

void ReadClient::Shutdown()
{
    AbortExistingExchangeContext();
    ShutdownInternal();
}

void ReadClient::ShutdownInternal()
{
    if (IsSubscription())
    {
        mRetryCounter = 0;
        mFinalSyncIntervalSeconds = 0;
        mSubscriptionId = 0;
        mEnableResubscribe = false;
        CancelLivenessCheckTimer();

        if (mSubscribePrepareParams.mpAttributePathParamsList != nullptr) {
            delete mSubscribePrepareParams.mpAttributePathParamsList;
            mSubscribePrepareParams.mpAttributePathParamsList = nullptr;
        }

        if (mSubscribePrepareParams.mpEventPathParamsList != nullptr) {
            delete mSubscribePrepareParams.mpEventPathParamsList;
            mSubscribePrepareParams.mpEventPathParamsList = nullptr;
        }
        mSubscription = false;
    }
    mpExchangeMgr = nullptr;
    mpExchangeCtx = nullptr;
    mpDelegate    = nullptr;
    MoveToState(ClientState::Uninitialized);
}

const char * ReadClient::GetStateStr() const
{
#if CHIP_DETAIL_LOGGING
    switch (mState)
    {
    case ClientState::Uninitialized:
        return "UNINIT";
    case ClientState::Initialized:
        return "INIT";
    case ClientState::AwaitingResponse:
        return "AwaitingResponse";
    case ClientState::Subscribing:
        return "Subscribing";
    case ClientState::SubscriptionIdle:
        return "SubscriptionIdle";
    }
#endif // CHIP_DETAIL_LOGGING
    return "N/A";
}

void ReadClient::MoveToState(const ClientState aTargetState)
{
    mState = aTargetState;
    ChipLogDetail(DataManagement, "Client[%u] moving to [%s]", InteractionModelEngine::GetInstance()->GetReadClientArrayIndex(this),
                  GetStateStr());
}

CHIP_ERROR ReadClient::SendReadRequest(NodeId aNodeId, FabricIndex aFabricIndex, SecureSessionHandle * apSecureSession,
                                       EventPathParams * apEventPathParamsList, size_t aEventPathParamsListSize,
                                       AttributePathParams * apAttributePathParamsList, size_t aAttributePathParamsListSize,
                                       EventNumber aEventNumber)
{
    // TODO: SendRequest parameter is too long, need to have the structure to represent it
    CHIP_ERROR err = CHIP_NO_ERROR;
    System::PacketBufferHandle msgBuf;
    ChipLogDetail(DataManagement, "%s: Client[%u] [%5.5s]", __func__,
                  InteractionModelEngine::GetInstance()->GetReadClientArrayIndex(this), GetStateStr());
    VerifyOrExit(ClientState::Initialized == mState, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpDelegate != nullptr, err = CHIP_ERROR_INCORRECT_STATE);

    // Discard any existing exchange context. Effectively we can only have one exchange per ReadClient
    // at any one time.
    AbortExistingExchangeContext();

    {
        System::PacketBufferTLVWriter writer;
        ReadRequest::Builder request;

        msgBuf = System::PacketBufferHandle::New(kMaxSecureSduLengthBytes);
        VerifyOrExit(!msgBuf.IsNull(), err = CHIP_ERROR_NO_MEMORY);

        writer.Init(std::move(msgBuf));

        err = request.Init(&writer);
        SuccessOrExit(err);

        if (aEventPathParamsListSize != 0 && apEventPathParamsList != nullptr)
        {
            EventPathList::Builder & eventPathListBuilder = request.CreateEventPathListBuilder();
            SuccessOrExit(err = eventPathListBuilder.GetError());
            err = GenerateEventPathList(eventPathListBuilder, apEventPathParamsList, aEventPathParamsListSize);
            SuccessOrExit(err);
            if (aEventNumber != 0)
            {
                // EventNumber is optional
                request.EventNumber(aEventNumber);
            }
        }

        if (aAttributePathParamsListSize != 0 && apAttributePathParamsList != nullptr)
        {
            AttributePathList::Builder attributePathListBuilder = request.CreateAttributePathListBuilder();
            SuccessOrExit(err = attributePathListBuilder.GetError());
            err = GenerateAttributePathList(attributePathListBuilder, apAttributePathParamsList, aAttributePathParamsListSize);
            SuccessOrExit(err);
        }

        request.EndOfReadRequest();
        SuccessOrExit(err = request.GetError());

        err = writer.Finalize(&msgBuf);
        SuccessOrExit(err);
    }

    if (apSecureSession != nullptr)
    {
        mpExchangeCtx = mpExchangeMgr->NewContext(*apSecureSession, this);
    }
    else
    {
        mpExchangeCtx = mpExchangeMgr->NewContext({ aNodeId, 0, aFabricIndex }, this);
    }
    VerifyOrExit(mpExchangeCtx != nullptr, err = CHIP_ERROR_NO_MEMORY);
    mpExchangeCtx->SetResponseTimeout(kImMessageTimeoutMsec);

    err = mpExchangeCtx->SendMessage(Protocols::InteractionModel::MsgType::ReadRequest, std::move(msgBuf),
                                     Messaging::SendFlags(Messaging::SendMessageFlags::kExpectResponse));
    SuccessOrExit(err);
    MoveToState(ClientState::AwaitingResponse);

exit:
    ChipLogFunctError(err);

    if (err != CHIP_NO_ERROR)
    {
        AbortExistingExchangeContext();
    }

    return err;
}

CHIP_ERROR ReadClient::SendStatusReport(CHIP_ERROR aError, bool aExpectResponse)
{
    Protocols::SecureChannel::GeneralStatusCode generalCode = Protocols::SecureChannel::GeneralStatusCode::kSuccess;
    uint32_t protocolId           = Protocols::InteractionModel::Id.ToFullyQualifiedSpecForm();
    uint16_t protocolCode         = to_underlying(Protocols::InteractionModel::ProtocolCode::Success);
    VerifyOrReturnLogError(mpExchangeCtx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    // Need to add chunk support for multiple report
    if (aError != CHIP_NO_ERROR)
    {
        generalCode = Protocols::SecureChannel::GeneralStatusCode::kFailure;
        protocolCode = to_underlying(Protocols::InteractionModel::ProtocolCode::InvalidSubscription);
    }

    ChipLogProgress(DataManagement, "SendStatusReport with error %s", ErrorStr(aError));
    Protocols::SecureChannel::StatusReport report(generalCode, protocolId, protocolCode);

    Encoding::LittleEndian::PacketBufferWriter buf(System::PacketBufferHandle::New(kMaxSecureSduLengthBytes));
    report.WriteToBuffer(buf);
    System::PacketBufferHandle msgBuf = buf.Finalize();
    VerifyOrReturnLogError(!msgBuf.IsNull(), CHIP_ERROR_NO_MEMORY);

    if (aExpectResponse)
    {
        ReturnLogErrorOnFailure(
                mpExchangeCtx->SendMessage(Protocols::SecureChannel::MsgType::StatusReport, std::move(msgBuf), Messaging::SendFlags(Messaging::SendMessageFlags::kExpectResponse)));
    }
    else
    {
        ReturnLogErrorOnFailure(
                mpExchangeCtx->SendMessage(Protocols::SecureChannel::MsgType::StatusReport, std::move(msgBuf)));
    }

    if (IsSubscribing())
    {
        MoveToState(ClientState::SubscriptionIdle);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ReadClient::GenerateEventPathList(EventPathList::Builder & aEventPathListBuilder, EventPathParams * apEventPathParamsList,
                                             size_t aEventPathParamsListSize)
{
    CHIP_ERROR err                                = CHIP_NO_ERROR;

    for (size_t eventIndex = 0; eventIndex < aEventPathParamsListSize; ++eventIndex)
    {
        EventPath::Builder eventPathBuilder = aEventPathListBuilder.CreateEventPathBuilder();
        EventPathParams eventPath           = apEventPathParamsList[eventIndex];
        eventPathBuilder.NodeId(eventPath.mNodeId)
            .EventId(eventPath.mEventId)
            .EndpointId(eventPath.mEndpointId)
            .ClusterId(eventPath.mClusterId)
            .EndOfEventPath();
        SuccessOrExit(err = eventPathBuilder.GetError());
    }

    aEventPathListBuilder.EndOfEventPathList();
    SuccessOrExit(err = aEventPathListBuilder.GetError());

exit:
    ChipLogFunctError(err);
    return err;
}

CHIP_ERROR ReadClient::GenerateAttributePathList(AttributePathList::Builder & aAttributePathListBuilder, AttributePathParams * apAttributePathParamsList,
                                                 size_t aAttributePathParamsListSize)
{
    for (size_t index = 0; index < aAttributePathParamsListSize; index++)
    {
        AttributePath::Builder attributePathBuilder = aAttributePathListBuilder.CreateAttributePathBuilder();
        attributePathBuilder.NodeId(apAttributePathParamsList[index].mNodeId)
            .EndpointId(apAttributePathParamsList[index].mEndpointId)
            .ClusterId(apAttributePathParamsList[index].mClusterId);
        if (apAttributePathParamsList[index].mFlags.Has(AttributePathParams::Flags::kFieldIdValid))
        {
            attributePathBuilder.FieldId(apAttributePathParamsList[index].mFieldId);
        }

        if (apAttributePathParamsList[index].mFlags.Has(AttributePathParams::Flags::kListIndexValid))
        {
            VerifyOrReturnError(apAttributePathParamsList[index].mFlags.Has(AttributePathParams::Flags::kFieldIdValid),
                                CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);
            attributePathBuilder.ListIndex(apAttributePathParamsList[index].mListIndex);
        }

        attributePathBuilder.EndOfAttributePath();
        ReturnErrorOnFailure(attributePathBuilder.GetError());
    }
    aAttributePathListBuilder.EndOfAttributePathList();
    return aAttributePathListBuilder.GetError();
}


CHIP_ERROR ReadClient::OnMessageReceived(Messaging::ExchangeContext * apExchangeContext, const PacketHeader & aPacketHeader,
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
        //  on server, please compare exchange context for first status report
        err = ProcessReportData(std::move(aPayload));
    }
    else
    {
        err = CHIP_ERROR_INVALID_MESSAGE_TYPE;
    }

exit:
    ChipLogFunctError(err);
    if (err != CHIP_NO_ERROR || !IsSubscription())
    {
        ShutdownInternal();
    }

    return err;
}

CHIP_ERROR ReadClient::AbortExistingExchangeContext()
{
    if (mpExchangeCtx != nullptr)
    {
        mpExchangeCtx->Abort();
        mpExchangeCtx = nullptr;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ReadClient::ProcessReportData(System::PacketBufferHandle && aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    ReportData::Parser report;

    bool isEventListPresent         = false;
    bool isAttributeDataListPresent = false;
    bool suppressResponse           = false;
    bool moreChunkedMessages        = false;
    uint64_t subscriptionId         = 0;
    EventList::Parser eventList;
    AttributeDataList::Parser attributeDataList;
    System::PacketBufferTLVReader reader;

    reader.Init(std::move(aPayload));
    reader.Next();

    err = report.Init(reader);
    SuccessOrExit(err);

#if CHIP_CONFIG_IM_ENABLE_SCHEMA_CHECK
    err = report.CheckSchemaValidity();
    SuccessOrExit(err);
#endif

    err = report.GetSuppressResponse(&suppressResponse);
    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);

    err = report.GetSubscriptionId(&subscriptionId);
    if (CHIP_NO_ERROR == err)
    {
        if (!IsValidSubscription(subscriptionId))
        {
            err = CHIP_ERROR_INVALID_ARGUMENT;
        }
    }
    else if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);

    err = report.GetMoreChunkedMessages(&moreChunkedMessages);
    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);

    err                = report.GetEventDataList(&eventList);
    isEventListPresent = (err == CHIP_NO_ERROR);
    if (err == CHIP_END_OF_TLV)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);

    if (isEventListPresent && nullptr != mpDelegate)
    {
        chip::TLV::TLVReader eventListReader;
        eventList.GetReader(&eventListReader);
        err = mpDelegate->EventStreamReceived(mpExchangeCtx, &eventListReader);
        SuccessOrExit(err);
    }

    err                        = report.GetAttributeDataList(&attributeDataList);
    isAttributeDataListPresent = (err == CHIP_NO_ERROR);
    if (err == CHIP_END_OF_TLV)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);
    if (isAttributeDataListPresent && nullptr != mpDelegate && !moreChunkedMessages)
    {
        chip::TLV::TLVReader attributeDataListReader;
        attributeDataList.GetReader(&attributeDataListReader);
        err = ProcessAttributeDataList(attributeDataListReader);
        SuccessOrExit(err);
    }

    //TODO: Add suppress response support

exit:
    err = SendStatusReport(err, false);
    if (err != CHIP_NO_ERROR)
    {
        mpDelegate->ReportError(this, err);
    }
    else
    {
        mpDelegate->ReportProcessed(this);
        err = RefreshLivenessCheckTimer();
    }
    return err;
}

void ReadClient::OnResponseTimeout(Messaging::ExchangeContext * apExchangeContext)
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
        //rename AwaitingResponse
        else if (ClientState::AwaitingResponse  == mState)
        {
            mpDelegate->ReportError(this, CHIP_ERROR_TIMEOUT);
            ShutdownInternal();
        }
    }

    if (IsSubscription())
    {
        InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->ScheduleWork(
                OnRetryCallback, this);
    }
}

CHIP_ERROR ReadClient::ProcessAttributeDataList(TLV::TLVReader & aAttributeDataListReader)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    while (CHIP_NO_ERROR == (err = aAttributeDataListReader.Next()))
    {
        chip::TLV::TLVReader dataReader;
        AttributeDataElement::Parser element;
        AttributePath::Parser attributePathParser;
        ClusterInfo clusterInfo;
        uint16_t statusU16 = 0;
        auto status        = Protocols::InteractionModel::ProtocolCode::Success;

        TLV::TLVReader reader = aAttributeDataListReader;
        err                   = element.Init(reader);
        SuccessOrExit(err);

        err = element.GetAttributePath(&attributePathParser);
        SuccessOrExit(err);

        err = attributePathParser.GetNodeId(&(clusterInfo.mNodeId));
        SuccessOrExit(err);

        err = attributePathParser.GetEndpointId(&(clusterInfo.mEndpointId));
        SuccessOrExit(err);

        err = attributePathParser.GetClusterId(&(clusterInfo.mClusterId));
        SuccessOrExit(err);

        err = attributePathParser.GetFieldId(&(clusterInfo.mFieldId));
        if (CHIP_NO_ERROR == err)
        {
            clusterInfo.mFlags.Set(ClusterInfo::Flags::kFieldIdValid);
        }
        else if (CHIP_END_OF_TLV == err)
        {
            err = CHIP_NO_ERROR;
        }
        SuccessOrExit(err);

        err = attributePathParser.GetListIndex(&(clusterInfo.mListIndex));
        if (CHIP_NO_ERROR == err)
        {
            VerifyOrExit(clusterInfo.mFlags.Has(ClusterInfo::Flags::kFieldIdValid), err = CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);
            clusterInfo.mFlags.Set(ClusterInfo::Flags::kListIndexValid);
        }
        else if (CHIP_END_OF_TLV == err)
        {
            err = CHIP_NO_ERROR;
        }
        SuccessOrExit(err);

        err = element.GetData(&dataReader);
        if (err == CHIP_END_OF_TLV)
        {
            // The spec requires that one of data or status code must exist, thus failure to read data and status code means we
            // received malformed data from server.
            SuccessOrExit(err = element.GetStatus(&statusU16));
            status = static_cast<Protocols::InteractionModel::ProtocolCode>(statusU16);
        }
        else if (err != CHIP_NO_ERROR)
        {
            ExitNow();
        }
        mpDelegate->OnReportData(this, clusterInfo, &dataReader, status);
    }

    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }

exit:
    ChipLogFunctError(err);
    return err;
}

CHIP_ERROR ReadClient::RefreshLivenessCheckTimer()
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

void ReadClient::CancelLivenessCheckTimer()
{
    InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->CancelTimer(OnRetryCallback, this);
}

void ReadClient::OnRetryCallback(System::Layer * apSystemLayer, void * apAppState)
{
    CHIP_ERROR err                   = CHIP_NO_ERROR;
    uint16_t retryTimeSec = 0;
    ReadClient * const client = reinterpret_cast<ReadClient *>(apAppState);
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

void ReadClient::OnReSubscribeTimerCallback(System::Layer * apSystemLayer, void * apAppState)
{
    ReadClient * const client = reinterpret_cast<ReadClient *>(apAppState);
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
void ReadClient::ResetResubscribe()
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

void ReadClient::CancelResubscribe()
{
    InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionMgr()->SystemLayer()->CancelTimer(OnReSubscribeTimerCallback, this);
}

CHIP_ERROR ReadClient::ProcessSubscribeResponse(System::PacketBufferHandle && aPayload)
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


CHIP_ERROR ReadClient::SendSubscribeRequest(SubscribePrepareParams & aSubscribePrepareParams)
{
    mSubscription = true;
    mSubscribePrepareParams = aSubscribePrepareParams;
    aSubscribePrepareParams.mpEventPathParamsList = nullptr;
    aSubscribePrepareParams.mEventPathParamsListSize   = 0;
    aSubscribePrepareParams.mpAttributePathParamsList  = nullptr;
    aSubscribePrepareParams.mAttributePathParamsListSize = 0;
    return SendSubscribeRequestInternal();
}

CHIP_ERROR ReadClient::SendSubscribeRequestInternal()
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


}; // namespace app
}; // namespace chip
