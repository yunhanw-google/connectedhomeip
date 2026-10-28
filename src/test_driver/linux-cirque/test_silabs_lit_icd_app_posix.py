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
(chip-silabs-lit-icd-app) and AndroidBleThreadSilabsLitIcdTest.
"""

import os
from pathlib import Path
import re
import struct
import unittest

from AndroidBleThreadSilabsLitIcdTest import (
    DEVICE_CONFIG,
    SRP_BRIDGER_SCRIPT,
    TestAndroidBleThreadSilabsLitIcdDevice,
    _ANDROID_POLL_LOGCAT_CMD,
    _device_tx_checkin_counters,
    _invert_key_hex,
    _is_chiptool_srp_registered,
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
    'TestSilabsLitIcdAppPosix',
    '_invert_key_hex',
    '_sleepy_child_rloc16s',
]


class TestSilabsLitIcdAppPosix(unittest.TestCase):
  """Unit tests for AndroidBleThreadSilabsLitIcdTest and silabs-lit-icd-app."""

  def test_silabs_lit_icd_device_class_selects_silabs_lit_icd_binary(self):
    harness = TestAndroidBleThreadSilabsLitIcdDevice()
    self.assertNotIn('device2', harness.device_config)
    self.assertEqual(
        harness._get_end_device_binary_esc(),
        CHIP_SILABS_LIT_ICD_APP_ESC,
    )

  def test_only_assert_helpers_defined_by_chip_virtual_home_are_used(self):
    # CHIPVirtualHome is not a unittest.TestCase; it only defines a subset of
    # assert helpers, so e.g. self.assertIsNotNone raises AttributeError at
    # runtime in the E2E test.
    harness = TestAndroidBleThreadSilabsLitIcdDevice()
    test_dir = CHIP_REPO_PATH / 'src/test_driver/linux-cirque'
    for name in (
        'AndroidBleThreadSilabsLitIcdTest.py',
        'AndroidBleThreadMobileDeviceTest.py',
    ):
      source = (test_dir / name).read_text(encoding='utf-8')
      for helper in sorted(set(re.findall(r'self\.(assert\w+)\(', source))):
        self.assertTrue(
            callable(getattr(harness, helper, None)),
            f'{name} uses self.{helper}, which CHIPVirtualHome lacks',
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

  def test_device_tx_checkin_counters_parses_only_sent_checkins(self):
    log = (
        '[1792818213.142] [1374938:1374938] [EM] <<< [E:24262i S:0'
        ' M:79822948] (U) Msg TX from 7A15A47DF515E279 to'
        ' 0:0000000000000000 [0000] [UDP:[fd11:22:0:0:0:0:0:3]:40297] ---'
        ' Type 0000:50 (SecureChannel:ICD_CheckInMessage) (B:57)\n'
        '[1792818223.350] [1374938:1374938] [EM] <<< [E:24263i S:0'
        ' M:79822949] (U) Msg TX from 21C3CABBE919D0F0 to'
        ' 0:0000000000000000 [0000] [UDP:[fd11:22:0:0:0:0:0:3]:40297] ---'
        ' Type 0000:50 (SecureChannel:ICD_CheckInMessage) (B:57)\n'
        # Received (>>>) Check-Ins and other message types are not counted.
        '[1792818224.000] [1374938:1374938] [EM] >>> [E:1r S:0 M:5] (U) Msg'
        ' RX --- Type 0000:50 (SecureChannel:ICD_CheckInMessage) (B:57)\n'
        '[1792818225.000] [1374938:1374938] [EM] <<< [E:9i S:0 M:7] (U) Msg'
        ' TX --- Type 0000:10 (SecureChannel:StandaloneAck) (B:26)\n'
    )
    self.assertEqual(_device_tx_checkin_counters(log), {79822948, 79822949})

  def test_invert_key_hex_flips_all_bits(self):
    orig = '00112233445566778899aabbccddeeff'
    inv = _invert_key_hex(orig)
    self.assertEqual(inv, 'ffeeddccbbaa99887766554433221100')
    self.assertEqual(_invert_key_hex(inv), orig)

  def test_cirque_tests_sh_registers_android_ble_thread_silabs_lit_icd_test(
      self,
  ):
    cirque_tests_sh = (
        CHIP_REPO_PATH / 'scripts/tests/cirque_tests.sh'
    ).read_text(encoding='utf-8')
    self.assertIn('"AndroidBleThreadSilabsLitIcdTest"', cirque_tests_sh)
    self.assertNotIn('AndroidBleWiFiSilabsLitIcdTest', cirque_tests_sh)
    self.assertNotIn('"test_silabs_lit_icd_app_posix"', cirque_tests_sh)

    gn_gen_sh = (
        CHIP_REPO_PATH / 'scripts/build/gn_gen_cirque_silabs_arm32.sh'
    ).read_text(encoding='utf-8')
    self.assertIn('AndroidBleThreadSilabsLitIcdTest', gn_gen_sh)
    self.assertNotIn('AndroidBleWiFiSilabsLitIcdTest', gn_gen_sh)
    self.assertNotIn('test_silabs_lit_icd_app_posix', gn_gen_sh)
    self.assertTrue(
        (
            CHIP_REPO_PATH
            / 'src/test_driver/linux-cirque/AndroidBleThreadSilabsLitIcdTest.py'
        ).is_file()
    )
    self.assertFalse(
        (
            CHIP_REPO_PATH
            / 'src/test_driver/linux-cirque/AndroidBleWiFiSilabsLitIcdTest.py'
        ).exists()
    )

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

  def test_is_chiptool_srp_registered_verifies_active_service_block(self):
    only_lit_icd = (
        '6BA098F258162966-0000000000000001'
        '._matter._tcp.default.service.arpa.\n'
        '    deleted: false\n'
        '    subtypes: _I6BA098F258162966\n'
        '    port: 5540\n'
        '    host: C2E1D8FB45BC6892.default.service.arpa.\n'
        '    addresses: [fdde:ad00:beef:0:84f9:d1a9:ef32:c56d]\n'
        'Done\n'
    )
    self.assertFalse(
        _is_chiptool_srp_registered(
            only_lit_icd, '6BA098F258162966-000000000001B669', '36532'
        )
    )

    chiptool_hdr = (
        '6BA098F258162966-000000000001B669._matter._tcp.default.service.arpa.\n'
    )
    with_chiptool = (
        only_lit_icd
        + chiptool_hdr
        + '    deleted: false\n'
        '    subtypes: (null)\n'
        '    port: 36532\n'
        '    host: android-chiptool.default.service.arpa.\n'
        '    addresses: [fd11:22:0:0:0:0:0:3]\n'
        'Done\n'
    )
    self.assertTrue(
        _is_chiptool_srp_registered(
            with_chiptool, '6BA098F258162966-000000000001B669', '36532'
        )
    )

  def test_register_chiptool_srp_service_retries_on_socket_reset(self):
    harness = TestAndroidBleThreadSilabsLitIcdDevice()
    calls = []
    responses = [
        {'output': 'Done\nread: Connection reset by peer\nDone\n'},
        {'output': 'Done\nDone\nDone\nDone\n'},
        {
            'output': (
                '6BA098F258162966-000000000001B669'
                '._matter._tcp.default.service.arpa.\n'
                '    deleted: false\n'
                '    port: 36532\n'
                '    host: android-chiptool.default.service.arpa.\n'
                '    addresses: [fd11:22:0:0:0:0:0:3]\n'
                'Done\n'
            )
        },
    ]

    def fake_exec(dev_id, cmd):
      calls.append((dev_id, cmd))
      return responses.pop(0)

    harness.execute_device_cmd = fake_exec
    ok = harness._register_chiptool_srp_service(
        'tbr0', '6BA098F258162966-000000000001B669', '36532', max_attempts=3
    )
    self.assertTrue(ok)
    self.assertEqual(len(calls), 3)
    self.assertIn('/tmp/ot_ctl.lock', SRP_BRIDGER_SCRIPT)

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
