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
 *      This file defines notification engine for Weave
 *      Data Management (WDM) profile.
 *
 */

#ifndef _CHIP_DATA_MODEL_REPORTING_ENGINE_H
#define _CHIP_DATA_MODEL_REPORTING_ENGINE_H

#include <core/WeaveCore.h>


namespace chip {
namespace app {
namespace reporting {

/**
 * @class IDataElementAccessControlDelegate
 *
 * @brief Interface that is to be implemented by a processor of data
 * elements in a ReportingRequest
 */
class IDataElementAccessControlDelegate
{
public:
    virtual CHIP_ERROR DataElementAccessCheck(const TraitPath & aTraitPath, const TraitCatalogBase<TraitDataSink> & aCatalog) = 0;
};

/*
 *  @class ReportingEngine
 *
 *  @brief The reporting engine is responsible for generating reports to the initiator. It is able to find the intersection between
 *         the path interest set of each initiator with what has changed in the publisher data store and generate tailored reports
 *
 *         To achieve this, the engine tracks data-changes (i.e data dirtiness) at a couple of different levels:
 *
 *         - Per subscriber, per trait instance dirtiness: Every subscriber tracks trait-changes at a per-instance granularity.
 *           Anytime a data source makes known that a property handle within has changed, the RE will iterate over every subscriber
 *           that has subscribed to that cluster instance and mark the fact that that instance is now dirty.
 *
 *         - Granular per trait instance, per property handle dirtiness: If selected through compile-time options by the user, the
 *           engine will mark dirtiness down to the property handle. This allows it to generate compact notifies that convey as
 *           succinctly as possible the data that has changed. This will be described in more detail in the solvers section.
 *
 *         At its core, it iterates over every subscription, then every dirty instance within that subscription and tries to gather
 *         and pack as much relevant data as possible into a notify message before sending that to the subscriber. It continues to
 *         do so until it has no more work to do. This could be due to a couple of reasons:
 *
 *         - Notifies are in flight to the subscriber(s)
 *         - We have exceeded the maximum number of notifies that can be flight across all subscribers.
 *         - We have no more space in the packet to stuff in more data.
 *         - We have no more dirty data to process for a particular set of subscriptions.
 *
 *         Once it surmises there is no more work to be done, it returns. If all work for a subscription has been completed, it will
 *         invoke a method in the SubscriptionHandler to finish processing that subscription (which might involve sending out
 *         subscription responses).
 *
 *         During subscription establishment, the NE works slightly differently than at other times - it will retrieve *all* the
 *         data for a particular trait. There-after, it will only retrieve new, changed data.
 *
 *         Some notable features:
 *
 *         - Subscription fairness: The engine round-robins over all subscriptions and will always resume its work loop at the last
 *           subscription it was trying to process to ensure all subscriptions are handled with equal priority.
 *
 *         - Trait instance fairness: Within a subscription, the engine also rounds robins over all trait instances and will resume
 *           its work loop at the last trait instance that was being processed *for that subscription*. This ensures trait instances
 *           that have a high rate of change don't starve out others.
 *
 *         - Inter-trait chunking across multiple notifies: The engine supports splitting trait data over multiple notifies. It will
 *           however only do this split at the trait instance granularity. It cannot chunk up data within a trait.
 *
 *         - Graceful degradation due to resource shortages: If it runs out space in the dirty stores, the engine will degrade
 *           gracefully by generating sub-optimal notify messages that have more data in them while still being protocol correct.
 *
 */
class ReportingEngine
{
public:
    /**
     * Initializes the engine. Should only be called once.
     *
     * @retval #WEAVE_NO_ERROR On success.
     * @retval other           Was unable to retrieve data and write it into the writer.
     */
    CHIP_ERROR Init(void);

    /**
     * Main work-horse function that executes the run-loop.
     */
    void Run(void);

    /**
     * Main work-horse function that executes the run-loop asynchronously on the CHIP thread
     */
    void ScheduleRun(void);

    enum ReportingRequestBuilderState
    {
        kReportingRequestBuilder_Idle = 0,      ///< The request has not been opened or has been closed and finalized
        kReportingRequestBuilder_Ready,         ///< The request has been initialized and is ready for any optional toplevel elements
        kReportingRequestBuilder_BuildAttributeList, ///< The request is building the AttributeList portion of the structure
        kReportingRequestBuilder_BuildEventList ///< The request is building the EventList portion of the structure
    };

    /**
     *  @class ReportingRequestBuilder
     *
     *  @brief This provides a helper class to compose notifies and abstract away the construction and structure of the message from
     *         its consumers. This is a more compact version of a similar class provided in MessageDef.cpp that aims to be sensitive
     *         to the flash and ram needs of the device.
     */
    class ReportingRequestBuilder
    {
    public:
        /**
         * Initializes the builder. Should only be called once.
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval other           Was unable to initialize the builder.
         */
        CHIP_ERROR Init(PacketBuffer * aBuf, TLV::TLVWriter * aWriter, SubscriptionHandler * aSubHandler,
                         uint32_t aMaxPayloadSize);

        /**
         * Start the construction of the notify.
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval #CHIP_ERROR_INCORRECT_STATE If the request is not at the toplevel of the buffer.
         * @retval other           Unable to construct the end of the notify.
         */

        CHIP_ERROR StartReportingRequest();

        /**
         * End the construction of the notify.
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval #CHIP_ERROR_INCORRECT_STATE If the request is not at Reporting container.
         * @retval other           Unable to construct the end of the notify.
         */
        CHIP_ERROR EndReportingRequest();

        /**
         * Starts the construction of the data list array.
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval #CHIP_ERROR_INCORRECT_STATE If the request is not at the Reporting container.
         * @retval other           Unable to construct the beginning of the data list.
         */
        CHIP_ERROR StartDataList(void);

        /**
         * End the construction of the data list array.
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval #CHIP_ERROR_INCORRECT_STATE If the request is not at the DataList container.
         * @retval other           Unable to construct the end of the data list.
         */
        CHIP_ERROR EndDataList();

        /**
         * Starts the construction of the event list.
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval #CHIP_ERROR_INCORRECT_STATE If the request is not at the Reporting container.
         * @retval other           Unable to construct the beginning of the data list.
         */
        CHIP_ERROR StartEventList();

        /**
         * End the construction of the event list.
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval #CHIP_ERROR_INCORRECT_STATE If the request is not at the EventList container.
         * @retval other           Unable to construct the end of the data list.
         */
        CHIP_ERROR EndEventList();

        /**
         * Given a trait path, write out the data element associated with that path. The caller can also optionally pass in a handle
         * set allows for leveraging the merge operation with a narrower set of immediate child nodes of the parent property path
         * handle.
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval other           Unable to retrieve and write the data element.
         */
        CHIP_ERROR WriteDataElement(TraitDataHandle aTraitDataHandle, PropertyPathHandle aPropertyPathHandle,
                                     SchemaVersion aSchemaVersion, PropertyPathHandle * aMergeDataHandleSet,
                                     uint32_t aNumMergeDataHandles, PropertyPathHandle * aDeleteHandleSet,
                                     uint32_t aNumDeleteHandles);

        /**
         * Checkpoint the request state into a TLVWriter
         *
         * @param[out] aPoint A writer to checkpoint the state of the TLV writer into.
         *
         * @retval #WEAVE_NO_ERROR On success.
         */
        CHIP_ERROR Checkpoint(TLV::TLVWriter & aPoint);

        /**
         * Rollback the request state into the checkpointed TLVWriter
         *
         * @param[in] aPoint A writer to that captured the state at some point in the past
         *
         * @retval #WEAVE_NO_ERROR On success.
         */
        CHIP_ERROR Rollback(TLV::TLVWriter & aPoint);

        TLV::TLVWriter * GetWriter(void) { return mWriter; }

        /**
         * The main state transition function. The function takes the desired state (i.e., the phase of the notify request builder
         * that we would like to reach), and transitions the request into that state. If the desired state is the same as the
         * current state, the function does nothing. Otherwise, an PacketBuffer is allocated (if needed); the function first
         * transitions the request into the toplevel notify request (either opening the notify request TLV structure, or closing the
         * current TLV data container as needed), and then transitions the Reporting request either by opening the appropriate TLV data
         * container or by closing the overarching Reporting request.
         *
         * @param aDesiredState  The desired state the request should transition into
         *
         * @retval #WEAVE_NO_ERROR On success.
         * @retval #CHIP_ERROR_NO_MEMORY Could not transition into the state because of insufficient memory.
         * @retval #CHIP_ERROR_INCORRECT_STATE Corruption of the internal state machine.
         * @retval other When the state machine could not record the state in its buffer, likely indicates a design flaw rather than
         * a runtime issue.
         */
        CHIP_ERROR MoveToState(ReportingRequestBuilderState aDesiredState);

    private:
        TLV::TLVWriter * mWriter;
        ReportingRequestBuilderState mState;
        PacketBuffer * mBuf;
        uint32_t mMaxPayloadSize;
    };

private:
    CHIP_ERROR BuildSingleReportingRequestDataList(SubscriptionHandler * aSubHandler, ReportingRequestBuilder & aReportingRequest,
                                                 bool & isSubscriptionClean, bool & aNeWriteInProgress);
    CHIP_ERROR BuildSingleReportingRequestEventList(SubscriptionHandler * aSubHandler, ReportingRequestBuilder & aReportingRequest,
                                                  bool & isSubscriptionClean, bool & aNeWriteInProgress);
    CHIP_ERROR BuildSingleReportingRequest(SubscriptionHandler * aSubHandler, bool & aSubscriptionHandled,
                                         bool & isSubscriptionClean);

    CHIP_ERROR SendReporting(PacketBuffer * aBuf, SubscriptionHandler * aSubHandler);

    CHIP_ERROR SendReportingRequest();

    static void Run(System::Layer * aSystemLayer, void * aAppState, System::Error);

    uint32_t mNumNotifiesInFlight;
    chip::TLV::TLVType mOuterContainerType;
};

}; // namespace reporting
}; // namespace app
}; // namespace chip

#endif // _CHIP_DATA_MODEL_REPORTING_ENGINE_H
