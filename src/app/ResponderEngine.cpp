/*
 *
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
 *      This file implements subscription engine for Weave
 *      Data Management (WDM) profile.
 *
 */

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif // __STDC_FORMAT_MACROS

#include <Weave/Profiles/WeaveProfiles.h>
#include <Weave/Profiles/common/CommonProfile.h>
#include <Weave/Profiles/data-management/Current/WdmManagedNamespace.h>
#include <Weave/Profiles/data-management/DataManagement.h>
#include <Weave/Profiles/data-management/NotificationEngine.h>
#include <Weave/Profiles/status-report/StatusReportProfile.h>
#include <Weave/Profiles/time/WeaveTime.h>
#include <Weave/Support/crypto/WeaveCrypto.h>
#include <Weave/Support/WeaveFaultInjection.h>
#include <SystemLayer/SystemStats.h>

#ifndef WEAVE_WDM_ALIGNED_TYPE
#define WEAVE_WDM_ALIGNED_TYPE(address, type) reinterpret_cast<type *> WEAVE_SYSTEM_ALIGN_SIZE((size_t)(address), 4)
#endif

namespace chip {
namespace app {

ResponderEngine::ResponderEngine() { }

void ResponderEngine::SetEventCallback(void * const aAppState, const EventCallback aEventCallback)
{
    mAppState      = aAppState;
    mEventCallback = aEventCallback;
}

void ResponderEngine::DefaultEventHandler(EventID aEvent, const InEventParam & aInParam, OutEventParam & aOutParam)
{
    IgnoreUnusedVariable(aInParam);
    IgnoreUnusedVariable(aOutParam);

    ChipLogDetail(DataManagement, "%s event: %d", __func__, aEvent);
}

CHIP_ERROR ResponderEngine::Init(chip::ExchangeManager * const apExchangeMgr, void * const aAppState,
                                     const EventCallback aEventCallback)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    mExchangeMgr   = apExchangeMgr;
    mAppState      = aAppState;
    mEventCallback = aEventCallback;
    mLock          = NULL;

    err = mExchangeMgr->RegisterUnsolicitedMessageHandler(chip::Protocol::kChipProtocol_InteractionModel, UnsolicitedMessageHandler, this);
    SuccessOrExit(err);

    err = mReportingEngine.Init();
    SuccessOrExit(err);

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        mHandlers[i].InitAsFree();
    }

    // erase everything
    DisablePublisher();

    mNumTraitInfosInPool = 0;

exit:
    ChipLogFunctError(err);

    return err;
}

#if CHIP_DETAIL_LOGGING
void ResponderEngine::LogSubscriptionFreed(void) const
{
    // Report number of clients and handlers that are still allocated
    uint32_t countAllocatedClients  = 0;
    uint32_t countAllocatedHandlers = 0;

    for (int i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if (SubscriptionClient::kState_Free != mClients[i].mCurrentState)
        {
            ++countAllocatedClients;
        }
    }

    for (int i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        if (SubscriptionHandler::kState_Free != mHandlers[i].mCurrentState)
        {
            ++countAllocatedHandlers;
        }
    }

    ChipLogDetail(DataManagement, "Allocated clients: %" PRIu32 ". Allocated handlers: %" PRIu32 ".", countAllocatedClients,
                   countAllocatedHandlers);
}
#endif // #if WEAVE_DETAIL_LOGGING

#if WDM_ENABLE_SUBSCRIPTION_CLIENT

uint16_t ResponderEngine::GetClientId(const SubscriptionClient * const apClient) const
{
    return static_cast<uint16_t>(apClient - mClients);
}

CHIP_ERROR ResponderEngine::NewClient(SubscriptionClient ** const appClient, Binding * const apBinding, void * const apAppState,
                                          SubscriptionClient::EventCallback const aEventCallback,
                                          const TraitCatalogBase<TraitDataSink> * const apCatalog,
                                          const uint32_t aInactivityTimeoutDuringSubscribingMsec, IWeaveWDMMutex * aUpdateMutex)
{
    CHIP_ERROR err = CHIP_ERROR_NO_MEMORY;

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
    uint32_t maxSize = WDM_MAX_UPDATE_SIZE;
#else
    VerifyOrExit(aUpdateMutex == NULL, err = CHIP_ERROR_INVALID_ARGUMENT);
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE

    *appClient = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if (SubscriptionClient::kState_Free == mClients[i].mCurrentState)
        {
            *appClient = &mClients[i];
            err =
                (*appClient)
                    ->Init(apBinding, apAppState, aEventCallback, apCatalog, aInactivityTimeoutDuringSubscribingMsec, aUpdateMutex);

            if (CHIP_NO_ERROR != err)
            {
                *appClient = NULL;
                ExitNow();
            }
#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
            mClients[i].SetMaxUpdateSize(maxSize);
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE
            SYSTEM_STATS_INCREMENT(chip::System::Stats::kWDM_NumSubscriptionClients);
            break;
        }
    }

exit:

    return err;
}

CHIP_ERROR ResponderEngine::NewClient(SubscriptionClient ** const appClient, Binding * const apBinding, void * const apAppState,
                                          SubscriptionClient::EventCallback const aEventCallback,
                                          const TraitCatalogBase<TraitDataSink> * const apCatalog,
                                          const uint32_t aInactivityTimeoutDuringSubscribingMsec)
{
    return NewClient(appClient, apBinding, apAppState, aEventCallback, apCatalog, aInactivityTimeoutDuringSubscribingMsec, NULL);
}

/**
 * Reply to a request with a StatuReport message.
 *
 * @param[in]   aEC         Pointer to the ExchangeContext on which the request was received.
 *                          This function does not take ownership of this object. The ExchangeContext
 *                          must be closed or aborted by the calling function according to the
 *                          CHIP_ERROR returned.
 * @param[in]   aProfileId  The profile to be put in the StatusReport payload.
 * @param[in]   aStatusCode The status code to be put in the StatusReport payload; must refer to the
 *                          profile passed in aProfileId, but this function does not enforce this
 *                          condition.
 *
 * @return      WEAVE_NO_ERROR in case of success.
 *              WEAVE_NO_MEMORY if no pbufs are available.
 *              Any other CHIP_ERROR code returned by ExchangeContext::SendMessage
 */
CHIP_ERROR ResponderEngine::SendStatusReport(chip::ExchangeContext * aEC, uint32_t aProfileId, uint16_t aStatusCode)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    err = chip::WeaveServerBase::SendStatusReport(aEC, aProfileId, aStatusCode, WEAVE_NO_ERROR);
    WeaveLogFunctError(err);

    return err;
}

/**
 * Unsolicited message handler for all WDM messages.
 * This function is a @ref ExchangeContext::MessageReceiveFunct.
 */
void ResponderEngine::UnsolicitedMessageHandler(chip::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                                   const chip::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId,
                                                   uint8_t aMsgType, PacketBuffer * aPayload)
{
    chip::ExchangeContext::MessageReceiveFunct func = OnUnknownMsgType;

    // If the message was received over UDP and the peer requested an ACK, arrange for
    // any message sent as a response to also request an ACK.
    if (aMsgInfo->InCon == NULL && GetFlag(aMsgInfo->Flags, kWeaveMessageFlag_PeerRequestedAck))
    {
        aEC->SetAutoRequestAck(true);
    }

    switch (aMsgType)
    {
    case kMsgType_ReportDataRequest:
        func = OnReportDataRequest;

        WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_TreatNotifyAsCancel, func = OnCancelRequest);
        break;

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER

    case kMsgType_SubscribeRequest:
        func = OnSubscribeRequest;
        break;

    case kMsgType_SubscribeConfirmRequest:
        func = OnSubscribeConfirmRequest;
        break;

    case kMsgType_CustomCommandRequest:
    case kMsgType_OneWayCommand:
        func = OnCustomCommand;
        break;

#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

#if WDM_ENABLE_SUBSCRIPTION_CANCEL
    case kMsgType_SubscribeCancelRequest:
        func = OnCancelRequest;
        break;
#endif // WDM_ENABLE_SUBSCRIPTION_CANCEL

#if WDM_ENABLE_SUBSCRIPTIONLESS_NOTIFICATION
    case kMsgType_SubscriptionlessNotification:
        func = OnSubscriptionlessNotification;
        break;
#endif // WDM_ENABLE_SUBSCRIPTIONLESS_NOTIFICATION

#if WDM_ENABLE_PUBLISHER_UPDATE_SERVER_SUPPORT
    case kMsgType_UpdateRequest:
        func = OnUpdateRequest;
        break;

    case kMsgType_PartialUpdateRequest:
        WeaveLogDetail(DataManagement, "PartialUpdateRequest not supported yet for update server");
        break;
#endif // WDM_ENABLE_PUBLISHER_UPDATE_SERVER_SUPPORT

    default:
        break;
    }

    func(aEC, aPktInfo, aMsgInfo, aProfileId, aMsgType, aPayload);
}

/**
 * Unsolicited message handler for unsupported WDM messages.
 * This function is a @ref ExchangeContext::MessageReceiveFunct.
 */
void ResponderEngine::OnUnknownMsgType(chip::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                          const chip::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                          PacketBuffer * aPayload)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    PacketBuffer::Free(aPayload);
    aPayload = NULL;

    WeaveLogDetail(DataManagement, "Msg type %" PRIu8 " not supported", aMsgType);

    err = SendStatusReport(aEC, chip::Profiles::kWeaveProfile_Common, chip::Profiles::Common::kStatus_UnsupportedMessage);
    SuccessOrExit(err);

    aEC->Close();
    aEC = NULL;

exit:
    WeaveLogFunctError(err);

    if (NULL != aEC)
    {
        aEC->Abort();
        aEC = NULL;
    }
}

void ResponderEngine::OnReportDataRequest(chip::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                               const chip::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                               PacketBuffer * aPayload)
{
    CHIP_ERROR err                    = WEAVE_NO_ERROR;
    ResponderEngine * const pEngine = reinterpret_cast<ResponderEngine *>(aEC->AppState);
    uint64_t SubscriptionId            = 0;

    {
        chip::TLV::TLVReader reader;
        ReeportDataRequest::Parser reportData;

        reader.Init(aPayload);

        err = reader.Next();
        SuccessOrExit(err);

        err = reportData.Init(reader);
        SuccessOrExit(err);

        // Note that it is okay to bail out, without any response, if the message doesn't even have a subscription ID in it
        err = reportData.GetSubscriptionID(&SubscriptionId);
        SuccessOrExit(err);
    }

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if ((SubscriptionClient::kState_SubscriptionEstablished_Idle == pEngine->mClients[i].mCurrentState) ||
            (SubscriptionClient::kState_SubscriptionEstablished_Confirming == pEngine->mClients[i].mCurrentState))
        {
            if (pEngine->mClients[i].mSubscriptionId == SubscriptionId)
            {
                pEngine->mClients[i].ReportDataRequestHandler(aEC, aPktInfo, aMsgInfo, aPayload);
                aPayload = NULL;
                aEC      = NULL;
                ExitNow();
            }
        }
    }

    ChipLogDetail(DataManagement, "%s: couldn't find matching client. Subscription ID: 0x%" PRIX64, __func__, SubscriptionId);

    err = SendStatusReport(aEC, chip::Protocol::kInteractionModel, kStatus_InvalidSubscriptionID);
    SuccessOrExit(err);

exit:
    ChipLogFunctError(err);

    if (NULL != aPayload)
    {
        PacketBuffer::Free(aPayload);
        aPayload = NULL;
    }

    if (NULL != aEC)
    {
        aEC->Abort();
        aEC = NULL;
    }
}

CHIP_ERROR ResponderEngine::ProcessDataList(chip::TLV::TLVReader & aReader,
                                                const TraitCatalogBase<TraitDataSink> * aCatalog, bool & aOutIsPartialChange,
                                                TraitDataHandle & aOutTraitDataHandle,
                                                IDataElementAccessControlDelegate & acDelegate)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    // TODO: We currently don't support changes that span multiple notifies, nor changes
    // that get aborted and restarted within the same notify. See WEAV-1586 for more details.
    bool isPartialChange = false;
    uint8_t flags;

    VerifyOrExit(aCatalog != NULL, err = CHIP_ERROR_INVALID_ARGUMENT);

    while (WEAVE_NO_ERROR == (err = aReader.Next()))
    {
        chip::TLV::TLVReader pathReader;

        {
            DataElement::Parser element;

            err = element.Init(aReader);
            SuccessOrExit(err);

            err = element.GetReaderOnPath(&pathReader);
            SuccessOrExit(err);

            isPartialChange = false;
            err             = element.GetPartialChangeFlag(&isPartialChange);
            VerifyOrExit(err == WEAVE_NO_ERROR || err == WEAVE_END_OF_TLV, );
        }

        TraitPath traitPath;
        TraitDataSink * dataSink;
        TraitDataHandle handle;
        PropertyPathHandle pathHandle;
        SchemaVersionRange versionRange;

        err = aCatalog->AddressToHandle(pathReader, handle, versionRange);

        if (err == CHIP_ERROR_INVALID_PROFILE_ID)
        {
            // AddressToHandle() can return an error if the sink has been removed from the catalog. In that case,
            // continue to next entry
            err = WEAVE_NO_ERROR;
            continue;
        }

        SuccessOrExit(err);

        if (aCatalog->Locate(handle, &dataSink) != WEAVE_NO_ERROR)
        {
            // Ideally, this code will not be reached as Locate() should find the entry in the catalog.
            // Otherwise, the earlier AddressToHandle() call would have continued.
            // However, keeping this check here for consistency and code safety
            continue;
        }

        err = dataSink->GetSchemaEngine()->MapPathToHandle(pathReader, pathHandle);
#if TDM_DISABLE_STRICT_SCHEMA_COMPLIANCE
        // if we're not in strict compliance mode, we can ignore data elements that refer to paths we can't map due to mismatching
        // schema. The eventual call to StoreDataElement will correctly deal with the presence of a null property path handle that
        // has been returned by the above call. It's necessary to call into StoreDataElement with this null handle to ensure
        // the requisite OnEvent calls are made to the application despite the presence of an unknown tag. It's also necessary to
        // ensure that we update the internal version tracked by the sink.
        if (err == CHIP_ERROR_TLV_TAG_NOT_FOUND)
        {
            WeaveLogDetail(DataManagement, "Ignoring un-mappable path!");
            err = WEAVE_NO_ERROR;
        }
#endif
        SuccessOrExit(err);

        traitPath.mTraitDataHandle    = handle;
        traitPath.mPropertyPathHandle = pathHandle;

        err = acDelegate.DataElementAccessCheck(traitPath, *aCatalog);

        if (err == CHIP_ERROR_ACCESS_DENIED)
        {
            WeaveLogDetail(DataManagement, "Ignoring path. Subscriptionless notification not accepted by data sink.");

            continue;
        }
        SuccessOrExit(err);

        pathReader = aReader;
        flags      = 0;

#if WDM_ENABLE_PROTOCOL_CHECKS
        // If we previously had a partial change, the current handle should match the previous one.
        // If they don't, we have a partial change violation.
        if (aOutIsPartialChange && (aOutTraitDataHandle != handle))
        {
            WeaveLogError(DataManagement, "Encountered partial change flag violation (%u, %x, %x)", aOutIsPartialChange,
                          aOutTraitDataHandle, handle);
            err = CHIP_ERROR_INVALID_DATA_LIST;
            goto exit;
        }
#endif

        if (!aOutIsPartialChange)
        {
            flags = TraitDataSink::kFirstElementInChange;
        }

        if (!isPartialChange)
        {
            flags |= TraitDataSink::kLastElementInChange;
        }

        err = dataSink->StoreDataElement(pathHandle, pathReader, flags, NULL, NULL, handle);
        SuccessOrExit(err);

        aOutIsPartialChange = isPartialChange;

#if WDM_ENABLE_PROTOCOL_CHECKS
        aOutTraitDataHandle = handle;
#endif
    }

    // if we have exhausted this container
    if (WEAVE_END_OF_TLV == err)
    {
        err = WEAVE_NO_ERROR;
    }

exit:
    return err;
}

SubscriptionClient * ResponderEngine::FindClient(const uint64_t aPeerNodeId, const uint64_t aSubscriptionId)
{
    SubscriptionClient * result = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if ((mClients[i].mCurrentState >= SubscriptionClient::kState_Subscribing_IdAssigned) &&
            (mClients[i].mCurrentState <= SubscriptionClient::kState_SubscriptionEstablished_Confirming))
        {
            if ((aPeerNodeId == mClients[i].mBinding->GetPeerNodeId()) && (mClients[i].mSubscriptionId == aSubscriptionId))
            {
                result = &mClients[i];
                break;
            }
        }
    }

    return result;
}

bool ResponderEngine::UpdateClientLiveness(const uint64_t aPeerNodeId, const uint64_t aSubscriptionId, const bool aKill)
{
    CHIP_ERROR err              = WEAVE_NO_ERROR;
    bool found                   = false;
    SubscriptionClient * pClient = FindClient(aPeerNodeId, aSubscriptionId);

    if (NULL != pClient)
    {
        found = true;

        if (aKill)
        {
            err = CHIP_ERROR_TRANSACTION_CANCELED;
        }
        else
        {
            WeaveLogDetail(DataManagement, "Client[%d] [%5.5s] liveness confirmed", GetClientId(pClient), pClient->GetStateStr());

            // emit a subscription activity event
            pClient->IndicateActivity();

            // ignore incorrect state error, otherwise, let it flow through
            err = pClient->RefreshTimer();
            if (CHIP_ERROR_INCORRECT_STATE == err)
            {
                err = WEAVE_NO_ERROR;

                WeaveLogDetail(DataManagement, "Client[%d] [%5.5s] liveness confirmation failed, ignore", GetClientId(pClient),
                               pClient->GetStateStr());
            }
        }

        if (WEAVE_NO_ERROR != err)
        {
            WeaveLogDetail(DataManagement, "Client[%d] [%5.5s] bound mutual subscription is going away", GetClientId(pClient),
                           pClient->GetStateStr());

            pClient->TerminateSubscription(err, NULL, false);
        }
    }

    return found;
}

#endif // WDM_ENABLE_SUBSCRIPTION_CLIENT

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER

uint16_t ResponderEngine::GetHandlerId(const SubscriptionHandler * const apHandler) const
{
    return static_cast<uint16_t>(apHandler - mHandlers);
}

uint16_t ResponderEngine::GetCommandObjId(const Command * const apHandle) const
{
    return static_cast<uint16_t>(apHandle - mCommandObjs);
}

bool ResponderEngine::UpdateHandlerLiveness(const uint64_t aPeerNodeId, const uint64_t aSubscriptionId, const bool aKill)
{
    CHIP_ERROR err                = WEAVE_NO_ERROR;
    bool found                     = false;
    SubscriptionHandler * pHandler = FindHandler(aPeerNodeId, aSubscriptionId);
    if (NULL != pHandler)
    {
        found = true;

        if (aKill)
        {
            err = CHIP_ERROR_TRANSACTION_CANCELED;
        }
        else
        {
            WeaveLogDetail(DataManagement, "Handler[%d] [%5.5s] liveness confirmed", GetHandlerId(pHandler),
                           pHandler->GetStateStr());

            // ignore incorrect state error, otherwise, let it flow through
            err = pHandler->RefreshTimer();
            if (CHIP_ERROR_INCORRECT_STATE == err)
            {
                err = WEAVE_NO_ERROR;

                WeaveLogDetail(DataManagement, "Handler[%d] [%5.5s] liveness confirmation failed, ignore", GetHandlerId(pHandler),
                               pHandler->GetStateStr());
            }
        }

        if (WEAVE_NO_ERROR != err)
        {
            WeaveLogDetail(DataManagement, "Handler[%d] [%5.5s] bound mutual subscription is going away", GetHandlerId(pHandler),
                           pHandler->GetStateStr());

            pHandler->TerminateSubscription(err, NULL, false);
        }
    }

    return found;
}

SubscriptionHandler * ResponderEngine::FindHandler(const uint64_t aPeerNodeId, const uint64_t aSubscriptionId)
{
    SubscriptionHandler * result = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        if ((mHandlers[i].mCurrentState >= SubscriptionHandler::kState_SubscriptionInfoValid_Begin) &&
            (mHandlers[i].mCurrentState <= SubscriptionHandler::kState_SubscriptionInfoValid_End))
        {
            if ((aPeerNodeId == mHandlers[i].mBinding->GetPeerNodeId()) && (aSubscriptionId == mHandlers[i].mSubscriptionId))
            {
                result = &mHandlers[i];
                break;
            }
        }
    }

    return result;
}

CHIP_ERROR ResponderEngine::GetMinEventLogPosition(size_t & outLogPosition) const
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    for (size_t subIdx = 0; subIdx < kMaxNumSubscriptionHandlers; ++subIdx)
    {
        const SubscriptionHandler * subHandler = &(mHandlers[subIdx]);
        if (subHandler->mCurrentState == SubscriptionHandler::kState_Free)
        {
            continue;
        }

        if (subHandler->mBytesOffloaded < outLogPosition)
        {
            outLogPosition = subHandler->mBytesOffloaded;
        }
    }

    return err;
}

void ResponderEngine::ReclaimTraitInfo(SubscriptionHandler * const aHandlerToBeReclaimed)
{
    SubscriptionHandler::TraitInstanceInfo * const traitInfoList = aHandlerToBeReclaimed->mTraitInstanceList;
    const uint16_t numTraitInstances                             = aHandlerToBeReclaimed->mNumTraitInstances;
    size_t numTraitInstancesToBeAffected;

    aHandlerToBeReclaimed->mTraitInstanceList = NULL;
    aHandlerToBeReclaimed->mNumTraitInstances = 0;

    if (!numTraitInstances)
    {
        WeaveLogDetail(DataManagement, "No trait instances allocated for this subscription");
        ExitNow();
    }

    // make sure everything is still sane
    WeaveLogIfFalse(traitInfoList >= mTraitInfoPool);
    WeaveLogIfFalse(numTraitInstances <= mNumTraitInfosInPool);

    // mPathGroupPool + kMaxNumPathGroups is a pointer which points to the last+1byte of this array
    // traitInfoList is a pointer to the first trait instance to be released
    // the result of subtraction is the number of trait instances from traitInfoList to the end of this array
    numTraitInstancesToBeAffected = (mTraitInfoPool + mNumTraitInfosInPool) - traitInfoList;

    // Shrink the traitInfosInPool by the number of trait instances in this subscription.
    mNumTraitInfosInPool -= numTraitInstances;
    SYSTEM_STATS_DECREMENT_BY_N(chip::System::Stats::kWDM_NumTraits, numTraitInstances);

    if (numTraitInstances == numTraitInstancesToBeAffected)
    {
        WeaveLogDetail(DataManagement, "Releasing the last block of trait instances");
        ExitNow();
    }

    WeaveLogDetail(DataManagement, "Moving %u trait instances forward",
                   static_cast<unsigned int>(numTraitInstancesToBeAffected - numTraitInstances));

    memmove(traitInfoList, traitInfoList + numTraitInstances,
            sizeof(SubscriptionHandler::TraitInstanceInfo) * (numTraitInstancesToBeAffected - numTraitInstances));

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        SubscriptionHandler * const pHandler = mHandlers + i;

        if ((aHandlerToBeReclaimed != pHandler) && (pHandler->mTraitInstanceList > traitInfoList))
        {
            pHandler->mTraitInstanceList -= numTraitInstances;
        }
    }

exit:
    WeaveLogDetail(DataManagement, "Number of allocated trait instances: %u", mNumTraitInfosInPool);
}

CHIP_ERROR ResponderEngine::EnablePublisher(IWeavePublisherLock * aLock,
                                                TraitCatalogBase<TraitDataSource> * const aPublisherCatalog)
{
    // force abandon all subscription first, so we can have a clean slate
    DisablePublisher();

    mLock = aLock;

    // replace catalog
    mPublisherCatalog = aPublisherCatalog;

    mIsPublisherEnabled = true;

    mNextHandlerToNotify = 0;

    return WEAVE_NO_ERROR;
}

CHIP_ERROR ResponderEngine::Lock()
{
    if (mLock)
    {
        return mLock->Lock();
    }

    return WEAVE_NO_ERROR;
}

CHIP_ERROR ResponderEngine::Unlock()
{
    if (mLock)
    {
        return mLock->Unlock();
    }

    return WEAVE_NO_ERROR;
}

void ResponderEngine::DisablePublisher()
{
    mIsPublisherEnabled = false;
    mPublisherCatalog   = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        switch (mHandlers[i].mCurrentState)
        {
        case SubscriptionHandler::kState_Free:
        case SubscriptionHandler::kState_Terminated:
            break;
        default:
            mHandlers[i].AbortSubscription();
        }
    }

    // Note that the command objects are not closed when publisher is disabled.
    // This is because the processing flow of commands are not directly linked
    // with subscriptions.
}

CHIP_ERROR ResponderEngine::NewSubscriptionHandler(SubscriptionHandler ** subHandler)
{
    CHIP_ERROR err = CHIP_ERROR_NO_MEMORY;

    *subHandler = NULL;

    WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_SubscriptionHandlerNew, ExitNow());

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        if (SubscriptionHandler::kState_Free == mHandlers[i].mCurrentState)
        {
            WeaveLogIfFalse(0 == mHandlers[i].mRefCount);
            *subHandler = &mHandlers[i];
            err         = WEAVE_NO_ERROR;

            SYSTEM_STATS_INCREMENT(chip::System::Stats::kWDM_NumSubscriptionHandlers);

            break;
        }
    }

    ExitNow(); // silence warnings about unused labels.
exit:
    return err;
}

void ResponderEngine::OnSubscribeRequest(chip::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                            const chip::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                            PacketBuffer * aPayload)
{
    CHIP_ERROR err                    = WEAVE_NO_ERROR;
    ResponderEngine * const pEngine = reinterpret_cast<ResponderEngine *>(aEC->AppState);
    SubscriptionHandler * handler      = NULL;
    uint32_t reasonProfileId           = chip::Profiles::kWeaveProfile_Common;
    uint16_t reasonStatusCode          = chip::Profiles::Common::kStatus_InternalServerProblem;
    InEventParam inParam;
    OutEventParam outParam;
    Binding * binding;
    uint64_t subscriptionId = 0;

    if (pEngine->mIsPublisherEnabled && (NULL != pEngine->mEventCallback))
    {
        outParam.mIncomingSubscribeRequest.mAutoClosePriorSubscription = true;
        outParam.mIncomingSubscribeRequest.mRejectRequest              = false;
        outParam.mIncomingSubscribeRequest.mpReasonProfileId           = &reasonProfileId;
        outParam.mIncomingSubscribeRequest.mpReasonStatusCode          = &reasonStatusCode;

        inParam.mIncomingSubscribeRequest.mEC      = aEC;
        inParam.mIncomingSubscribeRequest.mPktInfo = aPktInfo;
        inParam.mIncomingSubscribeRequest.mMsgInfo = aMsgInfo;
        inParam.mIncomingSubscribeRequest.mPayload = aPayload;
        inParam.mIncomingSubscribeRequest.mBinding = binding;

        // note the binding is exposed to app layer for configuration here, and again later after
        // the request is fully parsed
        pEngine->mEventCallback(pEngine->mAppState, kEvent_OnIncomingSubscribeRequest, inParam, outParam);

        // Make sure messages sent through this EC are sent with proper re-transmission/timeouts settings
        // This is mainly for rejections, as the EC would be configured again in SubscriptionHandler::AcceptSubscribeRequest

        err = binding->AdjustResponseTimeout(aEC);
        SuccessOrExit(err);
    }
    else
    {
        ExitNow(err = CHIP_ERROR_NO_MESSAGE_HANDLER);
    }

    if (outParam.mIncomingSubscribeRequest.mRejectRequest)
    {
        // reject this request (without touching existing subscriptions)
        ExitNow(err = CHIP_ERROR_TRANSACTION_CANCELED);
    }
    else
    {
        if (outParam.mIncomingSubscribeRequest.mAutoClosePriorSubscription)
        {
            // if not rejected, default behavior is to abort any prior communication with this node id
            for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
            {
                if ((pEngine->mHandlers[i].mCurrentState >= SubscriptionHandler::kState_SubscriptionInfoValid_Begin) &&
                    (pEngine->mHandlers[i].mCurrentState <= SubscriptionHandler::kState_SubscriptionInfoValid_End))
                {
                    uint64_t nodeId = pEngine->mHandlers[i].GetPeerNodeId();

                    if (nodeId == aEC->PeerNodeId)
                    {
                        pEngine->mHandlers[i].TerminateSubscription(err, NULL, false);
                    }
                }
            }
        }

        err = chip::Platform::Security::GetSecureRandomData((uint8_t *) &subscriptionId, sizeof(subscriptionId));
        SuccessOrExit(err);

        err = pEngine->NewSubscriptionHandler(&handler);
        if (err != WEAVE_NO_ERROR)
        {
            // try to give slightly more detail on the issue for this potentially common problem
            reasonStatusCode = (err == CHIP_ERROR_NO_MEMORY ? chip::Profiles::Common::kStatus_OutOfMemory
                                                             : chip::Profiles::Common::kStatus_InternalServerProblem);

            ExitNow();
        }
        else
        {
            handler->mAppState      = outParam.mIncomingSubscribeRequest.mHandlerAppState;
            handler->mEventCallback = outParam.mIncomingSubscribeRequest.mHandlerEventCallback;
            uint32_t maxSize        = WDM_MAX_NOTIFICATION_SIZE;

            WEAVE_FAULT_INJECT_WITH_ARGS(
                FaultInjection::kFault_WDM_NotificationSize,
                // Code executed with the Manager's lock:
                if (numFaultArgs > 0) { maxSize = static_cast<uint32_t>(faultArgs[0]); } else {
                    maxSize = WDM_MAX_NOTIFICATION_SIZE / 2;
                },
                // Code executed withouth the Manager's lock:
                WeaveLogDetail(DataManagement, "Handler[%d] Payload size set to %d", pEngine->GetHandlerId(handler), maxSize));

            handler->SetMaxNotificationSize(maxSize);

            handler->InitWithIncomingRequest(binding, subscriptionId, aEC, aPktInfo, aMsgInfo, aPayload);
            aEC      = NULL;
            aPayload = NULL;
        }
    }

exit:
    WeaveLogFunctError(err);

    if (NULL != aPayload)
    {
        PacketBuffer::Free(aPayload);
        aPayload = NULL;
    }

    if (NULL != aEC)
    {
        err = SendStatusReport(aEC, reasonProfileId, reasonStatusCode);
        WeaveLogFunctError(err);

        aEC->Close();
        aEC = NULL;
    }

    if (NULL != binding)
    {
        binding->Release();
    }
}

void ResponderEngine::OnSubscribeConfirmRequest(chip::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                                   const chip::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId,
                                                   uint8_t aMsgType, PacketBuffer * aPayload)
{
    CHIP_ERROR err                    = WEAVE_NO_ERROR;
    ResponderEngine * const pEngine = reinterpret_cast<ResponderEngine *>(aEC->AppState);
    uint32_t reasonProfileId           = chip::Profiles::kWeaveProfile_Common;
    uint16_t reasonStatusCode          = chip::Profiles::Common::kStatus_InternalServerProblem;
    uint64_t subscriptionId;

    {
        chip::TLV::TLVReader reader;
        SubscribeConfirmRequest::Parser request;

        reader.Init(aPayload);

        err = reader.Next();
        SuccessOrExit(err);

        err = request.Init(reader);
        SuccessOrExit(err);

        err = request.GetSubscriptionID(&subscriptionId);
        SuccessOrExit(err);
    }

    // Discard the buffer so that it may be reused by the code below.
    PacketBuffer::Free(aPayload);
    aPayload = NULL;

    if (pEngine->mIsPublisherEnabled)
    {
        // find a matching subscription
        bool found = false;

#if WDM_ENABLE_SUBSCRIPTION_CLIENT
        if (pEngine->UpdateClientLiveness(aEC->PeerNodeId, subscriptionId))
        {
            found = true;
        }
#endif // WDM_ENABLE_SUBSCRIPTION_CLIENT

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
        if (pEngine->UpdateHandlerLiveness(aEC->PeerNodeId, subscriptionId))
        {
            found = true;
        }
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

        if (found)
        {
            reasonStatusCode = chip::Profiles::Common::kStatus_Success;
        }
        else
        {
            reasonProfileId  = chip::Profiles::kWeaveProfile_WDM;
            reasonStatusCode = kStatus_InvalidSubscriptionID;
        }
    }
    else
    {
        reasonStatusCode = chip::Profiles::Common::kStatus_Busy;
    }

    {
        err = SendStatusReport(aEC, reasonProfileId, reasonStatusCode);
        SuccessOrExit(err);
    }

exit:
    WeaveLogFunctError(err);

    if (aPayload != NULL)
    {
        PacketBuffer::Free(aPayload);
    }

    // aEC is guaranteed to be non-NULL.
    aEC->Close();
}

CHIP_ERROR ResponderEngine::AllocateRightSizedBuffer(PacketBuffer *& buf, const uint32_t desiredSize, const uint32_t minSize,
                                                         uint32_t & outMaxPayloadSize)
{
    CHIP_ERROR err          = WEAVE_NO_ERROR;
    uint32_t bufferAllocSize = 0;
    uint32_t maxWeavePayloadSize;
    uint32_t weaveTrailerSize = WEAVE_TRAILER_RESERVE_SIZE;
    uint32_t weaveHeaderSize  = WEAVE_SYSTEM_CONFIG_HEADER_RESERVE_SIZE;

    bufferAllocSize = chip::min(
        desiredSize, static_cast<uint32_t>(WEAVE_SYSTEM_CONFIG_PACKETBUFFER_CAPACITY_MAX - weaveHeaderSize - weaveTrailerSize));

    // Add the Weave Trailer size as NewWithAvailableSize() includes that in
    // availableSize.
    bufferAllocSize += weaveTrailerSize;

    buf = PacketBuffer::NewWithAvailableSize(weaveHeaderSize, bufferAllocSize);
    VerifyOrExit(buf != NULL, err = CHIP_ERROR_NO_MEMORY);

    maxWeavePayloadSize = WeaveMessageLayer::GetMaxWeavePayloadSize(buf, true, WEAVE_CONFIG_DEFAULT_UDP_MTU_SIZE);

    outMaxPayloadSize = chip::min(maxWeavePayloadSize, bufferAllocSize);

    if (outMaxPayloadSize < minSize)
    {
        err = CHIP_ERROR_BUFFER_TOO_SMALL;

        PacketBuffer::Free(buf);
        buf = NULL;
    }

exit:
    return err;
}
}; // namespace app
}; // namespace chip
