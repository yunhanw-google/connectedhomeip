/**
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

/*
 *
 *    Copyright (c) 2015-2017 Nest Labs, Inc.
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
// __STDC_FORMAT_MACROS must be defined for PRIX64 to be defined for pre-C++11 clib
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif // __STDC_FORMAT_MACROS

#include <core/CHIPEventLoggingConfig.h>
#include <inttypes.h>
#include <support/CodeUtils.h>
#include <support/ErrorStr.h>
#include <support/logging/CHIPLogging.h>
#include <core/CHIPTLVUtilities.hpp>
#include <system/SystemTimer.h>
#include "LoggingConfiguration.h"
#include "LoggingManagement.h"

using namespace chip::TLV;

namespace chip {
namespace app {
namespace reporting {

// Events are embedded in an anonymous structure: 1 for the control byte, 1 for end-of-container
#define EVENT_CONTAINER_OVERHEAD_TLV_SIZE 2
// Event importance element consumes 3 bytes: control byte, 1-byte tag, and 1 byte value
#define IMPORTANCE_TLV_SIZE 3
// Overhead of embedding something in a (short) byte string: 1 byte control, 1 byte tag, 1 byte length
#define EXTERNAL_EVENT_BYTE_STRING_TLV_SIZE 3

// Static instance: embedded platforms not always implement a proper
// C++ runtime; instead, the instance is initialized via placement new
// in CreateLoggingManagement.

static LoggingManagement sInstance;

LoggingManagement & LoggingManagement::GetInstance(void)
{
    return sInstance;
}

struct ReclaimEventCtx
{
    CircularEventBuffer * mEventBuffer;
    size_t mSpaceNeededForEvent;
};

CHIP_ERROR LoggingManagement::AlwaysFail(chip::TLV::CHIPCircularTLVBuffer & inBuffer, void * inAppData,
                                          chip::TLV::TLVReader & inReader)
{
    return CHIP_ERROR_NO_MEMORY;
}

CHIP_ERROR LoggingManagement::CopyToNextBuffer(CircularEventBuffer * inEventBuffer)
{
    CircularTLVWriter writer;
    CircularTLVReader reader;
    CHIPCircularTLVBuffer checkpoint   = inEventBuffer->mNext->mBuffer;
    CHIPCircularTLVBuffer * nextBuffer = &(inEventBuffer->mNext->mBuffer);
    CHIP_ERROR err;

    // Set up the next buffer s.t. it fails if needs to evict an element
    nextBuffer->mProcessEvictedElement = AlwaysFail;

    writer.Init(nextBuffer);

    // Set up the reader s.t. it is positioned to read the head event
    reader.Init(&(inEventBuffer->mBuffer));

    err = reader.Next();
    SuccessOrExit(err);

    err = writer.CopyElement(reader);
    SuccessOrExit(err);

    err = writer.Finalize();
    SuccessOrExit(err);

exit:
    if (err != CHIP_NO_ERROR)
    {
        inEventBuffer->mNext->mBuffer = checkpoint;
    }
    return err;
}

CHIP_ERROR LoggingManagement::EnsureSpace(size_t inRequiredSpace)
{
    CHIP_ERROR err                   = CHIP_NO_ERROR;
    size_t requiredSpace              = inRequiredSpace;
    CircularEventBuffer * eventBuffer = mEventBuffer;
    CHIPCircularTLVBuffer * circularBuffer;
    ReclaimEventCtx ctx;

    // check whether we actually need to do anything, exit if we don't
    VerifyOrExit(requiredSpace > eventBuffer->mBuffer.AvailableDataLength(), err = CHIP_NO_ERROR);

    while (true)
    {
        circularBuffer = &(eventBuffer->mBuffer);
        // check that the request can ultimately be satisfied.
        VerifyOrExit(requiredSpace <= circularBuffer->GetQueueSize(), err = CHIP_ERROR_BUFFER_TOO_SMALL);

        if (requiredSpace > circularBuffer->AvailableDataLength())
        {
            ctx.mEventBuffer         = eventBuffer;
            ctx.mSpaceNeededForEvent = 0;

            circularBuffer->mProcessEvictedElement = EvictEvent;
            circularBuffer->mAppData               = &ctx;
            err                                    = circularBuffer->EvictHead();

            // one of two things happened: either the element was evicted,
            // or we figured out how much space we need to evict it into
            // the next buffer

            if (err != CHIP_NO_ERROR)
            {
                VerifyOrExit(ctx.mSpaceNeededForEvent != 0, /* no-op, return err */);
                if (ctx.mSpaceNeededForEvent <= eventBuffer->mNext->mBuffer.AvailableDataLength())
                {
                    // we can copy the event outright.  copy event and
                    // subsequently evict head s.t. evicting the head
                    // element always succeeds.
                    // Since we're calling CopyElement and we've checked
                    // that there is space in the next buffer, we don't expect
                    // this to fail.
                    err = CopyToNextBuffer(eventBuffer);
                    SuccessOrExit(err);

                    // success; evict head unconditionally
                    circularBuffer->mProcessEvictedElement = NULL;
                    err                                    = circularBuffer->EvictHead();
                    // if unconditional eviction failed, this
                    // means that we have no way of further
                    // clearing the buffer.  fail out and let the
                    // caller know that we could not honor the
                    // request
                    SuccessOrExit(err);
                    continue;
                }
                // we cannot copy event outright. We remember the
                // current required space in mAppData, we note the
                // space requirements for the event in the current
                // buffer and make that space in the next buffer.
                circularBuffer->mAppData = reinterpret_cast<void *>(requiredSpace);
                eventBuffer              = eventBuffer->mNext;

                // Sanity check: Die here on null event buffer.  If
                // eventBuffer->mNext were null, then the `EvictBuffer`
                // in line 130 would have succeeded -- the event was
                // already in the final buffer.
                VerifyOrDie(eventBuffer != NULL);

                requiredSpace = ctx.mSpaceNeededForEvent;
            }
        }
        else
        {
            if (eventBuffer == mEventBuffer)
                break;
            eventBuffer   = eventBuffer->mPrev;
            requiredSpace = reinterpret_cast<size_t>(eventBuffer->mBuffer.mAppData);
            err           = CHIP_NO_ERROR;
        }
    }

    // On exit, configure the top-level s.t. it will always fail to evict an element
    mEventBuffer->mBuffer.mProcessEvictedElement = AlwaysFail;
    mEventBuffer->mBuffer.mAppData               = NULL;

exit:
    return err;
}

/**
 * @brief Helper function for writing event header and data according to event
 *   logging protocol.
 *
 * @param[inout] aContext   EventLoadOutContext, initialized with stateful
 *                          information for the buffer. State is updated
 *                          and preserved by BlitEvent using this context.
 *
 * @param[in] inSchema      Schema defining importance, profile ID, and
 *                          structure type of this event.
 *
 * @param[in] inEventWriter The callback to invoke to serialize the event data.
 *
 * @param[in] inAppData     Application context for the callback.
 *
 * @param[in] inOptions     EventOptions describing timestamp and other tags
 *                          relevant to this event.
 *
 */
CHIP_ERROR LoggingManagement::BlitEvent(EventLoadOutContext * aContext, const EventSchema & inSchema,
                                         EventWriterFunct inEventWriter, void * inAppData, const EventOptions * inOptions)
{

    CHIP_ERROR err      = CHIP_NO_ERROR;
    TLVWriter checkpoint = aContext->mWriter;
    TLVType containerType;

    VerifyOrExit(aContext->mCurrentEventID >= aContext->mStartingEventID,
                 /* no-op: don't write event, but advance current event ID */);

    VerifyOrExit(inOptions != NULL, err = CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrExit(inOptions->timestampType != kTimestampType_Invalid, err = CHIP_ERROR_INVALID_ARGUMENT);

    err = aContext->mWriter.StartContainer(AnonymousTag, kTLVType_Structure, containerType);
    SuccessOrExit(err);

    // Event metadata

    err = aContext->mWriter.Put(ContextTag(kTag_EventPath), static_cast<uint16_t>(inSchema.mPath));
    SuccessOrExit(err);

    // Importance
    err = aContext->mWriter.Put(ContextTag(kTag_EventImportanceLevel), static_cast<uint8_t>(inSchema.mImportance));
    SuccessOrExit(err);

    err = aContext->mWriter.Put(ContextTag(kTag_EventID), aContext->mCurrentEventID);
    SuccessOrExit(err);

    // if mFirst, record absolute time
    if (aContext->mFirst)
    {
        err = aContext->mWriter.Put(ContextTag(kTag_EventSystemTimestamp), inOptions->timestamp.systemTimestamp);
        SuccessOrExit(err);
    }
    // else record delta
    else
    {
        {
            uint32_t deltatime = inOptions->timestamp.systemTimestamp - aContext->mCurrentTime;
            err               = aContext->mWriter.Put(ContextTag(kTag_EventDeltaSystemTime), deltatime);
            SuccessOrExit(err);
        }
    }

    // Callback to write the EventData
    err = inEventWriter(aContext->mWriter, kTag_EventData, inAppData);
    SuccessOrExit(err);

    err = aContext->mWriter.EndContainer(containerType);
    SuccessOrExit(err);

    err = aContext->mWriter.Finalize();
    SuccessOrExit(err);

    // only update mFirst if an event was successfully written.
    if (aContext->mFirst)
    {
        aContext->mFirst = false;
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        aContext->mWriter = checkpoint;
    }
    else
    {
        // update these variables since BlitEvent can be used to track the
        // state of a set of events over multiple calls.
        aContext->mCurrentEventID++;
        {
            aContext->mCurrentTime = inOptions->timestamp.systemTimestamp;
        }
    }
    return err;
}

/**
 * @brief Helper function to skip writing an event corresponding to an allocated
 *   event id.
 *
 * @param[inout] aContext   EventLoadOutContext, initialized with stateful
 *                          information for the buffer. State is updated
 *                          and preserved by BlitEvent using this context.
 *
 */
void LoggingManagement::SkipEvent(EventLoadOutContext * aContext)
{
    aContext->mCurrentEventID++; // Advance the event id without writing anything
}

/**
 * @brief Create LoggingManagement object and initialize the logging management
 *   subsystem with provided resources.
 *
 * Initialize the LoggingManagement with an array of LogStorageResources.  The
 * array must provide a resource for each valid importance level, the elements
 * of the array must be in increasing numerical value of importance (and in
 * decreasing importance); the first element in the array corresponds to the
 * resources allocated for the most critical events, and the last element
 * corresponds to the least important events.
 *
 * @param[in] inMgr         ExchangeManager to be used with this logging subsystem
 *
 * @param[in] inNumBuffers  Number of elements in inLogStorageResources array
 *
 * @param[in] inLogStorageResources  An array of LogStorageResources for each importance level.
 *
 * @note This function must be called prior to the logging being used.
 */
void LoggingManagement::CreateLoggingManagement(chip::ExchangeManager * inMgr,
                                                size_t inNumBuffers,
                                                const LogStorageResources * const inLogStorageResources)
{
    new (&sInstance) LoggingManagement(inMgr, inNumBuffers, inLogStorageResources);
}

/**
 * @brief Perform any actions we need to on shutdown.
 */
void LoggingManagement::DestroyLoggingManagement(void)
{
    Platform::CriticalSectionEnter();
    sInstance.mState       = kLoggingManagementState_Shutdown;
    sInstance.mEventBuffer = NULL;
    Platform::CriticalSectionExit();
}

/**
 * @brief Set the ExchangeManager to be used with this logging subsystem.  On some
 *   platforms, this may need to happen separately from CreateLoggingManagement() above.
 *
 * @param[in] inMgr         ExchangeManager to be used with this logging subsystem
 */
CHIP_ERROR LoggingManagement::SetExchangeManager(chip::ExchangeManager * inMgr)
{
    mExchangeMgr = inMgr;
    return CHIP_NO_ERROR;
}

/**
 * @brief
 *   LoggingManagement constructor
 *
 * Initialize the LoggingManagement with an array of LogStorageResources.  The
 * array must provide a resource for each valid importance level, the elements
 * of the array must be in increasing numerical value of importance (and in
 * decreasing importance); the first element in the array corresponds to the
 * resources allocated for the most critical events, and the last element
 * corresponds to the least important events.
 *
 * @param[in] inMgr         ExchangeManager to be used with this logging subsystem
 *
 * @param[in] inNumBuffers  Number of elements in inLogStorageResources array
 *
 * @param[in] inLogStorageResources  An array of LogStorageResources for each importance level.
 *
 */

LoggingManagement::LoggingManagement(chip::ExchangeManager * inMgr,
                                     size_t inNumBuffers,
                                     const LogStorageResources * const inLogStorageResources)
{
    CircularEventBuffer * current = NULL;
    CircularEventBuffer * prev    = NULL;
    CircularEventBuffer * next    = NULL;
    size_t i, j;

    VerifyOrDie(inNumBuffers > 0);

    mThrottled   = 0;
    mExchangeMgr = inMgr;

    for (j = 0; j < inNumBuffers; j++)
    {
        i = inNumBuffers - 1 - j;

        next = (i > 0) ? static_cast<CircularEventBuffer *>(inLogStorageResources[i - 1].mBuffer) : NULL;

        VerifyOrDie(inLogStorageResources[i].mBufferSize > sizeof(CircularEventBuffer));

        new (inLogStorageResources[i].mBuffer)
            CircularEventBuffer(static_cast<uint8_t *>(inLogStorageResources[i].mBuffer) + sizeof(CircularEventBuffer),
                                (uint32_t)(inLogStorageResources[i].mBufferSize - sizeof(CircularEventBuffer)), prev, next);

        current = prev                          = static_cast<CircularEventBuffer *>(inLogStorageResources[i].mBuffer);
        current->mBuffer.mProcessEvictedElement = AlwaysFail;
        current->mBuffer.mAppData               = NULL;
        current->mImportance                    = inLogStorageResources[i].mImportance;
        if ((inLogStorageResources[i].mCounterStorage != NULL) && (inLogStorageResources[i].mCounterKey != NULL) &&
            (inLogStorageResources[i].mCounterEpoch != 0))
        {

            // We have been provided storage for a counter for this importance level.
            new (inLogStorageResources[i].mCounterStorage) PersistedCounter();
            CHIP_ERROR err = inLogStorageResources[i].mCounterStorage->Init(*(inLogStorageResources[i].mCounterKey),
                                                                             inLogStorageResources[i].mCounterEpoch);
            if (err != CHIP_NO_ERROR)
            {
                ChipLogError(EventLogging, "%s PersistedCounter[%d]->Init() failed with %d", __FUNCTION__, j, err);
            }
            current->mEventIdCounter = inLogStorageResources[i].mCounterStorage;
        }
        else
        {
            // No counter has been provided, so we'll use our
            // "built-in" non-persisted counter.
            current->mNonPersistedCounter.Init(1);
            current->mEventIdCounter = &(current->mNonPersistedCounter);
        }

        current->mFirstEventID = current->mEventIdCounter->GetValue();
    }
    mEventBuffer = static_cast<CircularEventBuffer *>(inLogStorageResources[kImportanceLevel_Last - kImportanceLevel_First].mBuffer);

    mState               = kLoggingManagementState_Idle;
    mBytesWritten        = 0;
    mUploadRequested     = false;
    mMaxImportanceBuffer = kImportanceLevel_Last;
}

/**
 * @brief
 *   LoggingManagement default constructor. Provided primarily to make the compiler happy.
 *

 * @return LoggingManagement
 */
LoggingManagement::LoggingManagement(void) :
    mEventBuffer(NULL), mExchangeMgr(NULL), mState(kLoggingManagementState_Idle), mBytesWritten(0),
    mThrottled(0), mMaxImportanceBuffer(kImportanceLevel_Invalid), mUploadRequested(false)
{ }

/**
 * @brief
 *   A function to get the current importance of a profile.
 *
 * The function returns the current importance of a profile as
 * currently configured in the #LoggingConfiguration trait.  When
 * per-profile importance is supported, it is used; otherwise only
 * global importance is supported.  When the log is throttled, we only
 * record the Production events
 *
 * @param profileId Profile against which the event is being logged
 *
 * @return Importance of the current profile based on the config
 */
ImportanceLevel LoggingManagement::GetCurrentImportance(uint32_t profileId)
{
    const LoggingConfiguration & config = LoggingConfiguration::GetInstance();
    ImportanceLevel retval;

    if (mThrottled != 0)
    {
        retval = Production;
    }
    else if (config.SupportsPerProfileImportance())
    {
        retval = config.GetProfileImportance(profileId);
    }
    else
    {
        retval = config.mGlobalImportance;
    }
    return (retval < mMaxImportanceBuffer ? retval : mMaxImportanceBuffer);
}

/**
 * @brief
 *   Get the max available importance of the system.
 *
 *  This function returns the max importance stored by logging management,
 *  as defined by both the global importance and number of buffers available
 *
 * @return ImportanceLevel Max importance type currently stored.
 */
ImportanceLevel LoggingManagement::GetMaxImportance(void)
{
    const LoggingConfiguration & config = LoggingConfiguration::GetInstance();
    return (config.mGlobalImportance < mMaxImportanceBuffer ? config.mGlobalImportance : mMaxImportanceBuffer);
}

/**
 * @brief
 *   Allocate a new event ID based on the event importance, and advance the counter
 *   if we have one.
 *
 * @return event_number_t Event ID for this importance.
 */
event_number_t CircularEventBuffer::VendEventID(void)
{
    event_number_t retval = 0;
    CHIP_ERROR err   = CHIP_NO_ERROR;

    // Assign event ID to the buffer's counter's value.
    retval       = mEventIdCounter->GetValue();
    mLastEventID = static_cast<event_number_t>(retval);

    // Now advance the counter.
    err = mEventIdCounter->Advance();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(EventLogging, "%s Advance() for importance %d failed with %d", __FUNCTION__, mImportance, err);
    }

    return retval;
}

/**
 * @brief
 *   Fetch the most recently vended ID for a particular importance level
 *
 * @param inImportance Importance level
 *
 * @return event_number_t most recently vended event ID for that event importance
 */
event_number_t LoggingManagement::GetLastEventID(ImportanceLevel inImportance)
{
    return GetImportanceBuffer(inImportance)->mLastEventID;
}

/**
 * @brief
 *   Fetch the first event ID currently stored for a particular importance level
 *
 * @param inImportance Importance level
 *
 * @return event_number_t First currently stored event ID for that event importance
 */
event_number_t LoggingManagement::GetFirstEventID(ImportanceLevel inImportance)
{
    return GetImportanceBuffer(inImportance)->mFirstEventID;
}

CircularEventBuffer * LoggingManagement::GetImportanceBuffer(ImportanceLevel inImportance) const
{
    CircularEventBuffer * buf = mEventBuffer;
    while (!buf->IsFinalDestinationForImportance(inImportance))
    {
        buf = buf->mNext;
    }
    return buf;
}

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
/**
 * @brief
 *   The public API for registering a set of externally stored events.
 *
 * Register a callback of form #FetchExternalEventsFunct. This API requires
 * the platform to know the number of events on registration. The internal
 * workings also require this number to be constant. Since this API
 * does not allow the platform to register specific event IDs, this prevents
 * the platform from persisting storage of events (at least with unique event
 * IDs).
 *
 * The callback will be called whenever a subscriber attempts to fetch event IDs
 * within the range any number of times until it is unregistered.
 *
 * This variant of the function should be used when the external
 * provider desires a notifification neither when the external events have
 * been delivered nor when the external events object is evicted.
 *
 * The pointer to the ExternalEvents struct will be NULL on failure, otherwise
 * will be populated with start and end event IDs assigned to the callback.
 * This pointer should be used to unregister the set of events.
 *
 * See the documentation for #FetchExternalEventsFunct for details on what
 * the callback must implement.
 *
 * @param[in] inImportance      Importance level
 *
 * @param[in] inCallback        Callback to register to fetch external events
 *
 * @param[in] inNumEvents       Number of events in this set
 *
 * @param[out] outLastEventID   Pointer to an event_number_t; on successful registration of external events the function will store the
 *                              event ID corresponding to the last event ID of the external event block. The parameter may be NULL.
 *
 * @retval CHIP_ERROR_NO_MEMORY        If no more callback slots are available.
 * @retval CHIP_ERROR_INVALID_ARGUMENT Null function callback or no events to register.
 * @retval CHIP_NO_ERROR               On success.
 */
CHIP_ERROR LoggingManagement::RegisterEventCallbackForImportance(ImportanceLevel inImportance, FetchExternalEventsFunct inCallback,
                                                                  size_t inNumEvents, event_number_t * outLastEventID)
{
    return RegisterEventCallbackForImportance(inImportance, inCallback, NULL, NULL, inNumEvents, outLastEventID);
}

/**
 * @brief
 *   The public API for registering a set of externally stored events.
 *
 * Register a callback of form #FetchExternalEventsFunct. This API requires
 * the platform to know the number of events on registration. The internal
 * workings also require this number to be constant. Since this API
 * does not allow the platform to register specific event IDs, this prevents
 * the platform from persisting storage of events (at least with unique event
 * IDs).
 *
 * The callback will be called whenever a subscriber attempts to fetch event IDs
 * within the range any number of times until it is unregistered.
 *
 * This variant of the function should be used when the external
 * provider wants to be notified when the events have been delivered
 * to a subscriber, but not when the external events object is evicted.
 * When the events are delivered, the external provider is notified
 * about that along with the node ID of the recipient and the id of the
 * last event delivered to that recipient.  Note that the external
 * provider may be notified several times for the same event ID.
 * There are no specific restrictions on the handler, in particular,
 * the handler may unregister the external event IDs.
 *
 * The pointer to the ExternalEvents struct will be NULL on failure, otherwise
 * will be populated with start and end event IDs assigned to the callback.
 * This pointer should be used to unregister the set of events.
 *
 * See the documentation for #FetchExternalEventsFunct for details on what
 * the callback must implement.
 *
 * @param[in] inImportance      Importance level
 *
 * @param[in] inCallback        Callback to register to fetch external events
 *
 * @param[in] inNotifyCallback  Callback to register for delivery notification
 *
 * @param[in] inNumEvents       Number of events in this set
 *
 * @param[out] outLastEventID   Pointer to an event_number_t; on successful registration of external events the function will store the
 *                              event ID corresponding to the last event ID of the external event block. The parameter may be NULL.
 *
 * @retval CHIP_ERROR_NO_MEMORY        If no more callback slots are available.
 * @retval CHIP_ERROR_INVALID_ARGUMENT Null function callback or no events to register.
 * @retval CHIP_NO_ERROR               On success.
 */
CHIP_ERROR LoggingManagement::RegisterEventCallbackForImportance(ImportanceLevel inImportance,
                                                                  FetchExternalEventsFunct inFetchCallback,
                                                                  NotifyExternalEventsDeliveredFunct inNotifyCallback,
                                                                  size_t inNumEvents, event_number_t * outLastEventID)
{
    return RegisterEventCallbackForImportance(inImportance, inFetchCallback, inNotifyCallback, NULL, inNumEvents, outLastEventID);
}

/**
 * @brief
 *   The public API for registering a set of externally stored events.
 *
 * Register a callback of form #FetchExternalEventsFunct. This API requires
 * the platform to know the number of events on registration. The internal
 * workings also require this number to be constant. Since this API
 * does not allow the platform to register specific event IDs, this prevents
 * the platform from persisting storage of events (at least with unique event
 * IDs).
 *
 * The callback will be called whenever a subscriber attempts to fetch event IDs
 * within the range any number of times until it is unregistered.
 *
 * This variant of the function should be used when the external
 * provider wants to be notified both when the events have been delivered
 * to a subscriber and if the external events object is evicted.
 *
 * When the events are delivered, the external provider is notified
 * about that along with the node ID of the recipient and the id of the
 * last event delivered to that recipient.  Note that the external
 * provider may be notified several times for the same event ID.
 * There are no specific restrictions on the handler, in particular,
 * the handler may unregister the external event IDs.
 *
 * If the external events object is evicted from the log buffers,
 * the external provider is notified along with a copy of the external
 * events object.
 *
 * The pointer to the ExternalEvents struct will be NULL on failure, otherwise
 * will be populated with start and end event IDs assigned to the callback.
 * This pointer should be used to unregister the set of events.
 *
 * See the documentation for #FetchExternalEventsFunct for details on what
 * the callback must implement.
 *
 * @param[in] inImportance      Importance level
 *
 * @param[in] inFetchCallback   Callback to register to fetch external events
 *
 * @param[in] inNotifyCallback  Callback to register for delivery notification
 *
 * @param[in] inEvictedCallback Callback to register for eviction notification
 *
 * @param[in] inNumEvents       Number of events in this set
 *
 * @param[out] outLastEventID   Pointer to an event_number_t; on successful registration of external events the function will store the
 *                              event ID corresponding to the last event ID of the external event block. The parameter may be NULL.
 *
 * @retval CHIP_ERROR_NO_MEMORY        If no more callback slots are available.
 * @retval CHIP_ERROR_INVALID_ARGUMENT Null function callback or no events to register.
 * @retval CHIP_NO_ERROR               On success.
 */
CHIP_ERROR LoggingManagement::RegisterEventCallbackForImportance(ImportanceLevel inImportance,
                                                                  FetchExternalEventsFunct inFetchCallback,
                                                                  NotifyExternalEventsDeliveredFunct inNotifyCallback,
                                                                  NotifyExternalEventsEvictedFunct inEvictedCallback,
                                                                  size_t inNumEvents, event_number_t * outLastEventID)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    ExternalEvents ev;
    CircularEventBuffer * buf = GetImportanceBuffer(inImportance);
    CircularTLVWriter writer;

    Platform::CriticalSectionEnter();

    CHIPCircularTLVBuffer checkpoint = mEventBuffer->mBuffer;

    VerifyOrExit(inFetchCallback != NULL, err = CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrExit(inNumEvents > 0, err = CHIP_ERROR_INVALID_ARGUMENT);

    ev.mFirstEventID = buf->VendEventID();
    ev.mLastEventID  = ev.mFirstEventID;
    // need to vend event IDs in a batch.
    for (size_t i = 1; i < inNumEvents; i++)
    {
        ev.mLastEventID = buf->VendEventID();
    }

    ev.mFetchEventsFunct           = inFetchCallback;
    ev.mNotifyEventsDeliveredFunct = inNotifyCallback;
    ev.mNotifyEventsEvictedFunct   = inEvictedCallback;

    // We know the size of the event, ensure we have the space for it.
    err = EnsureSpace(sizeof(ExternalEvents) + EVENT_CONTAINER_OVERHEAD_TLV_SIZE + IMPORTANCE_TLV_SIZE +
                      EXTERNAL_EVENT_BYTE_STRING_TLV_SIZE);

    SuccessOrExit(err);

    checkpoint = mEventBuffer->mBuffer;

    writer.Init(&(mEventBuffer->mBuffer));

    // can't quite use the BlitEvent method, use the specially created one

    err = BlitExternalEvent(writer, inImportance, ev);

    mBytesWritten += writer.GetLengthWritten();

exit:

    if (err != CHIP_NO_ERROR)
    {
        mEventBuffer->mBuffer = checkpoint;
    }
    else
    {
        if (outLastEventID != NULL)
        {
            *outLastEventID = ev.mLastEventID;
        }
    }

    Platform::CriticalSectionExit();

    return err;
}

CHIP_ERROR LoggingManagement::BlitExternalEvent(chip::TLV::TLVWriter & inWriter, ImportanceLevel inImportance,
                                                 ExternalEvents & inEvents)
{
    CHIP_ERROR err;
    TLVType containerType;

    err = inWriter.StartContainer(AnonymousTag, kTLVType_Structure, containerType);
    SuccessOrExit(err);

    // Importance
    err = inWriter.Put(ContextTag(kTag_EventImportance), static_cast<uint16_t>(inImportance));
    SuccessOrExit(err);

    // External event structure, blitted to the buffer as a byte string.  Must match the call in
    // UnregisterEventCallbackForImportance

    err = inWriter.PutBytes(ContextTag(kTag_ExternalEventStructure), static_cast<const uint8_t *>(static_cast<void *>(&inEvents)),
                            sizeof(ExternalEvents));
    SuccessOrExit(err);

    err = inWriter.EndContainer(containerType);
    SuccessOrExit(err);

    err = inWriter.Finalize();
    SuccessOrExit(err);

exit:
    return err;
}
/**
 * @brief
 *   The public API for unregistering a set of externally stored events.
 *
 * Unregistering the callback will prevent LoggingManagement from calling
 * the callback for a set of events. LoggingManagement will no longer send
 * those event IDs to subscribers.
 *
 * The intent is for one function to serve a set of events at a time. If
 * a new set of events need to be registered using the same function, the
 * callback should first be unregistered, then registered again. This
 * means the original set of events can no longer be fetched.
 *
 * This function succeeds unconditionally. If the callback was never
 * registered or was already unregistered, it's a no-op.
 *
 * @param[in] inImportance          Importance level
 * @param[in] inEventID             An event ID corresponding to any of the events in the external event block to be unregistered.
 */
void LoggingManagement::UnregisterEventCallbackForImportance(ImportanceLevel inImportance, event_number_t inEventID)
{
    ExternalEvents ev;
    CHIP_ERROR err = CHIP_NO_ERROR;
    TLVReader reader;
    CHIPCircularTLVBuffer * readBuffer;

    TLVType containerType;
    uint8_t * dataPtr;

    Platform::CriticalSectionEnter();

    err = GetExternalEventsFromEventId(inImportance, inEventID, &ev, reader);
    SuccessOrExit(err);

    dataPtr    = const_cast<uint8_t *>(reader.GetReadPoint());
    readBuffer = static_cast<CHIPCircularTLVBuffer *>((void *) reader.GetBufHandle());

    // The data pointer is positioned immediately after the element head.  The
    // element in question, an anonymous structure, has element head of size 1.
    // move the pointer back by 1, accounting for the details of the circular
    // buffer.
    if (readBuffer->GetQueue() != dataPtr)
    {
        dataPtr -= 1;
    }
    else
    {
        dataPtr = readBuffer->GetQueue() + readBuffer->GetQueueSize() - 1;
    }

    if (ev.IsValid())
    {
        // Reader is positioned on the external event element.
        CHIPCircularTLVBuffer writeBuffer(readBuffer->GetQueue(), readBuffer->GetQueueSize(), dataPtr);
        CircularTLVWriter writer;

        VerifyOrExit(reader.GetTag() == AnonymousTag, );

        VerifyOrExit(reader.GetType() == kTLVType_Structure, );

        err = reader.EnterContainer(containerType);
        SuccessOrExit(err);

        err = reader.Next(kTLVType_UnsignedInteger, ContextTag(kTag_EventImportance));
        SuccessOrExit(err);

        err = reader.Next(kTLVType_ByteString, ContextTag(kTag_ExternalEventStructure));
        SuccessOrExit(err);

        // At this point, the reader is positioned correctly, and dataPtr points to the beginning of the string
        ev.mFetchEventsFunct           = NULL;
        ev.mNotifyEventsDeliveredFunct = NULL;
        ev.mNotifyEventsEvictedFunct   = NULL;

        writer.Init(&writeBuffer);

        err = BlitExternalEvent(writer, inImportance, ev);
    }

exit:
    Platform::CriticalSectionExit();
}

#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT

// Internal API used in copying an event out of the event buffers

CHIP_ERROR LoggingManagement::CopyAndAdjustDeltaTime(const TLVReader & aReader, size_t aDepth, void * aContext)
{
    CHIP_ERROR err;
    CopyAndAdjustDeltaTimeContext * ctx = static_cast<CopyAndAdjustDeltaTimeContext *>(aContext);
    TLVReader reader(aReader);

    if (aReader.GetTag() == chip::TLV::ContextTag(kTag_EventDeltaSystemTime))
    {
        if (ctx->mContext->mFirst) // First event gets a timestamp, subsequent ones get a delta T
        {
            err = ctx->mWriter->Put(chip::TLV::ContextTag(kTag_EventSystemTimestamp), ctx->mContext->mCurrentTime);
        }
        else
        {
            err = ctx->mWriter->CopyElement(reader);
        }
    }
    else
    {
        err = ctx->mWriter->CopyElement(reader);
    }

    return err;
}

/**
 * @brief
 *   Log an event via a callback, with options.
 *
 * The function logs an event represented as an ::EventWriterFunct and
 * an app-specific `appData` context.  The function writes the event
 * metadata and calls the `inEventWriter` with an chip::TLV::TLVWriter
 * reference and `inAppData` context so that the user code can emit
 * the event data directly into the event log.  This form of event
 * logging minimizes memory consumption, as event data is serialized
 * directly into the target buffer.  The event data MUST contain
 * context tags to be interpreted within the schema identified by
 * `inProfileID` and `inEventType`. The tag of the first element will be
 * ignored; the event logging system will replace it with the
 * eventData tag.
 *
 * The event is logged if the schema importance exceeds the logging
 * threshold specified in the LoggingConfiguration.  If the event's
 * importance does not meet the current threshold, it is dropped and
 * the function returns a `0` as the resulting event ID.
 *
 * This variant of the invocation permits the caller to set any
 * combination of `EventOptions`:
 * - timestamp, when 0 defaults to the current time at the point of
 *   the call,
 * - "root" section of the event source (event source and trait ID);
 *   if NULL, it defaults to the current device. the event is marked as
 *   relating to the device that is making the call,
 * - a related event ID for grouping event IDs; when the related event
 *   ID is 0, the event is marked as not relating to any other events,
 * - urgency; by default non-urgent.
 *
 * @param[in] inSchema     Schema defining importance, profile ID, and
 *                         structure type of this event.
 *
 * @param[in] inEventWriter The callback to invoke to actually
 *                         serialize the event data
 *
 * @param[in] inAppData    Application context for the callback.
 *
 * @param[in] inOptions    The options for the event metadata. May be NULL.
 *
 * @return event_number_t      The event ID if the event was written to the
 *                         log, 0 otherwise.
 */
event_number_t LoggingManagement::LogEvent(const EventSchema & inSchema, EventWriterFunct inEventWriter, void * inAppData,
                                       const EventOptions * inOptions)
{
    event_number_t event_id = 0;

    Platform::CriticalSectionEnter();

    // Make sure we're alive.
    VerifyOrExit(mState != kLoggingManagementState_Shutdown, /* no-op */);

    event_id = LogEventPrivate(inSchema, inEventWriter, inAppData, inOptions);

exit:
    Platform::CriticalSectionExit();
    return event_id;
}

// Note: the function below must be called with the critical section
// locked, and only when the logger is not shutting down

inline event_number_t LoggingManagement::LogEventPrivate(const EventSchema & inSchema, EventWriterFunct inEventWriter, void * inAppData,
                                                     const EventOptions * inOptions)
{
    event_number_t event_id = 0;
    CircularTLVWriter writer;
    CHIP_ERROR err    = CHIP_NO_ERROR;
    size_t requestSize = CHIP_CONFIG_EVENT_SIZE_RESERVE;
    bool didWriteEvent = false;
    CHIPCircularTLVBuffer checkpoint = mEventBuffer->mBuffer;
    EventLoadOutContext ctxt =
        EventLoadOutContext(writer, inSchema.mImportance, GetImportanceBuffer(inSchema.mImportance)->mLastEventID, NULL);
    EventOptions opts = EventOptions(static_cast<timestamp_t>(System::Timer::GetCurrentEpoch()));

    // check whether the entry is to be logged or discarded silently
    VerifyOrExit(inSchema.mImportance <= GetCurrentImportance(inSchema.mClusterId), /* no-op */);

    // Create all event specific data
    // Timestamp; encoded as a delta time
    if ((inOptions != NULL) && (inOptions->timestampType == kTimestampType_System))
    {
        opts.timestamp.systemTimestamp = inOptions->timestamp.systemTimestamp;
    }

    if (GetImportanceBuffer(inSchema.mImportance)->mFirstEventTimestamp == 0)
    {
        GetImportanceBuffer(inSchema.mImportance)->AddEvent(opts.timestamp.systemTimestamp);
    }

    if (inOptions != NULL)
    {
        opts.eventSource       = inOptions->eventSource;
        opts.relatedEventID    = inOptions->relatedEventID;
        opts.relatedImportance = inOptions->relatedImportance;
    }

    ctxt.mFirst          = false;
    ctxt.mCurrentEventID = GetImportanceBuffer(inSchema.mImportance)->mLastEventID;
    ctxt.mCurrentTime    = GetImportanceBuffer(inSchema.mImportance)->mLastEventTimestamp;

    // Begin writing
    while (!didWriteEvent)
    {
        // Ensure we have space in the in-memory logging queues
        err = EnsureSpace(requestSize);
        // If we fail to ensure the initial reserve size, then the
        // logging subsystem will never be able to make progress.  In
        // that case, it is best to assert
        if ((requestSize == CHIP_CONFIG_EVENT_SIZE_RESERVE) && (err != CHIP_NO_ERROR))
            ChipDie();
        SuccessOrExit(err);

        // save a checkpoint for the underlying buffer.  Note that with
        // the current event buffering scheme, only the mEventBuffer will
        // be affected by the writes to the `writer` below, and thus
        // that's the only thing we need to checkpoint.
        checkpoint = mEventBuffer->mBuffer;

        // Start the event container (anonymous structure) in the circular buffer
        writer.Init(&(mEventBuffer->mBuffer));

        err = BlitEvent(&ctxt, inSchema, inEventWriter, inAppData, &opts);

        if (err == CHIP_ERROR_NO_MEMORY)
        {
            // try again
            err = CHIP_NO_ERROR;
            requestSize += CHIP_CONFIG_EVENT_SIZE_INCREMENT;
            mEventBuffer->mBuffer = checkpoint;
            continue;
        }

        didWriteEvent = true;
    }

    {
        // Check the number of bytes written.  If the event is loo large
        // to be evicted from subsequent buffers, drop it now.
        CircularEventBuffer * buffer = mEventBuffer;
        do
        {
            VerifyOrExit(buffer->mBuffer.GetQueueSize() >= writer.GetLengthWritten(), err = CHIP_ERROR_BUFFER_TOO_SMALL);
            if (buffer->IsFinalDestinationForImportance(inSchema.mImportance))
                break;
            else
                buffer = buffer->mNext;
        } while (true);
    }

    mBytesWritten += writer.GetLengthWritten();

exit:

    if (err != CHIP_NO_ERROR)
    {
        mEventBuffer->mBuffer = checkpoint;
    }
    else if (inSchema.mImportance <= GetCurrentImportance(inSchema.mClusterId))
    {
        event_id = GetImportanceBuffer(inSchema.mImportance)->VendEventID();

        {
            GetImportanceBuffer(inSchema.mImportance)->AddEvent(opts.timestamp.systemTimestamp);

#if CHIP_CONFIG_EVENT_LOGGING_VERBOSE_DEBUG_LOGS
            ChipLogDetail(
                EventLogging, "LogEvent event id: %u importance: %u profile id: 0x%x structure id: 0x%x sys timestamp: 0x%" PRIx32,
                event_id, inSchema.mImportance, inSchema.mClusterId, inSchema.mStructureType, opts.timestamp.systemTimestamp);
#endif // CHIP_CONFIG_EVENT_LOGGING_VERBOSE_DEBUG_LOGS
        }

        ScheduleFlushIfNeeded(inOptions == NULL ? false : inOptions->urgent);
    }

    return event_id;
}

/**
 * @brief
 *   ThrottleLogger elevates the effective logging level to the Production level.
 *
 */
void LoggingManagement::ThrottleLogger(void)
{
    ChipLogProgress(EventLogging, "LogThrottle on");

    __sync_add_and_fetch(&mThrottled, 1);
}

/**
 * @brief
 *   UnthrottleLogger restores the effective logging level to the configured logging level.
 *
 */
void LoggingManagement::UnthrottleLogger(void)
{
    uint32_t throttled = __sync_sub_and_fetch(&mThrottled, 1);

    if (throttled == 0)
    {
        ChipLogProgress(EventLogging, "LogThrottle off");
    }
}

// internal API, used to copy events to external buffers
CHIP_ERROR LoggingManagement::CopyEvent(const TLVReader & aReader, TLVWriter & aWriter, EventLoadOutContext * aContext)
{
    TLVReader reader;
    TLVType containerType;
    CopyAndAdjustDeltaTimeContext context(&aWriter, aContext);
    const bool recurse = false;
    CHIP_ERROR err    = CHIP_NO_ERROR;

    reader.Init(aReader);
    err = reader.EnterContainer(containerType);
    SuccessOrExit(err);

    err = aWriter.StartContainer(AnonymousTag, kTLVType_Structure, containerType);
    SuccessOrExit(err);

    err = chip::TLV::Utilities::Iterate(reader, CopyAndAdjustDeltaTime, &context, recurse);
    VerifyOrExit(err == CHIP_NO_ERROR || err == CHIP_END_OF_TLV, );

    err = aWriter.EndContainer(containerType);
    SuccessOrExit(err);

    err = aWriter.Finalize();

exit:
    return err;
}

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT

CHIP_ERROR LoggingManagement::FindExternalEvents(const TLVReader & aReader, size_t aDepth, void * aContext)
{
    CHIP_ERROR err;
    EventLoadOutContext * context = static_cast<EventLoadOutContext *>(aContext);

    err = EventIterator(aReader, aDepth, aContext);
    if (err == CHIP_EVENT_ID_FOUND)
    {
        err = CHIP_NO_ERROR;
    }
    if ((err == CHIP_END_OF_TLV) && context->mExternalEvents->IsValid())
    {
        err = CHIP_ERROR_MAX;
    }
    return err;
}

#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT

/**
 * @brief Internal iterator function used to scan and filter though event logs
 *
 * The function is used to scan through the event log to find events matching the spec in the supplied context.
 */

CHIP_ERROR LoggingManagement::EventIterator(const TLVReader & aReader, size_t aDepth, void * aContext)
{
    CHIP_ERROR err    = CHIP_NO_ERROR;
    const bool recurse = false;
    TLVReader innerReader;
    TLVType tlvType;
    EventEnvelopeContext event;
    EventLoadOutContext * loadOutContext = static_cast<EventLoadOutContext *>(aContext);

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
    event.mExternalEvents = loadOutContext->mExternalEvents;
    if (event.mExternalEvents != NULL)
    {
        event.mExternalEvents->Invalidate();
    }
#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT

    innerReader.Init(aReader);
    err = innerReader.EnterContainer(tlvType);
    SuccessOrExit(err);

    err = chip::TLV::Utilities::Iterate(innerReader, FetchEventParameters, &event, recurse);
    VerifyOrExit(event.mNumFieldsToRead == 0, err = CHIP_NO_ERROR);

    err = CHIP_NO_ERROR;

    if (event.mImportance == loadOutContext->mImportance)
    {

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
        if ((event.mExternalEvents != NULL) && (event.mExternalEvents->IsValid()))
        {
            // external event structure for the thing we want to read
            // out.  if there is a chance this is to be written out by
            // the app, kick it up to FetchEventsSince, otherwise skip
            // over the block of external events.

            // if we're in the process of writing, kick it up to FetchEventsSince
            VerifyOrExit(loadOutContext->mCurrentEventID < loadOutContext->mStartingEventID, err = CHIP_END_OF_TLV);

            // if the external events are of interest, kick it up to the caller
            VerifyOrExit(event.mExternalEvents->mLastEventID < loadOutContext->mStartingEventID, err = CHIP_END_OF_TLV);

            // otherwise, skip over the block of external events
            loadOutContext->mCurrentEventID = event.mExternalEvents->mLastEventID + 1;
        }
        else
#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT

        {
            loadOutContext->mCurrentTime += event.mDeltaTime;
            VerifyOrExit(loadOutContext->mCurrentEventID < loadOutContext->mStartingEventID, err = CHIP_EVENT_ID_FOUND);
            loadOutContext->mCurrentEventID++;
        }
    }

exit:
    return err;
}

/**
 * @brief
 *   Internal API used to implement #FetchEventsSince
 *
 * Iterator function to used to copy an event from the log into a
 * TLVWriter. The included aContext contains the context of the copy
 * operation, including the TLVWriter that will hold the copy of an
 * event.  If event cannot be written as a whole, the TLVWriter will
 * be rolled back to event boundary.
 *
 * @retval #CHIP_END_OF_TLV             Function reached the end of the event
 * @retval #CHIP_ERROR_NO_MEMORY        Function could not write a portion of
 *                                       the event to the TLVWriter.
 * @retval #CHIP_ERROR_BUFFER_TOO_SMALL Function could not write a
 *                                       portion of the event to the TLVWriter.
 */
CHIP_ERROR LoggingManagement::CopyEventsSince(const TLVReader & aReader, size_t aDepth, void * aContext)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    chip::TLV::TLVWriter checkpoint;
    EventLoadOutContext * loadOutContext = static_cast<EventLoadOutContext *>(aContext);

    err = EventIterator(aReader, aDepth, aContext);
    if (err == CHIP_EVENT_ID_FOUND)
    {
        // checkpoint the writer
        checkpoint = loadOutContext->mWriter;

        err = CopyEvent(aReader, loadOutContext->mWriter, loadOutContext);

        // CHIP_NO_ERROR and CHIP_END_OF_TLV signify a
        // successful copy.  In all other cases, roll back the
        // writer state back to the checkpoint, i.e., the state
        // before we began the copy operation.
        VerifyOrExit((err == CHIP_NO_ERROR) || (err == CHIP_END_OF_TLV), loadOutContext->mWriter = checkpoint);

        loadOutContext->mCurrentTime = 0;
        loadOutContext->mFirst       = false;
        loadOutContext->mCurrentEventID++;
    }

exit:
    return err;
}

/**
 * @brief
 *   A function to retrieve events of specified importance since a specified event ID.
 *
 * Given a chip::TLV::TLVWriter, an importance type, and an event ID, the
 * function will fetch events of specified importance since the
 * specified event.  The function will continue fetching events until
 * it runs out of space in the chip::TLV::TLVWriter or in the log. The function
 * will terminate the event writing on event boundary.
 *
 * @param[in] ioWriter     The writer to use for event storage
 *
 * @param[in] inImportance The importance of events to be fetched
 *
 * @param[inout] ioEventID On input, the ID of the event immediately
 *                         prior to the one we're fetching.  On
 *                         completion, the ID of the last event
 *                         fetched.
 *
 * @retval #CHIP_END_OF_TLV             The function has reached the end of the
 *                                       available log entries at the specified
 *                                       importance level
 *
 * @retval #CHIP_ERROR_NO_MEMORY        The function ran out of space in the
 *                                       ioWriter, more events in the log are
 *                                       available.
 *
 * @retval #CHIP_ERROR_BUFFER_TOO_SMALL The function ran out of space in the
 *                                       ioWriter, more events in the log are
 *                                       available.
 *
 */
CHIP_ERROR LoggingManagement::FetchEventsSince(TLVWriter & ioWriter, ImportanceLevel inImportance, event_number_t & ioEventID)
{
    CHIP_ERROR err    = CHIP_NO_ERROR;
    const bool recurse = false;
    TLVReader reader;

    CircularEventBuffer * buf = mEventBuffer;
    Platform::CriticalSectionEnter();

    while (!buf->IsFinalDestinationForImportance(inImportance))
    {
        buf = buf->mNext;
    }

    aContext.mCurrentTime = buf->mFirstEventTimestamp;
    aContext.mCurrentEventID = buf->mFirstEventID;
    err                      = GetEventReader(reader, inImportance);
    SuccessOrExit(err);

    err = chip::TLV::Utilities::Iterate(reader, CopyEventsSince, &aContext, recurse);

exit:
    ioEventID = aContext.mCurrentEventID;

    Platform::CriticalSectionExit();
    return err;
}

/**
 * @brief
 *   A helper method useful for examining the in-memory log buffers
 *
 * @param[inout] ioReader A reference to the reader that will be
 *                        initialized with the backing storage from
 *                        the event log
 *
 * @param[in] inImportance The starting importance for the reader.
 *                         Note that in this case the starting
 *                         importance is somewhat counter intuitive:
 *                         more important events share the buffers
 *                         with less important events, in addition to
 *                         their dedicated buffers.  As a result, the
 *                         reader will traverse the least data when
 *                         the Debug importance is passed in.
 *
 * @return                 #CHIP_NO_ERROR Unconditionally.
 */
CHIP_ERROR LoggingManagement::GetEventReader(TLVReader & ioReader, ImportanceLevel inImportance)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    CircularEventBuffer * buffer;
    CircularEventReader reader;
    for (buffer = mEventBuffer; buffer != NULL && !buffer->IsFinalDestinationForImportance(inImportance); buffer = buffer->mNext)
        ;
    VerifyOrExit(buffer != NULL, err = CHIP_ERROR_INVALID_ARGUMENT);

    reader.Init(buffer);

    ioReader.Init(reader);
exit:
    return err;
}

// internal API
CHIP_ERROR LoggingManagement::FetchEventParameters(const TLVReader & aReader, size_t aDepth, void * aContext)
{
    CHIP_ERROR err                 = CHIP_NO_ERROR;
    EventEnvelopeContext * envelope = static_cast<EventEnvelopeContext *>(aContext);
    TLVReader reader;
    uint16_t extImportance; // Note: the type here matches the type case in LoggingManagement::LogEvent, importance section
    reader.Init(aReader);

    VerifyOrExit(envelope->mNumFieldsToRead > 0, err = CHIP_END_OF_TLV);

    if ((reader.GetTag() == chip::TLV::ContextTag(kTag_ExternalEventStructure)) && (envelope->mExternalEvents != NULL))
    {
        err = reader.GetBytes(static_cast<uint8_t *>(static_cast<void *>(envelope->mExternalEvents)), sizeof(ExternalEvents));
        VerifyOrExit(err == CHIP_NO_ERROR, *(envelope->mExternalEvents) = ExternalEvents());
        envelope->mNumFieldsToRead--;
    }

    if (reader.GetTag() == chip::TLV::ContextTag(kTag_EventImportance))
    {
        err = reader.Get(extImportance);
        SuccessOrExit(err);
        envelope->mImportance = static_cast<ImportanceLevel>(extImportance);

        envelope->mNumFieldsToRead--;
    }

    if (reader.GetTag() == chip::TLV::ContextTag(kTag_EventDeltaSystemTime))
    {
        err = reader.Get(envelope->mDeltaTime);
        SuccessOrExit(err);

        envelope->mNumFieldsToRead--;
    }

exit:
    return err;
}

// internal API: determine importance of an event, and the space the event requires

CHIP_ERROR LoggingManagement::EvictEvent(CHIPCircularTLVBuffer & inBuffer, void * inAppData, TLVReader & inReader)
{
    ReclaimEventCtx * ctx             = static_cast<ReclaimEventCtx *>(inAppData);
    CircularEventBuffer * eventBuffer = ctx->mEventBuffer;
    TLVType containerType;
    EventEnvelopeContext context;
    const bool recurse = false;
    CHIP_ERROR err;
    ImportanceLevel imp = kImportanceLevel_Invalid;

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
    ExternalEvents ev;

    ev.Invalidate();
    context.mExternalEvents = &ev;
#else
    context.mExternalEvents = NULL;
#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT

    // pull out the delta time, pull out the importance
    err = inReader.Next();
    SuccessOrExit(err);

    err = inReader.EnterContainer(containerType);
    SuccessOrExit(err);

    chip::TLV::Utilities::Iterate(inReader, FetchEventParameters, &context, recurse);

    err = inReader.ExitContainer(containerType);

    SuccessOrExit(err);

    imp = static_cast<ImportanceLevel>(context.mImportance);

    if (eventBuffer->IsFinalDestinationForImportance(imp))
    {
        // event is getting dropped.  Increase the eventid and first timestamp.
        uint32_t numEventsToDrop = 1;

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
        if (ev.IsValid())
        {
            numEventsToDrop = ev.mLastEventID - ev.mFirstEventID + 1;

            if (ev.mNotifyEventsEvictedFunct != NULL)
            {
                ev.mNotifyEventsEvictedFunct(&ev);
            }
        }
#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT

        eventBuffer->RemoveEvent(numEventsToDrop);
        eventBuffer->mFirstEventTimestamp += context.mDeltaTime;
        ChipLogProgress(EventLogging, "Dropped events do to overflow: { importance_level: %d, count: %d };", imp, numEventsToDrop);
        ctx->mSpaceNeededForEvent = 0;
    }
    else
    {
        // event is not getting dropped. Note how much space it requires, and return.
        ctx->mSpaceNeededForEvent = inReader.GetLengthRead();
        err                       = CHIP_END_OF_TLV;
    }

exit:
    return err;
}

// Notes: called as a result of the timer expiration.  Main job:
// figure out whether trigger still applies, if it does, then kick off
// the upload.  If it does not, perform the appropriate backoff.

void LoggingManagement::LoggingFlushHandler(System::Layer * systemLayer, void * appState, INET_ERROR err)
{
    LoggingManagement * logger = static_cast<LoggingManagement *>(appState);
    logger->FlushHandler(systemLayer, err);
}

// FlushHandler is only called by the CHIP thread. As such, guard variables
// do not need to be atomically set or checked.
void LoggingManagement::FlushHandler(System::Layer * inSystemLayer, INET_ERROR inErr)
{
#if CHIP_CONFIG_EVENT_LOGGING_BDX_OFFLOAD
    const LoggingConfiguration & config = LoggingConfiguration::GetInstance();
#endif // CHIP_CONFIG_EVENT_LOGGING_BDX_OFFLOAD

    switch (mState)
    {

    case kLoggingManagementState_Idle: {
#if CHIP_CONFIG_EVENT_LOGGING_WDM_OFFLOAD
        if (mExchangeMgr != NULL)
        {
            chip::Profiles::DataManagement::SubscriptionEngine::GetInstance()->GetReportingEngine()->Run();
            mUploadRequested = false;
        }
#endif // CHIP_CONFIG_EVENT_LOGGING_WDM_OFFLOAD

        break;
    }
    }

    case kLoggingManagementState_InProgress:
    case kLoggingManagementState_Shutdown: {
        // should never end in these states in this function
        break;
    }
    }
}

void LoggingManagement::SignalUploadDone(void)
{
}

/**
 * @brief
 *  Schedule a log offload task.
 *
 * The function decides whether to schedule a task offload process,
 * and if so, it schedules the `LoggingFlushHandler` to be run
 * asynchronously on the Chip thread.
 *
 * The decision to schedule a flush is dependent on three factors:
 *
 * -- an explicit request to flush the buffer
 *
 * -- the state of the event buffer and the amount of data not yet
 *    synchronized with the event consumers
 *
 * -- whether there is an already pending request flush request event.
 *
 * The explicit request to schedule a flush is passed via an input
 * parameter.
 *
 * The automatic flush is typically scheduled when the event buffers
 * contain enough data to merit starting a new offload.  Additional
 * triggers -- such as minimum and maximum time between offloads --
 * may also be taken into account depending on the offload strategy.
 *
 * The pending state of the event log is indicated by
 * `mUploadRequested` variable. Since this function can be called by
 * multiple threads, `mUploadRequested` must be atomically read and
 * set, to avoid scheduling a redundant `LoggingFlushHandler` before
 * the notification has been sent.
 *
 * @param inRequestFlush A boolean value indicating whether the flush
 *                       should be scheduled regardless of internal
 *                       buffer management policy.
 *
 * @retval #CHIP_ERROR_INCORRECT_STATE LoggingManagement module was not initialized fully.
 * @retval #CHIP_NO_ERROR              On success.
 */
CHIP_ERROR LoggingManagement::ScheduleFlushIfNeeded(bool inRequestFlush)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

#if CHIP_CONFIG_EVENT_LOGGING_BDX_OFFLOAD
    inRequestFlush |= CheckShouldRunBDX();
#endif // CHIP_CONFIG_EVENT_LOGGING_BDX_OFFLOAD
#if CHIP_CONFIG_EVENT_LOGGING_WDM_OFFLOAD
    inRequestFlush |= CheckShouldRunWDM();
#endif // CHIP_CONFIG_EVENT_LOGGING_WDM_OFFLOAD

    if (inRequestFlush && __sync_bool_compare_and_swap(&mUploadRequested, false, true))
    {
        if ((mExchangeMgr != NULL) && (mExchangeMgr->GetSessionMgr() != NULL) && (mExchangeMgr->GetSessionMgr()->SystemLayer() != NULL))
        {
            mExchangeMgr->GetSessionMgr()->SystemLayer()->ScheduleWork(LoggingFlushHandler, this);
        }
        else
        {
            err              = CHIP_ERROR_INCORRECT_STATE;
            mUploadRequested = false;
        }
    }

    return err;
}

#if CHIP_CONFIG_EVENT_LOGGING_WDM_OFFLOAD
/**
 * @brief
 *  Decide whether to offload events based on the number of bytes in event buffers unscheduled for upload.
 *
 * The behavior of the function is controlled via
 * #CHIP_CONFIG_EVENT_LOGGING_BYTE_THRESHOLD constant.  If the system
 * wrote more than that number of bytes since the last time a WDM
 * Notification was sent, the function will indicate it is time to
 * trigger the ReportingEngine.
 *
 * @retval true   Events should be offloaded
 * @retval false  Otherwise
 */
bool LoggingManagement::CheckShouldRunWDM(void)
{
    CHIP_ERROR err              = CHIP_NO_ERROR;
    size_t minimalBytesOffloaded = mBytesWritten;
    bool ret                     = false;

    // Get the minimal log position (in bytes) across all subscribers
    err = chip::Profiles::DataManagement::SubscriptionEngine::GetInstance()->GetMinEventLogPosition(minimalBytesOffloaded);
    SuccessOrExit(err);

    // return true if we can offload more than the threshold bytes to a subscription
    ret = ((minimalBytesOffloaded + CHIP_CONFIG_EVENT_LOGGING_BYTE_THRESHOLD) < mBytesWritten);

exit:
    return ret;
}

#endif // CHIP_CONFIG_EVENT_LOGGING_WDM_OFFLOAD

CHIP_ERROR LoggingManagement::SetLoggingEndpoint(event_number_t * inEventEndpoints, size_t inNumImportanceLevels,
                                                  size_t & outBytesOffloaded)
{
    CHIP_ERROR err                   = CHIP_NO_ERROR;
    CircularEventBuffer * eventBuffer = mEventBuffer;

    Platform::CriticalSectionEnter();

    outBytesOffloaded = mBytesWritten;

    while (eventBuffer != NULL && inNumImportanceLevels > 0)
    {
        if ((eventBuffer->mImportance >= kImportanceLevel_First) &&
            ((static_cast<size_t>(eventBuffer->mImportance - kImportanceLevel_First)) < inNumImportanceLevels))
        {
            inEventEndpoints[eventBuffer->mImportance - kImportanceLevel_First] = eventBuffer->mLastEventID;
        }
        eventBuffer = eventBuffer->mNext;
    }

    Platform::CriticalSectionExit();
    return err;
}

/**
 * @brief
 *   Get the total number of bytes written (across all event
 *   importances) to this log since its instantiation
 *
 * @returns The number of bytes written to the log.
 */
uint32_t LoggingManagement::GetBytesWritten(void) const
{
    return mBytesWritten;
}

void LoggingManagement::NotifyEventsDelivered(ImportanceLevel inImportance, event_number_t inLastDeliveredEventID,
                                              uint64_t inRecipientNodeID)
{

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
    ExternalEvents ev;
    CHIP_ERROR err = CHIP_NO_ERROR;
    TLVReader reader;
    event_number_t currentId;

    Platform::CriticalSectionEnter();
    currentId = GetFirstEventID(inImportance);
    while (currentId <= inLastDeliveredEventID)
    {
        err = GetExternalEventsFromEventId(inImportance, currentId, &ev, reader);
        SuccessOrExit(err);

        VerifyOrExit(ev.IsValid(), );

        VerifyOrExit(ev.mFirstEventID <= inLastDeliveredEventID, );

        if (ev.mNotifyEventsDeliveredFunct != NULL)
        {
            ev.mNotifyEventsDeliveredFunct(&ev, inLastDeliveredEventID, inRecipientNodeID);
        }

        currentId = ev.mLastEventID + 1;
    }

exit:
    Platform::CriticalSectionExit();
#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
}

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
/**
 * @brief
 *   Retrieve ExternalEvent descriptor based on the importance and event ID of the external event.
 *
 * @param[in]  inImportance      The importance of the event
 * @param[in]  inEventID         The ID of the event
 *
 * @param[out] outExternalEvents A pointer to the ExternalEvent structure.  If the external event specified via inImportance /
 *                               inEventID tuple corresponds to a valid external ID, the structure will be populated with the
 *                               descriptor holding all relevant information about that particular block of external events.
 *
 * @param[out] outReader         A reference to a TLVReader. On successful retrieval of ExternalEvent structure, the reader will be
 *                               positioned to the beginning of the TLV struct containing the external events.
 *
 * @retval CHIP_NO_ERROR                On successful retrieval of the ExternalEvents
 *
 * @retval CHIP_ERROR_INVALID_ARGUMENT  When the arguments passed in do not correspond to an external event, or if the external
 *                                       event was already either dropped or unregistered.
 */

CHIP_ERROR LoggingManagement::GetExternalEventsFromEventId(ImportanceLevel inImportance, event_number_t inEventID,
                                                            ExternalEvents * outExternalEvents, TLVReader & outReader)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    uint32_t dummyBuf;
    const bool recurse = false;
    TLVWriter writer;
    EventLoadOutContext aContext(writer, inImportance, inEventID, outExternalEvents);
    CircularEventBuffer * buf = mEventBuffer;
    TLVReader resultReader;

    writer.Init(static_cast<uint8_t *>(static_cast<void *>(&dummyBuf)), sizeof(uint32_t));

    while (!buf->IsFinalDestinationForImportance(inImportance))
    {
        buf = buf->mNext;
    }

    aContext.mCurrentTime = buf->mFirstEventTimestamp;
    aContext.mCurrentEventID = buf->mFirstEventID;
    err                      = GetEventReader(outReader, inImportance);
    SuccessOrExit(err);

    err = chip::TLV::Utilities::Find(outReader, FindExternalEvents, &aContext, resultReader, recurse);
    if (err == CHIP_NO_ERROR)
        outReader.Init(resultReader);

exit:

    return err;
}
#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT

/**
 * @brief
 *   A constructor for the CircularEventBuffer (internal API).
 *
 * @param[in] inBuffer       The actual storage to use for event storage.
 *
 * @param[in] inBufferLength The length of the \c inBuffer in bytes.
 *
 * @param[in] inPrev         The pointer to CircularEventBuffer storing
 *                           events of lesser priority.
 *
 * @param[in] inNext         The pointer to CircularEventBuffer storing
 *                           events of greater priority.
 *
 * @return CircularEventBuffer
 */
CircularEventBuffer::CircularEventBuffer(uint8_t * inBuffer, uint32_t inBufferLength, CircularEventBuffer * inPrev,
                                         CircularEventBuffer * inNext) :
    mBuffer(inBuffer, inBufferLength),
    mPrev(inPrev), mNext(inNext), mImportance(kImportanceLevel_First), mFirstEventID(1), mLastEventID(0), mFirstEventTimestamp(0),
    mLastEventTimestamp(0),
    mEventIdCounter(NULL)
{
    // TODO: hook up the platform-specific persistent event ID.
}

/**
 * @brief
 *   A helper function that determines whether the event of
 *   specified importance is dropped from this buffer.
 *
 * @param[in]   inImportance   Importance of the event.
 *
 * @retval true  The event will be dropped from this buffer as
 *               a result of queue overflow.
 * @retval false The event will be bumped to the next queue.
 */
bool CircularEventBuffer::IsFinalDestinationForImportance(ImportanceLevel inImportance) const
{
    return !((mNext != NULL) && (mNext->mImportance >= inImportance));
}

/**
 * @brief
 *   Given a timestamp of an event, compute the delta time to store in the log
 *
 * @param inEventTimestamp The event timestamp.
 *
 * @return int32_t         Time delta to encode for the event.
 */
void CircularEventBuffer::AddEvent(timestamp_t inEventTimestamp)
{
    if (mFirstEventTimestamp == 0)
    {
        mFirstEventTimestamp = inEventTimestamp;
        mLastEventTimestamp  = inEventTimestamp;
    }
    mLastEventTimestamp = inEventTimestamp;
}

void CircularEventBuffer::RemoveEvent(uint32_t aNumEvents)
{
    mFirstEventID += aNumEvents;
}

/**
 * @brief
 *   Initializes a TLVReader object backed by CircularEventBuffer
 *
 * Reading begins in the CircularTLVBuffer belonging to this
 * CircularEventBuffer.  When the reader runs out of data, it begins
 * to read from the previous CircularEventBuffer.
 *
 * @param[in] inBuf A pointer to a fully initialized CircularEventBuffer
 *
 */
void CircularEventReader::Init(CircularEventBuffer * inBuf)
{
    CircularTLVReader reader;
    CircularEventBuffer * prev;
    reader.Init(&inBuf->mBuffer);
    TLVReader::Init(reader);
    mBufHandle    = (uintptr_t) inBuf;
    GetNextBuffer = CircularEventBuffer::GetNextBufferFunct;
    for (prev = inBuf->mPrev; prev != NULL; prev = prev->mPrev)
    {
        reader.Init(&prev->mBuffer);
        mMaxLen += reader.GetRemainingLength();
    }
}

CHIP_ERROR CircularEventBuffer::GetNextBufferFunct(TLVReader & ioReader, uintptr_t & inBufHandle, const uint8_t *& outBufStart,
                                                    uint32_t & outBufLen)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    CircularEventBuffer * buf;

    VerifyOrExit(inBufHandle != 0, err = CHIP_ERROR_INVALID_ARGUMENT);

    buf = static_cast<CircularEventBuffer *>((void *) inBufHandle);

    err = buf->mBuffer.GetNextBuffer(ioReader, outBufStart, outBufLen);
    SuccessOrExit(err);

    if ((outBufLen == 0) && (buf->mPrev != NULL))
    {
        inBufHandle = (uintptr_t) buf->mPrev;
        outBufStart = NULL;
        err         = GetNextBufferFunct(ioReader, inBufHandle, outBufStart, outBufLen);
    }

exit:
    return err;
}

CopyAndAdjustDeltaTimeContext::CopyAndAdjustDeltaTimeContext(TLVWriter * inWriter, EventLoadOutContext * inContext) :
    mWriter(inWriter), mContext(inContext)
{ }

EventEnvelopeContext::EventEnvelopeContext(void) :
    mNumFieldsToRead(2), // read out importance and either system or utc delta time. events do not store both deltas.
    mDeltaTime(0),
    mImportance(kImportanceLevel_First), mExternalEvents(NULL)
{ }

} // namespace reporting
} // namespace app
} // namespace chip
