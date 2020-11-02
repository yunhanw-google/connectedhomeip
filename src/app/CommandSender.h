/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2017-2019 Nest Labs, Inc.
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
 *      This file defines invoke command sender for CHIP
 *      Interaction Management profile.
 *
 */


#ifndef _CHIP_INTERACTION_MODEL_MESSAGE_DEF_H
#define _CHIP_INTERACTION_MODEL_MESSAGE_DEF_H

#include <core/CHIPCore.h>
#include <core/CHIPTLV.h>
#include <support/CodeUtils.h>
#include <support/DLLUtil.h>
#include <support/logging/CHIPLogging.h>

namespace chip {
namespace app {

class InvokeCommandRequestSender
{
public:
    enum CommandSendState
    {
        kState_Uninitialized = 0,    ///< The command sender has not been initialized
        kState_Initialized,          ///< The command sender has been initialized and is ready
        kState_AddCommandInList,           ///< The command sender has added Command
        kState_AwaitingResponse,     ///< The command sender has sent the invoke command request, and pending for response
    };

    enum EventType
    {
        kEvent_CommunicationError                 = 1,        // All errors during communication (transmission error, failure to get a response timeout) will be communicated through this event
        kEvent_StatusReportReceived               = 2,        // On receipt of a status report
        kEvent_ResponseReceived                   = 3,        // On receipt of a command response

        kEvent_DefaultCheck                       = 100,    //< Used to verify correct default event handling in the application.
    };

    /**
     * Data returned to the sender by the recipient of the custom command.
     */
    struct InEventParam
    {
        void Clear(void) { memset(this, 0, sizeof(*this)); }

        union {
            struct
            {
                Profiles::StatusReporting::StatusReport *statusReport;
            } StatusReportReceived;

            /*
             * All communication errors, including transport errors on transmission
             * as well as failure to receive a response (CHIP_ERROR_TIMEOUT).
             */
            struct
            {
                CHIP_ERROR error;
            } CommunicationError;

            /* On receipt of a command response, both a reader positioned at the
             * payload as well as a pointer to the packet buffer is provided. If the applicaton
             * desires to hold on to the buffer, it can do so by incrementing the ref count on the buffer
             */
            struct
            {
                chip::TLV::TLVReader *reader;
                chip::PacketBuffer *packetBuf;
            } ResponseReceived;
        };
    };

    /**
     * Data returned back to a CommandSender object from an EventCallback
     * function.
     */
    struct OutEventParam
    {
        void Clear(void) { memset(this, 0, sizeof(*this)); }
        bool defaultHandlerCalled;  /**< should be set if DefaultEventHandler is called */
    };

    typedef void (*EventCallback)(void * const aAppState, EventType aEvent, const InEventParam &aInParam, OutEventParam &aOutEventParam);

    /**
     * Encapsulates arguments to be passed into SendCommand().
     *
     * At minimum, ResourceId, ProfileId, and CommandType should be set before
     * the struct is passed to SendCommand(). InstanceId, Flags, and
     * VersionRange will default to 0 if not set.
     */
    struct SendCommandParams {
        uint16_t aEndpintId;
        uint32_t aGroupId;
        uint32_t aClusterId;
        uint16_t aCommandId,
        uint8_t  Flags;
    };

    typedef enum CommandFlags
    {
        kCommandFlag_EndpointIdValid   = 0x0001,  /**< Set when the EndpointId field is valid */
        kCommandFlag_GroupIdValid  = 0x0002,     /**< Set when the GroupId field is valid */
    } CommandFlags;

public:
    static void DefaultEventHandler(void *aAppState, EventType aEvent, const InEventParam& aInParam, OutEventParam& aOutParam);
    CHIP_ERROR Init(const EventCallback aEventCallback, void * const aAppState);
    CHIP_ERROR AddCommandInList(PacketBuffer *apCommandBuf, uint16_t aEndpintId, uint32_t aGroupId, uint32_t aClusterId, uint16_t aCommandId, uint8_t Flags)
    CHIP_ERROR AddCommandInList(PacketBuffer *apCommandBuf, SendCommandParams &aSendCommandParams);
    CHIP_ERROR Flush();

    void ResetCommand(bool aAbort = false);
    void ResetCommand(void);
    CHIP_ERROR Shutdown(void)

private:
    static void OnMessageReceived(chip::ExchangeContext *aEC, const chip::IPPacketInfo *aPktInfo, const chip::WeaveMessageInfo *aMsgInfo, uint32_t aProfileId, uint8_t aMsgType, chip::PacketBuffer *aPayload);
    static void OnResponseTimeout(chip::ExchangeContext *aEC);
    static void OnSendError(chip::ExchangeContext *aEC, CHIP_ERROR sendError, void *aMsgCtxt);

    void FlushExistingExchangeContext(const bool aAbortNow);

    #if CHIP_DETAIL_LOGGING
    const char * GetStateStr() const
    #enfif // CHIP_DETAIL_LOGGING
    void MoveToState(const InvokeCommandRequestState aTargetState);
    void ClearState(void);

    EventCallback mEventCallback;
    chip::PacketBuffer *mPacketBuf = NULL;
    chip::ExchangeContext *mEC = NULL;
    uint8_t mFlags = 0;
    CommandList::Builder mCommandList;
    InvokeCommandRequest::Builder mRequest;
    void *mAppState = NULL;
};

}; // namespace app
}; // namespace chip

#endif // _CHIP_INTERACTION_MODEL_MESSAGE_DEF_H
