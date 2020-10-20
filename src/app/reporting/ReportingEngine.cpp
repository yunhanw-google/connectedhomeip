/*
 *
 *    Copyright (c) 2016-2018 Nest Labs, Inc.
 *    Copyright (c) 2019 Google, LLC.
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
 *      This file implements reporting engine for CHIP
 *      Data Model profile.
 *
 */

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif // __STDC_FORMAT_MACROS

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif //__STDC_LIMIT_MACROS


using namespace ::chip;
using namespace ::chip::TLV;


CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::Init(PacketBuffer * aBuf, TLV::TLVWriter * aWriter,
                                                           SubscriptionHandler * aSubHandler,
                                                           uint32_t aMaxPayloadSize)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(aBuf != NULL, err = CHIP_ERROR_INVALID_ARGUMENT);

    mWriter         = aWriter;
    mState          = kReportDataRequestBuilder_Idle;
    mBuf            = aBuf;
    mSub            = aSubHandler;
    mMaxPayloadSize = aMaxPayloadSize;

exit:

    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::StartReportRequest()
{
    TLVType dummyType;
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit((mState == kReportDataRequestBuilder_Idle) && (mBuf != NULL), err = CHIP_ERROR_INCORRECT_STATE);

    mWriter->Init(mBuf, mMaxPayloadSize);

    err = mWriter->StartContainer(AnonymousTag, kTLVType_Structure, dummyType);
    SuccessOrExit(err);

    if (mSub)
    {
        err = mWriter->Put(ContextTag(BaseMessageWithSubscribeId::kCsTag_SubscriptionId), mSub->mSubscriptionId);
        SuccessOrExit(err);
    }

    mState = kReportDataRequestBuilder_Ready;

exit:
    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::EndNotifyRequest()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(mState == kReportDataRequestBuilder_Ready, err = CHIP_ERROR_INCORRECT_STATE);

    err = mWriter->EndContainer(kTLVType_NotSpecified);
    SuccessOrExit(err);

    err = mWriter->Finalize();
    SuccessOrExit(err);

    mState = kReportDataRequestBuilder_Idle;

exit:
    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::StartDataList()
{
    TLVType dummyType; // Per spec requirement, will be set to TLVStructure
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(mState == kReportDataRequestBuilder_Ready, err = CHIP_ERROR_INCORRECT_STATE);

    err = mWriter->StartContainer(ContextTag(NotificationRequest::kCsTag_DataList), kTLVType_Array, dummyType);
    SuccessOrExit(err);

    mState = kReportDataRequestBuilder_BuildAttributeList;

exit:
    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::EndDataList()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(mState == kReportDataRequestBuilder_BuildAttributeList, err = CHIP_ERROR_INCORRECT_STATE);

    err = mWriter->EndContainer(kTLVType_Structure); // corresponds to dummyType in Start*List
    SuccessOrExit(err);

    mState = kReportDataRequestBuilder_Ready;
exit:
    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::StartEventList()
{
    TLVType dummyType; // Per spec requirement, will be set to TLVStructure
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(mState == kReportDataRequestBuilder_Ready, err = CHIP_ERROR_INCORRECT_STATE);

    err = mWriter->StartContainer(ContextTag(NotificationRequest::kCsTag_EventList), kTLVType_Array, dummyType);
    SuccessOrExit(err);

    mState = kReportDataRequestBuilder_BuildEventList;

exit:
    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::EndEventList()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(mState == kReportDataRequestBuilder_BuildEventList, err = CHIP_ERROR_INCORRECT_STATE);

    err = mWriter->EndContainer(kTLVType_Structure); // corresponds to dummyType in Start*List
    SuccessOrExit(err);

    mState = kReportDataRequestBuilder_Ready;
exit:
    return err;
}

CHIP_ERROR
ReportingEngine::ReportDataRequestBuilder::WriteDataElement(TraitDataHandle aTraitDataHandle, PropertyPathHandle aPropertyPathHandle,
                                                           SchemaVersion aSchemaVersion, PropertyPathHandle * aMergeDataHandleSet,
                                                           uint32_t aNumMergeDataHandles, PropertyPathHandle * aDeleteHandleSet,
                                                           uint32_t aNumDeleteHandles)
{
    CHIP_ERROR err;
    TLVType dummyContainerType;
    TraitDataSource * dataSource;
    bool retrievingData = false;
    SchemaVersionRange versionRange;

    VerifyOrExit(mState == kReportDataRequestBuilder_BuildAttributeList, err = CHIP_ERROR_INCORRECT_STATE);

    err = mWriter->StartContainer(AnonymousTag, kTLVType_Structure, dummyContainerType);
    SuccessOrExit(err);

    err = SubscriptionEngine::GetInstance()->mPublisherCatalog->Locate(aTraitDataHandle, &dataSource);
    SuccessOrExit(err);

    versionRange.mMaxVersion = aSchemaVersion;
    versionRange.mMinVersion = dataSource->GetSchemaEngine()->GetLowestCompatibleVersion(versionRange.mMaxVersion);

    err = mWriter->StartContainer(ContextTag(DataElement::kCsTag_Path), kTLVType_Path, dummyContainerType);
    SuccessOrExit(err);

    err = SubscriptionEngine::GetInstance()->mPublisherCatalog->HandleToAddress(aTraitDataHandle, *mWriter, versionRange);
    SuccessOrExit(err);

    err = dataSource->GetSchemaEngine()->MapHandleToPath(aPropertyPathHandle, *mWriter);
    SuccessOrExit(err);

    err = mWriter->EndContainer(dummyContainerType);
    SuccessOrExit(err);

    err = mWriter->Put(ContextTag(DataElement::kCsTag_Version), dataSource->GetVersion());
    SuccessOrExit(err);

    if (aNumMergeDataHandles > 0 || aNumDeleteHandles > 0)
    {
        const TraitSchemaEngine * schemaEngine = dataSource->GetSchemaEngine();

#if TDM_ENABLE_PUBLISHER_DICTIONARY_SUPPORT
        if (aNumDeleteHandles > 0)
        {
            err =
                mWriter->StartContainer(ContextTag(DataElement::kCsTag_DeletedDictionaryKeys), kTLVType_Array, dummyContainerType);
            SuccessOrExit(err);

            for (size_t i = 0; i < aNumDeleteHandles; i++)
            {
                err = mWriter->Put(AnonymousTag, GetPropertyDictionaryKey(aDeleteHandleSet[i]));
                SuccessOrExit(err);
            }

            err = mWriter->EndContainer(dummyContainerType);
            SuccessOrExit(err);
        }
#endif // TDM_ENABLE_PUBLISHER_DICTIONARY_SUPPORT

        if (aNumMergeDataHandles > 0)
        {
            err = mWriter->StartContainer(ContextTag(DataElement::kCsTag_Data), kTLVType_Structure, dummyContainerType);
            SuccessOrExit(err);

            retrievingData = true;

            for (size_t i = 0; i < aNumMergeDataHandles; i++)
            {
                WeaveLogDetail(DataManagement, "<NE::WriteDE> Merging in 0x%08x", aMergeDataHandleSet[i]);

                err = dataSource->ReadData(aMergeDataHandleSet[i], schemaEngine->GetTag(aMergeDataHandleSet[i]), *mWriter);
                SuccessOrExit(err);
            }

            retrievingData = false;

            err = mWriter->EndContainer(dummyContainerType);
            SuccessOrExit(err);
        }
    }
    else
    {
        retrievingData = true;

        err = dataSource->ReadData(aPropertyPathHandle, ContextTag(DataElement::kCsTag_Data), *mWriter);
        SuccessOrExit(err);

        retrievingData = false;
    }

    err = mWriter->EndContainer(kTLVType_Array);
    SuccessOrExit(err);

exit:
    if (retrievingData && err != CHIP_NO_ERROR)
    {
        WeaveLogError(DataManagement, "Error retrieving data from trait (instanceHandle: %u, profileId: %08x), err = %d",
                      aTraitDataHandle, dataSource->GetSchemaEngine()->GetProfileId(), err);
    }

    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::MoveToState(ReportDataRequestBuilderState aDesiredState)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    // If we're already in the correct builder state, exit without doing anything else
    if (aDesiredState == mState)
    {
        ExitNow();
    }

    // Get to the toplevel of the request
    switch (mState)
    {
    case kReportDataRequestBuilder_Idle:
        err = StartNotifyRequest();
        break;
    case kReportDataRequestBuilder_Ready:
        break;
    case kReportDataRequestBuilder_BuildAttributeList:
        err = EndDataList();
        break;
    case kReportDataRequestBuilder_BuildEventList:
        err = EndEventList();
        break;
    }

    // verify that we're at the toplevel
    VerifyOrExit(err == CHIP_NO_ERROR, WeaveLogDetail(DataManagement, "<NE:Builder> Failed to reach Ready: %d", err));
    // Extra paranoia: verify that we're in toplevel state
    VerifyOrExit(mState == kReportDataRequestBuilder_Ready, err = CHIP_ERROR_INCORRECT_STATE);

    // Now, go to the desired state

    switch (aDesiredState)
    {
    case kReportDataRequestBuilder_Idle:
        err = EndNotifyRequest();
        break;
    case kReportDataRequestBuilder_Ready:
        break;
    case kReportDataRequestBuilder_BuildAttributeList:
        err = StartDataList();
        break;
    case kReportDataRequestBuilder_BuildEventList:
        err = StartEventList();
        break;
    }
    VerifyOrExit(err == CHIP_NO_ERROR, WeaveLogDetail(DataManagement, "<RE:Builder> Failed to reach desired state: %d", err));
    // Extra paranoia: verify that we're in desired state
    VerifyOrExit(mState == aDesiredState, err = CHIP_ERROR_INCORRECT_STATE);

exit:
    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::Checkpoint(TLVWriter & aPoint)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    aPoint          = *mWriter;
    return err;
}

CHIP_ERROR ReportingEngine::ReportDataRequestBuilder::Rollback(TLVWriter & aPoint)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    *mWriter        = aPoint;
    return err;
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ReportingEngine
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CHIP_ERROR ReportingEngine::Init(chip::ExchangeManager * const apExchangeMgr)
{
    mpExchangeMgr = apExchangeMgr;
    return CHIP_NO_ERROR;
}

CHIP_ERROR ReportingEngine::SendReportingData(PacketBuffer * aMsgBuf)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    CHIPLogDetail(Zcl, "%s Ref(%d)", __func__, mRefCount);

    mNumNotifiesInFlight++;
    // Make sure we're not freed by accident.
    _AddRef();

    // Create new exchange context when we're idle (otherwise we must be using the existing EC)
    if (kState_SubscriptionEstablished_Idle == mCurrentState)
    {
        err = ReplaceExchangeContext();
        SuccessOrExit(err);
    }

    // Note we're sending back a message using an EC initiated by the client
    err     = mEC->SendMessage(chip::protocols::kWeaveProfile_WDM, kMsgType_ReportData, aMsgBuf,
                               chip::ExchangeContext::kSendFlag_ExpectResponse);
    aMsgBuf = NULL;
    SuccessOrExit(err);

exit:
    ChipLogFunctError(err);

    if (err != CHIP_NO_ERROR)
    {
        mNumNotifiesInFlight--;
    }

    if (NULL != aMsgBuf)
    {
        PacketBuffer::Free(aMsgBuf);
        aMsgBuf = NULL;
    }

    _Release();

    return err;
}


void ReportingEngine::OnReportingConfirm(SubscriptionHandler * aSubHandler, bool aNotifyDelivered)
{
    VerifyOrDie(mNumNotifiesInFlight > 0);

    WeaveLogDetail(DataManagement, "<NE> OnReportingConfirm: NumReports-- = %d", mNumReportsInFlight - 1);
    mNumReportsInFlight--;

    if (aNotifyDelivered)
    {
        LoggingManagement & logger = LoggingManagement::GetInstance();

        for (int iterator = kImportanceLevel_First; iterator <= kImportanceLevel_Last; iterator++)
        {
            size_t i                  = static_cast<size_t>(iterator - kImportanceLevel_First);
            ImportanceLevel importance = (ImportanceLevel) iterator;
            logger.NotifyEventsDelivered(importance, aSubHandler->mSelfVendedEvents[i] - 1, aSubHandler->GetPeerNodeId());
        }
    }

    // Run Reporting Engine again now that a report has come back/error'ed out and that we might be able to do more work.
    Run();
}

/**
 *  @brief
 *    Given the `SubscriptionHandler`, fill in the `EventList` element
 *    within the `NotifyRequest`,
 *
 *  The function will fill in a `NotifyRequest`'s `EventList`. If the
 *  event logs occupy more space than available in the current
 *  `NotifyRequest`, the function will only pack enough events to fit
 *  within the buffer and adjust the state of the
 *  `SubscriptionHandler` to resume processing at unprocessed event.
 *  The events are sent in the order of priority. To avoid endless
 *  cycling through events, the function sets the end goal within the
 *  event log that it will reach before it considers the subscription
 *  clean.
 *
 *  @param[in] aSubHandler A pointer to the SubscriptionHandler for
 *                                   which we are attempting to create
 *                                   a `NotifyRequest`
 *
 *  @param[in] aNotificationRequest A builder object encapsulating the
 *                                   request we are trying to build
 *
 *  @param[out] aIsSubscriptionClean A boolean that is set to true
 *                                   when no further traits have data
 *                                   that needs to be processed by the
 *                                   SubscriptionHandler and to false
 *                                   otherwise.
 *
 *  @param[out] aNeWriteInProgress A boolean that is set to true
 *                                   when there is data written in notify request
 *                                   to false otherwise.
 *
 *  @retval #CHIP_NO_ERROR On success.
 *
 *  @retval other          The processing failed within the subroutines. The
 *                         errors may have resulted from state
 *                         corruption, TDM errors, insufficient
 *                         buffering, etc.
 */
CHIP_ERROR ReportingEngine::BuildSingleReportDataRequestEventList(ReportDataRequestBuilder & aReportDataRequest, bool & aReWriteInProgress)
{
    CHIP_ERROR err      = CHIP_NO_ERROR;
    aIsSubscriptionClean = true;

    event_number_t initialEvents[kImportanceLevel_Last - kImportanceLevel_First + 1];
    memcpy(initialEvents, aReadHandler->mSelfVendedEvents, sizeof(initialEvents));

    int event_count = 0;

    // events only enter the picture if the read handler is
    // subscribed to events.
    if (aReadHandler->mReadToAllEvents)
    {
        // Verify that we have events to transmit
        LoggingManagement & logger = LoggingManagement::GetInstance();

        // If the logger is not valid or has not been initialized,
        // skip the rest of processing
        VerifyOrExit(logger.IsValid(), /* no-op */);

        for (int i = 0; i < kImportanceLevel_Last - kImportanceLevel_First + 1; i++)
        {
            event_number_t tmp_id = logger.GetFirstEventID(static_cast<ImportanceLevel>(i));
            if (tmp_id > initialEvents[i])
            {
                initialEvents[i] = tmp_id;
            }
        }

        // Check whether we are in a middle of an upload
        if (aReadHandler->mCurrentImportance == kImportanceLevel_Invalid)
        {
            // Upload is not underway.  Check for new events, and set a checkpoint
            aIsReadClean = aReadHandler->CheckEventUpToDate(logger);
            if (!aIsReadClean)
            {
                // We have more events. snapshot last event IDs
                aReadHandler->SetEventLogEndpoint(logger);
            }

            // initialize the next importance level to transfer
            aReadHandler->mCurrentImportance = aReadHandler->FindNextImportanceForTransfer();
        }
        else
        {
            aReadHandler->mCurrentImportance = aReadHandler->FindNextImportanceForTransfer();
            aIsReadClean            = (aReadHandler->mCurrentImportance == kImportanceLevel_Invalid);
        }

        // proceed only if there are new events.
        if (aIsReadClean)
        {
            ExitNow(); // Read clean, move along
        }

        // Ensure we have a buffer and we've started EventList
        err = aReportDataRequest.MoveToState(kReportDataRequestBuilder_BuildEventList);
        // if we did not have enough space for event list at all,
        // squash the error and exit immediately
        if ((err == CHIP_ERROR_NO_MEMORY) || (err == CHIP_ERROR_BUFFER_TOO_SMALL))
        {
            err = CHIP_NO_ERROR;
            ExitNow();
        }
        SuccessOrExit(err);

        while (aReadHandler->mCurrentImportance != kImportanceLevel_Invalid)
        {
            size_t i = static_cast<size_t>(aReadHandler->mCurrentImportance - kImportanceLevel_First);
            err      = logger.FetchEventsSince(*aReportDataRequest.GetWriter(), aReadHandler->mCurrentImportance,
                                          aReadHandler->mSelfVendedEvents[i]);

            if ((err == CHIP_END_OF_TLV) || (err == CHIP_ERROR_TLV_UNDERRUN) || (err == CHIP_NO_ERROR))
            {
                // We have successfully reached the end of the log for
                // the current importance. Advance to the next
                // importance level.
                err                             = CHIP_NO_ERROR;
                aReadHandler->mCurrentImportance = aReadHandler->FindNextImportanceForTransfer();
            }
            else if ((err == CHIP_ERROR_BUFFER_TOO_SMALL) || (err == CHIP_ERROR_NO_MEMORY))
            {
                for (int t = 0; t <= kImportanceLevel_Last - kImportanceLevel_First; t++)
                {
                    if (aReadHandler->mSelfVendedEvents[t] > initialEvents[t])
                    {
                        event_count += aReadHandler->mSelfVendedEvents[t] - initialEvents[t];
                    }
                }

                WeaveLogDetail(DataManagement, "Fetched %d events", event_count);
                if (event_count > 0)
                {
                    aReWriteInProgress = true;
                }

                // when first cluster event is too big to fit in the packet, ignore that cluster event.
                if (!aReWriteInProgress)
                {
                    aReadHandler->mSelfVendedEvents[i]++;
                    CHIPLogDetail(DataManagement, "<RE:Run> cluster event is too big so that it fails to fit in the packet!");
                    err = CHIP_NO_ERROR;
                }
                else
                {
                    // `FetchEventsSince` has filled the available space
                    // within the allowed buffer before it fit all the
                    // available events.  This is an expected condition,
                    // so we do not propagate the error to higher levels;
                    // instead, we terminate the event processing for now
                    // (we will get another chance immediately afterwards,
                    // with a ew buffer) and do not advance the processing
                    // to the next importance level.
                    err = CHIP_NO_ERROR;
                    ExitNow();
                }
            }
            else
            {
                // All other errors are propagated to higher level.
                // Exiting here and returning an error will lead to
                // abandoning subscription.
                ExitNow();
            }
        }
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        WeaveLogError(DataManagement, "Error retrieving events, err = %d", err);
    }

    return err;
}

/**
 *  @brief
 *    Given the `ReadHandler or SubscriptionHandler`, fill in the `DataList` element
 *    within the `NotifyRequest`
 *
 *  The function will fill in a `NotifyRequest`'s `DataList`.  If
 *  the property changes occupy more space than available in the
 *  underlying buffer, the function will only pack enough elements to
 *  fit within the buffer and adjust the state of the
 *  `SubscriptionHandler` to resume processing at the first
 *  unprocessed trait.
 *
 *  @param[in] aReadHandler           A pointer to the SubscriptionHandler for
 *                                   which we are attempting to create
 *                                   a `NotifyRequest`
 *
 *  @param[in] aReportDataRequest  A builder object encapsulating the
 *                                   request we are trying to build
 *
 *  @param[out] aIsReadClean A boolean that is set to true
 *                                   when no further traits have data
 *                                   that needs to be processed by the
 *                                   SubscriptionHandler and to false
 *                                   otherwise.
 *
 *  @param[out] aNeWriteInProgress   A boolean that is set to true
 *                                   when there is data written in notify request
 *                                   to false otherwise.
 *
 *  @retval #CHIP_NO_ERROR On success.
 *
 *  @retval other          The processing failed within the subroutines. The
 *                         errors may have resulted from state
 *                         corruption, TDM errors, insufficient
 *                         buffering, etc.
 */

CHIP_ERROR ReportingEngine::BuildSingleReportDataRequestAttributeDataList(ReadHandler * aReadHandler,
                                                                 ReportDataRequestBuilder & aReportDataRequest, bool & aIsReadClean,
                                                                 bool & aReWriteInProgress)
{
//Todo: To be implemented
    CHIP_ERROR err   = CHIP_NO_ERROR;
exit:
    return err;
}

/**
 *  @brief
 *    Build and send a single notify request for a given subscription handler
 *
 *  The function creates and sends a single `ReportDataRequest` for a
 *  given `readHandler or subscriptionHandler`. If there are changes in the cluster data management or
 *  in the event log state, the function will allocate a buffer, fill
 *  it with attribute and event data (as appropriate) and send it the
 *  buffer to the responder.  If the data to be sent to the
 *  responder spans more than a single report data request, the function
 *  must be called multiple times to ensure that all the cluster and
 *  event data is synchronized between initiator and responder; in
 *  that case, the function will adjust the internal state of the
 *  `Handler` such that the subsequent `ReportDataRequest`s
 *  resume at a point where this request left off.
 *
 *  The function prioritizes cluster attributes over events: the cluster
 *  attributes are serialized first and events are serialized into
 *  space leftover after the attributes have been serialized.
 *
 *  The function allocates at most one `PacketBuffer`.  At the end of
 *  the function, either the ownership of this buffer is passed to the
 *  `CHIP MessageLayer` or the buffer is de-allocated.
 *
 *  If the function encounters any error that's not a CHIP_ERR_MEM,
 *  the function will abort the subscription.
 *
 *  @param[in] aReadHandler A pointer to the ReadHandler for
 *                                   which we are attempting to create
 *                                   a `ReadRequest`
 *
 *  @param[out] aReadHandled An output parameter used in
 *                                   tracking the iteration through
 *                                   subscriptions.
 *
 *  @param[out] aIsReadClean An output parameter that is set
 *                                   to true if there is no processing
 *                                   left for this
 *                                   `SubscriptionHandler`, and to
 *                                   false otherwise.
 *
 *  @retval #CHIP_NO_ERROR On success.
 *
 *  @retval #CHIP_ERR_MEM The function could not allocate memory. The
 *                         caller may need to abort its current
 *                         iteration and restart processing under less
 *                         memory pressure.
 */

CHIP_ERROR ReportingEngine::BuildSingleReportDataRequest(SubscriptionHandler * aSubHandler, bool & aSubscriptionHandled,
                                                         bool & aIsSubscriptionClean)
{
    CHIP_ERROR err    = CHIP_NO_ERROR;
    PacketBuffer * buf = NULL;
    TLVWriter writer;
    ReportDataRequestBuilder notifyRequest;

    bool neWriteInProgress = false;
    uint32_t maxNotificationSize = 0;
    uint32_t maxPayloadSize = 0;

    maxReportDataSize = aReadHandler->GetMaxNotificationSize();

    // Create a notify request.
    err = reportDataRequest.Init(buf, &writer, maxPayloadSize);
    SuccessOrExit(err);

    err = BuildSingleReportDataRequestAttributeDataList(aReadHandler, reportDataRequest, subClean, neWriteInProgress);
    SuccessOrExit(err);

    // Fill in the EventList.  Allocation may take place
    err = BuildSingleReportDataRequestEventList(reportDataRequest, readClean, reWriteInProgress);
    SuccessOrExit(err);

    // transition request builder to the Init state.  If buffer was
    // not allocated, then the function is a noop.  Otherwise, the TLV
    // elements get closed (through the NotificationRequest), and buf
    // is non-null
    err = reportDataRequest.MoveToState(kReportDataRequestBuilder_Idle);
    SuccessOrExit(err);

    // TODO: JIRA-1419. change the NotifyRequest to provide a facility of buffer
    // ownership transfer.  At this point, perhaps it is of minor
    // utility, but it we ever get tooling to track buffer ownership,
    // it would be handy.  As it is, at this point in the code, the
    // request builder should be dead.
    if (reWriteInProgress && buf)
    {
        ChipLogDetail(DataManagement, "<RE:Run> Sending report data...");

        err = SendReportingData(buf);
        // NULL out the buf since we've handed it over to the message layer
        buf = NULL;
        VerifyOrExit(err == CHIP_NO_ERROR, ChipLogError(zcl, "<NE:Run> Error sending out report data!"));
    }

exit:
    // On any error, abort the subscription, and consider it handled.
    if (err != CHIP_NO_ERROR)
    {
        err                  = CHIP_NO_ERROR;
    }

    if (buf != NULL)
    {
        PacketBuffer::Free(buf);
    }

    return err;
}

void ReportingEngine::Run(System::Layer * aSystemLayer, void * aAppState, System::Error)
{
    ReportingEngine * const pEngine = reinterpret_cast<ReportingEngine *>(aAppState);
    pEngine->Run();
}

void ReportingEngine::ScheduleRun()
{
    mExchangeMgr->GetSessionMgr()->SystemLayer()->ScheduleWork(Run, this);
}

void ReportingEngine::Run()
{
    CHIP_ERROR err                  = CHIP_NO_ERROR;

    ChipLogDetail(DataManagement, "<RE:Run> ReportsInFlight = %u", mNumReportsInFlight);

    err = BuildSingleReportDataRequest(readHandler, readHandled, isReadClean);
    SuccessOrExit(err);

exit:
    return;
}
