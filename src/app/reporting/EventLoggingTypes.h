/**
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2015-2017 Nest Labs, Inc.
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
 *   Enums, types, and tags used in Chip Event Logging.
 *
 */

#pragma once

#ifndef _CHIP_DATA_MODEL_EVENT_LOGGING_TYPES_H
#define _CHIP_DATA_MODEL_EVENT_LOGGING_TYPES_H

#include <core/CHIPCore.h>
#include <core/CHIPTLV.h>
#include "EventLoggingTags.h"
#include <system/SystemPacketBuffer.h>

namespace chip {
namespace app {
namespace reporting{

/**
 * @brief
 *   The Priority of the log entry.
 *
 * @details
 * Priority is used as a way to filter events before they are
 * actually emitted into the log. After the event is in the log, we
 * make no further provisions to expunge it from the log.
 * The priority level serves to prioritize event storage. If an
 * event of high priority is added to a full buffer, events are
 * dropped in order of priority (and age) to accommodate it. As such,
 * priority levels only have relative value. If a system is
 * using only one priority level, events are dropped only in order
 * of age, like a ring buffer.
 */

typedef enum PriorityLevel
{
    kPriorityLevel_First = 0,
    /**
     *  Debug priority denotes log entries of interest to the
     *  developers of the system and is used primarily in the
     *  development phase. Debug priority logs are
     *  not accounted for in the bandwidth or power budgets of the
     *  constrained devices; as a result, they must be used only over
     *  a limited time span in production systems.
     */
    DEBUG                 = kPriorityLevel_First,
    /**
     * Info priority denotes log entries that provide extra insight
     * and diagnostics into the running system. Info logging level may
     * be used over an extended period of time in a production system,
     * or may be used as the default log level in a field trial. On
     * the constrained devices, the entries logged with Info level must
     * be accounted for in the bandwidth and memory budget, but not in
     * the power budget.
     */
    INFO                  = 1,
    /**
     * Critical priority denotes events whose loss would
     * directly impact customer-facing features. Applications may use
     * loss of Production Critical events to indicate system failure.
     * On constrained devices, entries logged with Critical
     * priority must be accounted for in the power and memory budget,
     * as it is expected that they are always logged and offloaded
     * from the device.
     */
    CRITICAL              = 3,
    kPriorityLevel_Last = CRITICAL,
    kPriorityLevel_Invalid = 4
} PriorityLevel;

/**
 * @brief
 *   The structure that defines a schema for event metadata.
 */
struct EventSchema
{
    uint32_t mClusterId;        ///< ID of cluster
    uint32_t mStructureType;    ///< Type of structure
    PriorityLevel mPriority;    ///< Priority
};

/**
 * @typedef timestamp_t
 * Type used to describe the timestamp in milliseconds.
 */
typedef uint32_t timestamp_t;

/**
 * @typedef duration_t
 * Type used to describe the duration, in milliseconds.
 */
typedef uint32_t duration_t;

/**
 * @typedef event_number_t
 * The type of event number.
 */
typedef uint32_t event_number_t;

/**
 * @typedef utc_timestamp_t
 * Type used to describe the UTC timestamp in milliseconds.
 */
typedef uint64_t utc_timestamp_t;

// forward declaration

struct ExternalEvents;

// Structures used to describe in detail additional options for event encoding

/**
 * @brief
 *   The structure that provides a full resolution of the cluster instance.
 */
struct DetailedRootSection
{
    /**
     * Default constructor
     */
    DetailedRootSection(void) { };

    uint64_t ClusterInstanceID; /**< Cluster instance of the subject of this event. */
};

/**
 * @brief
 *   The validity and type of timestamp included in EventOptions.
 */
typedef enum TimestampType
{
    kTimestampType_Invalid = 0,
    kTimestampType_System,
    kTimestampType_UTC
} TimestampType;

/**
 * @brief
 *   The union that provides an application set system or UTC timestamp.
 */
union Timestamp
{
    /**
     * Default constructor.
     */
    Timestamp(void) : systemTimestamp(0) { };

    /**
     * UTC timestamp constructor.
     */
    Timestamp(utc_timestamp_t aUtc) : utcTimestamp(aUtc) { };

    /**
     * System timestamp constructor.
     */
    Timestamp(timestamp_t aSystem) : systemTimestamp(aSystem) { };

    timestamp_t systemTimestamp;  ///< System timestamp.
    utc_timestamp_t utcTimestamp; ///< UTC timestamp.
};

/**
 *   The structure that provides options for the different event fields.
 */
struct EventOptions
{
    EventOptions(void);
    EventOptions(bool);
    EventOptions(timestamp_t);
    EventOptions(utc_timestamp_t);
    EventOptions(timestamp_t, bool);
    EventOptions(utc_timestamp_t, bool);
    EventOptions(utc_timestamp_t, DetailedRootSection *, event_number_t, PriorityLevel, bool);
    EventOptions(timestamp_t, DetailedRootSection *, event_number_t, PriorityLevel, bool);

    Timestamp timestamp;               /**< A union holding either system or UTC timestamp. */

    DetailedRootSection * eventSource; /**< A pointer to the detailed resolution of the cluster instance.  When NULL, the event source
                                            is assumed to come from the resource equal to the local node ID, and from the default
                                            instance of the cluster. */

    PriorityLevel relatedPriority;  /**< EventPriority of the Related Event Number.  When this event and the related event are of the
                                            same priority, the field may be omitted.  A value of kPriorityLevel_Invalid implies the
                                            absence of any related event. */

    TimestampType timestampType;       /**< An enum indicating if the timestamp is valid and its type. */

    bool urgent;                       /**< A flag denoting that the event is time sensitive.  When set, it causes the event log to be flushed. */
};

/**
 * @brief
 *   Structure for copying event lists on output.
 */

struct EventLoadOutContext
{
    EventLoadOutContext(chip::TLV::TLVWriter & inWriter, PriorityLevel inPriority, uint32_t inStartingEventID, ExternalEvents * ioExternalEvent);

    chip::TLV::TLVWriter & mWriter;
    PriorityLevel mPriority;
    uint32_t mStartingEventID;
    uint32_t mCurrentTime;
    uint32_t mCurrentEventID;
    ExternalEvents *mExternalEvents;
#if CHIP_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS
    uint64_t mCurrentUTCTime;
    bool mFirstUtc;
#endif
    bool mFirst;
};

/**
 *  @brief
 *    A function that supplies eventData element for the event logging subsystem.
 *
 *  Functions of this type are expected to provide the eventData
 *  element for the event logging subsystem. The functions of this
 *  type are called after the event subsystem has generated all
 *  required event metadata. The function is called with a
 *  chip::TLV::TLVWriter object into which it will emit a single TLV element
 *  tagged kTag_EventData; the value of that element MUST be a
 *  structure containing the event data. The event data itself must
 *  be structured using context tags.
 *
 *  @sa PlainTextWriter
 *  @sa EventWriterTLVCopy
 *
 *  @param[inout] ioWriter A reference to the chip::TLV::TLVWriter object to be
 *                         used for event data serialization.
 *
 *  @param[in]    inDataTag A context tag for the TLV we're writing out.
 *
 *  @param[in]    appData  A pointer to an application specific context.
 *
 *  @retval #CHIP_NO_ERROR  On success.
 *
 *  @retval other           An appropriate error signaling to the
 *                          caller that the serialization of event
 *                          data could not be completed. Errors from
 *                          calls to the ioWriter should be propagated
 *                          without remapping. If the function
 *                          returns any type of error, the event
 *                          generation is aborted, and the event is not
 *                          written to the log.
 *
 */

typedef CHIP_ERROR (*EventWriterFunct)(chip::TLV::TLVWriter & ioWriter, uint8_t inDataTag, void * appData);

/**
 *  @brief
 *    A function prototype for platform callbacks fetching event data.
 *
 *  Similar to FetchEventsSince, this fetch function returns all events from
 *  EventLoadOutContext.mStartingEventID through ExternalEvents.mLastEventID.
 *
 *  The context pointer is of type FetchExternalEventsContext. This includes
 *  the EventLoadOutContext, with some helper variables for the format of the TLV.
 *  It also includes a pointer to the ExternalEvents struct created on registration
 *  of the callback. This specifies the event ID range for the callback.
 *
 *  On returning from the function, EventLoadOutContext.mCurrentEventID should
 *  reflect the first event ID that has not been successfully written to the
 *  TLV buffer. The platform must write the events header and data to the TLV
 *  writer in the correct format, specified by the EventLogging protocol. The
 *  platform must also maintain uniqueness of events and timestamps.
 *
 *  All TLV errors should be propagated to higher levels. For instance, running
 *  out of space in the buffer will trigger a sent message, followed by another
 *  call to the callback with whichever event ID remains.
 *
 *  @retval CHIP_ERROR_NO_MEMORY           If no space to write events.
 *  @retval CHIP_ERROR_BUFFER_TOO_SMALL    If no space to write events.
 *  @retval CHIP_NO_ERROR                  On success.
 *  @retval CHIP_END_OF_TLV                On success.
 */
typedef CHIP_ERROR (*FetchExternalEventsFunct)(EventLoadOutContext * aContext);

/**
 * @brief
 *
 *   A function prototype for a callback invoked when external events
 *   are delivered to the remote subscriber.
 *
 * When the external events are delivered to a remote subscriber, the
 * engine will provide a notification to the external event provider.
 * The callback contains the event of the last ID that was delivered,
 * and the ID of the subscriber that received the event.
 *
 * @param[in] inEv                       External events object corresponding
 *                                       to delivered events
 *
 * @param[in] inLastDeliveredEventID     ID of the last event delivered to
 *                                       the subscriber.
 * @param[in] inRecipientNodeID          CHIP node ID of the recipient
 *
 */
typedef void (*NotifyExternalEventsDeliveredFunct)(ExternalEvents * inEv, event_number_t inLastDeliveredEventID,
                                                   uint64_t inRecipientNodeID);

/**
 * @brief
 *
 *   A function prototype for a callback invoked when external events
 *   are evicted from the buffers.
 *
 * When the external events object is evicted from the outbound message
 * buffer, the engine will provide a notification to the external event
 * provider.  The callback contains the external event to be evicted.
 *
 * @param[in] inEv                       External events object to be evicted
 */
typedef void (*NotifyExternalEventsEvictedFunct)(ExternalEvents * inEv);

/**
 *  @brief
 *    Structure for tracking platform-stored events.
 */
struct ExternalEvents
{
    ExternalEvents(void) : mFirstEventID(1), mLastEventID(0), mFetchEventsFunct(NULL), mNotifyEventsDeliveredFunct(NULL), mNotifyEventsEvictedFunct(NULL) { };

    event_number_t mFirstEventID; /**< The first event ID stored externally. */
    event_number_t mLastEventID;  /**< The last event ID stored externally. */

    FetchExternalEventsFunct mFetchEventsFunct; /**< The callback to use to fetch the above IDs. */
    NotifyExternalEventsDeliveredFunct mNotifyEventsDeliveredFunct;
    NotifyExternalEventsEvictedFunct mNotifyEventsEvictedFunct;
    bool IsValid(void) const { return mFirstEventID <= mLastEventID; };
    void Invalidate(void) { mFirstEventID = 1; mLastEventID = 0; };
};

// internal API
typedef CHIP_ERROR (*LoggingBufferHandler)(void * inAppState, chip::System::PacketBuffer * inBuffer);

} // namespace reporting
} // namespace app
} // namespace chip
#endif //_CHIP_DATA_MODEL_EVENT_LOGGING_TYPES_H
