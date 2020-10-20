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

/**
 * @file
 *
 * @brief
 *   Management of the Weave Event Logging.
 *
 */
#ifndef _CHIP_DATA_MODEL_EVENT_LOGGING_MANAGEMENT_H
#define _CHIP_DATA_MODEL_EVENT_LOGGING_MANAGEMENT_H

#include <core/CHIPCircularTLVBuffer.h>
#include <messaging/ExchangeMgr.h>
#include <support/PersistedCounter.h>
#include "EventLoggingTypes.h"

namespace chip {
namespace app {
namespace reporting{

/**
 * @brief
 *   Internal event buffer, built around the chip::TLV::CHIPCircularTLVBuffer
 */
struct CircularEventBuffer
{
    // for doxygen, see the CPP file
    CircularEventBuffer(uint8_t * inBuffer, uint32_t inBufferLength, CircularEventBuffer * inPrev, CircularEventBuffer * inNext);

    // for doxygen, see the CPP file
    bool IsFinalDestinationForImportance(ImportanceLevel inImportance) const;

    event_number_t VendEventID(void);
    void RemoveEvent(uint32_t aNumEvents);

    // for doxygen, see the CPP file
    void AddEvent(timestamp_t inEventTimestamp);

    chip::TLV::CHIPCircularTLVBuffer mBuffer; ///< The underlying TLV buffer storing the events in a TLV representation

    CircularEventBuffer * mPrev; ///< A pointer #CircularEventBuffer storing events less important events
    CircularEventBuffer * mNext; ///< A pointer #CircularEventBuffer storing events more important events

    ImportanceLevel mImportance; ///< The buffer is the final bucket for events of this importance.  Events of lesser importance are
                                ///< dropped when they get bumped out of this buffer

    event_number_t mFirstEventID; ///< First event ID stored in the logging subsystem for this importance
    event_number_t mLastEventID;  ///< Last event ID vended for this importance

    timestamp_t mFirstEventTimestamp; ///< The timestamp of the first event in this buffer
    timestamp_t mLastEventTimestamp;  ///< The timestamp of the last event in this buffer

    // The counter we're going to actually use.
    chip::MonotonicallyIncreasingCounter * mEventIdCounter;

    // The backup counter to use if no counter is provided for us.
    chip::MonotonicallyIncreasingCounter mNonPersistedCounter;

    static CHIP_ERROR GetNextBufferFunct(chip::TLV::TLVReader & ioReader, uintptr_t & inBufHandle,
                                          const uint8_t *& outBufStart, uint32_t & outBufLen);
};

/**
 * @brief
 *   A TLVReader backed by CircularEventBuffer
 */
class CircularEventReader : public chip::TLV::TLVReader
{
    friend struct CircularEventBuffer;

public:
    void Init(CircularEventBuffer * inBuf);
};

/**
 * @brief
 *  Internal structure for traversing event list.
 */
struct CopyAndAdjustDeltaTimeContext
{
    CopyAndAdjustDeltaTimeContext(chip::TLV::TLVWriter * inWriter, EventLoadOutContext * inContext);

    chip::TLV::TLVWriter * mWriter;
    EventLoadOutContext * mContext;
};

/**
 * @brief
 *  Internal structure for traversing events.
 */
struct EventEnvelopeContext
{
    EventEnvelopeContext(void);

    size_t mNumFieldsToRead;
    uint32_t mDeltaTime;
#if CHIP_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS
    int64_t mDeltaUtc;
#endif
    ImportanceLevel mImportance;
    ExternalEvents * mExternalEvents;
};

enum LoggingManagementStates
{
    kLoggingManagementState_Idle       = 1, ///< No log offload in progress, log offload can begin without any constraints
    kLoggingManagementState_InProgress = 2, ///< Log offload in progress
    kLoggingManagementState_Shutdown   = 3  ///< Not capable of performing any logging operation
};

/**
 * @brief
 *   A helper class used in initializing logging management.
 *
 * The class is used to encapsulate the resources allocated by the caller and denotes
 * resources to be used in logging events of a particular importance.  Note that
 * while resources referring to the counters are used exclusively by the
 * particular importance level, the buffers are shared between `this` importance
 * level and events that are "more" important.
 */

struct LogStorageResources
{
    void * mBuffer;     ///< Buffer to be used as a storage at the particular importance level and shared with more important events.
                        ///< Must not be NULL.  Must be large enough to accommodate the largest event emitted by the system.
    size_t mBufferSize; ///< The size, in bytes, of the `mBuffer`.
    chip::Platform::PersistedStorage::Key *
        mCounterKey;        ///< Name of the key naming persistent counter for events of this importance.  When NULL, the persistent
                            ///< counters will not be used for this importance level.
    uint32_t mCounterEpoch; ///< The interval used in incrementing persistent counters.  When 0, the persistent counters will not be
                            ///< used for this importance level.
    chip::PersistedCounter *
        mCounterStorage; ///< Application-provided storage for persistent counter for this importance level. When NULL, persistent
                         ///< counters will not be used for this importance level.
    ImportanceLevel mImportance; ///< Log importance level associated with the resources provided in this structure.
};

/**
 * @brief
 *   A class for managing the in memory event logs.
 */

class LoggingManagement
{
public:
    LoggingManagement(chip::ExchangeManager * inMgr, size_t inNumBuffers, const LogStorageResources * const inLogStorageResources);
    LoggingManagement(void);

    static LoggingManagement & GetInstance(void);

    static void CreateLoggingManagement(chip::ExchangeManager * inMgr, size_t inNumBuffers, const LogStorageResources * const inLogStorageResources);

    static void DestroyLoggingManagement(void);

    CHIP_ERROR SetExchangeManager(chip::ExchangeManager * inMgr);

    event_number_t LogEvent(const EventSchema & inSchema, EventWriterFunct inEventWriter, void * inAppData,
                        const EventOptions * inOptions);

    CHIP_ERROR GetEventReader(chip::TLV::TLVReader & ioReader, ImportanceLevel inImportance);

    CHIP_ERROR FetchEventsSince(chip::TLV::TLVWriter & ioWriter, ImportanceLevel inImportance, event_number_t & ioEventID);

    CHIP_ERROR ScheduleFlushIfNeeded(bool inFlushRequested);

    CHIP_ERROR SetLoggingEndpoint(event_number_t * inEventEndpoints, size_t inNumImportanceLevels, size_t & outLoggingPosition);

    uint32_t GetBytesWritten(void) const;

    void NotifyEventsDelivered(ImportanceLevel inImportance, event_number_t inLastDeliveredEventID, uint64_t inRecipientNodeID);

    /**
     * @brief
     *   IsValid returns whether the LoggingManagement instance is valid
     *
     * @retval true  The instance is valid (initialized with the appropriate backing store)
     * @retval false Otherwise
     */
    bool IsValid(void) { return (mEventBuffer != NULL); };

    event_number_t GetLastEventID(ImportanceLevel inImportance);
    event_number_t GetFirstEventID(ImportanceLevel inImportance);

    void ThrottleLogger(void);
    void UnthrottleLogger(void);

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
    CHIP_ERROR RegisterEventCallbackForImportance(ImportanceLevel inImportance, FetchExternalEventsFunct inFetchCallback,
                                                   NotifyExternalEventsDeliveredFunct inNotifyCallback,
                                                   NotifyExternalEventsEvictedFunct inEvictedCallback, size_t inNumEvents,
                                                   event_number_t * outLastEventID);
    CHIP_ERROR RegisterEventCallbackForImportance(ImportanceLevel inImportance, FetchExternalEventsFunct inFetchCallback,
                                                   NotifyExternalEventsDeliveredFunct inNotifyCallback, size_t inNumEvents,
                                                   event_number_t * outLastEventID);
    CHIP_ERROR RegisterEventCallbackForImportance(ImportanceLevel inImportance, FetchExternalEventsFunct inFetchCallback,
                                                   size_t inNumEvents, event_number_t * outLastEventID);
    void UnregisterEventCallbackForImportance(ImportanceLevel inImportance, event_number_t inEventID);
#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
    CHIP_ERROR BlitEvent(EventLoadOutContext * aContext, const EventSchema & inSchema, EventWriterFunct inEventWriter,
                          void * inAppData, const EventOptions * inOptions);
    void SkipEvent(EventLoadOutContext * aContext);

private:
    event_number_t LogEventPrivate(const EventSchema & inSchema, EventWriterFunct inEventWriter, void * inAppData,
                               const EventOptions * inOptions);

    void FlushHandler(System::Layer * inSystemLayer, INET_ERROR inErr);
    void SignalUploadDone(void);
    CHIP_ERROR CopyToNextBuffer(CircularEventBuffer * inEventBuffer);
    CHIP_ERROR EnsureSpace(size_t inRequiredSpace);

    static CHIP_ERROR CopyEventsSince(const chip::TLV::TLVReader & aReader, size_t aDepth, void * aContext);
    static CHIP_ERROR EventIterator(const chip::TLV::TLVReader & aReader, size_t aDepth, void * aContext);
    static CHIP_ERROR FetchEventParameters(const chip::TLV::TLVReader & aReader, size_t aDepth, void * aContext);
    static CHIP_ERROR CopyAndAdjustDeltaTime(const chip::TLV::TLVReader & aReader, size_t aDepth, void * aContext);
    static CHIP_ERROR EvictEvent(chip::TLV::CHIPCircularTLVBuffer & inBuffer, void * inAppData,
                                  chip::TLV::TLVReader & inReader);
    static CHIP_ERROR AlwaysFail(chip::TLV::CHIPCircularTLVBuffer & inBuffer, void * inAppData,
                                  chip::TLV::TLVReader & inReader);
    static CHIP_ERROR CopyEvent(const chip::TLV::TLVReader & aReader, chip::TLV::TLVWriter & aWriter,
                                 EventLoadOutContext * aContext);

    static void LoggingFlushHandler(System::Layer * systemLayer, void * appState, INET_ERROR err);

    ImportanceLevel GetMaxImportance(void);
    ImportanceLevel GetCurrentImportance(uint32_t profileId);

private:
    CircularEventBuffer * GetImportanceBuffer(ImportanceLevel inImportance) const;

#if CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
    static CHIP_ERROR FindExternalEvents(const chip::TLV::TLVReader & aReader, size_t aDepth, void * aContext);
    CHIP_ERROR GetExternalEventsFromEventId(ImportanceLevel inImportance, event_number_t inEventId, ExternalEvents * outExternalEvents,
                                             chip::TLV::TLVReader & inReader);
    static CHIP_ERROR BlitExternalEvent(chip::TLV::TLVWriter & inWriter, ImportanceLevel inImportance,
                                         ExternalEvents & inEvents);
#endif // CHIP_CONFIG_EVENT_LOGGING_EXTERNAL_EVENT_SUPPORT
    CircularEventBuffer * mEventBuffer;
    ExchangeManager * mExchangeMgr;
    LoggingManagementStates mState;
    uint32_t mBytesWritten;
    uint32_t mThrottled;
    ImportanceLevel mMaxImportanceBuffer;
    bool mUploadRequested;
};

namespace Platform {
extern void CriticalSectionEnter(void);
extern void CriticalSectionExit(void);
} // namespace Platform

} // namespace reporting
} // namespace app
} // namespace chip

#endif //_CHIP_DATA_MODEL_EVENT_LOGGING_MANAGEMENT_H
