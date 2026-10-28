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

Hermetic contract tests for AndroidBleThreadSilabsLockRfRssiTest.py: the
harness binds the Silabs lock app, every self.assert* helper it calls exists
on CHIPVirtualHome, and its OpenThread output parsers behave.
"""

import os
import re
import sys
import unittest
import yaml

_TEST_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _TEST_DIR)

import AndroidBleThreadSilabsLockRfRssiTest as rf_e2e  # pylint: disable=g-import-not-at-top
from helper.coverage_report import LLVM_COVERAGE_BINARIES  # pylint: disable=g-import-not-at-top
from helper.paths import (  # pylint: disable=g-import-not-at-top
    CHIP_REPO_PATH,
    CHIP_SILABS_LOCK_APP_ESC,
)


class SilabsLockRfRssiHarnessContractTest(unittest.TestCase):

  def test_harness_binds_silabs_lock_app(self):
    harness = rf_e2e.TestAndroidBleThreadSilabsLockRfRssi()
    self.assertEqual(
        harness._get_end_device_binary_esc(), CHIP_SILABS_LOCK_APP_ESC
    )

  def test_every_assert_helper_exists_on_harness(self):
    harness = rf_e2e.TestAndroidBleThreadSilabsLockRfRssi()
    src_path = os.path.join(
        _TEST_DIR, 'AndroidBleThreadSilabsLockRfRssiTest.py'
    )
    with open(src_path, 'r', encoding='utf-8') as f:
      source = f.read()
    helpers = sorted(set(re.findall(r'self\.(assert\w+)\(', source)))
    self.assertTrue(helpers)
    for helper in helpers:
      self.assertTrue(
          callable(getattr(harness, helper, None)),
          f'AndroidBleThreadSilabsLockRfRssiTest.py uses self.{helper}, which'
          ' CHIPVirtualHome lacks',
      )

  def test_parse_ot_mac_counters(self):
    mac_sample = 'TxTotal: 42\n  TxRetry: 7\n  RxTotal: 39\nDone\n'
    self.assertEqual(
        rf_e2e.parse_ot_mac_counters(mac_sample),
        {'TxTotal': 42, 'TxRetry': 7, 'RxTotal': 39},
    )

  def test_parse_child_table_lq_in(self):
    child_sample = (
        '| ID  | RLOC16 | Timeout    | Age        | LQ In | C_VN |R|D|N|Ver|'
        'CSL|QMsgCnt|Suprvsn| Extended MAC     |\n'
        '|   1 | 0xa001 |        240 |          2 |     3 |  100 |0|0|1|  4|'
        ' 0 |     0 |   129 | c2f427b7acdd8935 |\n'
    )
    self.assertEqual(rf_e2e.parse_child_table_lq_in(child_sample), [3])

  def test_ci_workflow_and_parallel_silabs_lock_build_contract(self):
    cirque_yaml = (CHIP_REPO_PATH / '.github/workflows/cirque.yaml').read_text(
        encoding='utf-8'
    )
    parsed = yaml.safe_load(cirque_yaml)
    self.assertIn('build_silabs_arm32_lock', parsed.get('jobs', {}))
    self.assertIn('build_silabs_arm32_lock:', cirque_yaml)
    self.assertIn('name: Build ARM32 Silabs Lock App', cirque_yaml)
    self.assertIn(
        'SILABS_ARM32_APPS=lock scripts/build/gn_gen_cirque_silabs_arm32.sh',
        cirque_yaml,
    )
    self.assertIn('name: cirque-silabs-arm32-lock-app', cirque_yaml)
    self.assertIn(
        '"Build ARM32 Silabs Lock App": "Upload ARM32 Silabs Lock artifact"',
        cirque_yaml,
    )
    self.assertIn(
        'file out/arm-silabs-thread/chip-silabs-lock-app | grep -q'
        ' "ELF 32-bit LSB.*ARM"',
        cirque_yaml,
    )

    gn_gen_sh = (
        CHIP_REPO_PATH / 'scripts/build/gn_gen_cirque_silabs_arm32.sh'
    ).read_text(encoding='utf-8')
    self.assertIn(
        'lit-icd) thread_targets+=(chip-silabs-lit-icd-app) ;;', gn_gen_sh
    )
    self.assertIn('lock) thread_targets+=(chip-silabs-lock-app) ;;', gn_gen_sh)
    self.assertIn(
        'time ninja -C out/arm-silabs-thread "${targets[@]}"', gn_gen_sh
    )

    cirque_tests_sh = (
        CHIP_REPO_PATH / 'scripts/tests/cirque_tests.sh'
    ).read_text(encoding='utf-8')
    for test_name in (
        '"AndroidBleThreadSilabsLitIcdSupervisionRestartTest"',
        '"AndroidBleThreadSilabsLitIcdNoSupervisionRestartTest"',
        '"AndroidBleThreadSilabsLockRfRssiTest"',
    ):
      self.assertIn(test_name, cirque_tests_sh)

    self.assertIn(
        'out/arm-silabs-thread/chip-silabs-lock-app', LLVM_COVERAGE_BINARIES
    )


if __name__ == '__main__':
  unittest.main()
