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

Thread parent restart WITHOUT child supervision on the LIT ICD.

The end device runs with the OpenThread child supervision check disabled
(check timeout 0). After the parent restarts without its child table (same
RLOC16), the parent radio keeps acknowledging the child's data polls, so the
child never detaches and the border router cannot reach it for the whole
observation window: the black hole the supervision test bounds. See
silabs_lit_icd_parent_restart.
"""

import sys

from silabs_lit_icd_parent_restart import (
    PARENT_RESTART_DEVICE_CONFIG,
    TestAndroidBleThreadSilabsLitIcdParentRestart,
)

# Polls the parent must have acknowledged during the window, so that "no
# recovery" cannot come from a child that simply stopped polling.
_MIN_ACKED_POLLS = 3


class TestAndroidBleThreadSilabsLitIcdNoSupervisionRestart(
    TestAndroidBleThreadSilabsLitIcdParentRestart
):
  SUPERVISION_CHECK_TIMEOUT_S = 0

  def _assert_outcome(self, results: dict) -> None:
    downlink = results['downlink']
    self.assertFalse(
        downlink['recovered'],
        f'Child became reachable again without child supervision: {downlink}',
    )
    self.assertTrue(
        results['polls_acked_while_blind'] >= _MIN_ACKED_POLLS,
        f'Only {results["polls_acked_while_blind"]} child data polls were'
        ' MAC-acknowledged during the window; expected the child to keep'
        f' polling (>= {_MIN_ACKED_POLLS})',
    )
    self.assertTrue(
        results['child_detach_t'] is None,
        'Child detached without child supervision:'
        f' {results["child_roles_after_kill"]}',
    )
    self.assertTrue(
        results['first_parent_request_t'] is None,
        'Child sent an MLE Parent Request without child supervision',
    )
    self.assertEqual(
        results['checkins']['blind']['wifi'],
        0,
        'Uplink Matter black-hole failed: Check-In reached Wi-Fi during blind'
        ' window',
    )
    self.assertEqual(
        results['checkins']['blind']['android_cb'],
        0,
        'Uplink Matter black-hole failed: Check-In reached Android during blind'
        ' window',
    )
    self.assertTrue(
        results['uplink_data_acked_while_blind'] > 0,
        'No uplink MAC Data frames were Imm-Acked during the blind window',
    )
    self.assertTrue(
        results['otbr_unknown_neighbor_data_drops'] > 0,
        'No otbr-agent Unknown neighbor data drops were logged during the blind'
        ' window',
    )
    self.logger.info(
        'No-supervision restart oracles passed: child unreachable for'
        ' >= %.0fs, never detached, %d polls falsely acked (%d with frame'
        ' pending), %d MAC Data frames acked, %d Unknown neighbor drops (%d'
        ' Data drops), %d DNS resolution failures, Check-Ins in window: %s',
        downlink['blackhole_at_least_s'],
        results['polls_acked_while_blind'],
        results['polls_acked_while_blind_fp'],
        results['uplink_data_acked_while_blind'],
        results['otbr_unknown_neighbor_drops'],
        results['otbr_unknown_neighbor_data_drops'],
        results['checkin_dns_failures_while_blind'],
        results['checkins']['blind'],
    )


if __name__ == '__main__':
  sys.exit(
      TestAndroidBleThreadSilabsLitIcdNoSupervisionRestart(
          PARENT_RESTART_DEVICE_CONFIG
      ).run_test()
  )
