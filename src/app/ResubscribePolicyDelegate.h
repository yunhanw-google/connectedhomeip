/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
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

#pragma once

#include <lib/core/CHIPCore.h>
#include <lib/support/FibonacciUtils.h>

namespace chip {
namespace app {
/**
 * @brief An ResubscribePolicyDelegate is used to configure resubscribe policy. Run expect to input the number of retires as seed,
 * and get the interval for next retry and the boolean regarding if it should resubscribe
 */
class ResubscribePolicyDelegate
{
public:
    virtual ~ResubscribePolicyDelegate() {}
    virtual void Run(uint32_t aNumRetries, uint32_t & aIntervalMsec, bool & aShouldResubscribe) = 0;
};

/**
 * @brief The default policy implementation will pick a random timeslot
 * with millisecond resolution over an ever increasing window,
 * following a fibonacci sequence upto CHIP_RESUBSCRIBE_MAX_FIBONACCI_STEP_INDEX.
 * Average of the randomized wait time past the CHIP_RESUBSCRIBE_MAX_FIBONACCI_STEP_INDEX
 * will be around one hour.
 * When the retry count resets to 0, the sequence starts from the beginning again.
 */
class DefaultResubscribePolicyImpl : public ResubscribePolicyDelegate
{
public:
    void Run(uint32_t aNumRetries, uint32_t & aIntervalMsec, bool & aShouldResubscribe) final override
    {
        uint32_t fibonacciNum      = 0;
        uint32_t maxWaitTimeInMsec = 0;
        uint32_t waitTimeInMsec    = 0;
        uint32_t minWaitTimeInMsec = 0;

        if (aNumRetries == 2)
        {
            aShouldResubscribe = false;
            return;
        }
        if (aNumRetries <= CHIP_RESUBSCRIBE_MAX_FIBONACCI_STEP_INDEX)
        {
            fibonacciNum      = GetFibonacciForIndex(aNumRetries);
            maxWaitTimeInMsec = fibonacciNum * CHIP_RESUBSCRIBE_WAIT_TIME_MULTIPLIER_MS;
        }
        else
        {
            maxWaitTimeInMsec = CHIP_RESUBSCRIBE_MAX_RETRY_WAIT_INTERVAL_MS;
        }

        if (maxWaitTimeInMsec != 0)
        {
            minWaitTimeInMsec = (CHIP_RESUBSCRIBE_MIN_WAIT_TIME_INTERVAL_PERCENT_PER_STEP * maxWaitTimeInMsec) / 100;
            waitTimeInMsec    = minWaitTimeInMsec + (Crypto::GetRandU32() % (maxWaitTimeInMsec - minWaitTimeInMsec));
        }

        aIntervalMsec      = waitTimeInMsec;
        aShouldResubscribe = true;
        ChipLogProgress(DataManagement,
                        "Computing Resubscribe policy: attempts %" PRIu32 ", max wait time %" PRIu32
                        " ms, selected wait time %" PRIu32 " ms",
                        aNumRetries, maxWaitTimeInMsec, waitTimeInMsec);
        return;
    }
};
} // namespace app
} // namespace chip
