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
 *  @file
 *
 *  @brief
 *    API definitions for Chip Event Logging
 *
 *  This file contains the definitions for the Chip Event Logging.
 *  The file defines the API for configuring and controlling the
 *  logging subsystem as well as the API for emitting the individual
 *  log entries.
 */

#pragma once

#ifndef _CHIP_INTERACTION_MODEL_LOGGING__H
#define _CHIP_INTERACTION_MODEL_LOGGING__H

#include "EventLoggingTags.h"
#include "EventLoggingTypes.h"

namespace chip {
namespace app {
namespace reporting{

/**
 * @brief
 *   Log an event from a pre-serialized form.
 *
 * The function logs an event represented as a chip::TLV::TLVReader.  This
 * implies that the event data representation is already serialized in
 * the storage underlying the chip:TLV::TLVReader.  The chip::TLV::TLVReader is expected to
 * contain at least a single data element, that element must be a
 * structure. The first element read out of the reader is treated as
 * event data and stored in the event log.  The event data MUST
 * contain context tags to be interpreted within the schema identified
 * by inProfileID and inEventType. The tag of the first element will
 * be ignored; the event logging system will replace it with the
 * eventData tag.
 *
 * The event is logged if its inPriority exceeds the logging
 * threshold specified in the LoggingConfiguration.  If the event's
 * priority does not meet the current threshold, it is dropped and
 * the function returns a `0` as the resulting event ID.
 *
 * This variant of the invocation implicitly specifies all the default
 * event options:
 * - the event is timestamped with the current time at the point of the call,
 * - the event is marked as relating to the device that is making the call,
 * - the event is standalone, not relating to any other events,
 * - the event is marked as non-urgent,
 *
 * @param[in] inSchema     Schema defining priority, path of this event.
 *
 * @param[in] inData       The TLV reader containing the event data as the
 *                         first element.
 *
 * @return event_number_t      The event ID if the event was written to the
 *                         log, 0 otherwise.
 */
event_number_t LogEvent(const EventSchema & inSchema, chip::TLV::TLVReader & inData);

/**
 * @brief
 *   Log an event from a pre-serialized form, with additional options.
 *
 * The function logs an event represented as a chip::TLV::TLVReader.  This
 * implies that the event data representation is already serialized in
 * the storage underlying the chip::TLV::TLVReader.  The chip::TLV::TLVReader is expected to
 * contain at least a single data element, that element must be a
 * structure. The first element read out of the reader is treated as
 * event data and stored in the event log.  The event data MUST
 * contain context tags to be interpreted within the schema identified
 * by inProfileID and inEventType. The tag of the first element will
 * be ignored; the event logging system will replace it with the
 * eventData tag.
 *
 * The event is logged if its inPriority exceeds the logging
 * threshold specified in the LoggingConfiguration.  If the event's
 * priority does not meet the current threshold, it is dropped and
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
 * @param[in] inSchema     Schema defining priority, path type of this event.
 *
 * @param[in] inData       The TLV reader containing the event data as the
 *                         first element. Must not be NULL
 *
 * @param[in] inOptions    The options for the event metadata. May be NULL.
 *
 * @return event_number_t      The event ID if the event was written to the
 *                         log, 0 otherwise.
 */
event_number_t LogEvent(const EventSchema & inSchema, chip::TLV::TLVReader & inData, const EventOptions * inOptions);

/**
 * @brief
 *   Log an event via a callback.
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
 * The event is logged if its inPriority exceeds the logging
 * threshold specified in the LoggingConfiguration.  If the event's
 * priority does not meet the current threshold, it is dropped and
 * the function returns a `0` as the resulting event ID.
 *
 * This variant of the invocation implicitly specifies all the default
 * event options:
 * - the event is timestamped with the current time at the point of the call,
 * - the event is marked as relating to the device that is making the call,
 * - the event is standalone, not relating to any other events,
 * - the event is marked as non-urgent,
 *
 * @param[in] inSchema     Schema defining priority, patth type of this event.
 *
 * @param[in] inEventWriter The callback to invoke to actually
 *                         serialize the event data
 *
 * @param[in] inAppData    Application context for the callback.
 *
 * @return event_number_t      The event ID if the event was written to the
 *                         log, 0 otherwise.
 */
event_number_t LogEvent(const EventSchema & inSchema, EventWriterFunct inEventWriter, void * inAppData);

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
 * The event is logged if its inPriority exceeds the logging
 * threshold specified in the LoggingConfiguration.  If the event's
 * priority does not meet the current threshold, it is dropped and
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
 * @param[in] inSchema     Schema defining priority, path of this event.
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
event_number_t LogEvent(const EventSchema & inSchema, EventWriterFunct inEventWriter, void * inAppData, const EventOptions * inOptions);

/**
 * @brief
 *   LogFreeform emits a freeform string to the default event stream.
 *
 * The string will be encapsulated in an debug event structure,
 * structurally identical to other logged strings.  The event profile
 * ID will be that of a Nest Debug event, and the event type will be
 * `kNestDebug_StringLogEntryEvent`.
 *
 * @param[in] inPriority  Priority of the log entry; if the priority
 *                          falls below the current priority, the event is
 *                          not actually logged
 *
 * @param[in] inFormat      `printf`-compliant format string, followed by
 *                          arguments to be formatted
 *
 * @return event_number_t      The event ID if the event was written to the
 *                         log, 0 otherwise.
 *
 */

event_number_t LogFreeform(PriorityLevel inPriority, const char * inFormat, ...);

/**
 * @brief
 *   A helper function for emitting a freeform text as a debug
 *   event. The debug event is a structure with a logregion and a
 *   freeform text.
 *
 * @param[inout] ioWriter The writer to use for writing out the event
 *
 * @param[in] inDataTag   The tag to emit
 *
 * @param[in] appData     A pointer to the DebugLogContext, a structure
 *                        that holds a string format, arguments, and a
 *                        log region
 *
 * @retval #CHIP_NO_ERROR On success.
 *
 * @retval other          Other errors that may be returned from the ioWriter.
 *
 */
CHIP_ERROR PlainTextWriter(chip::TLV::TLVWriter & ioWriter, uint8_t inDataTag, void * appData);

} // namespace reporting
} // namespace app
} // namespace chip
#endif // _CHIP_INTERACTION_MODEL_LOGGING__H
