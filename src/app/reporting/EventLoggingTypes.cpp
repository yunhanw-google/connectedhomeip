/**
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2016-2017 Nest Labs, Inc.
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
 *   Implementations of enums, types, and tags used in Chip Event Logging.
 *
 */

#include "EventLoggingTypes.h"

namespace chip {
namespace app {
namespace reporting {

EventOptions::EventOptions(void) :
    timestamp(), eventSource(NULL),
    timestampType(kTimestampType_Invalid), urgent(false)
{ }

EventOptions::EventOptions(bool aUrgent) :
    timestamp(), eventSource(NULL),
    timestampType(kTimestampType_Invalid), urgent(aUrgent)
{ }

EventOptions::EventOptions(timestamp_t aSystemTimestamp) :
    timestamp(aSystemTimestamp), eventSource(NULL),
    timestampType(kTimestampType_System), urgent(false)
{ }

EventOptions::EventOptions(utc_timestamp_t aUtcTimestamp) :
    timestamp(aUtcTimestamp), eventSource(NULL),
    timestampType(kTimestampType_UTC), urgent(false)
{ }

EventOptions::EventOptions(timestamp_t aSystemTimestamp, bool aUrgent) :
    timestamp(aSystemTimestamp), eventSource(NULL),
    timestampType(kTimestampType_System), urgent(aUrgent)
{ }

EventOptions::EventOptions(utc_timestamp_t aUtcTimestamp, bool aUrgent) :
    timestamp(aUtcTimestamp), eventSource(NULL),
    timestampType(kTimestampType_UTC), urgent(aUrgent)
{ }

EventLoadOutContext::EventLoadOutContext(chip::TLV::TLVWriter & inWriter, ImportanceLevel inImportance,
                                         uint32_t inStartingEventID, ExternalEvents * ioExternalEvents) :
    mWriter(inWriter),
    mImportance(inImportance), mStartingEventID(inStartingEventID), mCurrentTime(0), mCurrentEventID(0),
    mExternalEvents(ioExternalEvents),
#if CHIP_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS
    mCurrentUTCTime(0), mFirstUtc(true),
#endif // CHIP_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS
    mFirst(true)
{ }

} // namespace reporting
} // namespace app
} // namespace chip

