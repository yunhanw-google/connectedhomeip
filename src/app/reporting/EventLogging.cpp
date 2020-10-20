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

#include <stdarg.h>
#include <core/CHIPCore.h>
#include <core/CHIPEncoding.h>
#include <core/CHIPSafeCasts.h>
#include <core/CHIPTLV.h>
#include <support/Base64.h>
#include <support/CodeUtils.h>
#include <support/ErrorStr.h>
#include <support/logging/CHIPLogging.h>
#include "EventLogging.h"
#include "EventLoggingTags.h"
#include "EventLoggingTypes.h"
#include "LoggingManagement.h"

using namespace chip::TLV;

namespace chip {
namespace app {
namespace reporting{

/**
 * @brief
 *   A helper function that translates an already serialized eventdata element into the event buffer.
 *
 * @param[inout] ioWriter The writer to use for writing out the event
 *
 * @param[in] inDataTag   A context tag for the TLV we're copying out.  Unused here,
 *                        but required by the typedef for EventWriterFunct.
 *
 * @param[in] appData     A pointer to the TLVReader that holds serialized event data.
 *
 * @retval #CHIP_NO_ERROR On success.
 *
 * @retval other          Other errors that mey be returned from the ioWriter.
 *
 */
static CHIP_ERROR EventWriterTLVCopy(TLVWriter & ioWriter, uint8_t inDataTag, void * appData)
{
    CHIP_ERROR err    = CHIP_NO_ERROR;
    TLVReader * reader = static_cast<TLVReader *>(appData);

    VerifyOrExit(reader != NULL, err = CHIP_ERROR_INVALID_ARGUMENT);

    err = reader->Next();
    SuccessOrExit(err);

    err = ioWriter.CopyElement(chip::TLV::ContextTag(kTag_EventData), *reader);

exit:
    return err;
}

event_number_t LogEvent(const EventSchema & inSchema, TLVReader & inData)
{
    return LogEvent(inSchema, inData, NULL);
}

event_number_t LogEvent(const EventSchema & inSchema, TLVReader & inData, const EventOptions * inOptions)
{
    return LogEvent(inSchema, EventWriterTLVCopy, &inData, inOptions);
}

event_number_t LogEvent(const EventSchema & inSchema, EventWriterFunct inEventWriter, void * inAppData)
{
    return LogEvent(inSchema, inEventWriter, inAppData, NULL);
}

event_number_t LogEvent(const EventSchema & inSchema, EventWriterFunct inEventWriter, void * inAppData, const EventOptions * inOptions)
{

    LoggingManagement & logManager = LoggingManagement::GetInstance();

    return logManager.LogEvent(inSchema, inEventWriter, inAppData, inOptions);
}

struct DebugLogContext
{
    const char * mRegion;
    const char * mFmt;
    va_list mArgs;
};

/**
 * @brief
 *   A helper function for emitting a freeform text as a debug
 *   event. The debug event is a structure with a logregion and a
 *   freeform text.
 *
 * @param[inout] ioWriter The writer to use for writing out the event
 *
 * @param[in] appData     A pointer to the DebugLogContext, a structure
 *                        that holds a string format, arguments, and a
 *                        log region
 *
 * @param[in]             inDataTag A context tag for the TLV we're writing out.  Unused here,
 *                        but required by the typedef for EventWriterFunct.
 *
 * @retval #CHIP_NO_ERROR On success.
 *
 * @retval other          Other errors that mey be returned from the ioWriter.
 *
 */
CHIP_ERROR PlainTextWriter(TLVWriter & ioWriter, uint8_t inDataTag, void * appData)
{
    CHIP_ERROR err           = CHIP_NO_ERROR;
    DebugLogContext * context = static_cast<DebugLogContext *>(appData);
    TLVType outer;

    err = ioWriter.StartContainer(chip::TLV::ContextTag(kTag_EventData), kTLVType_Structure, outer);
    SuccessOrExit(err);

    err = ioWriter.PutString(chip::TLV::ContextTag(kTag_Region), context->mRegion);
    SuccessOrExit(err);

    err = ioWriter.VPutStringF(chip::TLV::ContextTag(kTag_Message), reinterpret_cast<const char *>(context->mFmt),
                               context->mArgs);
    SuccessOrExit(err);

    err = ioWriter.EndContainer(outer);
    SuccessOrExit(err);

    ioWriter.Finalize();

exit:
    return err;
}

event_number_t LogFreeform(ImportanceLevel inImportance, const char * inFormat, ...)
{
    DebugLogContext context;
    event_number_t eid;
    EventSchema schema = { kChipDebug, kChipDebug_StringLogEntryEvent, inImportance };

    va_start(context.mArgs, inFormat);
    context.mRegion = "";
    context.mFmt    = inFormat;

    eid = LogEvent(schema, PlainTextWriter, &context, NULL);

    va_end(context.mArgs);

    return eid;
}

} // namespace reporting
} // namespace app
} // namespace chip
