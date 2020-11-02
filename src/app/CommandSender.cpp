/*
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2017-2019 Nest Labs, Inc.
 *    Copyright (c) 2016-2017 Nest Labs, Inc.
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
 *      This file defines invoke command sender for CHIP
 *      Interaction Management profile.
 *
 */

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif // __STDC_FORMAT_MACROS

#include "CommandSender.h"

namespace chip {
namespace app {

using namespace chip::TLV;

CHIP_ERROR InvokeCommandRequestSender::Init(const EventCallback aEventCallback, void * const aAppState)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    if (aEventCallback)
    {
        InEventParam inParam;
        OutEventParam outParam;

        mEventCallback = aEventCallback;
        mAppState = aAppState;

        inParam.Clear();
        outParam.Clear();

        // Test the application to ensure it's calling the default handler appropriately.
        aEventCallback(aAppState, kEvent_DefaultCheck, inParam, outParam);
        VerifyOrExit(outParam.defaultHandlerCalled, err = CHIP_ERROR_DEFAULT_EVENT_HANDLER_NOT_CALLED);
    }
    else
    {
        mEventCallback = DefaultEventHandler;
    }

    if (mpRequestBuf == NULL)
    {
        //TODO: Calculate the packet buffer size
        mpRequestBuf = PacketBuffer::New(2048);
        VerifyOrExit(mpRequestBuf != NULL, err = CHIP_ERROR_NO_MEMORY);
    }

    mReqWriter.Init(mpRequestBuf);
    err = mRequest.Init(&mReqWriter);
    SuccessOrExit(err);

    mCommandList = mReqWriter.CreateCommandListBuilder()
    _this->MoveToState(kState_Initialized);

exit:
    ChipLogFunctError(err);

    return err;
}

void InvokeCommandRequestSender::FlushExistingExchangeContext(const bool aAbortNow)
{
    if (NULL != mEC)
    {
        mEC->AppState          = NULL;
        mEC->OnMessageReceived = NULL;
        mEC->OnResponseTimeout = NULL;
        mEC->OnSendError       = NULL;
        mEC->OnAckRcvd         = NULL;

        if (aAbortNow)
        {
            mEC->Abort();
        }
        else
        {
            mEC->Close();
        }

        mEC = NULL;
    }
}

void InvokeCommandRequestSender::Reset(bool aAbort)
{
    if (kState_Uninitialized != mState)
    {
        FlushExistingExchangeContext(aAbort);

        if (mpRequestBuf)
        {
            PacketBuffer::Free(mpRequestBuf);
            mpRequestBuf = NULL;
        }

        if (kState_Initialized != mState)
        {
            MoveToState(kState_Initialized);
        }
    }
}

/**
 * Reset command client to initialized status. clear the buffer
 *
 * @retval #WEAVE_NO_ERROR On success.
 */
void InvokeCommandRequestSender::Reset(void)
{
    bool abort = true;

    ChipLogDetail(DataManagement, "InvokeCommandRequestSender::CancelCommand");

    Reset(abort);
}

/**
 * Release binding for the update. Should only be called once.
 *
 * @retval #CHIP_NO_ERROR On success.
 */
CHIP_ERROR InvokeCommandRequestSender::Shutdown(void)
{
    if (kState_Uninitialized != mState)
    {
        CancelCommand();

        mEventCallback = NULL;
        mpAppState      = NULL;
        MoveToState(kState_Uninitialized);
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR InvokeCommandRequestSender::AddCommandInList(PacketBuffer *apCommandBuf, uint16_t aEndpintId, uint32_t aGroupId, uint32_t aClusterId, uint16_t aCommandId, uint8_t Flags)
{
    SendCommandParams sendCommandParams;

    memset(&sendCommandParams, 0, sizeof(sendCommandParams));

    sendCommandParams.EndpointId = aEndpintId;
    sendCommandParams.GroupId = aGroupId;
    sendCommandParams.ClusterId = aClusterId;
    sendCommandParams.CommandId = aCommandId;
    sendCommandParams.Flags = aFlag;
    return AddCommandInList(aPayload, sendParams);
}

CHIP_ERROR InvokeCommandRequestSender::AddCommandinList(PacketBuffer *apCommandBuf, SendCommandParams &aSendCommandParams)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    const uint8_t *apCommandData;
    uint16_t apCommandLen;

    apCommandData = apCommandBuf->Start() + kMaxInvokeCommandRequestHeaderSize;
    apCommandLen = apCommandBuf->DataLength();
    if (apCommandLen > 0)
    {
        VerifyOrExit(apCommandLen > 2, err = WEAVE_ERROR_INVALID_ARGUMENT);
        VerifyOrExit(apCommandData[0] == kTLVElementType_Structure, err = WEAVE_ERROR_INVALID_ARGUMENT);
        VerifyOrExit(apCommandData[apCommandLen - 1] == kTLVElementType_EndOfContainer, err = WEAVE_ERROR_INVALID_ARGUMENT);

        // Strip off the anonymous structure supplied by the application, leaving just the raw
        // contents.
        apCommandData += 1;
        apCommandLen -= 1;
    }

    CommandDataElement::Builder &commandDataElement = mCommandList.CreateCommandDataElementBuilder();
    CommandPath::Builder &commandPath CommandDataElement.CreateCommandPathBuilder();
    if (aSendParams.Flags & kCommandPathFlag_EndpointId)
    {
        commandPath.EndpointId(aSendCommandParams.EndpointId);
    }

    if (aSendParams.Flags & kCommandPathFlag_GroupId)
    {
        commandPath.GroupId(aSendCommandParams.GroupId);
    }

    commandPath.ClusterId(aSendCommandParams.ClusterId).CommandId(aSendCommandParams.CommandId).EndOfCommandPath();

    err = commandPath.GetError();
    SuccessOrExit(err);

    if (apCommandLen > 0)
    {
        // Copy the application data into a new TLV structure field contained with the
        // request structure.  NOTE: The TLV writer will take care of moving the app data
        // to the correct location within the buffer.
        err = reqWriter.PutPreEncodedContainer(ContextTag(CustomCommand::kCsTag_Data), kTLVType_Structure, apCommandData, apCommandLen);
        SuccessOrExit(err);
    }
    commandDataElement.EndOfCommandDataElement();

    err = commandDataElement.GetError();
    SuccessOrExit(err);

    MoveToState(kState_AddCommandInList);

exit:
    if (err == CHIP_NO_ERROR && apCommandBuf != NULL)
    {
        PacketBuffer::Free(apCommandBuf);
        apCommandBuf = NULL;
    }

    ChipLogFunctError(err);
    return err;
}

CHIP_ERROR InvokeCommandRequestSender::Flush()
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    const uint8_t *appReqData;
    uint16_t appReqDataLen;
    uint16_t sendFlags = 0;
    TLVWriter reqWriter;
    uint8_t msgType = kMsgType_InvokeCommandRequest;

    mRequest.EndOfInvokeCommandRequest();
    err = request.GetError();
    SuccessOrExit(err);

    err = reqWriter.Finalize();
    SuccessOrExit(err);

    FlushExistingExchangeContext();
    err = NewExchangeContext(mEC);
    SuccessOrExit(err);

    mEC->AppState = this;
    mEC->OnMessageReceived = OnMessageReceived;
    mEC->OnResponseTimeout = OnResponseTimeout;
    mEC->OnSendError = OnSendError;
    mEC->OnAckRcvd = NULL;

    sendFlags |= ExchangeContext::kSendFlag_ExpectResponse;

    mFlags = aSendParams.Flags;

    err = mEC->SendMessage(kChipProtocl_InteractionModel, msgType, aRequestBuf, sendFlags);
    aRequestBuf = NULL;

    MoveToState(kState_AwaitingResponse);
exit:
    if ((err != CHIP_NO_ERROR) && (mpRequestBuf != NULL))
    {
        PacketBuffer::Free(mpRequestBuf);
        mpRequestBuf = NULL;
    }

    ChipLogFunctError(err);
    return err;
}

void InvokeCommandRequestSender::DefaultEventHandler(void *aAppState, EventType aEvent, const InEventParam& aInParam, OutEventParam& aOutParam)
{
    // No actions required for current implementation
    aOutParam.defaultHandlerCalled = true;
}

void InvokeCommandRequestSender::OnSendError(ExchangeContext *aEC, CHIP_ERROR sendError, void *aMsgCtxt)
{
    InvokeCommandRequestSender *_this = static_cast<InvokeCommandRequestSender *>(aEC->AppState);
    InEventParam inParam;
    OutEventParam outParam;
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(_this != NULL, err = CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrExit(_this->mEC != NULL, err = CHIP_ERROR_INCORRECT_STATE);

    inParam.CommunicationError.error = sendError;
    _this->mEventCallback(_this->mAppState, kEvent_CommunicationError, inParam, outParam);

    // After error, let's close out the exchange
    _this->Reset();
exit:

    ChipLogFunctError(err);
    return;
}

void InvokeCommandRequestSender::OnResponseTimeout(ExchangeContext *aEC)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    InvokeCommandRequestSender *_this = static_cast<InvokeCommandRequestSender *>(aEC->AppState);
    InEventParam inEventParam;
    OutEventParam outEventParam;

    VerifyOrExit(_this, err = CHIP_ERROR_INVALID_ARGUMENT);

    inEventParam.CommunicationError.error = CHIP_ERROR_TIMEOUT;
    _this->mEventCallback(_this->mAppState, kEvent_CommunicationError, inEventParam, outEventParam);

    _this->Reset();
exit:
    return;
}

void InvokeCommandRequestSender::OnMessageReceived(ExchangeContext *aEC, const IPPacketInfo *aPktInfo, const WeaveMessageInfo *aMsgInfo, uint32_t aProfileId, uint8_t aMsgType, PacketBuffer *aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    InvokeCommandRequestSender *_this = static_cast<InvokeCommandRequestSender *>(aEC->AppState);
    StatusReporting::StatusReport status;
    InEventParam inEventParam;
    OutEventParam outEventParam;

    VerifyOrExit(aEC == _this->mEC, err = CHIP_ERROR_INCORRECT_STATE);

    // One way commands shouldn't get a response back
    VerifyOrExit(!(_this->mFlags & kCommandFlag_IsOneWay), err = CHIP_ERROR_INCORRECT_STATE);

    if (aProtocolId == kWeaveProfile_Common && aMsgType == Common::kMsgType_StatusReport)
    {
        err = StatusReporting::StatusReport::parse(aPayload, status);
        SuccessOrExit(err);

        inEventParam.StatusReportReceived.statusReport = &status;

        // Tell the application about the receipt of this status report.
        _this->mEventCallback(_this->mAppState, kEvent_StatusReportReceived, inEventParam, outEventParam);

        _this->Close();
    }
    else if (aProtocolId == kChipProtocol_Interation_Model&& aMsgType == kMsgType_InvokeCommandResponse)
    {
        ChipLogDetail(DataManagement, "Process Invoke Command Response");
    }
    else
    {
        err = CHIP_ERROR_INVALID_PROFILE_ID;
    }

    pCommandSender->MoveToState(kState_Initialized);

exit:
    ChipLogFunctError(err);

    if (err != CHIP_NO_ERROR)
    {
        _this->Close();
    }

    if (aPayload)
    {
        PacketBuffer::Free(aPayload);
    }

    return;
}

#if WEAVE_DETAIL_LOGGING
const char * InvokeCommandRequestSender::GetStateStr() const
{
    switch (mState)
    {
    case kState_Uninitialized:
        return "Uninitialized";

    case kState_Initialized:
        return "Initialized";

    case kState_AddCommandinList:
        return "AddCommandInList";

    case kState_AwaitingResponse:
        return "AwaitingResponse";
    }
    return "N/A";
}
#endif // CHIP_DETAIL_LOGGING

void InvokeCommandRequestSender::MoveToState(const InvokeCommandRequestState aTargetState)
{
    mState = aTargetState;
    ChipLogDetail(DataManagement, "ICR moving to [%10.10s]", GetStateStr());
}

void InvokeCommandRequestSender::ClearState(void)
{
    MoveToState(kState_Uninitialized);
}

}; // namespace app
}; // namespace chip

#endif
