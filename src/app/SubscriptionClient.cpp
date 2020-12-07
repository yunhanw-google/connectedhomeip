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
 *      This file implements subscription client for Weave
 *      Data Management (WDM) profile.
 *
 */

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif // __STDC_FORMAT_MACROS

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif // __STDC_CONSTANT_MACROS

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif // __STDC_LIMIT_MACROS

#include <Weave/Support/WeaveFaultInjection.h>
#include <Weave/Support/RandUtils.h>
#include <Weave/Support/FibonacciUtils.h>
#include <SystemLayer/SystemStats.h>

namespace chip {
namespace app {

class AlwaysAcceptDataElementAccessControlDelegate : public IDataElementAccessControlDelegate
{
public:

     // TODO : This Access Check function needs to be more sophisticated in
     // allowing access to subscription-based notifications.

     CHIP_ERROR DataElementAccessCheck(const TraitPath & aTraitPath,
                                        const TraitCatalogBase<TraitDataSink> & aCatalog)
     {
         return WEAVE_NO_ERROR;
     }
};

// Do nothing
SubscriptionClient::SubscriptionClient() { }

void SubscriptionClient::InitAsFree()
{
    mCurrentState = kState_Free;
    mRefCount     = 0;
    Reset();
}

void SubscriptionClient::Reset(void)
{
    mBinding                                = NULL;
    mEC                                     = NULL;
    mAppState                               = NULL;
    mEventCallback                          = NULL;
    mResubscribePolicyCallback              = NULL;
    mDataSinkCatalog                        = NULL;
    mInactivityTimeoutDuringSubscribingMsec = kNoTimeOut;
    mLivenessTimeoutMsec                    = kNoTimeOut;
    mSubscriptionId                         = 0;
    mConfig                                 = kConfig_Down;
    mRetryCounter                           = 0;



#if WDM_ENABLE_PROTOCOL_CHECKS
    mPrevTraitDataHandle = -1;
#endif

    mPrevIsPartialChange = false;
}

// AddRef to Binding
// store pointers to binding and delegate
// null out EC
CHIP_ERROR SubscriptionClient::Init(Binding * const apBinding, void * const apAppState, EventCallback const aEventCallback,
                                     const TraitCatalogBase<TraitDataSink> * const apCatalog,
                                     const uint32_t aInactivityTimeoutDuringSubscribingMsec, IWeaveWDMMutex * aUpdateMutex)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    WeaveLogIfFalse(0 == mRefCount);

    // add reference to the binding
    apBinding->AddRef();

    // make a copy of the pointers
    mBinding                                = apBinding;
    mAppState                               = apAppState;
    mEventCallback                          = aEventCallback;

    // Set the protocol callback on the binding object so that the SubscriptionClient gets
    // notified of changes in the binding's state.
    mBinding->SetProtocolLayerCallback(BindingEventCallback, this);

    if (NULL == apCatalog)
    {
        mDataSinkCatalog = NULL;
    }
    else
    {
        mDataSinkCatalog = const_cast <TraitCatalogBase<TraitDataSink>*>(apCatalog);
    }

    mInactivityTimeoutDuringSubscribingMsec = aInactivityTimeoutDuringSubscribingMsec;


#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
    mUpdateMutex                            = aUpdateMutex;
    mUpdateInFlight                         = false;
    mMaxUpdateSize                          = 0;

#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE
    MoveToState(kState_Initialized);

    _AddRef();

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE

    err = mUpdateClient.Init(mBinding, this, UpdateEventCallback);
    SuccessOrExit(err);

    ConfigureUpdatableSinks();

exit:
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE
    return err;
}

#if WEAVE_DETAIL_LOGGING
const char * SubscriptionClient::GetStateStr() const
{
    switch (mCurrentState)
    {
    case kState_Free:
        return "FREE";

    case kState_Initialized:
        return "INIT";

    case kState_Subscribing:
        return "SReq1";

    case kState_Subscribing_IdAssigned:
        return "SReq2";

    case kState_SubscriptionEstablished_Idle:
        return "ALIVE";

    case kState_SubscriptionEstablished_Confirming:
        return "CONFM";

    case kState_Canceling:
        return "CANCL";

    case kState_Resubscribe_Holdoff:
        return "RETRY";

    case kState_Terminated:
        return "TERM";
    }
    return "N/A";
}
#else  // WEAVE_DETAIL_LOGGING
const char * SubscriptionClient::GetStateStr() const
{
    return "N/A";
}
#endif // WEAVE_DETAIL_LOGGING

void SubscriptionClient::MoveToState(const ClientState aTargetState)
{
    mCurrentState = aTargetState;
    WeaveLogDetail(DataManagement, "Client[%u] moving to [%5.5s] Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), mRefCount);

#if WEAVE_DETAIL_LOGGING
    if (kState_Free == mCurrentState)
    {
        SubscriptionEngine::GetInstance()->LogSubscriptionFreed();
    }
#endif // #if WEAVE_DETAIL_LOGGING
}

/**
 * @brief Enable automatic resubscribes. Attach a callback to specify
 * the next retry time on failure.
 *
 * @param[in] aCallback     Optional callback to fetch the amount of time to
 *                          wait before retrying after a failure. If NULL use
 *                          a default policy.
 */
void SubscriptionClient::EnableResubscribe(ResubscribePolicyCallback aCallback)
{
    if (aCallback)
    {
        mResubscribePolicyCallback = aCallback;
    }
    else
    {
        mResubscribePolicyCallback = DefaultResubscribePolicyCallback;
    }
}

/**
 * @brief Disable the resubscribe mechanism. This will abort if a resubscribe
 * was pending.
 */
void SubscriptionClient::DisableResubscribe(void)
{
    mResubscribePolicyCallback = NULL;

    if (mCurrentState == kState_Resubscribe_Holdoff)
    {
        // cancel timer
        CancelSubscriptionTimer();

        // app doesn't need to know since it triggered this
        AbortSubscription();
    }
}

/**
 * @brief Kick the resubscribe mechanism. This will initiate an immediate retry
 */
void SubscriptionClient::ResetResubscribe(void)
{
    if (mCurrentState == kState_Resubscribe_Holdoff)
    {
        // cancel timer
        CancelSubscriptionTimer();
        MoveToState(kState_Initialized);
    }

    mRetryCounter = 0;

    if (mCurrentState == kState_Initialized || mCurrentState == kState_Resubscribe_Holdoff)
    {
        SetRetryTimer(WEAVE_NO_ERROR);
    }
}

void SubscriptionClient::IndicateActivity(void)
{
    // emit an OnSubscriptionActivity event
    InEventParam inParam;
    OutEventParam outParam;

    inParam.mSubscriptionActivity.mClient = this;
    mEventCallback(mAppState, kEvent_OnSubscriptionActivity, inParam, outParam);
}

CHIP_ERROR SubscriptionClient::GetSubscriptionId(uint64_t * const apSubscriptionId)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    *apSubscriptionId = 0;

    switch (mCurrentState)
    {
    case kState_Subscribing_IdAssigned:
    case kState_SubscriptionEstablished_Idle:
    case kState_SubscriptionEstablished_Confirming:
    case kState_Canceling:
        *apSubscriptionId = mSubscriptionId;
        ExitNow();
        break;

    default:
        ExitNow(err = CHIP_ERROR_INCORRECT_STATE);
    }

exit:
    WeaveLogFunctError(err);

    return err;
}

void SubscriptionClient::DefaultEventHandler(EventID aEvent, const InEventParam & aInParam, OutEventParam & aOutParam)
{
    IgnoreUnusedVariable(aInParam);
    IgnoreUnusedVariable(aOutParam);

    WeaveLogDetail(DataManagement, "%s event: %d", __func__, aEvent);
}

/**
 * @brief The default policy implementation will pick a random timeslot
 * with millisecond resolution over an ever increasing window,
 * following a fibonacci sequence upto WDM_RESUBSCRIBE_MAX_FIBONACCI_STEP_INDEX.
 * Average of the randomized wait time past the WDM_RESUBSCRIBE_MAX_FIBONACCI_STEP_INDEX
 * will be around one hour.
 * When the retry count resets to 0, the sequence starts from the beginning again.
 */

void SubscriptionClient::DefaultResubscribePolicyCallback(void * const aAppState, ResubscribeParam & aInParam,
                                                          uint32_t & aOutIntervalMsec)
{
    IgnoreUnusedVariable(aAppState);

    uint32_t fibonacciNum      = 0;
    uint32_t maxWaitTimeInMsec = 0;
    uint32_t waitTimeInMsec    = 0;
    uint32_t minWaitTimeInMsec = 0;

    if (aInParam.mNumRetries <= WDM_RESUBSCRIBE_MAX_FIBONACCI_STEP_INDEX)
    {
        fibonacciNum      = GetFibonacciForIndex(aInParam.mNumRetries);
        maxWaitTimeInMsec = fibonacciNum * WDM_RESUBSCRIBE_WAIT_TIME_MULTIPLIER_MS;
    }
    else
    {
        maxWaitTimeInMsec = WDM_RESUBSCRIBE_MAX_RETRY_WAIT_INTERVAL_MS;
    }

    if (maxWaitTimeInMsec != 0)
    {
        minWaitTimeInMsec = (WDM_RESUBSCRIBE_MIN_WAIT_TIME_INTERVAL_PERCENT_PER_STEP * maxWaitTimeInMsec) / 100;
        waitTimeInMsec    = minWaitTimeInMsec + (GetRandU32() % (maxWaitTimeInMsec - minWaitTimeInMsec));
    }

    aOutIntervalMsec = waitTimeInMsec;

    WeaveLogDetail(DataManagement,
                   "Computing %s policy: attempts %" PRIu32 ", max wait time %" PRIu32 " ms, selected wait time %" PRIu32
                   " ms",
                   aInParam.mRequestType == ResubscribeParam::kSubscription ? "resubscribe" : "update",
                   aInParam.mNumRetries, maxWaitTimeInMsec, waitTimeInMsec);

    return;
}

void SubscriptionClient::_InitiateSubscription(void)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), __func__, mRefCount);

    // Make sure the client object is not freed during the callback to the application.
    _AddRef();

    VerifyOrExit(kState_Subscribing != mCurrentState && kState_Subscribing_IdAssigned != mCurrentState, /* no-op */);

    VerifyOrExit(kState_Initialized >= mCurrentState, err = CHIP_ERROR_INCORRECT_STATE);

#if WDM_ENABLE_PROTOCOL_CHECKS
    mPrevTraitDataHandle = -1;
#endif

    mPrevIsPartialChange = false;

    // If the binding is ready...
    if (mBinding->IsReady())
    {
        // Enter the Subscribing state.
        if (IsInitiator())
        {
            MoveToState(kState_Subscribing);
        }
        else
        {
            MoveToState(kState_Subscribing_IdAssigned);
        }

        // Using the binding, form and send a SubscribeRequest to the publisher.
        err = SendSubscribeRequest();
        SuccessOrExit(err);

        err = RefreshTimer();
        SuccessOrExit(err);
    }
    else
    {
        err = _PrepareBinding();
        SuccessOrExit(err);
    }

exit:
    WeaveLogFunctError(err);

    if (WEAVE_NO_ERROR != err)
    {
        TerminateSubscription(err, NULL, false);
    }

    _Release();
}

CHIP_ERROR SubscriptionClient::_PrepareBinding()
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    _AddRef();

    if (mBinding->IsReady())
    {
        ExitNow();
    }
    else if (mBinding->CanBePrepared())
    {
        // Ask the application prepare the binding by delivering a PrepareRequested API event to it via the
        // binding's callback.  At some point the binding will callback into the SubscriptionClient signaling
        // that preparation has completed (successfully or otherwise).  Note that this callback can happen
        // synchronously within the RequestPrepare() method, implying that _InitiateSubscription() will recurse.
        err = mBinding->RequestPrepare();
        SuccessOrExit(err);
    }
    // Otherwise, verify that the binding is in one of the preparing states.  Once preparation completes, the
    // binding will call back, at which point, if preparation was successful, _InitiateSubscription() will be
    // called again.
    else
    {
        VerifyOrExit(mBinding->IsPreparing(), err = CHIP_ERROR_INCORRECT_STATE);
    }

exit:
    _Release();

    return err;
}

CHIP_ERROR SubscriptionClient::SendSubscribeRequest(void)
{
    CHIP_ERROR err       = WEAVE_NO_ERROR;
    PacketBuffer * msgBuf = NULL;
    uint8_t msgType       = kMsgType_SubscribeRequest;
    InEventParam inSubscribeParam;
    OutEventParam outSubscribeParam;

    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), __func__, mRefCount);

    inSubscribeParam.Clear();
    outSubscribeParam.Clear();

    outSubscribeParam.mSubscribeRequestPrepareNeeded.mVersionedPathList = NULL;
    outSubscribeParam.mSubscribeRequestPrepareNeeded.mPathList          = NULL;

    inSubscribeParam.mSubscribeRequestPrepareNeeded.mClient = this;
    mEventCallback(mAppState, kEvent_OnSubscribeRequestPrepareNeeded, inSubscribeParam, outSubscribeParam);

    if (IsCounterSubscriber())
    {
        mSubscriptionId = outSubscribeParam.mSubscribeRequestPrepareNeeded.mSubscriptionId;
    }

    VerifyOrExit((kState_Subscribing == mCurrentState || kState_Subscribing_IdAssigned == mCurrentState), err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit((outSubscribeParam.mSubscribeRequestPrepareNeeded.mTimeoutSecMin <= kMaxTimeoutSec) ||
                     (kNoTimeOut == outSubscribeParam.mSubscribeRequestPrepareNeeded.mTimeoutSecMin),
                 err = CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrExit((outSubscribeParam.mSubscribeRequestPrepareNeeded.mTimeoutSecMax <= kMaxTimeoutSec) ||
                     (kNoTimeOut == outSubscribeParam.mSubscribeRequestPrepareNeeded.mTimeoutSecMax),
                 err = CHIP_ERROR_INVALID_ARGUMENT);

    msgBuf = PacketBuffer::New();
    VerifyOrExit(NULL != msgBuf, err = CHIP_ERROR_NO_MEMORY);

    {
        chip::TLV::TLVWriter writer;
        SubscribeRequest::Builder request;

        writer.Init(msgBuf);

        err = request.Init(&writer);
        SuccessOrExit(err);

        if (kNoTimeOut != outSubscribeParam.mSubscribeRequestPrepareNeeded.mTimeoutSecMin)
        {
            request.SubscribeTimeoutMin(outSubscribeParam.mSubscribeRequestPrepareNeeded.mTimeoutSecMin);
        }
        if (kNoTimeOut != outSubscribeParam.mSubscribeRequestPrepareNeeded.mTimeoutSecMax)
        {
            request.SubscribeTimeoutMax(outSubscribeParam.mSubscribeRequestPrepareNeeded.mTimeoutSecMax);
        }
        if (IsCounterSubscriber())
        {
            request.SubscriptionID(mSubscriptionId);
        }

        // It's safe to bail out after a series of operation, for
        // SubscriptionRequest::Builder would internally turn to NOP after error is logged
        SuccessOrExit(err = request.GetError());

        {
            PathList::Builder & pathList = request.CreatePathListBuilder();

            for (size_t i = 0; i < outSubscribeParam.mSubscribeRequestPrepareNeeded.mPathListSize; ++i)
            {
                TraitDataSink * pDataSink;
                chip::TLV::TLVType dummyContainerType;
                SchemaVersionRange versionIntersection;
                VersionedTraitPath versionedTraitPath;

                // Applications can set either the versioned or non versioned path lists for now. We pick either
                // depending on which is non-NULL. If both are non-NULL, we then select the versioned list.
                if (outSubscribeParam.mSubscribeRequestPrepareNeeded.mVersionedPathList)
                {
                    versionedTraitPath = outSubscribeParam.mSubscribeRequestPrepareNeeded.mVersionedPathList[i];
                }
                else
                {
                    versionedTraitPath.mTraitDataHandle =
                        outSubscribeParam.mSubscribeRequestPrepareNeeded.mPathList[i].mTraitDataHandle;
                    versionedTraitPath.mPropertyPathHandle =
                        outSubscribeParam.mSubscribeRequestPrepareNeeded.mPathList[i].mPropertyPathHandle;
                }

                if (mDataSinkCatalog->Locate(versionedTraitPath.mTraitDataHandle, &pDataSink) != WEAVE_NO_ERROR)
                {
                    // Locate() can return an error if the sink has been removed from the catalog. In that case,
                    // continue to next entry
                    continue;
                }

                // Start the TLV Path
                err = writer.StartContainer(chip::TLV::AnonymousTag, chip::TLV::kTLVType_Path, dummyContainerType);
                SuccessOrExit(err);

                // Start, fill, and close the TLV Structure that contains ResourceID, ProfileID, and InstanceID
                err = mDataSinkCatalog->HandleToAddress(versionedTraitPath.mTraitDataHandle, writer,
                                                        versionedTraitPath.mRequestedVersionRange);

                if (err == CHIP_ERROR_INVALID_ARGUMENT)
                {
                    // Ideally, this code will not be reached as HandleToAddress() should find the entry in the catalog.
                    // Otherwise, the earlier Locate() call would have continued.
                    // However, keeping this check here for consistency and code safety
                    err = WEAVE_NO_ERROR;
                    continue;
                }

                SuccessOrExit(err);

                // Append zero or more TLV tags based on the Path Handle
                err = pDataSink->GetSchemaEngine()->MapHandleToPath(versionedTraitPath.mPropertyPathHandle, writer);
                SuccessOrExit(err);

                // Close the TLV Path
                err = writer.EndContainer(dummyContainerType);
                SuccessOrExit(err);
            }

            pathList.EndOfPathList();
            SuccessOrExit(err = pathList.GetError());
        }

        {
            VersionList::Builder & versionList = request.CreateVersionListBuilder();

            for (size_t i = 0; i < outSubscribeParam.mSubscribeRequestPrepareNeeded.mPathListSize; ++i)
            {
                TraitDataSink * pDataSink;
                VersionedTraitPath versionedTraitPath;

                if (outSubscribeParam.mSubscribeRequestPrepareNeeded.mVersionedPathList)
                {
                    versionedTraitPath = outSubscribeParam.mSubscribeRequestPrepareNeeded.mVersionedPathList[i];
                }
                else
                {
                    versionedTraitPath.mTraitDataHandle =
                        outSubscribeParam.mSubscribeRequestPrepareNeeded.mPathList[i].mTraitDataHandle;
                    versionedTraitPath.mPropertyPathHandle =
                        outSubscribeParam.mSubscribeRequestPrepareNeeded.mPathList[i].mPropertyPathHandle;
                }

                if (mDataSinkCatalog->Locate(versionedTraitPath.mTraitDataHandle, &pDataSink) != WEAVE_NO_ERROR)
                {
                    // Locate() can return an error if the sink has been removed from the catalog. In that case,
                    // continue to next entry
                    continue;
                }

                if (pDataSink->IsVersionValid())
                {
                    versionList.AddVersion(pDataSink->GetVersion());
                }
                else
                {
                    versionList.AddNull();
#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
                    if (pDataSink->IsUpdatableDataSink() && ( ! pDataSink->IsVersionValid()))
                    {
                        TraitUpdatableDataSink *updatableSink = static_cast<TraitUpdatableDataSink *>(pDataSink);
                        ClearPotentialDataLoss(versionedTraitPath.mTraitDataHandle, *updatableSink);
                    }
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE

                }
            }

            versionList.EndOfVersionList();
            SuccessOrExit(err = versionList.GetError());
        }

        if (outSubscribeParam.mSubscribeRequestPrepareNeeded.mNeedAllEvents)
        {
            request.SubscribeToAllEvents(true);

            if (outSubscribeParam.mSubscribeRequestPrepareNeeded.mLastObservedEventListSize > 0)
            {

                EventList::Builder & eventList = request.CreateLastObservedEventIdListBuilder();

                for (size_t n = 0; n < outSubscribeParam.mSubscribeRequestPrepareNeeded.mLastObservedEventListSize; ++n)
                {
                    Event::Builder & event = eventList.CreateEventBuilder();
                    event.SourceId(outSubscribeParam.mSubscribeRequestPrepareNeeded.mLastObservedEventList[n].mSourceId)
                        .Importance(outSubscribeParam.mSubscribeRequestPrepareNeeded.mLastObservedEventList[n].mImportance)
                        .EventId(outSubscribeParam.mSubscribeRequestPrepareNeeded.mLastObservedEventList[n].mEventId)
                        .EndOfEvent();
                    SuccessOrExit(err = event.GetError());
                }
                eventList.EndOfEventList();
                SuccessOrExit(err = eventList.GetError());
            }
        }

        request.EndOfRequest();
        SuccessOrExit(err = request.GetError());

        err = writer.Finalize();
        SuccessOrExit(err);
    }

    err = ReplaceExchangeContext();
    SuccessOrExit(err);

    // NOTE: State could be changed in sync error callback by message layer
    WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_SendUnsupportedReqMsgType, msgType += 50);

#if 0 // WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK
    // TODO: SubscribeRequest::CheckSchemaValidity is correct and rejects empty PathLists;
    // but the loop above can encode an empty PathList, and
    // the parser in SubscriptionHandler::ParsePathVersionEventLists will accept it.
    // Need to fix this in a way that's backward compatible.
    {
        chip::TLV::TLVReader reader;
        SubscribeRequest::Parser request;
        reader.Init(msgBuf);

        err = reader.Next();
        SuccessOrExit(err);

        err = request.Init(reader);
        SuccessOrExit(err);

        err = request.CheckSchemaValidity();
        SuccessOrExit(err);
    }
#endif // WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK

    err    = mEC->SendMessage(chip::Profiles::kWeaveProfile_WDM, msgType, msgBuf,
                              chip::ExchangeContext::kSendFlag_ExpectResponse);
    msgBuf = NULL;
    SuccessOrExit(err);

exit:
    WeaveLogFunctError(err);

    if (NULL != msgBuf)
    {
        PacketBuffer::Free(msgBuf);
        msgBuf = NULL;
    }

    return err;
}

/**
 * Configure the SubscriptionClient as an initiator (as opposed to
 * a counter-subscriber) and bring the subscription up if it is not.
 */
void SubscriptionClient::InitiateSubscription(void)
{
    mConfig = kConfig_Initiator;

    if (IsRetryEnabled())
    {
        if (false == mBinding->IsPreparing())
        {
            ResetResubscribe();
        }
    }
    else
    {
        _InitiateSubscription();
    }
}

void SubscriptionClient::InitiateCounterSubscription(const uint32_t aLivenessTimeoutSec)
{
    mConfig = kConfig_CounterSubscriber;

    // the liveness timeout spec is given and not part of the subscription setup
    mLivenessTimeoutMsec = aLivenessTimeoutSec * 1000;

    _InitiateSubscription();
}

void SubscriptionClient::_AddRef()
{
    WeaveLogIfFalse(mRefCount < INT8_MAX);

    ++mRefCount;

    // 0: free
    // 1: in some phase of subscription
    // increase: in downcall to message layer, some callback might come from message layer (send error/connection broken)
    // increase: in callback to app layer
}

void SubscriptionClient::_Release()
{
    WeaveLogIfFalse(mRefCount > 0);

    // If releasing the last reference...
    if (1 == mRefCount)
    {
        // Just to be safe, call AbortSubscription() to ensure that the subscription
        // is properly terminated. If the state transition logic is correct everywhere
        // else in the code, the subscription will already have been terminated and
        // this call will be a no-op.
        AbortSubscription();

        // Clean up resources/state associated with the client object.
        _Cleanup();

        // Return the client to the Free state.
        // NOTE: mRefCount is set to zero here solely to satisfy automated tests that look for
        // a specific reference count in the "Moving to [ FREE]" log message.
        mRefCount = 0;
        MoveToState(kState_Free);

        // Re-initialize all state data.
        InitAsFree();

        SYSTEM_STATS_DECREMENT(chip::System::Stats::kWDM_NumSubscriptionClients);
    }
    else
    {
        --mRefCount;
    }
}

Binding * SubscriptionClient::GetBinding() const
{
    return mBinding;
}

uint64_t SubscriptionClient::GetPeerNodeId(void) const
{
    return (mBinding != NULL) ? mBinding->GetPeerNodeId() : kNodeIdNotSpecified;
}

CHIP_ERROR SubscriptionClient::ReplaceExchangeContext()
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    InEventParam inParam;
    OutEventParam outParam;

    // Make sure we're not freed by accident.
    _AddRef();

    FlushExistingExchangeContext();

    err = mBinding->NewExchangeContext(mEC);
    SuccessOrExit(err);

    mEC->AppState          = this;
    mEC->OnMessageReceived = OnMessageReceivedFromLocallyInitiatedExchange;
    mEC->OnResponseTimeout = OnResponseTimeout;
#if WEAVE_CONFIG_ENABLE_RELIABLE_MESSAGING
    mEC->OnSendError       = OnSendError;
    mEC->OnAckRcvd         = NULL;
#endif

    inParam.mExchangeStart.mEC     = mEC;
    inParam.mExchangeStart.mClient = this;

    // NOTE: app layer is not supposed to change state/ref count in this callback
    mEventCallback(mAppState, kEvent_OnExchangeStart, inParam, outParam);

exit:
    WeaveLogFunctError(err);

    _Release();

    return err;
}

void SubscriptionClient::FlushExistingExchangeContext(const bool aAbortNow)
{
    if (NULL != mEC)
    {
        mEC->AppState          = NULL;
        mEC->OnMessageReceived = NULL;
        mEC->OnResponseTimeout = NULL;
#if WEAVE_CONFIG_ENABLE_RELIABLE_MESSAGING
        mEC->OnSendError       = NULL;
        mEC->OnAckRcvd         = NULL;
#endif
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

/**
 * Gracefully end a client subscription
 *
 * Gracefully terminates the client end of a subscription.  If subscription cancel
 * support is enabled, a SubscribeCancelRequest message is sent to the subscription
 * publisher and the system awaits a reply before terminating the subscription;
 * otherwise the subscription is immediately terminated in a similar manner to
 * AbortSubscription().  If a mutual subscription exists, the counter subscription
 * is terminated as well.
 *
 * While awaiting a response to a SubscribeCancelRequest, the \c SubscriptionClient
 * enters the \c Canceling state.
 *
 * Once the termination process begins, the \c SubscriptionClient object enters the
 * `Terminated` state and an \c OnSubscriptionTerminated event is delivered to
 * the application's event handler.  Note that, if cancel support is _not_ enabled,
 * the event handler may be called synchronously within the call to EndSubscription().
 *
 * After the application's event handler returns, the \c SubscriptionClient object enters
 * the `Initialized` state.   At this point the \c SubscriptionClient object may be used
 * to initiate another subscription, or it may be freed by calling the Free() method.
 */
CHIP_ERROR SubscriptionClient::EndSubscription()
{
    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), __func__, mRefCount);

#if WDM_ENABLE_SUBSCRIPTION_CANCEL

    CHIP_ERROR err       = WEAVE_NO_ERROR;
    PacketBuffer * msgBuf = NULL;
    chip::TLV::TLVWriter writer;
    SubscribeCancelRequest::Builder request;

    // Make sure we're not freed by accident.
    _AddRef();

    mConfig = kConfig_Down;

    switch (mCurrentState)
    {
    case kState_Subscribing:
    case kState_Resubscribe_Holdoff:
        // fall through
    case kState_Subscribing_IdAssigned:

        // If the subscription is not full established, simply terminate it without
        // informing the peer.
        TerminateSubscription(WEAVE_NO_ERROR, NULL, false);
        break;

    case kState_SubscriptionEstablished_Confirming:
        // forget we're in the middle of confirmation, as the outcome
        // has become irrelevant
        FlushExistingExchangeContext();

        // fall through

    case kState_SubscriptionEstablished_Idle:
        msgBuf = PacketBuffer::NewWithAvailableSize(request.kBaseMessageSubscribeId_PayloadLen);
        VerifyOrExit(NULL != msgBuf, err = CHIP_ERROR_NO_MEMORY);

        err = ReplaceExchangeContext();
        SuccessOrExit(err);

        writer.Init(msgBuf);
        request.Init(&writer);
        err = request.SubscriptionID(mSubscriptionId).EndOfRequest().GetError();
        SuccessOrExit(err);

        err = writer.Finalize();
        SuccessOrExit(err);

        // NOTE: State could be changed if there is a sync error callback from message layer
        err    = mEC->SendMessage(chip::Profiles::kWeaveProfile_WDM, kMsgType_SubscribeCancelRequest, msgBuf,
                                  chip::ExchangeContext::kSendFlag_ExpectResponse);
        msgBuf = NULL;
        SuccessOrExit(err);

        MoveToState(kState_Canceling);
        break;

    // Cancel is not supported in any other state
    default:
        ExitNow(err = CHIP_ERROR_INCORRECT_STATE);
    }

exit:
    WeaveLogFunctError(err);

    if (NULL != msgBuf)
    {
        PacketBuffer::Free(msgBuf);
        msgBuf = NULL;
    }

    _Release();

    return err;

#else // WDM_ENABLE_SUBSCRIPTION_CANCEL

    // When Cancel support is not enabled, simply terminate the subscription without
    // informing the peer.
    mConfig = kConfig_Down;
    TerminateSubscription(WEAVE_NO_ERROR, NULL, false);
    return WEAVE_NO_ERROR;

#endif // WDM_ENABLE_SUBSCRIPTION_CANCEL
}

void SubscriptionClient::_Cleanup(void)
{
    if (mBinding)
    {
        mBinding->SetProtocolLayerCallback(NULL, NULL);
        mBinding->Release();
        mBinding = NULL;
    }

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
    mUpdateClient.Shutdown();

    mDataSinkCatalog->Iterate(CleanupUpdatableSinkTrait, this);
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE
}

/**
 * Abort a client subscription
 *
 * Terminates the client end of a subscription, without notifying the subscription
 * publisher and without delivering an \c OnSubscriptionTerminated event to the
 * application's event handler.  If a mutual subscription exists, the counter
 * subscription is terminated as well.
 *
 * Upon calling AbortSubscription(), the \c SubscriptionClient object enters the
 * `Terminated` state. Once the termination process completes, the object enters
 * the `Initialized` state. Both transitions happen synchronously within the call
 * to AbortSubscription().
 *
 * After AbortSubscription() returns, the \c SubscriptionClient object may be used to
 * initiate another subscription, or it may be freed by calling the Free() method.
 */
void SubscriptionClient::AbortSubscription(void)
{
    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), __func__, mRefCount);

    // Immediately terminate any active or in progress subscription.  Since Abort
    // is always synchronous in nature, suppress the OnSubscriptionTerminated callback
    // to the application (but not the SubscriptionTerminated event to the trait handlers).
    mConfig = kConfig_Down;
    TerminateSubscription(WEAVE_NO_ERROR, NULL, true);
}

void SubscriptionClient::TerminateSubscription(CHIP_ERROR aReason, StatusReporting::StatusReport * aStatusReport, bool suppressAppCallback)
{
    // If the SubscriptionClient is active...
    if (mCurrentState != kState_Initialized && mCurrentState != kState_Terminated)
    {
        const ClientState prevState = mCurrentState;

        WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                       GetStateStr(), __func__, mRefCount);

        // Ensure that the client object isn't freed while any callbacks are active.
        _AddRef();

        // Abort any in-progress exchange.
        FlushExistingExchangeContext(true);

        // Stop the subscription timer
        CancelSubscriptionTimer();

        MoveToState(kState_Terminated);

        if (prevState >= kState_Subscribing && prevState <= kState_Canceling)
        {
#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
            const uint64_t subscriptionId = mSubscriptionId;
            const uint64_t peerNodeId = mBinding->GetPeerNodeId();
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

            // Deliver SubscriptionTerminated event to trait handlers.
            if (NULL != mDataSinkCatalog)
            {
                mDataSinkCatalog->DispatchEvent(TraitDataSink::kEventSubscriptionTerminated, NULL);
            }

            // Deliver OnSubscriptionTerminated event to application.
            if (NULL != mEventCallback && !suppressAppCallback)
            {
                InEventParam inParam;
                OutEventParam outParam;

                inParam.Clear();
                outParam.Clear();

                inParam.mSubscriptionTerminated.mReason            = aReason;
                inParam.mSubscriptionTerminated.mClient            = this;
                inParam.mSubscriptionTerminated.mWillRetry         = (ShouldSubscribe() && IsRetryEnabled());
                if (aStatusReport != NULL)
                {
                    inParam.mSubscriptionTerminated.mIsStatusCodeValid = true;
                    inParam.mSubscriptionTerminated.mStatusProfileId   = aStatusReport->mProfileId;
                    inParam.mSubscriptionTerminated.mStatusCode        = aStatusReport->mStatusCode;
                    inParam.mSubscriptionTerminated.mAdditionalInfoPtr = &(aStatusReport->mAdditionalInfo);
                }

                mEventCallback(mAppState, kEvent_OnSubscriptionTerminated, inParam, outParam);
            }

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
            SubscriptionEngine::GetInstance()->UpdateHandlerLiveness(peerNodeId, subscriptionId, true);
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER
        }

        if (mCurrentState == kState_Terminated)
        {
            if (ShouldSubscribe() && IsRetryEnabled())
            {
                SetRetryTimer(aReason);
            }
            else
            {
                MoveToState(kState_Initialized);
                mRetryCounter = 0;
                mSubscriptionId = 0;
            }
        }

        _Release();
    }
}

void SubscriptionClient::SetRetryTimer(CHIP_ERROR aReason)
{
    CHIP_ERROR err                   = WEAVE_NO_ERROR;

    _AddRef();

    // this check serves to see whether we already have a timer set
    // and if resubscribes are enabled
    if (ShouldSubscribe() && IsRetryEnabled() && mCurrentState != kState_Resubscribe_Holdoff)
    {
        uint32_t timeoutMsec = 0;

        MoveToState(kState_Resubscribe_Holdoff);

        ResubscribeParam param;
        param.mNumRetries = mRetryCounter;
        param.mReason     = aReason;
        param.mRequestType = ResubscribeParam::kSubscription;

        mResubscribePolicyCallback(mAppState, param, timeoutMsec);

        err = SubscriptionEngine::GetInstance()->GetExchangeManager()->MessageLayer->SystemLayer->StartTimer(timeoutMsec,
                                                                                                             OnTimerCallback, this);
        SuccessOrExit(err);

        WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d) timeout: %u",
                       SubscriptionEngine::GetInstance()->GetClientId(this), GetStateStr(), __func__, mRefCount, timeoutMsec);
    }

exit:
    // all errors are considered fatal in this function
    if (err != WEAVE_NO_ERROR)
    {
        mConfig = kConfig_Down;
        TerminateSubscription(err, NULL, false);
    }

    _Release();
}

void SubscriptionClient::CancelSubscriptionTimer(void)
{
    SubscriptionEngine::GetInstance()->GetExchangeManager()->MessageLayer->SystemLayer->CancelTimer(OnTimerCallback, this);
}

/**
 * Free a \c SubscriptionClient object.
 *
 * Frees the \c SubscriptionClient object.  If a subscription is active or in-progress, the
 * subscription is immediately terminated in a similar manner to calling AbortSubscription().
 * If any update requests are in progress, they are similarly aborted.
 *
 * The application is responsible for calling Free() exactly once during the lifetime of
 * a \c SubscriptionClient object. After Free() is called, no further references should be
 * made to the object.
 */
void SubscriptionClient::Free()
{
    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), __func__, mRefCount);

    WeaveLogIfFalse(kState_Free != mCurrentState);
    WeaveLogIfFalse(mRefCount > 0);

    // Abort the subscription if active.
    AbortSubscription();

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
    AbortUpdates(WEAVE_NO_ERROR);
#endif

    // If mRefCount == 1, _Release would decrement it to 0, call _Cleanup and move us to FREE state
    _Release();
}

void SubscriptionClient::BindingEventCallback(void * const aAppState, const Binding::EventType aEvent,
                                              const Binding::InEventParam & aInParam, Binding::OutEventParam & aOutParam)
{
    SubscriptionClient * const pClient = reinterpret_cast<SubscriptionClient *>(aAppState);

    pClient->_AddRef();

    switch (aEvent)
    {
    case Binding::kEvent_BindingReady:
#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
        if (pClient->IsUpdatePendingOrInProgress())
        {
            if (false == pClient->IsUpdateInFlight())
            {
                pClient->StartUpdateRetryTimer(WEAVE_NO_ERROR);
            }
        }
#endif
        // The binding is ready.  If the SubscriptionClient is still in a state where
        // a subscription is desired, go send the subscription request now.
        if (pClient->mCurrentState == kState_Initialized && pClient->ShouldSubscribe())
        {
            pClient->_InitiateSubscription();
        }
        break;

    case Binding::kEvent_PrepareFailed:

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
        if (pClient->IsUpdatePendingOrInProgress())
        {
            pClient->StartUpdateRetryTimer(aInParam.PrepareFailed.Reason);
        }
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE

        // Binding preparation failed.  If the SubscriptionClient is still in a state where
        // a subscription is desired, arm the subscription retry timer.
        if (pClient->mCurrentState == kState_Initialized && pClient->ShouldSubscribe())
        {
            pClient->SetRetryTimer(aInParam.PrepareFailed.Reason);
        }

        break;

    case Binding::kEvent_BindingFailed:

        // The binding has failed.  This can happen because an underling connection has closed,
        // or a security session has failed.

        // Cancel any in-progress Update request and arrange to re-try it after a delay.
#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
        pClient->mUpdateClient.CancelUpdate();
        if (pClient->IsUpdatePendingOrInProgress())
        {
            pClient->StartUpdateRetryTimer(aInParam.BindingFailed.Reason);
        }
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE

        // If a subscription is in-progress, established or being canceled, terminate the
        // subscription immediately. Do nothing if the SubscriptionClient is idle (Initialized)
        // or waiting to re-subscribe (Resubscribe_Holdoff). In those cases, when the time arrives
        // to subscribe again, the binding will be re-prepared.
        if (pClient->IsInProgressOrEstablished() || pClient->mCurrentState == kState_Canceling)
        {
            pClient->TerminateSubscription(aInParam.BindingFailed.Reason, NULL, false);
        }

        break;

    default:
        Binding::DefaultEventHandler(aAppState, aEvent, aInParam, aOutParam);
    }

    pClient->_Release();
}

void SubscriptionClient::OnTimerCallback(System::Layer * aSystemLayer, void * aAppState, System::Error)
{
    SubscriptionClient * const pClient = reinterpret_cast<SubscriptionClient *>(aAppState);

    pClient->TimerEventHandler();
}

CHIP_ERROR SubscriptionClient::RefreshTimer(void)
{
    CHIP_ERROR err      = WEAVE_NO_ERROR;
    bool isTimerNeeded   = false;
    uint32_t timeoutMsec = 0;

    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), __func__, mRefCount);

    // Cancel timer first
    CancelSubscriptionTimer();

    // Arm timer according to current state
    switch (mCurrentState)
    {
    case kState_Subscribing:
    case kState_Subscribing_IdAssigned:
        if (kNoTimeOut != mInactivityTimeoutDuringSubscribingMsec)
        {
            // Note that loss of range is not expected, as ExchangeManager::Timeout is indeed uint32_t
            timeoutMsec   = mInactivityTimeoutDuringSubscribingMsec;
            isTimerNeeded = true;

            WeaveLogDetail(DataManagement,
                           "Client[%u] [%5.5s] %s Ref(%d) Set inactivity time limit during subscribing to %" PRIu32 " msec",
                           SubscriptionEngine::GetInstance()->GetClientId(this), GetStateStr(), __func__, mRefCount, timeoutMsec);
        }
        break;
    case kState_SubscriptionEstablished_Idle:
        if (kNoTimeOut != mLivenessTimeoutMsec)
        {
            timeoutMsec = mLivenessTimeoutMsec;
            isTimerNeeded = true;

#if WEAVE_CONFIG_ENABLE_RELIABLE_MESSAGING

            // If the subscription is over Weave Reliable Messaging and the local node
            // is the subscription initiator...
            if (IsInitiator() && mBinding->IsWRMTransport())
            {
                // Calculate margin to reserve for WRM activity, so we send out SubscribeConfirm earlier
                // Note that wrap around could happen, if the system is configured with excessive delays and number of retries
                const chip::WRMPConfig & defaultWRMPConfig = mBinding->GetDefaultWRMPConfig();
                const uint32_t marginMsec = (defaultWRMPConfig.mMaxRetrans + 1) * defaultWRMPConfig.mInitialRetransTimeout;

                // If the margin is smaller than the desired liveness timeout, set a timer for the difference.
                // Otherwise, fail with an error.
                if (marginMsec < timeoutMsec)
                {
                    timeoutMsec = timeoutMsec - marginMsec;
                }
                else
                {
                    // This is a system configuration problem
                    WeaveLogDetail(DataManagement,
                                   "Client[%u] Liveness period (%" PRIu32 " msec) <= margin reserved for WRM (%" PRIu32 " msec)",
                                   SubscriptionEngine::GetInstance()->GetClientId(this), mLivenessTimeoutMsec, marginMsec);

                    ExitNow(err = CHIP_ERROR_TIMEOUT);
                }
            }

#endif // WEAVE_CONFIG_ENABLE_RELIABLE_MESSAGING

            WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d) Set timer for liveness confirmation to %" PRIu32 " msec",
                           SubscriptionEngine::GetInstance()->GetClientId(this), GetStateStr(), __func__, mRefCount, timeoutMsec);
        }
        break;
    case kState_SubscriptionEstablished_Confirming:
        // Do nothing
        break;
    default:
        ExitNow(err = CHIP_ERROR_INCORRECT_STATE);
    }

    if (isTimerNeeded)
    {
        err = SubscriptionEngine::GetInstance()->GetExchangeManager()->MessageLayer->SystemLayer->StartTimer(timeoutMsec,
                                                                                                             OnTimerCallback, this);

        VerifyOrExit(WEAVE_SYSTEM_NO_ERROR == err, /* no-op */);
    }

exit:
    WeaveLogFunctError(err);

    return err;
}

void SubscriptionClient::TimerEventHandler(void)
{
    CHIP_ERROR err       = WEAVE_NO_ERROR;
    PacketBuffer * msgBuf = NULL;
    bool skipTimerCheck   = false;

    if ((0 == mRefCount) || (mCurrentState < kState_TimerTick_Begin) || (mCurrentState > kState_TimerTick_End))
    {
        skipTimerCheck = true;
        ExitNow();
    }

    // Make sure we're not freed by accident
    _AddRef();

    switch (mCurrentState)
    {
    case kState_Subscribing:
    case kState_Subscribing_IdAssigned:
        WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d) Timeout for subscribing phase, abort",
                       SubscriptionEngine::GetInstance()->GetClientId(this), GetStateStr(), __func__, mRefCount);

        ExitNow(err = CHIP_ERROR_TIMEOUT);
        break;

    case kState_SubscriptionEstablished_Idle:
        if (IsInitiator())
        {
            WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d) Confirming liveness",
                           SubscriptionEngine::GetInstance()->GetClientId(this), GetStateStr(), __func__, mRefCount);

            // timeout for liveness check
            chip::TLV::TLVWriter writer;
            SubscribeConfirmRequest::Builder request;
            msgBuf = PacketBuffer::NewWithAvailableSize(request.kBaseMessageSubscribeId_PayloadLen);
            VerifyOrExit(NULL != msgBuf, err = CHIP_ERROR_NO_MEMORY);

            err = ReplaceExchangeContext();
            SuccessOrExit(err);

            writer.Init(msgBuf);
            request.Init(&writer);
            err = request.SubscriptionID(mSubscriptionId).EndOfRequest().GetError();
            SuccessOrExit(err);

            err = writer.Finalize();
            SuccessOrExit(err);

            // NOTE: State could be changed if there is a send error callback from message layer
            err    = mEC->SendMessage(chip::Profiles::kWeaveProfile_WDM, kMsgType_SubscribeConfirmRequest, msgBuf,
                                      chip::ExchangeContext::kSendFlag_ExpectResponse);
            msgBuf = NULL;
            SuccessOrExit(err);

            if (kState_SubscriptionEstablished_Idle == mCurrentState)
            {
                MoveToState(kState_SubscriptionEstablished_Confirming);
            }
            else
            {
                // state has changed, probably because some callback from message layer
                ExitNow(err = CHIP_ERROR_INCORRECT_STATE);
            }
        }
        else
        {
            // We are not the initiator, so we cannot send out the subscribe confirm
            WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d) Timeout",
                           SubscriptionEngine::GetInstance()->GetClientId(this), GetStateStr(), __func__, mRefCount);

            // abort the subscription as we've timed out
            ExitNow(err = CHIP_ERROR_TIMEOUT);
        }
        break;

    case kState_Resubscribe_Holdoff:
        mRetryCounter++;

        MoveToState(kState_Initialized);

        if (ShouldSubscribe())
        {
            _InitiateSubscription();
        }
        break;

    default:
        WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d) Timer event fired at wrong state, ignore",
                       SubscriptionEngine::GetInstance()->GetClientId(this), GetStateStr(), __func__, mRefCount);
        break;
    }

exit:
    WeaveLogFunctError(err);

    if (NULL != msgBuf)
    {
        PacketBuffer::Free(msgBuf);
        msgBuf = NULL;
    }

    if (err != WEAVE_NO_ERROR)
    {
        TerminateSubscription(err, NULL, false);
    }

    if (!skipTimerCheck)
    {
        _Release();
    }
}

CHIP_ERROR SubscriptionClient::ProcessDataList(chip::TLV::TLVReader & aReader)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    AlwaysAcceptDataElementAccessControlDelegate acDelegate;

    err = SubscriptionEngine::ProcessDataList(aReader, mDataSinkCatalog, mPrevIsPartialChange, mPrevTraitDataHandle, acDelegate);
    SuccessOrExit(err);

exit:
    return err;
}

void SubscriptionClient::NotificationRequestHandler(chip::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                                    const chip::WeaveMessageInfo * aMsgInfo, PacketBuffer * aPayload)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    InEventParam inParam;
    OutEventParam outParam;
    NotificationRequest::Parser notify;
    const ClientState StateWhenEntered = mCurrentState;
    chip::TLV::TLVReader reader;
    bool isDataListPresent = false;
#if WEAVE_CONFIG_SERIALIZATION_ENABLE_DESERIALIZATION
    bool isEventListPresent = false;
#endif
    uint8_t statusReportLen = 6;
    bool incomingEC = (mEC != aEC);
    PacketBuffer * msgBuf   = PacketBuffer::NewWithAvailableSize(statusReportLen);

    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), __func__, mRefCount);

    // Make sure we're not freed by accident
    _AddRef();

    if (incomingEC)
    {
        // only re-configure if this is an incoming EC
        mBinding->AdjustResponseTimeout(aEC);
    }

    VerifyOrExit(NULL != msgBuf, err = CHIP_ERROR_NO_MEMORY);

    // skip the first 6 bytes in status report, as they are reserved for the profile ID and status code
    msgBuf->SetDataLength(statusReportLen);

    switch (StateWhenEntered)
    {
    case kState_Subscribing:
    case kState_Subscribing_IdAssigned:
        // In these two states, all notifications must come in the same exchange context
        VerifyOrExit(aEC == mEC, err = CHIP_ERROR_INCORRECT_STATE);

        // refresh inactivity monitor every time we receive a notification request
        err = RefreshTimer();
        SuccessOrExit(err);
        break;

    case kState_SubscriptionEstablished_Idle:
    case kState_SubscriptionEstablished_Confirming:

        // refresh inactivity monitor every time we receive a notification request
        err = RefreshTimer();
        SuccessOrExit(err);

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
        SubscriptionEngine::GetInstance()->UpdateHandlerLiveness(mBinding->GetPeerNodeId(), mSubscriptionId);
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

        break;

    // we are going to ignore any notification requests in other states
    default:
        ExitNow();
    }

    // Emit an OnSubscriptionActivity event
    inParam.Clear();
    inParam.mSubscriptionActivity.mClient = this;
    mEventCallback(mAppState, kEvent_OnSubscriptionActivity, inParam, outParam);

    inParam.Clear();
    outParam.Clear();
    inParam.mNotificationRequest.mEC      = aEC;
    inParam.mNotificationRequest.mMessage = aPayload;
    inParam.mNotificationRequest.mClient  = this;

    // NOTE: state could be changed in the callback to app layer
    mEventCallback(mAppState, kEvent_OnNotificationRequest, inParam, outParam);

    mDataSinkCatalog->DispatchEvent(TraitDataSink::kEventNotifyRequestBegin, NULL);

    // jump to Exit if the state has been changed in the callback to app layer
    VerifyOrExit(StateWhenEntered == mCurrentState, /* no-op */);

    reader.Init(aPayload);
    reader.Next();

    err = notify.Init(reader);
    SuccessOrExit(err);

#if WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK
    // simple schema checking
    err = notify.CheckSchemaValidity();
    SuccessOrExit(err);
#endif // WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK

#if WEAVE_CONFIG_SERIALIZATION_ENABLE_DESERIALIZATION
    {
        EventList::Parser eventList;

        err = notify.GetEventList(&eventList);
        if (WEAVE_NO_ERROR == err)
        {
            isEventListPresent = true;
        }
        else if (WEAVE_END_OF_TLV == err)
        {
            isEventListPresent = false;
            err                = WEAVE_NO_ERROR;
        }
        SuccessOrExit(err);

        // re-initialize the reader (reuse to save stack depth).
        eventList.GetReader(&reader);
    }

    if (isEventListPresent)
    {
        inParam.mEventStreamReceived.mReader = &reader;
        inParam.mEventStreamReceived.mClient = this;

        // Invoke our callback.
        mEventCallback(mAppState, kEvent_OnEventStreamReceived, inParam, outParam);
    }
#endif // WEAVE_CONFIG_SERIALIZATION_ENABLE_DESERIALIZATION

    // TODO: As I've commented in Weave PR#614, there is no support for event sink

    inParam.mNotificationProcessed.mClient = this;

    // NOTE: state could be changed in the callback to app layer
    mEventCallback(mAppState, kEvent_OnNotificationProcessed, inParam, outParam);

    mDataSinkCatalog->DispatchEvent(TraitDataSink::kEventNotifyRequestEnd, NULL);

    // jump to Exit if the state has been changed in the callback to app layer
    VerifyOrExit(StateWhenEntered == mCurrentState, /* no-op */);

    {
        uint8_t * p = msgBuf->Start();
        chip::Encoding::LittleEndian::Write32(p, chip::Profiles::kWeaveProfile_Common);
        chip::Encoding::LittleEndian::Write16(p, chip::Profiles::Common::kStatus_Success);

        err    = aEC->SendMessage(chip::Profiles::kWeaveProfile_Common, chip::Profiles::Common::kMsgType_StatusReport, msgBuf);
        msgBuf = NULL;
        SuccessOrExit(err);
    }

exit:
    WeaveLogFunctError(err);

    if (NULL != msgBuf)
    {
        PacketBuffer::Free(msgBuf);
        msgBuf = NULL;
    }

    if (NULL != aPayload)
    {
        PacketBuffer::Free(aPayload);
        aPayload = NULL;
    }

    // If this is not a locally initiated exchange, always close the exchange
    if (incomingEC)
    {
        aEC->Close();
        aEC = NULL;
    }

    if (WEAVE_NO_ERROR != err)
    {
        // If we're not aborted yet, make a callback to app layer
        TerminateSubscription(err, NULL, false);
    }

    _Release();
}

#if WDM_ENABLE_SUBSCRIPTION_CANCEL
void SubscriptionClient::CancelRequestHandler(chip::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                              const chip::WeaveMessageInfo * aMsgInfo, PacketBuffer * aPayload)
{
    CHIP_ERROR err         = WEAVE_NO_ERROR;
    uint8_t statusReportLen = 6;
    PacketBuffer * msgBuf   = PacketBuffer::NewWithAvailableSize(statusReportLen);
    uint8_t * p;
    bool canceled          = true;
    uint32_t statusProfile = chip::Profiles::kWeaveProfile_Common;
    uint16_t statusCode    = chip::Profiles::Common::kStatus_Success;

    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(this),
                   GetStateStr(), __func__, mRefCount);

    // Make sure we're not freed by accident
    _AddRef();

    VerifyOrExit(NULL != msgBuf, err = CHIP_ERROR_NO_MEMORY);

    // Verify the cancel request is truly from the publisher.  If not, reject the request with
    // "invalid subscription id" to avoid revealing the existence of the subscription.
    if (!mBinding->IsAuthenticMessageFromPeer(aMsgInfo))
    {
        WeaveLogDetail(DataManagement, "Rejecting SubscribeCancelRequest from unauthorized source");
        canceled      = false;
        statusProfile = chip::Profiles::kWeaveProfile_WDM;
        statusCode    = kStatus_InvalidSubscriptionID;
    }

    p = msgBuf->Start();
    chip::Encoding::LittleEndian::Write32(p, statusProfile);
    chip::Encoding::LittleEndian::Write16(p, statusCode);
    msgBuf->SetDataLength(statusReportLen);

    err    = aEC->SendMessage(chip::Profiles::kWeaveProfile_Common, chip::Profiles::Common::kMsgType_StatusReport, msgBuf);
    msgBuf = NULL;
    SuccessOrExit(err);

    // Proactively close the exchange.  This prevents the call to TerminateSubscription()
    // below from aborting the exchange, which, when using WRM, would prevent the
    // StatusReport message from being re-transmitted if necessary.
    FlushExistingExchangeContext();

exit:
    WeaveLogFunctError(err);

    PacketBuffer::Free(msgBuf);

    // If the subscription was canceled, or if an error occurred while handing
    // the Cancel request, terminate the subscription and notify the application.
    if (canceled || WEAVE_NO_ERROR != err)
    {
        mConfig = kConfig_Down;
        TerminateSubscription(err, NULL, false);
    }

    _Release();
}
#endif // WDM_ENABLE_SUBSCRIPTION_CANCEL

void SubscriptionClient::OnSendError(ExchangeContext * aEC, CHIP_ERROR aErrorCode, void * aMsgSpecificContext)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    IgnoreUnusedVariable(aMsgSpecificContext);
    SubscriptionClient * const pClient = reinterpret_cast<SubscriptionClient *>(aEC->AppState);
    bool subscribeRequestFailed        = false;

    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(pClient),
                   pClient->GetStateStr(), __func__, pClient->mRefCount);

    // Make sure we're not freed by accident
    pClient->_AddRef();

    switch (pClient->mCurrentState)
    {
    case kState_Subscribing:
    case kState_Subscribing_IdAssigned:
        // Subscribe request failed, deliver mSubscriptionRequestFailedEventParam
        subscribeRequestFailed = true;
        ExitNow(err = aErrorCode);
        break;

    case kState_SubscriptionEstablished_Confirming:
        // Subscribe Confirm request failed, so no point trying to send a cancel.
        // Go ahead and terminate it.
        ExitNow(err = aErrorCode);
        break;

    case kState_Resubscribe_Holdoff:
        // OnResponseTimeout posts an error to OnSendError (this function).
        // That can happen after we've already received a cb for OnSendError.
        // So if we've already set a timeout, then we can ignore this error.
        if (aErrorCode == CHIP_ERROR_TIMEOUT)
        {
            err = WEAVE_NO_ERROR;
        }
        break;

    case kState_Canceling:
        ExitNow(err = aErrorCode);
        break;

    // In any of these states, we must not see this callback
    default:
        ExitNow(err = CHIP_ERROR_INCORRECT_STATE);
    }

exit:
    WeaveLogFunctError(err);

    if ((subscribeRequestFailed) || (WEAVE_NO_ERROR != err))
    {
        pClient->TerminateSubscription(err, NULL, false);
    }

    pClient->_Release();
}

void SubscriptionClient::OnResponseTimeout(chip::ExchangeContext * aEC)
{
    SubscriptionClient * const pClient = reinterpret_cast<SubscriptionClient *>(aEC->AppState);

    IgnoreUnusedVariable(pClient);

    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(pClient),
                   pClient->GetStateStr(), __func__, pClient->mRefCount);

    OnSendError(aEC, CHIP_ERROR_TIMEOUT, NULL);
}

void SubscriptionClient::OnMessageReceivedFromLocallyInitiatedExchange(chip::ExchangeContext * aEC,
                                                                       const nl::Inet::IPPacketInfo * aPktInfo,
                                                                       const chip::WeaveMessageInfo * aMsgInfo,
                                                                       uint32_t aProfileId, uint8_t aMsgType,
                                                                       PacketBuffer * aPayload)
{
    // Notification Requests during initial setup
    // Subscribe response
    // Status Report for Subscribe request
    // Status Report for Subscribe Cancel request
    // Status Report for Subscribe Confirm request

    CHIP_ERROR err              = WEAVE_NO_ERROR;
    SubscriptionClient * pClient = reinterpret_cast<SubscriptionClient *>(aEC->AppState);
    bool terminateSubscription = false;
    bool retainExchangeContext = false;
    bool isStatusReportValid   = false;
    chip::Profiles::StatusReporting::StatusReport status;

    WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] %s Ref(%d)", SubscriptionEngine::GetInstance()->GetClientId(pClient),
                   pClient->GetStateStr(), __func__, pClient->mRefCount);

    // Make sure we're not freed by accident
    pClient->_AddRef();

    WeaveLogIfFalse(aEC == pClient->mEC);

    if ((chip::Profiles::kWeaveProfile_Common == aProfileId) &&
        (chip::Profiles::Common::kMsgType_StatusReport == aMsgType))
    {
        // Note that payload is not freed in this call to parse
        err = chip::Profiles::StatusReporting::StatusReport::parse(aPayload, status);
        SuccessOrExit(err);
        isStatusReportValid = true;
        WeaveLogDetail(DataManagement, "Received StatusReport %s", nl::StatusReportStr(status.mProfileId, status.mStatusCode));
    }

    switch (pClient->mCurrentState)
    {
    case kState_Subscribing:
    case kState_Subscribing_IdAssigned:
        if (isStatusReportValid)
        {
            ExitNow(err = CHIP_ERROR_STATUS_REPORT_RECEIVED);
        }
        else if ((chip::Profiles::kWeaveProfile_WDM == aProfileId) && (kMsgType_NotificationRequest == aMsgType))
        {
            // notification request, don't close the exchange context, for more notification requests might arrive
            // through this same exchange context
            retainExchangeContext = true;
            pClient->NotificationRequestHandler(aEC, aPktInfo, aMsgInfo, aPayload);
            aPayload = NULL;
        }
        else if ((chip::Profiles::kWeaveProfile_WDM == aProfileId) && (kMsgType_SubscribeResponse == aMsgType))
        {
            // capture subscription ID and liveness timeout
            chip::TLV::TLVReader reader;
            reader.Init(aPayload);
            err = reader.Next();
            SuccessOrExit(err);

            SubscribeResponse::Parser response;
            err = response.Init(reader);
            SuccessOrExit(err);

#if WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK
            // simple schema checking
            err = response.CheckSchemaValidity();
            SuccessOrExit(err);
#endif // WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK

            {
                uint64_t subscriptionId;
                err = response.GetSubscriptionID(&subscriptionId);
                SuccessOrExit(err);
                if (kState_Subscribing == pClient->mCurrentState)
                {
                    // capture subscription ID
                    pClient->mSubscriptionId = subscriptionId;
                }
                else
                {
                    // verify they are the same
                    VerifyOrExit(pClient->mSubscriptionId == subscriptionId, err = CHIP_ERROR_INVALID_TLV_ELEMENT);
                }
            }

            if (kState_Subscribing == pClient->mCurrentState)
            {
                uint32_t livenessTimeoutSec;
                err = response.GetSubscribeTimeout(&livenessTimeoutSec);

                if (WEAVE_NO_ERROR == err)
                {
                    VerifyOrExit(livenessTimeoutSec <= kMaxTimeoutSec, err = CHIP_ERROR_INVALID_TLV_ELEMENT);

                    // capture liveness timeout
                    pClient->mLivenessTimeoutMsec = livenessTimeoutSec * 1000;
                }
                else if (WEAVE_END_OF_TLV == err)
                {
                    err = WEAVE_NO_ERROR;
                }
                else
                {
                    ExitNow();
                }
            }

            // Subscribe response, move to alive-idle state (and close the exchange context)
            pClient->MoveToState(kState_SubscriptionEstablished_Idle);

            err = pClient->RefreshTimer();
            SuccessOrExit(err);

            // Release the response buffer before initiating any callbacks to reduce overall
            // buffer pressure.
            PacketBuffer::Free(aPayload);
            aPayload = NULL;

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
            SubscriptionEngine::GetInstance()->UpdateHandlerLiveness(pClient->mBinding->GetPeerNodeId(), pClient->mSubscriptionId);
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

            pClient->mRetryCounter = 0;

            {
                InEventParam inParam;
                OutEventParam outParam;

                // Emit an OnSubscriptionActivity event to the application.
                inParam.mSubscriptionActivity.mClient = pClient;
                pClient->mEventCallback(pClient->mAppState, kEvent_OnSubscriptionActivity, inParam, outParam);

                // Emit an OnSubscriptionEstablished event to the application.
                // Note that it's allowed to cancel or even abandon this subscription right inside this callback.
                inParam.mSubscriptionEstablished.mSubscriptionId = pClient->mSubscriptionId;
                inParam.mSubscriptionEstablished.mClient         = pClient;
                pClient->mEventCallback(pClient->mAppState, kEvent_OnSubscriptionEstablished, inParam, outParam);
            }

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
            pClient->LockUpdateMutex();
            if (false == pClient->IsUpdatePendingOrInProgress())
            {
                if (pClient->CheckForSinksWithDataLoss())
                {
                    ExitNow(err = CHIP_ERROR_WDM_POTENTIAL_DATA_LOSS);
                }
            }
            pClient->UnlockUpdateMutex();
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE

            ExitNow();
        }
        else
        {
            // protocol error
            ExitNow(err = CHIP_ERROR_INVALID_MESSAGE_TYPE);
        }
        break;

    case kState_SubscriptionEstablished_Confirming:

        // Verify the response is a status report.
        VerifyOrExit(isStatusReportValid, err = CHIP_ERROR_INVALID_MESSAGE_TYPE);

        // Verify that response is Success.
        VerifyOrExit(status.success(), err = CHIP_ERROR_STATUS_REPORT_RECEIVED);

        // Close the exchange context and move back to idle state
        pClient->FlushExistingExchangeContext();
        pClient->MoveToState(kState_SubscriptionEstablished_Idle);

        WeaveLogDetail(DataManagement, "Client[%u] [%5.5s] liveness confirmed",
                       SubscriptionEngine::GetInstance()->GetClientId(pClient), pClient->GetStateStr());

        // Emit an OnSubscriptionActivity event
        {
            InEventParam inParam;
            OutEventParam outParam;
            inParam.mSubscriptionActivity.mClient = pClient;
            pClient->mEventCallback(pClient->mAppState, kEvent_OnSubscriptionActivity, inParam, outParam);
        }

        // Restart the subscription timer.
        err = pClient->RefreshTimer();
        SuccessOrExit(err);

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
        SubscriptionEngine::GetInstance()->UpdateHandlerLiveness(pClient->mBinding->GetPeerNodeId(), pClient->mSubscriptionId);
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

        break;

#if WDM_ENABLE_SUBSCRIPTION_CANCEL
    case kState_Canceling:

        // Verify the response is a status report.
        // NOTE: It doesn't really matter what status code we receive from the other end as
        // the subscription is being terminated regardless.
        VerifyOrExit(isStatusReportValid, err = CHIP_ERROR_INVALID_MESSAGE_TYPE);
        terminateSubscription = true;

        break;
#endif // WDM_ENABLE_SUBSCRIPTION_CANCEL

    // We must not receive this callback in any other states
    default:
        WeaveLogDetail(DataManagement, "Received message in some wrong state, ignore");
        ExitNow();
    }

exit:
    WeaveLogFunctError(err);

    // If the exchange is over, close the exchange context.
    if (!retainExchangeContext)
    {
        pClient->FlushExistingExchangeContext();
    }

    // Terminate the subscription if indicated, or if an unexpected error occurred.
    // Pass the status report information to the application's OnSubscriptionTerminated
    // callback if its pertinent in this case.
    if (terminateSubscription || err != WEAVE_NO_ERROR)
    {
        pClient->TerminateSubscription(err, (err == CHIP_ERROR_STATUS_REPORT_RECEIVED) ? &status : NULL, false);
    }

    // Free the message buffer if it hasn't been done already.  Note that in the case the
    // response was a status report, this must be done *after* the call to TerminateSubscription
    // as the StatusReport object that is passed to that method may contain a pointer into
    // the buffer.
    PacketBuffer::Free(aPayload);

    pClient->_Release();
}

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
void SubscriptionClient::StartUpdateRetryTimer(CHIP_ERROR aReason)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    uint32_t timeoutMsec = 0;

    VerifyOrExit(false == mUpdateRetryScheduled, );

    VerifyOrExit(mResubscribePolicyCallback != NULL, WeaveLogDetail(DataManagement, "Update timed out with the retry policy disabled"));

    if (WEAVE_NO_ERROR == aReason)
    {
        mUpdateRetryCounter = 0;
    }
    ResubscribeParam param;
    param.mNumRetries = mUpdateRetryCounter;
    mUpdateRetryCounter++;
    param.mReason     = aReason;
    param.mRequestType = ResubscribeParam::kUpdate;

    mResubscribePolicyCallback(mAppState, param, timeoutMsec);

    WeaveLogDetail(DataManagement, "Will send update in %" PRIu32 " msec", timeoutMsec);

    err = SubscriptionEngine::GetInstance()->GetExchangeManager()->MessageLayer->SystemLayer->StartTimer(timeoutMsec,
            OnUpdateTimerCallback, this);

    if (err != WEAVE_NO_ERROR)
    {
        WeaveDie();
    }

    mUpdateRetryScheduled = true;

exit:
    return;
}

void SubscriptionClient::UpdateTimerEventHandler()
{
    WeaveLogDetail(DataManagement, "%s", __func__);

    mUpdateRetryScheduled = false;

    VerifyOrExit(false == mSuspendUpdateRetries, WeaveLogDetail(DataManagement, "Holding off updates"));

    FormAndSendUpdate();

exit:
    return;
}

void SubscriptionClient::OnUpdateTimerCallback(System::Layer * aSystemLayer, void * aAppState, System::Error)
{
    SubscriptionClient * const pClient = reinterpret_cast<SubscriptionClient *>(aAppState);

    pClient->UpdateTimerEventHandler();
}

void SubscriptionClient::OnUpdateScheduleWorkCallback(System::Layer * aSystemLayer, void * aAppState, System::Error)
{
    SubscriptionClient * const pClient = reinterpret_cast<SubscriptionClient *>(aAppState);

    pClient->LockUpdateMutex();

    // If the application has called AbortUpdates() or Free(), no need to do anything.
    VerifyOrExit(pClient->mUpdateFlushScheduled, );

    pClient->mUpdateFlushScheduled = false;

    // Start the timer with a retry count of zero, unless the timer has already been scheduled.
    // There is some overhead, but this gives the application a chance to add a one-off delay
    // at the beginning of the sequence.
    pClient->StartUpdateRetryTimer(WEAVE_NO_ERROR);

exit:
    pClient->UnlockUpdateMutex();
    pClient->_Release();
}

void SubscriptionClient::SetMaxUpdateSize(const uint32_t aMaxSize)
{
    if (aMaxSize > UINT16_MAX)
        mMaxUpdateSize = 0;
    else
        mMaxUpdateSize = aMaxSize;
}

/**
 * Move paths from the dispatched store back to the pending one.
 * Skip the private ones, as they will be re-added during the recursion.
 */
CHIP_ERROR SubscriptionClient::MoveInProgressToPending(void)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    uint32_t count = 0;
    TraitDataSink *dataSink;
    TraitPath traitPath;

    for (size_t i = mInProgressUpdateList.GetFirstValidItem();
            i < mInProgressUpdateList.GetPathStoreSize();
            i = mInProgressUpdateList.GetNextValidItem(i))
    {
        mInProgressUpdateList.GetItemAt(i, traitPath);

        if ( ! mInProgressUpdateList.AreFlagsSet(i, kFlag_Private))
        {
            // Locate() can return an error if the sink has been removed from the catalog. In that case,
            // skip this path
            if (mDataSinkCatalog->Locate(traitPath.mTraitDataHandle, &dataSink) == WEAVE_NO_ERROR)
            {
                err = AddItemPendingUpdateSet(traitPath, dataSink->GetSchemaEngine());
                SuccessOrExit(err);

                count++;
            }

            mInProgressUpdateList.RemoveItemAt(i);
        }
    }

    // Move the state to Ready only if it was Empty; if the application is adding paths,
    // let it decide when to call FlushUpdate.
    if ((mPendingUpdateSet.GetNumItems() > 0) && (mPendingSetState == kPendingSetEmpty))
    {
        SetPendingSetState(kPendingSetReady);
    }

    // Call clear to remove the private ones as well and anything else.
    mInProgressUpdateList.Clear();

    mUpdateRequestContext.Reset();

exit:
    WeaveLogDetail(DataManagement, "Moved %" PRIu32 " items from InProgress to Pending; err %" PRId32 "", count, err);

    return err;
}

// Move the pending set to the in-progress list, grouping the
// paths by trait instance
CHIP_ERROR SubscriptionClient::MovePendingToInProgress(void)
{
    VerifyOrDie(mInProgressUpdateList.IsEmpty());

    if (mDataSinkCatalog)
    {
        mDataSinkCatalog->Iterate(MovePendingToInProgressUpdatableSinkTrait, this);
    }

    mPendingUpdateSet.Clear();
    SetPendingSetState(kPendingSetEmpty);

    return WEAVE_NO_ERROR;
}

void SubscriptionClient::MovePendingToInProgressUpdatableSinkTrait(void * aDataSink, TraitDataHandle aDataHandle, void * aContext)
{
    SubscriptionClient * subClient = static_cast<SubscriptionClient *>(aContext);
    TraitDataSink * dataSink = static_cast<TraitDataSink *>(aDataSink);
    CHIP_ERROR err = WEAVE_NO_ERROR;
    int count = 0;

    VerifyOrExit(dataSink->IsUpdatableDataSink() == true, /* no error */);

    for (size_t i = subClient->mPendingUpdateSet.GetFirstValidItem(aDataHandle);
            i < subClient->mPendingUpdateSet.GetPathStoreSize();
            i = subClient->mPendingUpdateSet.GetNextValidItem(i, aDataHandle))
    {
        TraitPath traitPath;

        subClient->mPendingUpdateSet.GetItemAt(i, traitPath);

        err = subClient->mInProgressUpdateList.AddItem(traitPath);
        SuccessOrExit(err);
        count++;
    }

exit:
    WeaveLogDetail(DataManagement, "Moved %d items from Pending to InProgress; err %" PRId32 "", count, err);
    return;
}

/**
 * Notify the application for each failed pending path and
 * remove it from the pending set.
 * Return the number of paths removed.
 * Note that the application is allowed to call SetUpdated()
 * from the callback.
 */
void SubscriptionClient::PurgeAndNotifyFailedPaths(CHIP_ERROR aErr, TraitPathStore &aPathStore, size_t &aCount)
{
    TraitPath traitPath;
    TraitUpdatableDataSink *updatableDataSink = NULL;
    aCount = 0;

    for (size_t j = 0; j < aPathStore.GetPathStoreSize(); j++)
    {
        if (! aPathStore.IsItemInUse(j))
        {
            continue;
        }
        if (aPathStore.IsItemFailed(j))
        {
            bool isPrivate = aPathStore.AreFlagsSet(j, kFlag_Private);

            aPathStore.GetItemAt(j, traitPath);
            updatableDataSink = Locate(traitPath.mTraitDataHandle, mDataSinkCatalog);

            if (updatableDataSink == NULL)
            {
                // Locate() can return NULL if the datasink has been removed from the catalog.
                // In that case, remove the stale item in the path store and continue to next entry
                aPathStore.RemoveItemAt(j);
                continue;
            }

            updatableDataSink->ClearVersion();
            updatableDataSink->ClearUpdateRequiredVersion();
            updatableDataSink->SetConditionalUpdate(false);

            aPathStore.RemoveItemAt(j);

            if (false == isPrivate)
            {
                UpdateCompleteEventCbHelper(traitPath,
                        chip::Profiles::kWeaveProfile_Common,
                        chip::Profiles::Common::kStatus_InternalError,
                        aErr,
                        false);
            }
            aCount++;
        }
    }

    if (&aPathStore == &mPendingUpdateSet && aPathStore.IsEmpty())
    {
        SetPendingSetState(kPendingSetEmpty);
    }
    if (&aPathStore == &mInProgressUpdateList)
    {
        mUpdateRequestContext.Reset();
    }

    return;
}

CHIP_ERROR SubscriptionClient::AddItemPendingUpdateSet(const TraitPath &aItem, const TraitSchemaEngine * const aSchemaEngine)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;

    err = mPendingUpdateSet.AddItemDedup(aItem, aSchemaEngine);

    WeaveLogDetail(DataManagement, "%s t%u, p%u, err %d", __func__, aItem.mTraitDataHandle, aItem.mPropertyPathHandle, err);
    return err;
}

void SubscriptionClient::ClearPotentialDataLoss(TraitDataHandle aTraitDataHandle, TraitUpdatableDataSink &aUpdatableSink)
{
    if (aUpdatableSink.IsPotentialDataLoss())
    {
        WeaveLogDetail(DataManagement, "Potential data loss cleared for traitDataHandle: %" PRIu16 ", trait %08x",
                aTraitDataHandle, aUpdatableSink.GetSchemaEngine()->GetProfileId());
    }

    aUpdatableSink.SetPotentialDataLoss(false);
}

void SubscriptionClient::MarkFailedPendingPaths(TraitDataHandle aTraitDataHandle, TraitUpdatableDataSink &aSink, const DataVersion &aLatestVersion)
{
    if (aSink.IsConditionalUpdate() &&
            IsVersionNewer(aLatestVersion, aSink.GetUpdateRequiredVersion()))
    {
        WeaveLogDetail(DataManagement, "<MarkFailedPendingPaths> current version 0x%" PRIx64
                ", valid: %d"
                ", updateRequiredVersion: 0x%" PRIx64
                ", latest known version: 0x%" PRIx64 "",
                aSink.GetVersion(),
                aSink.IsVersionValid(),
                aSink.GetUpdateRequiredVersion(),
                aLatestVersion);

        mPendingUpdateSet.SetFailedTrait(aTraitDataHandle);
    }

    return;
}

bool SubscriptionClient::FilterNotifiedPath(TraitDataHandle aTraitDataHandle,
                                            PropertyPathHandle aLeafPathHandle,
                                            const TraitSchemaEngine * const aSchemaEngine)
{
    bool retval = false;

    retval = mInProgressUpdateList.Includes(TraitPath(aTraitDataHandle, aLeafPathHandle), aSchemaEngine) ||
             mPendingUpdateSet.Includes(TraitPath(aTraitDataHandle, aLeafPathHandle), aSchemaEngine);

    if (retval)
    {
        TraitUpdatableDataSink *updatableDataSink = Locate(aTraitDataHandle, mDataSinkCatalog);

        if (NULL != updatableDataSink && false == updatableDataSink->IsPotentialDataLoss())
        {
            updatableDataSink->SetPotentialDataLoss(true);

            WeaveLogDetail(DataManagement, "Potential data loss set for traitDataHandle: %" PRIu16 ", trait %08x pathHandle: %" PRIu32 "",
                    aTraitDataHandle, aSchemaEngine->GetProfileId(), aLeafPathHandle);
        }
    }

    return retval;
}

void SubscriptionClient::LockUpdateMutex()
{
    if (mUpdateMutex)
    {
        mUpdateMutex->Lock();
    }
}

void SubscriptionClient::UnlockUpdateMutex()
{
    if (mUpdateMutex)
    {
        mUpdateMutex->Unlock();
    }
}

bool SubscriptionClient::WillRetryUpdate(CHIP_ERROR aErr, uint32_t aStatusProfileId, uint16_t aStatusCode)
{
    bool retval = false;

    if (aErr == CHIP_ERROR_TIMEOUT)
    {
        retval = true;
    }

    if (aErr == CHIP_ERROR_STATUS_REPORT_RECEIVED &&
            aStatusProfileId == chip::Profiles::kWeaveProfile_Common &&
              (aStatusCode == chip::Profiles::Common::kStatus_Busy ||
               aStatusCode == chip::Profiles::Common::kStatus_Timeout))
    {
        retval = true;
    }

    return retval;
}

void SubscriptionClient::UpdateCompleteEventCbHelper(const TraitPath &aTraitPath,
                                                     uint32_t aStatusProfileId,
                                                     uint16_t aStatusCode,
                                                     CHIP_ERROR aReason,
                                                     bool aWillRetry)
{
    InEventParam inParam;
    OutEventParam outParam;

    if (aReason == WEAVE_NO_ERROR &&
            false == (aStatusProfileId == chip::Profiles::kWeaveProfile_Common &&
                      aStatusCode == chip::Profiles::Common::kStatus_Success))
    {
        aReason = CHIP_ERROR_STATUS_REPORT_RECEIVED;
    }

    inParam.Clear();
    outParam.Clear();
    inParam.mUpdateComplete.mClient = this;
    inParam.mUpdateComplete.mStatusProfileId = aStatusProfileId;
    inParam.mUpdateComplete.mStatusCode = aStatusCode;
    inParam.mUpdateComplete.mReason = aReason;
    inParam.mUpdateComplete.mTraitDataHandle = aTraitPath.mTraitDataHandle;
    inParam.mUpdateComplete.mPropertyPathHandle = aTraitPath.mPropertyPathHandle;
    inParam.mUpdateComplete.mWillRetry = aWillRetry;

    mEventCallback(mAppState, kEvent_OnUpdateComplete, inParam, outParam);
}

void SubscriptionClient::NoMorePendingEventCbHelper()
{
    InEventParam inParam;
    OutEventParam outParam;

    inParam.Clear();
    outParam.Clear();

    mEventCallback(mAppState, kEvent_OnNoMorePendingUpdates, inParam, outParam);
}

void SubscriptionClient::SetPendingSetState(PendingSetState aState)
{
    if (aState != mPendingSetState)
    {
        WeaveLogDetail(DataManagement, "PendingSetState %d -> %d", mPendingSetState, aState);
    }
    mPendingSetState = aState;
}

// TODO: Break this method down into smaller methods.
void SubscriptionClient::OnUpdateResponse(CHIP_ERROR aReason, chip::Profiles::StatusReporting::StatusReport * apStatus)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    CHIP_ERROR callbackerr;
    TraitPath traitPath;
    TraitUpdatableDataSink * updatableDataSink = NULL;
    UpdateResponse::Parser response;
    ReferencedTLVData additionalInfo;
    StatusList::Parser statusList;
    VersionList::Parser versionList;
    uint64_t versionCreated = 0;
    bool IsVersionListPresent = false;
    bool IsStatusListPresent = false;
    bool wholeRequestSucceeded = false;
    bool needToResubscribe = false;
    uint32_t profileID;
    uint16_t statusCode;
    chip::TLV::TLVReader reader;
    bool isPathSuccessful;
    bool isPathPrivate;
    bool willRetryPath;

    // This method invokes callbacks into the upper layer.
    _AddRef();

    LockUpdateMutex();

    additionalInfo = apStatus->mAdditionalInfo;
    ClearUpdateInFlight();

    if (mUpdateRequestContext.mIsPartialUpdate)
    {
        WeaveLogDetail(DataManagement, "Got StatusReport in the middle of a long update");
    }

    WeaveLogDetail(DataManagement, "Received StatusReport %s", nl::StatusReportStr(apStatus->mProfileId, apStatus->mStatusCode));
    WeaveLogDetail(DataManagement, "Received StatusReport additional info %u",
                   additionalInfo.theLength);

    if (apStatus->mProfileId == chip::Profiles::kWeaveProfile_Common &&
            apStatus->mStatusCode == chip::Profiles::Common::kStatus_Success)
    {
        // If the whole update has succeeded, the status list
        // is allowed to be empty.
        wholeRequestSucceeded = true;

        // Also reset the retry counter.
        mUpdateRetryCounter = 0;
    }

    profileID = apStatus->mProfileId;
    statusCode = apStatus->mStatusCode;

    if (additionalInfo.theLength != 0)
    {
        reader.Init(additionalInfo.theData, additionalInfo.theLength);

        err = reader.Next();
        SuccessOrExit(err);

        err = response.Init(reader);
        SuccessOrExit(err);

#if WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK
        err = response.CheckSchemaValidity();
        SuccessOrExit(err);
#endif

        err = response.GetVersionList(&versionList);
        switch (err)
        {
        case WEAVE_NO_ERROR:
            IsVersionListPresent = true;
            break;
        case WEAVE_END_OF_TLV:
            err = WEAVE_NO_ERROR;
            break;
        default:
            ExitNow();
            break;
        }

        err = response.GetStatusList(&statusList);
        switch (err)
        {
        case WEAVE_NO_ERROR:
            IsStatusListPresent = true;
            break;
        case WEAVE_END_OF_TLV:
            err = WEAVE_NO_ERROR;
            break;
        default:
            ExitNow();
            break;
        }
    }

    if ((wholeRequestSucceeded) && !(IsStatusListPresent && IsVersionListPresent))
    {
        WeaveLogDetail(DataManagement, "<OnUpdateResponse> version/status list missing");
        ExitNow(err = CHIP_ERROR_WDM_MALFORMED_UPDATE_RESPONSE);
    }

    // TODO: validate that the version and status lists are either empty or contain
    // the same number of items as the dispatched list

    for (size_t j = mInProgressUpdateList.GetFirstValidItem();
            j < mInProgressUpdateList.GetPathStoreSize();
            j = mInProgressUpdateList.GetNextValidItem(j))
    {
        if (IsVersionListPresent)
        {
            err = versionList.Next();
            SuccessOrExit(err);

            err = versionList.GetVersion(&versionCreated);
            SuccessOrExit(err);
        }

        if (IsStatusListPresent)
        {
            err = statusList.Next();
            SuccessOrExit(err);

            err = statusList.GetProfileIDAndStatusCode(&profileID, &statusCode);
            SuccessOrExit(err);
        }

        WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_UpdateResponseBusy,
                profileID = chip::Profiles::kWeaveProfile_Common;
                statusCode = chip::Profiles::Common::kStatus_Busy;
                wholeRequestSucceeded = false;
                );

        err = WEAVE_NO_ERROR;

        isPathSuccessful = (profileID == chip::Profiles::kWeaveProfile_Common &&
                            statusCode == chip::Profiles::Common::kStatus_Success);

        callbackerr = isPathSuccessful ? WEAVE_NO_ERROR : CHIP_ERROR_STATUS_REPORT_RECEIVED;

        willRetryPath = WillRetryUpdate(callbackerr, profileID, statusCode);

        isPathPrivate = mInProgressUpdateList.AreFlagsSet(j, kFlag_Private);

        mInProgressUpdateList.GetItemAt(j, traitPath);

        updatableDataSink = Locate(traitPath.mTraitDataHandle, mDataSinkCatalog);

        if (updatableDataSink == NULL)
        {
            // Locate() can return an error if the sink has been removed from the catalog. In that case, ignore this path
            WeaveLogDetail(DataManagement, "item: %zu, traitDataHandle: % potentially removed from the catalog" PRIu16 ", pathHandle: %" PRIu32 "",
                    j, traitPath.mTraitDataHandle, traitPath.mPropertyPathHandle);
            mInProgressUpdateList.RemoveItemAt(j);
            continue;
        }

        WeaveLogDetail(DataManagement, "item: %zu, profile: %" PRIu32 ", statusCode: 0x% " PRIx16 ", version 0x%" PRIx64 "",
                j, profileID, statusCode, versionCreated);
        WeaveLogDetail(DataManagement, "item: %zu, traitDataHandle: %" PRIu16 ", pathHandle: %" PRIu32 "",
                j, traitPath.mTraitDataHandle, traitPath.mPropertyPathHandle);

        if (isPathSuccessful)
        {
            mInProgressUpdateList.RemoveItemAt(j);

            if (updatableDataSink->IsConditionalUpdate())
            {
                if (updatableDataSink->IsVersionValid() &&
                        versionCreated > updatableDataSink->GetVersion() &&
                        updatableDataSink->GetVersion() >= updatableDataSink->GetUpdateStartVersion())
                {
                    updatableDataSink->SetVersion(versionCreated);
                }
                if (mPendingUpdateSet.IsPresent(traitPath))
                {
                    updatableDataSink->SetUpdateRequiredVersion(versionCreated);
                }
                else
                {
                    updatableDataSink->ClearUpdateRequiredVersion();
                    updatableDataSink->SetConditionalUpdate(false);
                }
            }

            if (updatableDataSink->IsPotentialDataLoss() &&
                    updatableDataSink->IsVersionValid() &&
                    versionCreated >= updatableDataSink->GetVersion() &&
                    updatableDataSink->GetVersion() >= updatableDataSink->GetUpdateStartVersion())
            {
                ClearPotentialDataLoss(traitPath.mTraitDataHandle, *updatableDataSink);
            }
        } // Success
        else
        {
            if (profileID == chip::Profiles::kWeaveProfile_WDM &&
                    statusCode == chip::Profiles::DataManagement::kStatus_VersionMismatch)
            {
                mInProgressUpdateList.RemoveItemAt(j);

                // Fail all pending ones as well for VersionMismatch and force resubscribe
                if (mPendingUpdateSet.IsTraitPresent(traitPath.mTraitDataHandle))
                {
                    mPendingUpdateSet.SetFailedTrait(traitPath.mTraitDataHandle);
                }
                updatableDataSink->ClearVersion();
                updatableDataSink->ClearUpdateRequiredVersion();
                updatableDataSink->SetConditionalUpdate(false);
                needToResubscribe = true;
            }
            else
            {
                // If the publisher is busy or has an internal timeout, retry later.
                // Else, throw away all updates in the trait instance.
                if (false == willRetryPath)
                {
                    mInProgressUpdateList.RemoveItemAt(j);

                    if (updatableDataSink->IsConditionalUpdate() &&
                            mPendingUpdateSet.IsTraitPresent(traitPath.mTraitDataHandle))
                    {
                        mPendingUpdateSet.SetFailedTrait(traitPath.mTraitDataHandle);
                        updatableDataSink->ClearUpdateRequiredVersion();
                        updatableDataSink->SetConditionalUpdate(false);
                    }

                    if (updatableDataSink->IsVersionValid())
                    {
                        WeaveLogDetail(DataManagement, "Clearing version for tdh %d, trait %08x",
                                traitPath.mTraitDataHandle,
                                updatableDataSink->GetSchemaEngine()->GetProfileId());

                        updatableDataSink->ClearVersion();
                        needToResubscribe = true;
                    }
                }
            }
        } // Not success

        if (false == isPathPrivate)
        {
            UpdateCompleteEventCbHelper(traitPath, profileID, statusCode, callbackerr, willRetryPath);
            // The application can call DiscardUpdates() from the callback. In that case,
            // the next item in the list will be invalid, and the loop will terminate.
            // Either this method or DiscardUpdates will trigger a resubscription.
        }
    } // for all paths in mInProgressUpdateList

exit:

    if (err != WEAVE_NO_ERROR)
    {
        size_t count;

        // If the loop above exited early for an error, the application
        // is notified for any remaining path by the following method.
        // These paths are not retried.
        mInProgressUpdateList.SetFailed();
        PurgeAndNotifyFailedPaths(err, mInProgressUpdateList, count);
        needToResubscribe = true;
    }
    else
    {
        // Whatever was not discarded above should be retried
        err = MoveInProgressToPending();
        if (err != WEAVE_NO_ERROR)
        {
            AbortUpdates(err);
        }
    }

    mUpdateRequestContext.Reset();

    PurgePendingUpdate();

    if (mPendingSetState == kPendingSetEmpty)
    {
        mUpdateRetryCounter = 0;

        NoMorePendingEventCbHelper();

        if (CheckForSinksWithDataLoss())
        {
            needToResubscribe = true;
        }
    }

    if (mPendingSetState == kPendingSetReady)
    {
        StartUpdateRetryTimer(wholeRequestSucceeded ? WEAVE_NO_ERROR : CHIP_ERROR_STATUS_REPORT_RECEIVED);
    }

    // If we need to resubscribe, bring it down
    if (needToResubscribe && IsInProgressOrEstablished())
    {
        WeaveLogDetail(DataManagement, "UpdateResponse: triggering resubscription");
        TerminateSubscription(err, NULL, false);
    }

    UnlockUpdateMutex();

    WeaveLogFunctError(err);

    _Release();
}

/**
 * This handler is optimized for the case that the request never reached the
 * responder: the dispatched paths are put back in the pending queue and retried.
 */
void SubscriptionClient::OnUpdateNoResponse(CHIP_ERROR aError)
{
    TraitPath traitPath;
    CHIP_ERROR err = WEAVE_NO_ERROR;

    _AddRef();

    LockUpdateMutex();

    ClearUpdateInFlight();

    // Notify the app for all dispatched paths.
    for (size_t j = mInProgressUpdateList.GetFirstValidItem();
            j < mInProgressUpdateList.GetPathStoreSize();
            j = mInProgressUpdateList.GetNextValidItem(j))
    {
        if (! mInProgressUpdateList.AreFlagsSet(j, kFlag_Private))
        {
            mInProgressUpdateList.GetItemAt(j, traitPath);

            UpdateCompleteEventCbHelper(traitPath,
                                        chip::Profiles::kWeaveProfile_Common,
                                        chip::Profiles::Common::kStatus_Success,
                                        aError,
                                        true);
        }
    }

    //Move paths from DispatchedUpdates to PendingUpdates for all TIs.
    err = MoveInProgressToPending();
    if (err != WEAVE_NO_ERROR)
    {
        AbortUpdates(err);
    }
    else
    {
        PurgePendingUpdate();
    }

    if (mPendingUpdateSet.IsEmpty())
    {
        NoMorePendingEventCbHelper();
    }
    else
    {
        StartUpdateRetryTimer(aError);
    }

    UnlockUpdateMutex();

    _Release();
}

void SubscriptionClient::UpdateEventCallback (void * const aAppState,
                                              UpdateClient::EventType aEvent,
                                              const UpdateClient::InEventParam & aInParam,
                                              UpdateClient::OutEventParam & aOutParam)
{
    SubscriptionClient * const pSubClient = reinterpret_cast<SubscriptionClient *>(aAppState);

    switch (aEvent)
    {
    case UpdateClient::kEvent_UpdateComplete:
        WeaveLogDetail(DataManagement, "UpdateComplete event: %d", aEvent);

        if (aInParam.UpdateComplete.Reason == WEAVE_NO_ERROR)
        {
            pSubClient->OnUpdateResponse(aInParam.UpdateComplete.Reason, aInParam.UpdateComplete.StatusReportPtr);
        }
        else
        {
            pSubClient->OnUpdateNoResponse(aInParam.UpdateComplete.Reason);
        }

        break;
    case UpdateClient::kEvent_UpdateContinue:
        WeaveLogDetail(DataManagement, "UpdateContinue event: %d", aEvent);
        pSubClient->ClearUpdateInFlight();
        pSubClient->FormAndSendUpdate();
        break;
    default:
        WeaveLogDetail(DataManagement, "Unknown UpdateClient event: %d", aEvent);
        break;
    }

    return;
}

CHIP_ERROR SubscriptionClient::SetUpdated(TraitUpdatableDataSink * aDataSink, PropertyPathHandle aPropertyHandle, bool aIsConditional)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    TraitDataHandle dataHandle;
    const TraitSchemaEngine * schemaEngine;
    bool needToSetUpdateRequiredVersion = false;
    bool isTraitInstanceInUpdate = false;

    LockUpdateMutex();

    if (aIsConditional)
    {
        if (!aDataSink->IsVersionValid())
        {
            err = CHIP_ERROR_WDM_LOCAL_DATA_INCONSISTENT;
            WeaveLogDetail(DataManagement, "Rejected mutation with error %d", err);
            ExitNow();
        }
    }

    schemaEngine = aDataSink->GetSchemaEngine();

    err = mDataSinkCatalog->Locate(aDataSink, dataHandle);
    SuccessOrExit(err);

    isTraitInstanceInUpdate = mPendingUpdateSet.IsTraitPresent(dataHandle) ||
                              mInProgressUpdateList.IsTraitPresent(dataHandle);

    // It is not supported to mix conditional and non-conditional updates
    // in the same trait.
    if (isTraitInstanceInUpdate)
    {
        VerifyOrExit(aIsConditional == aDataSink->IsConditionalUpdate(), err = CHIP_ERROR_WDM_INCONSISTENT_CONDITIONALITY);
    }
    else
    {
        if (aIsConditional)
        {
            needToSetUpdateRequiredVersion = true;
        }
    }

    err = AddItemPendingUpdateSet(TraitPath(dataHandle, aPropertyHandle), schemaEngine);
    SuccessOrExit(err);


    if (needToSetUpdateRequiredVersion)
    {
        uint64_t requiredDataVersion = aDataSink->GetVersion();
        aDataSink->SetUpdateRequiredVersion(requiredDataVersion);
        WeaveLogDetail(DataManagement, "<SetUpdated> Set update required version to 0x%" PRIx64 "", aDataSink->GetUpdateRequiredVersion());
    }
    aDataSink->SetConditionalUpdate(aIsConditional);

    SetPendingSetState(kPendingSetOpen);

exit:

    UnlockUpdateMutex();

    return err;
}

CHIP_ERROR SubscriptionClient::ClearUpdated(TraitUpdatableDataSink * aDataSink, PropertyPathHandle aPropertyHandle)
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    TraitDataHandle dataHandle;
    bool isTraitInstanceInUpdate = false;

    LockUpdateMutex();

    err = mDataSinkCatalog->Locate(aDataSink, dataHandle);
    SuccessOrExit(err);

    isTraitInstanceInUpdate = mPendingUpdateSet.IsTraitPresent(dataHandle);

    if (!isTraitInstanceInUpdate)
    {
        WeaveLogDetail(DataManagement, "trait %d is not in update pending set, skip ClearUpdated", dataHandle);
        goto exit;
    }

    mPendingUpdateSet.RemoveItem(TraitPath(dataHandle, aPropertyHandle));

exit:
    UnlockUpdateMutex();
    return err;
}

/**
 * Tells the SubscriptionClient to empty the set of TraitPaths pending to be updated and abort the
 * update request that is in progress, if any.
 * This method can be invoked from any callback.
 */
void SubscriptionClient::DiscardUpdates()
{
    AbortUpdates(WEAVE_NO_ERROR);
}

/**
 * Empties the set of TraitPaths pending to be updated and aborts the
 * update request that is in progress, if any.
 * Calls the application callback for every path if the error code passed is not WEAVE_NO_ERROR.
 *
 * Note that this method is written to be reentrant. If this is called internally with an
 * error code, the application can in turn call SetUpdated()/FlushUpdate() or DiscardUpdates()
 * from any of the paths.
 * A call to SetUpdated() will add a path to mPendingUpdateSet as usual, and that path won't be
 * deleted by the AbortUpdates in progress.
 * If the application calls DiscardUpdates, another AbortUpdates will start executing; this will
 * mark any new paths as failed and empty the stores without further callbacks. When the original
 * AbortUpdates resumes, it will find no more paths to notify the application about.
 *
 * @param[in]   aErr    The CHIP_ERROR to notify the application with.
 */
void SubscriptionClient::AbortUpdates(CHIP_ERROR aErr)
{
    size_t numPending = 0;
    size_t numInProgress = 0;
    mResubscribeNeeded = false;

    LockUpdateMutex();

    SubscriptionEngine::GetInstance()->GetExchangeManager()->MessageLayer->SystemLayer->CancelTimer(OnUpdateTimerCallback, this);
    mUpdateRetryScheduled = false;

    mUpdateFlushScheduled = false;

    ClearUpdateInFlight();

    mUpdateClient.CancelUpdate();

    if (mDataSinkCatalog)
    {
        mDataSinkCatalog->Iterate(RefreshUpdatableSinkTrait, this);
    }

    if (mResubscribeNeeded && IsInProgressOrEstablished())
    {
        TerminateSubscription(WEAVE_NO_ERROR, NULL, false);
    }

    // If there's an error code, notify the application
    if (aErr == WEAVE_NO_ERROR)
    {
        numPending = mPendingUpdateSet.GetNumItems();
        mPendingUpdateSet.Clear();
        SetPendingSetState(kPendingSetEmpty);

        numInProgress = mInProgressUpdateList.GetNumItems();
        mInProgressUpdateList.Clear();
    }
    else
    {
        // Note that the application can call DiscardUpdates() or SetUpdated()/FlushUpdate()
        // from any callback.
        // SetUpdated() can re-add paths to the pending list.
        // FlushUpdate() can re-start the retry timer.
        // A call to DiscardUpdates() does nothing because all paths are already marked as failed,
        // unless SetUpdated() has been by a callback for an earlier element.

        mPendingUpdateSet.SetFailed();
        mInProgressUpdateList.SetFailed();
        PurgeAndNotifyFailedPaths(aErr, mPendingUpdateSet, numPending);
        PurgeAndNotifyFailedPaths(aErr, mInProgressUpdateList, numInProgress);
    }

    WeaveLogDetail(DataManagement, "Discarded %" PRIu32 " pending  and %" PRIu32 " inProgress paths",
            numPending, numInProgress);

    UnlockUpdateMutex();

    return;
}

void SubscriptionClient::RefreshUpdatableSinkTrait(void * aDataSink, TraitDataHandle aDataHandle, void * aContext)
{
    SubscriptionClient * subClient = static_cast<SubscriptionClient *>(aContext);
    TraitDataSink * dataSink = static_cast<TraitDataSink *>(aDataSink);
    TraitUpdatableDataSink * updatableDataSink = NULL;
    bool refreshTraitInstance = false;

    VerifyOrExit(dataSink->IsUpdatableDataSink() == true, /* no error */);

    updatableDataSink = static_cast<TraitUpdatableDataSink *>(dataSink);

    if (subClient->mPendingUpdateSet.IsTraitPresent(aDataHandle))
    {
        refreshTraitInstance = true;
    }

    if (subClient->mInProgressUpdateList.IsTraitPresent(aDataHandle))
    {
        refreshTraitInstance = true;
    }

    if (refreshTraitInstance)
    {
        updatableDataSink->SetConditionalUpdate(false);
        updatableDataSink->ClearUpdateRequiredVersion();
        updatableDataSink->ClearVersion();
        subClient->mResubscribeNeeded = true;
    }

exit:
    return;
}

/**
 * Tells the SubscriptionClient to stop retrying update requests.
 * Allows the application to suspend updates for a period of time
 * without discarding all metadata.
 * Updates and retries will be resumed when FlushUpdate is called.
 * When called to suspend updates while an update is in-flight, the update
 * is not canceled but in case it fails it will not be retried until FlushUpdate
 * is called again.
 */
void SubscriptionClient::SuspendUpdateRetries()
{
    LockUpdateMutex();

    SubscriptionEngine::GetInstance()->GetExchangeManager()->MessageLayer->SystemLayer->CancelTimer(OnUpdateTimerCallback, this);
    mUpdateRetryScheduled = false;

    if (false == mSuspendUpdateRetries)
    {
        WeaveLogDetail(DataManagement, "%s false -> true", __func__);

        mSuspendUpdateRetries = true;
    }

    UnlockUpdateMutex();
}

/**
 * Fail all conditional pending paths that have become obsolete and
 * notify the application.
 */
CHIP_ERROR SubscriptionClient::PurgePendingUpdate()
{
    CHIP_ERROR err = WEAVE_NO_ERROR;
    size_t numPendingPathsDeleted = 0;

    WeaveLogDetail(DataManagement, "PurgePendingUpdate: numItems before: %d", mPendingUpdateSet.GetNumItems());

    VerifyOrExit(mPendingUpdateSet.GetNumItems() > 0, );

    if (mDataSinkCatalog)
    {
        mDataSinkCatalog->Iterate(PurgePendingUpdatableSinkTrait, this);
    }

    PurgeAndNotifyFailedPaths(CHIP_ERROR_WDM_VERSION_MISMATCH, mPendingUpdateSet, numPendingPathsDeleted);

    if ((numPendingPathsDeleted > 0) && IsInProgressOrEstablished())
    {
        TerminateSubscription(CHIP_ERROR_WDM_VERSION_MISMATCH, NULL, false);
    }

exit:
    WeaveLogDetail(DataManagement, "PurgePendingUpdate: numItems after: %d", mPendingUpdateSet.GetNumItems());

    return err;
}

void SubscriptionClient::PurgePendingUpdatableSinkTrait(void * aDataSink, TraitDataHandle aDataHandle, void * aContext)
{
    SubscriptionClient * subClient = static_cast<SubscriptionClient *>(aContext);
    TraitDataSink * dataSink = static_cast<TraitDataSink *>(aDataSink);
    TraitUpdatableDataSink * updatableDataSink = NULL;

    VerifyOrExit(dataSink->IsUpdatableDataSink() == true, /* no error */);

    updatableDataSink = static_cast<TraitUpdatableDataSink *>(dataSink);

    if (updatableDataSink->IsVersionValid())
    {
        subClient->MarkFailedPendingPaths(aDataHandle, *updatableDataSink, updatableDataSink->GetVersion());
    }

exit:
    return;
}

void SubscriptionClient::SetUpdateStartVersions(void)
{
    TraitPath traitPath;
    TraitUpdatableDataSink *updatableSink;

    for (size_t i = mInProgressUpdateList.GetFirstValidItem();
            i < mInProgressUpdateList.GetPathStoreSize();
            i = mInProgressUpdateList.GetNextValidItem(i))
    {
        mInProgressUpdateList.GetItemAt(i, traitPath);

        updatableSink = Locate(traitPath.mTraitDataHandle, mDataSinkCatalog);
        if (NULL != updatableSink)
        {
            updatableSink->SetUpdateStartVersion();
        }
    }
}

CHIP_ERROR SubscriptionClient::SendSingleUpdateRequest(void)
{
    CHIP_ERROR err   = WEAVE_NO_ERROR;
    uint32_t maxUpdateSize;
    uint32_t maxPayloadSize = 0;
    PacketBuffer* pBuf = NULL;
    UpdateEncoder::Context context;

    maxUpdateSize = GetMaxUpdateSize();
    err = mUpdateClient.mpBinding->AllocateRightSizedBuffer(pBuf, maxUpdateSize, WDM_MIN_UPDATE_SIZE, maxPayloadSize);
    SuccessOrExit(err);

    mUpdateRequestContext.mIsPartialUpdate = false;

    context.mBuf = pBuf;
    context.mMaxPayloadSize = maxPayloadSize;
    context.mUpdateRequestIndex = mUpdateRequestContext.mUpdateRequestIndex;
    context.mExpiryTimeMicroSecond = 0;
    context.mItemInProgress = mUpdateRequestContext.mItemInProgress;
    context.mNextDictionaryElementPathHandle = mUpdateRequestContext.mNextDictionaryElementPathHandle;
    context.mInProgressUpdateList = &mInProgressUpdateList;
    context.mDataSinkCatalog = mDataSinkCatalog;

    err = mUpdateEncoder.EncodeRequest(context);
    SuccessOrExit(err);

    mUpdateRequestContext.mNextDictionaryElementPathHandle = context.mNextDictionaryElementPathHandle;

    if (context.mItemInProgress < mInProgressUpdateList.GetPathStoreSize())
    {
        // This is a PartialUpdateRequest; increase the index for the next one
        mUpdateRequestContext.mIsPartialUpdate = true;
        mUpdateRequestContext.mUpdateRequestIndex++;
    }


    if (context.mNumDataElementsAddedToPayload > 0)
    {
        if (false == mUpdateRequestContext.mIsPartialUpdate)
        {
            // TODO: Should this happen at the first PartialUpdateRequest, or at the final UpdateRequest?
            SetUpdateStartVersions();
        }

        WeaveLogDetail(DataManagement, "Sending %sUpdateRequest with %" PRIu16 " DEs",
                mUpdateRequestContext.mIsPartialUpdate ? "Partial" : "",
                context.mNumDataElementsAddedToPayload);

        // TODO: SetUpdateInFlight is here instead of after SendUpdate
        // to be able to inject timeouts; must improve this..
        SetUpdateInFlight();

        err = mUpdateClient.SendUpdate(mUpdateRequestContext.mIsPartialUpdate, pBuf, context.mUpdateRequestIndex == 0);
        pBuf = NULL;
        SuccessOrExit(err);

        mUpdateRequestContext.mItemInProgress = context.mItemInProgress;
    }
    else
    {
        mUpdateClient.CancelUpdate();
    }

exit:

    if (NULL != pBuf)
    {
        PacketBuffer::Free(pBuf);
        pBuf = NULL;
    }

    if (err == CHIP_ERROR_BUFFER_TOO_SMALL)
    {
        WeaveLogDetail(DataManagement, "illegal oversized trait property is too big to fit in the packet");
    }

    return err;
}

void SubscriptionClient::FormAndSendUpdate()
{
    CHIP_ERROR err                  = WEAVE_NO_ERROR;

    LockUpdateMutex();

    VerifyOrExit(!IsUpdateInFlight(), WeaveLogDetail(DataManagement, "Update request in flight"));

    WeaveLogDetail(DataManagement, "Eval Subscription: (state = %s)!", GetStateStr());

    if (mBinding->IsReady())
    {
        if (mInProgressUpdateList.IsEmpty() && mPendingSetState == kPendingSetReady)
        {
            MovePendingToInProgress();
        }

        err = SendSingleUpdateRequest();
        SuccessOrExit(err);

        WeaveLogDetail(DataManagement, "Done update processing!");
    }
    else if (false == mBinding->IsPreparing())
    {
        err = _PrepareBinding();
        SuccessOrExit(err);
    }

exit:

    if (WEAVE_NO_ERROR != err)
    {
        // If anything failed, the UpdateRequest payload was not sent.
        // Move paths back to pending and retry later.
        OnUpdateNoResponse(err);
    }

    UnlockUpdateMutex();

    WeaveLogFunctError(err);

    return;
}

/**
 * Signals that the application has finished mutating all TraitUpdatableDataSinks.
 * Unless a previous update exchange is in progress, the client will
 * take all data marked as updated and send it to the responder in one update request.
 * This method can be called from any thread.
 *
 * @param[in] aForce    If true, causes the update to be sent immediately even if
 *                      a retry has been scheduled in the future. This parameter is
 *                      considered false by default.
 *
 * @return WEAVE_NO_ERROR in case of success; other CHIP_ERROR codes in case of failure.
 */
CHIP_ERROR SubscriptionClient::FlushUpdate()
{
    return FlushUpdate(false);
}
CHIP_ERROR SubscriptionClient::FlushUpdate(bool aForce)
{
    CHIP_ERROR err                  = WEAVE_NO_ERROR;

    WeaveLogDetail(DataManagement, "%s", __func__);

    LockUpdateMutex();

    mSuspendUpdateRetries = false;

    if (mPendingSetState == kPendingSetOpen)
    {
        SetPendingSetState(kPendingSetReady);
    }

    VerifyOrExit(mPendingSetState == kPendingSetReady,
            WeaveLogDetail(DataManagement, "%s: PendingSetState: %d; err = %s", __func__, mPendingSetState, nl::ErrorStr(err)));

    VerifyOrExit(false == IsUpdateInFlight(),
            WeaveLogDetail(DataManagement, "%s: update already in flight", __func__));

    if (aForce)
    {
        // Mark the timer as not running, so the ScheduleWork handler will reset it and
        // start an immediate attempt.
        mUpdateRetryScheduled = false;
    }

    VerifyOrExit(false == mUpdateRetryScheduled, );

    VerifyOrExit(false == mUpdateFlushScheduled, );

    // Tell the Weave thread to try sending the update immediately
    err = SubscriptionEngine::GetInstance()->GetExchangeManager()->MessageLayer->SystemLayer->ScheduleWork(OnUpdateScheduleWorkCallback, this);
    SuccessOrExit(err);

    _AddRef();
    mUpdateFlushScheduled = true;

exit:
    UnlockUpdateMutex();

    if (mPendingSetState == kPendingSetEmpty)
    {
        NoMorePendingEventCbHelper();
    }

    return err;
}

bool SubscriptionClient::CheckForSinksWithDataLoss()
{
    bool needToResubscribe = false;

    mDataSinkCatalog->Iterate(CheckForSinksWithDataLossIteratorCb, &needToResubscribe);

    return needToResubscribe;
}

void SubscriptionClient::CheckForSinksWithDataLossIteratorCb(void * aDataSink, TraitDataHandle aDataHandle, void * aContext)
{
    TraitDataSink * dataSink = static_cast<TraitDataSink *>(aDataSink);
    TraitUpdatableDataSink * updatableDataSink = NULL;
    bool *needToResubscribe = static_cast<bool *>(aContext);

    VerifyOrExit(dataSink->IsUpdatableDataSink() == true, /* no error */);

    updatableDataSink = static_cast<TraitUpdatableDataSink *>(dataSink);

    if (updatableDataSink->IsPotentialDataLoss())
    {
        WeaveLogDetail(DataManagement, "Need to resubscribe for potential data loss in TDH %d, trait %08x",
                aDataHandle,
                updatableDataSink->GetSchemaEngine()->GetProfileId());

        updatableDataSink->ClearVersion();
        updatableDataSink->ClearUpdateRequiredVersion();
        updatableDataSink->SetConditionalUpdate(false);
        *needToResubscribe = true;
    }

exit:
    return;
}

/**
 * This method should be called when the TraitDataSink catalog
 * has been modified.
 */
void SubscriptionClient::OnCatalogChanged()
{
    ConfigureUpdatableSinks();
}

/**
 * Iterates over the TraitDataSink catalog to configure updatable sinks
 */
void SubscriptionClient::ConfigureUpdatableSinks()
{
    LockUpdateMutex();

    if (mDataSinkCatalog)
    {
        mDataSinkCatalog->Iterate(ConfigureUpdatableSinkTrait, this);
    }

    UnlockUpdateMutex();
}

void SubscriptionClient::ConfigureUpdatableSinkTrait(void * aDataSink, TraitDataHandle aDataHandle, void * aContext)
{
    SubscriptionClient * subClient = static_cast<SubscriptionClient *>(aContext);
    TraitDataSink * dataSink = static_cast<TraitDataSink *>(aDataSink);
    TraitUpdatableDataSink * updatableDataSink = NULL;

    VerifyOrExit(dataSink->IsUpdatableDataSink() == true, /* no error */);

    updatableDataSink = static_cast<TraitUpdatableDataSink *>(dataSink);

    if (updatableDataSink->GetSubscriptionClient() != subClient)
    {
        updatableDataSink->SetSubscriptionClient(subClient);
        updatableDataSink->SetUpdateEncoder(&subClient->mUpdateEncoder);
        updatableDataSink->ClearUpdateRequiredVersion();
        updatableDataSink->ClearUpdateStartVersion();
        updatableDataSink->SetConditionalUpdate(false);
        updatableDataSink->SetPotentialDataLoss(false);
    }

    exit:
        return;
}

void SubscriptionClient::CleanupUpdatableSinkTrait(void * aDataSink, TraitDataHandle aDataHandle, void * aContext)
{
    TraitUpdatableDataSink * updatableDataSink = NULL;
    TraitDataSink * dataSink = static_cast<TraitDataSink *>(aDataSink);

    VerifyOrExit(dataSink->IsUpdatableDataSink() == true, /* no error */);

    updatableDataSink = static_cast<TraitUpdatableDataSink *>(dataSink);

    updatableDataSink->SetSubscriptionClient(NULL);
    updatableDataSink->SetUpdateEncoder(NULL);

exit:
    return;
}

void SubscriptionClient::UpdateRequestContext::Reset()
{
    mItemInProgress = 0;
    mNextDictionaryElementPathHandle = kNullPropertyPathHandle;
    mUpdateRequestIndex = 0;

    mIsPartialUpdate = false;
}

#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE
}; // namespace reporting
}; // namespace chip
