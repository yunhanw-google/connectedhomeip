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

Unit and live binary tests for POSIX-hosted Silicon Labs LIT ICD App
(chip-silabs-lit-icd-app) and AndroidBleWiFiSilabsLitIcdTest.
"""

import os
from pathlib import Path
import struct
import unittest

from AndroidBleWiFiSilabsLitIcdTest import (
    DEVICE_CONFIG,
    SRP_BRIDGER_SCRIPT,
    TestAndroidBleThreadSilabsLitIcdDevice,
    TestAndroidBleWiFiSilabsLitIcdDevice,
    _ANDROID_POLL_LOGCAT_CMD,
    _invert_key_hex,
    _sleepy_child_rloc16s,
)
from helper.paths import (
    CHIP_REPO_PATH,
    CHIP_SILABS_LIT_ICD_APP_ESC,
    CHIP_SILABS_LIT_ICD_APP_PATH,
)

__all__ = [
    'DEVICE_CONFIG',
    'SRP_BRIDGER_SCRIPT',
    'TestAndroidBleThreadSilabsLitIcdDevice',
    'TestAndroidBleWiFiSilabsLitIcdDevice',
    'TestSilabsLitIcdAppPosix',
    '_invert_key_hex',
    '_sleepy_child_rloc16s',
]


class TestSilabsLitIcdAppPosix(unittest.TestCase):
  """Unit tests for AndroidBleWiFiSilabsLitIcdTest and silabs-lit-icd-app."""

  def test_silabs_lit_icd_device_class_selects_silabs_lit_icd_binary(self):
    harness = TestAndroidBleWiFiSilabsLitIcdDevice()
    self.assertIs(
        TestAndroidBleThreadSilabsLitIcdDevice,
        TestAndroidBleWiFiSilabsLitIcdDevice,
    )
    self.assertNotIn('device2', harness.device_config)
    self.assertEqual(
        harness._get_end_device_binary_esc(),
        CHIP_SILABS_LIT_ICD_APP_ESC,
    )

  def test_sleepy_child_rloc16s_only_returns_rx_off_when_idle_children(self):
    child_table = (
        '| ID  | RLOC16 | Timeout    | Age        | LQ In | C_VN |R|D|N|Ver|'
        'CSL|QMsgCnt|Suprvsn| Extended MAC     |\n'
        '+-----+--------+------------+------------+-------+------+-+-+-+---+'
        '---+-------+-------+------------------+\n'
        '|   1 | 0xa001 |        240 |          2 |     3 |  100 |0|0|1|  4|'
        ' 0 |     0 |   129 | c2f427b7acdd8935 |\n'
        '|   2 | 0xa002 |        240 |          5 |     3 |  101 |1|1|1|  4|'
        ' 0 |     0 |     0 | 1122334455667788 |\n'
    )
    self.assertEqual(_sleepy_child_rloc16s(child_table), {0xA001})

  def test_invert_key_hex_flips_all_bits(self):
    orig = '00112233445566778899aabbccddeeff'
    inv = _invert_key_hex(orig)
    self.assertEqual(inv, 'ffeeddccbbaa99887766554433221100')
    self.assertEqual(_invert_key_hex(inv), orig)

  def test_cirque_tests_sh_registers_android_ble_wifi_silabs_lit_icd_test(self):
    cirque_tests_sh = (
        CHIP_REPO_PATH / 'scripts/tests/cirque_tests.sh'
    ).read_text(encoding='utf-8')
    self.assertIn('"AndroidBleWiFiSilabsLitIcdTest"', cirque_tests_sh)
    self.assertNotIn('"test_silabs_lit_icd_app_posix"', cirque_tests_sh)

    gn_gen_sh = (
        CHIP_REPO_PATH / 'scripts/build/gn_gen_cirque_silabs_arm32.sh'
    ).read_text(encoding='utf-8')
    self.assertIn('AndroidBleWiFiSilabsLitIcdTest', gn_gen_sh)
    self.assertNotIn('test_silabs_lit_icd_app_posix', gn_gen_sh)

  def test_logcat_poll_and_stale_flask_cleanup_in_branch(self):
    self.assertIn('NsdManagerServiceResolver', _ANDROID_POLL_LOGCAT_CMD)
    self.assertIn('ChipDeviceController', _ANDROID_POLL_LOGCAT_CMD)
    self.assertIn('onCheckInComplete', _ANDROID_POLL_LOGCAT_CMD)
    cirque_tests_sh = (
        CHIP_REPO_PATH / 'scripts/tests/cirque_tests.sh'
    ).read_text(encoding='utf-8')
    self.assertIn(
        "pkill -f 'cirque/restservice/service.py|python3 -m flask run'",
        cirque_tests_sh,
    )

  @unittest.skipUnless(
      CHIP_SILABS_LIT_ICD_APP_PATH.is_file(),
      f'{CHIP_SILABS_LIT_ICD_APP_PATH} not built',
  )
  def test_silabs_lit_icd_binary_is_32bit_arm_elf(self):
    with open(CHIP_SILABS_LIT_ICD_APP_PATH, 'rb') as f:
      hdr = f.read(20)
    self.assertEqual(hdr[:4], b'\x7fELF')
    self.assertEqual(hdr[4], 1, 'Expected ELFCLASS32 (32-bit ELF)')
    e_machine = struct.unpack('<H', hdr[18:20])[0]
    self.assertEqual(e_machine, 40, f'Expected EM_ARM (40), got {e_machine}')


if __name__ == '__main__':
  unittest.main()
