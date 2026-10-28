#!/usr/bin/env python3
"""Copyright (c) 2026 Project CHIP Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Thread parent restart WITH child supervision on the LIT ICD.

The end device runs with an OpenThread child supervision check timeout of
SUPERVISION_CHECK_TIMEOUT_S. After the parent restarts without its child
table (same RLOC16), the child must notice within about that timeout: it
sends an MLE Child Update Request, gets no answer, detaches, re-attaches,
and the border router can reach it again. See silabs_lit_icd_parent_restart.
"""

import sys

from silabs_lit_icd_parent_restart import (
    PARENT_RESTART_DEVICE_CONFIG,
    TestAndroidBleThreadSilabsLitIcdParentRestart,
)

# Slack on top of the check timeout: Child Update Request retries, the MLE
# attach (Parent Request / Child ID Request) and the 1s ping probe period.
_RECOVERY_SLACK_S = 45.0


class TestAndroidBleThreadSilabsLitIcdSupervisionRestart(
    TestAndroidBleThreadSilabsLitIcdParentRestart
):
  SUPERVISION_CHECK_TIMEOUT_S = 30

  def _assert_outcome(self, results: dict) -> None:
    t_up = results['restart']['t_up']
    downlink = results['downlink']
    bound = self.SUPERVISION_CHECK_TIMEOUT_S + _RECOVERY_SLACK_S
    self.assertTrue(
        downlink['recovered'],
        f'Child stayed unreachable for the whole {self.OBSERVE_S}s window'
        f' with child supervision on: {downlink}',
    )
    self.assertTrue(
        downlink['blackhole_s'] <= bound,
        f'Black hole lasted {downlink["blackhole_s"]:.1f}s, over the'
        f' {bound:.0f}s bound (check timeout + slack)',
    )
    self.assertTrue(
        results['polls_acked_while_blind'] >= 1,
        'No child data poll was MAC-acknowledged by the restarted parent'
        ' before recovery; the stale-parent trap was not exercised',
    )
    self.assertTrue(
        results['first_child_update_request_t'] is not None,
        'No MLE Child Update Request from the child after the restart in'
        ' thread_0.pcap',
    )
    self.assertTrue(
        results['child_detach_t'] is not None
        and results['child_reattach_t'] is not None,
        'Child did not detach and re-attach after the restart:'
        f' {results["child_roles_after_kill"]}',
    )
    self.assertTrue(
        results['first_child_update_request_t']
        <= results['child_detach_t'] + 1.0,
        'Child detached before probing its parent with a Child Update'
        ' Request',
    )
    after = results['checkins']['after_recovery']
    self.assertTrue(
        after is not None and after['android_cb'] >= 1,
        'No Check-In reached Android CHIPTool after the child recovered:'
        f' {after}',
    )
    self.logger.info(
        'Supervision restart oracles passed: black hole %.1fs (bound %.0fs),'
        ' Child Update Request at +%.1fs, detach at +%.1fs, re-attach at'
        ' +%.1fs, %d polls falsely acked, %d Check-Ins delivered after'
        ' recovery',
        downlink['blackhole_s'],
        bound,
        results['first_child_update_request_t'] - t_up,
        results['child_detach_t'] - t_up,
        results['child_reattach_t'] - t_up,
        results['polls_acked_while_blind'],
        after['android_cb'],
    )


if __name__ == '__main__':
  sys.exit(
      TestAndroidBleThreadSilabsLitIcdSupervisionRestart(
          PARENT_RESTART_DEVICE_CONFIG
      ).run_test()
  )
