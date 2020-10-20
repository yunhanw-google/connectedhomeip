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

/**
 * @file
 *
 * @brief
 *   Configuration of the CHIP Event Logging.
 *
 */


#include "EventLoggingTypes.h"

#ifndef _CHIP_DATA_MODEL_EVENT_LOGGING_CONFIGURATION_H
#define _CHIP_DATA_MODEL_EVENT_LOGGING_CONFIGURATION_H

// Special node id values.
enum
{
    kNodeIdNotSpecified = 0ULL,
    kAnyNodeId          = 0xFFFFFFFFFFFFFFFFULL
};

namespace chip {
namespace app {
namespace reporting {

/**
 * @brief
 *   LoggingConfiguration encapsulates the configurable component
 *   of the chip Event Logging subsystem.
 */
class LoggingConfiguration
{
public:
    LoggingConfiguration(void);

    ImportanceLevel GetProfileImportance(uint32_t profileId) const;

    bool SupportsPerProfileImportance(void) const;

    uint64_t GetDestNodeId() const;

    chip::Inet::IPAddress GetDestNodeIPAddress() const;

public:
    static LoggingConfiguration & GetInstance(void);

    ImportanceLevel mGlobalImportance;
    timestamp_t mImportanceExpiration;
    duration_t mMinimumLogUploadInterval;
    duration_t mMaximumLogUploadInterval;
    char * mLoggingDestination; // should be changeable at runtime; need to drive the semantics of change.
    uint64_t mDestNodeId;
    chip::Inet::IPAddress mDestNodeIPAddress;
    uint32_t mUploadThreshold;
    uint32_t mLoggingVolume;
};

} // namespace reporting
} // namespace app
} // namespace chip

#endif // _CHIP_DATA_MODEL_EVENT_LOGGING_CONFIGURATION_H
