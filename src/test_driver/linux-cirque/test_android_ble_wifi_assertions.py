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

Verdict tests for AndroidBleWiFiMobileDeviceTest.

Fixtures are verbatim lines from one AndroidBleWiFiMobileDeviceTest run
(CHIPEndDevice-1792084598-f891d268.log and the matching Android logcat).
"""

import unittest

from AndroidBleWiFiMobileDeviceTest import TestAndroidBleWiFiMobileDevice

# Device log excerpt in run order. The first IM Read/Report pair is the
# commissioner's PASE-session read and must not satisfy the post-Toggle read.
DEVICE_LOG_LINES = [
    '[1792084520.727] [185:185] [DL] Got WiFi interface: wlan0',
    '[1792084520.877] [185:191] [DL] GATT application registered'
    ' successfully',
    '[1792084569.791] [185:191] [DL] New BLE connection: conn=0x7ffff0025d60'
    ' device=AA:BB:01:00:03:01 path=/org/bluez/hci0/dev_AA_BB_01_00_03_01',
    '[1792084570.216] [185:185] [BLE] selected BTP version 4',
    '[1792084570.307] [185:185] [DL] Receive kCHIPoBLEConnectionEstablished',
    '[1792084570.418] [185:185] [SVR] Commissioning completed session'
    ' establishment step',
    '[1792084570.459] [185:185] [EM] >>> [E:50658r S:3051 M:14777349] (S) Msg'
    ' RX from 0:FFFFFFFB00000000 [0000] to 0000000000000000 --- Type 0001:02'
    ' (IM:ReadRequest) (B:136)',
    '[1792084570.463] [185:185] [EM] <<< [E:50658r S:3051 M:171606724] (S) Msg'
    ' TX from 0000000000000000 to 0:FFFFFFFB00000000 [0000] [BLE] --- Type'
    ' 0001:05 (IM:ReportData) (B:762)',
    '[1792084571.430] [185:185] [DMG] Received command for Endpoint=0'
    ' Cluster=0x0000_0031 Command=0x0000_0002',
    '[1792084571.450] [185:185] [DMG] Received command for Endpoint=0'
    ' Cluster=0x0000_0031 Command=0x0000_0006',
    "[1792084571.450] [185:185] [NP] LinuxWiFiDriver: ConnectNetwork"
    " 'CHIP-VirtualWiFi-AP'",
    "[1792084572.275] [185:191] [DL] wpa_supplicant: Interface properties"
    " changed, state is 'completed'",
    '[1792084572.546] [185:185] [FS] GeneralCommissioning: Received'
    ' CommissioningComplete',
    '[1792084592.227] [185:185] [ZCL] Toggle ep1 on/off from state 0 to 1',
    '[1792084595.068] [185:185] [EM] >>> [E:50675r S:3052 M:94405901] (S) Msg'
    ' RX from 1:000000000001B669 [3853] to 0000000000000001 --- Type 0001:02'
    ' (IM:ReadRequest) (B:51)',
    '[1792084595.069] [185:185] [EM] <<< [E:50675r S:3052 M:119442748'
    ' (Ack:94405901)] (S) Msg TX from 0000000000000001 to'
    ' 1:000000000001B669 [3853] [UDP:[fe80::e232:8a84:6bba:d3de%wlan0]:46582]'
    ' --- Type 0001:05 (IM:ReportData) (B:69)',
]
TOGGLE_INDEX = 13
ADD_WIFI_NETWORK_INDEX = 8

LOGCAT_TOGGLE = (
    '10-15 17:16:31.875  5017  5105 E OnOffClientFragment: onResponse :'
    ' Endpoint 1, cluster 6, command 2, Code : 0'
)
LOGCAT_READ_TRUE = (
    '10-15 17:16:34.708  5017  5105 V OnOffClientFragment: On/Off attribute'
    ' value: true'
)


class TestAndroidBleWiFiAssertions(unittest.TestCase):

  def setUp(self):
    self.test_instance = TestAndroidBleWiFiMobileDevice(
        {'device1': {'type': 'CHIPEndDevice', 'id': 'device1'}}
    )
    self.test_instance.get_device_pretty_id = lambda x: 'CHIPEndDevice'

  def _assert_device_logs(self, lines):
    log = '\n'.join(lines).encode('utf-8')
    self.test_instance.get_device_log = lambda x: log
    self.test_instance._assert_ble_wifi_device_logs(['device1'])

  def test_real_device_log_passes(self):
    self._assert_device_logs(DEVICE_LOG_LINES)

  def test_missing_post_toggle_read_fails(self):
    # Only the PASE-session Read/Report before the Toggle remains.
    with self.assertRaises(AssertionError):
      self._assert_device_logs(DEVICE_LOG_LINES[: TOGGLE_INDEX + 1])
    # Read without a Report after the Toggle.
    with self.assertRaises(AssertionError):
      self._assert_device_logs(DEVICE_LOG_LINES[:-1])

  def test_missing_add_or_update_wifi_network_fails(self):
    lines = list(DEVICE_LOG_LINES)
    del lines[ADD_WIFI_NETWORK_INDEX]
    with self.assertRaises(AssertionError):
      self._assert_device_logs(lines)

  def test_real_logcat_toggle_and_read_pass(self):
    parse = TestAndroidBleWiFiMobileDevice._parse_toggle_read_logcat
    self.assertEqual(
        parse(f'{LOGCAT_TOGGLE}\n{LOGCAT_READ_TRUE}\n'), (True, True)
    )
    self.assertEqual(
        parse('D OnOffClientFragment: Toggle command success\n'),
        (True, False),
    )

  def test_old_loose_logcat_strings_fail(self):
    parse = TestAndroidBleWiFiMobileDevice._parse_toggle_read_logcat
    loose = (
        'I CHIPTool: onResponse\n'
        'D OnOffClientFragment: onOff\n'
        'D CHIPTool: attribute value: true\n'
        'D CHIPTool: value: true\n'
        'E OnOffClientFragment: onResponse : Endpoint 1, cluster 6,'
        ' command 2, Code : 1\n'
        'E OnOffClientFragment: onResponse : Endpoint 1, cluster 6,'
        ' command 1, Code : 0\n'
    )
    self.assertEqual(parse(loose), (False, False))
    self.assertEqual(
        parse('V OnOffClientFragment: On/Off attribute value: false\n'),
        (False, False),
    )


if __name__ == '__main__':
  unittest.main()
