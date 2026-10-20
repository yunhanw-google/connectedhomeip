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

Unit and live binary tests for POSIX-hosted Silicon Labs Lighting-App
(chip-silabs-lighting-app) and AndroidBleWiFiSilabsLightingTest.
"""

import os
import signal
import subprocess
import tempfile
import time
import unittest

from AndroidBleWiFiMobileDeviceTest import TestAndroidBleWiFiMobileDevice
from AndroidBleWiFiSilabsLightingTest import TestAndroidBleWiFiSilabsLightingDevice
from helper.paths import (
    CHIP_ALL_CLUSTERS_APP_ESC,
    CHIP_SILABS_LIGHTING_APP_ESC,
    CHIP_SILABS_LIGHTING_APP_PATH,
)

SILABS_DEVICE_LOG_LINES = [
    '[1792475782.010] [185:185] [DL] Got WiFi interface: wlan0',
    '[1792475782.011] [185:185] [SVR] [SILABS] =====SL-Light starting=====',
    '[1792475782.012] [185:193] [SVR] [SILABS-LIGHT] WSTK LEDs initialized',
    '[1792475782.012] [185:193] [SVR] [SILABS-LIGHT] LED 1 state=OFF',
    '[1792475782.012] [185:193] [SVR] [SILABS] App Task started',
    '[1792475782.015] [185:191] [DL] GATT application registered successfully',
    '[1792475785.100] [185:191] [DL] New BLE connection: conn=0x7ffff0025d60',
    '[1792475785.200] [185:185] [BLE] selected BTP version 4',
    '[1792475785.300] [185:185] [DL] Receive kCHIPoBLEConnectionEstablished',
    (
        '[1792475785.400] [185:185] [SVR] Commissioning completed session'
        ' establishment step'
    ),
    (
        '[1792475786.100] [185:185] [DMG] Received command for Endpoint=0'
        ' Cluster=0x0000_0031 Command=0x0000_0002'
    ),
    (
        '[1792475786.200] [185:185] [NP] LinuxWiFiDriver: ConnectNetwork'
        " 'CHIP-VirtualWiFi-AP'"
    ),
    (
        '[1792475787.000] [185:191] [DL] wpa_supplicant: Interface properties'
        " changed, state is 'completed'"
    ),
    (
        '[1792475788.000] [185:185] [SVR] [SILABS] Commissioning complete event'
        ' received'
    ),
    '[1792475790.000] [185:185] [ZCL] Toggle ep1 on/off from state 0 to 1',
    '[1792475790.001] [185:185] [ZCL] Cluster callback: 0x0000_0006',
    '[1792475790.001] [185:185] [SVR] [SILABS-LIGHT] LED 1 state=ON',
    '[1792475791.000] [185:185] [EM] >>> Type 0001:02 (IM:ReadRequest)',
    '[1792475791.001] [185:185] [EM] <<< Type 0001:05 (IM:ReportData)',
]


class TestSilabsLightingAppPosix(unittest.TestCase):

  def _make_silabs_harness(self):
    harness = TestAndroidBleWiFiSilabsLightingDevice(
        {'device1': {'type': 'CHIPEndDevice', 'id': 'device1'}}
    )
    harness.get_device_pretty_id = lambda x: 'CHIPEndDevice'
    return harness

  def test_silabs_lighting_device_class_selects_silabs_binary(self):
    silabs_test = self._make_silabs_harness()
    self.assertTrue(silabs_test._is_silabs_lighting_app())
    self.assertEqual(
        silabs_test._get_end_device_binary_esc(),
        CHIP_SILABS_LIGHTING_APP_ESC,
    )
    default_test = TestAndroidBleWiFiMobileDevice(
        {'device1': {'type': 'CHIPEndDevice', 'id': 'device1'}},
        end_device_app='all-clusters',
    )
    self.assertFalse(default_test._is_silabs_lighting_app())
    self.assertEqual(
        default_test._get_end_device_binary_esc(),
        CHIP_ALL_CLUSTERS_APP_ESC,
    )

  def test_start_wifi_end_devices_invokes_silabs_lighting_binary(self):
    silabs_test = self._make_silabs_harness()
    executed_cmds = []
    silabs_test.execute_device_cmd = lambda dev_id, cmd: executed_cmds.append(
        (dev_id, cmd)
    )
    silabs_test._start_wifi_end_devices(
        [{'id': 'device1', 'description': {'ble_adapt_id': 2}}],
        '/out/host_libs',
    )
    self.assertEqual(len(executed_cmds), 2)
    launch_cmd = executed_cmds[1][1]
    self.assertIn(CHIP_SILABS_LIGHTING_APP_ESC, launch_cmd)
    self.assertIn('--wifi --ble-controller 2 --discriminator 3840', launch_cmd)

  def test_silabs_lighting_device_log_sequence_assertions(self):
    silabs_test = self._make_silabs_harness()
    log = '\n'.join(SILABS_DEVICE_LOG_LINES).encode('utf-8')
    silabs_test.get_device_log = lambda x: log
    silabs_test._assert_ble_wifi_device_logs(['device1'])

    # Missing Silabs startup banner must fail
    missing_startup = [
        line
        for line in SILABS_DEVICE_LOG_LINES
        if '=====SL-Light starting=====' not in line
    ]
    silabs_test.get_device_log = lambda x: '\n'.join(missing_startup).encode(
        'utf-8'
    )
    with self.assertRaises(AssertionError):
      silabs_test._assert_ble_wifi_device_logs(['device1'])

    # Missing post-toggle Silabs LED 1 state=ON actuation must fail
    missing_led_on = [
        line for line in SILABS_DEVICE_LOG_LINES if 'LED 1 state=ON' not in line
    ]
    silabs_test.get_device_log = lambda x: '\n'.join(missing_led_on).encode(
        'utf-8'
    )
    with self.assertRaises(AssertionError):
      silabs_test._assert_ble_wifi_device_logs(['device1'])

  @unittest.skipUnless(
      CHIP_SILABS_LIGHTING_APP_PATH.is_file(),
      f'{CHIP_SILABS_LIGHTING_APP_PATH} not built',
  )
  def test_live_silabs_lighting_binary_boot_and_button_toggle(self):
    with tempfile.TemporaryDirectory(prefix='silabs_posix_test_') as tmpdir:
      fifo_path = os.path.join(tmpdir, 'app_pipe')
      kvs_path = os.path.join(tmpdir, 'chip_kvs')
      log_path = os.path.join(tmpdir, 'silabs.log')
      for stale_ini in (
          '/tmp/chip_factory.ini',
          '/tmp/chip_config.ini',
          '/tmp/chip_counters.ini',
          '/tmp/chip_kvs',
      ):
        if os.path.exists(stale_ini):
          os.remove(stale_ini)

      env = os.environ.copy()
      env['DBUS_SYSTEM_BUS_ADDRESS'] = (
          f'unix:path={os.path.join(tmpdir, "nonexistent_dbus")}'
      )
      with open(log_path, 'wb') as log_file:
        proc = subprocess.Popen(
            [
                str(CHIP_SILABS_LIGHTING_APP_PATH),
                '--KVS',
                kvs_path,
                '--app-pipe',
                fifo_path,
                '--discriminator',
                '3840',
                '--passcode',
                '20202021',
            ],
            stdout=log_file,
            stderr=subprocess.STDOUT,
            env=env,
        )
        try:
          ready = False
          for _ in range(40):
            if os.path.exists(fifo_path) and os.path.exists(log_path):
              with open(log_path, 'r', encoding='utf-8', errors='replace') as r:
                if 'Starting event loop' in r.read():
                  ready = True
                  break
            time.sleep(0.1)
          self.assertTrue(
              ready, 'chip-silabs-lighting-app did not enter event loop'
          )

          with open(fifo_path, 'w', encoding='utf-8') as pipe:
            pipe.write('{"Name":"Button","Button":1,"Action":1}\n')
            pipe.flush()
          time.sleep(0.3)

          with open(fifo_path, 'w', encoding='utf-8') as pipe:
            pipe.write('{"Name":"Button","Button":1,"Action":1}\n')
            pipe.flush()
          time.sleep(0.3)
        finally:
          proc.send_signal(signal.SIGINT)
          try:
            proc.wait(timeout=5)
          except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

      with open(log_path, 'r', encoding='utf-8', errors='replace') as r:
        log_output = r.read()

      self.assertIn('[SILABS] =====SL-Light starting=====', log_output)
      self.assertIn('[SILABS-LIGHT] WSTK LEDs initialized', log_output)
      self.assertIn('[SILABS-LIGHT] LED 1 state=OFF', log_output)
      self.assertIn('[SILABS] App Task started', log_output)
      self.assertIn('Toggle ep1 on/off from state 0 to 1', log_output)
      self.assertIn('Cluster callback: 0x0000_0006', log_output)
      self.assertIn('[SILABS-LIGHT] LED 1 state=ON', log_output)
      self.assertIn('Toggle ep1 on/off from state 1 to 0', log_output)


if __name__ == '__main__':
  unittest.main()
