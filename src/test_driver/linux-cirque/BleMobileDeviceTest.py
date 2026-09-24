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
"""

import logging
import shlex
import sys

from helper.CHIPTestBase import CHIPVirtualHome
from helper.paths import (
    CHIP_ALL_CLUSTERS_APP_ESC,
    CHIP_REPO_STR,
    CONTROLLER_TEST_SCRIPTS_DIR_PATH,
    MATTER_CONTROLLER_INSTALL_WHEELS,
    MATTER_DEVELOPMENT_PAA_ROOT_CERTS_ESC,
)

logger = logging.getLogger('BleMobileDeviceTest')
logger.setLevel(logging.INFO)

sh = logging.StreamHandler()
sh.setFormatter(
    logging.Formatter('%(asctime)s [%(name)s] %(levelname)s %(message)s')
)
logger.addHandler(sh)

CIRQUE_URL = 'http://localhost:5000'
TEST_EXTPANID = 'fedcba9876543210'
TEST_DISCRIMINATOR = 3840
TEST_SCRIPT_ESC = shlex.quote(
    str(CONTROLLER_TEST_SCRIPTS_DIR_PATH / 'mobile-device-ble-test.py')
)

DEVICE_CONFIG = {
    'device0': {
        'type': 'MobileDevice',
        'base_image': '@default',
        'capability': ['Bluetooth', 'TrafficControl', 'Mount'],
        'rcp_mode': True,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 25},
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
    'device1': {
        'type': 'CHIPEndDevice',
        'base_image': '@default',
        'capability': ['Thread', 'Bluetooth', 'TrafficControl', 'Mount'],
        'rcp_mode': True,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 25},
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
}


class TestBleMobileDevice(CHIPVirtualHome):

  def __init__(self, device_config):
    super().__init__(CIRQUE_URL, device_config)
    self.logger = logger

  def setup(self):
    self.initialize_home()

  def test_routine(self):
    self.run_ble_commissioning_and_thread_provisioning_test()

  @staticmethod
  def _extract_hci_index(device, default_index=0):
    if isinstance(device, dict):
      desc = device.get('description', {})
      if isinstance(desc, dict):
        if isinstance(desc.get('ble_adapt_id'), int):
          return desc['ble_adapt_id']
        ble_adapt = desc.get('ble_adapt', '')
        if isinstance(ble_adapt, str) and ble_adapt.startswith('hci'):
          return int(ble_adapt[3:])
      cap_bt = device.get('capability', {}).get('Bluetooth', {})
      if isinstance(cap_bt, dict):
        if isinstance(cap_bt.get('ble_adapt_id'), int):
          return cap_bt['ble_adapt_id']
        ble_adapt = cap_bt.get('ble_adapt', '')
        if isinstance(ble_adapt, str) and ble_adapt.startswith('hci'):
          return int(ble_adapt[3:])
      ble_adapt = device.get('ble_adapt', '')
      if isinstance(ble_adapt, str) and ble_adapt.startswith('hci'):
        return int(ble_adapt[3:])
    elif isinstance(device, str) and device.startswith('hci'):
      return int(device[3:])
    return default_index

  def run_ble_commissioning_and_thread_provisioning_test(self):
    server_devices = [
        device
        for device in self.non_ap_devices
        if device['type'] == 'CHIPEndDevice'
    ]
    req_devices = [
        device
        for device in self.non_ap_devices
        if device['type'] == 'MobileDevice'
    ]
    server_ids = [device['id'] for device in server_devices]
    host_libs_dir = shlex.quote(f'{CHIP_REPO_STR}/out/host_libs')

    for device in server_devices:
      server_id = device['id']
      ble_adapt_id = self._extract_hci_index(device, default_index=1)
      self.logger.info(
          'Starting chip-all-clusters-app on CHIPEndDevice %s with'
          ' --ble-controller %d',
          self.get_device_pretty_id(server_id),
          ble_adapt_id,
      )
      self.execute_device_cmd(
          server_id,
          f'CHIPCirqueDaemon.py -- run env LD_LIBRARY_PATH={host_libs_dir}'
          ' gdb -batch -return-child-result -q -ex'
          ' "set pagination off" -ex run -ex "thread apply all bt" --args'
          f' {CHIP_ALL_CLUSTERS_APP_ESC} --thread --ble-controller'
          f' {ble_adapt_id} --discriminator {TEST_DISCRIMINATOR}',
      )

    # Ensure Thread starts in 'disabled' state so ONLY BLE Thread provisioning
    # can bring it up.
    self.reset_thread_devices(server_ids)

    req_device = req_devices[0]
    req_device_id = req_device['id']
    req_ble_adapt_id = self._extract_hci_index(req_device, default_index=0)
    self.logger.info(
        'Configuring MobileDevice %s with --ble-adapter %d',
        self.get_device_pretty_id(req_device_id),
        req_ble_adapt_id,
    )

    self.execute_device_cmd(req_device_id, MATTER_CONTROLLER_INSTALL_WHEELS)

    command = (
        f'env LD_LIBRARY_PATH={host_libs_dir} '
        'gdb -batch -return-child-result -q -ex run -ex "thread apply all bt"'
        f' --args python3 {TEST_SCRIPT_ESC} -t 300 --ble-adapter'
        f' {req_ble_adapt_id} --paa-trust-store-path'
        f' {MATTER_DEVELOPMENT_PAA_ROOT_CERTS_ESC}'
    )
    ret = self.execute_device_cmd(req_device_id, command)
    mobile_output = ret.get('output', '')
    self.logger.info(
        '===== MobileDevice (%s) BLE Controller Output =====\n%s',
        self.get_device_pretty_id(req_device_id),
        mobile_output,
    )

    for device_id in server_ids:
      end_device_log = self.get_device_log(device_id).decode(
          'utf-8', errors='replace'
      )
      self.logger.info(
          '===== CHIPEndDevice (%s) Device Log =====\n%s',
          self.get_device_pretty_id(device_id),
          end_device_log,
      )

    self.assertEqual(
        ret['return_code'],
        '0',
        'BleMobileDeviceTest failed: non-zero return code from'
        ' mobile-device-ble-test.py',
    )

    # Audit MobileDevice log for pure BLE commissioning and Thread provisioning
    # markers.
    self.assertFalse(
        'Failed to start discovery' in mobile_output,
        'MobileDevice log contains BLE discovery failure',
    )
    self.assertFalse(
        'Commissionable node discovery over BLE failed' in mobile_output,
        'MobileDevice log contains BLE commissionable node discovery failure',
    )
    self.assertTrue(
        self.sequenceMatch(
            mobile_output,
            [
                'ChipDeviceScanner has started scanning',
                'ConnectDevice complete',
                'subscribe complete, ep =',
                'peripheral chose BTP version 4',
                (
                    "Commissioning stage next step: 'SendNOC' ->"
                    " 'ThreadNetworkSetup'"
                ),
                (
                    'Commissioning stage next step:'
                    " 'FailsafeBeforeThreadEnable' -> 'ThreadNetworkEnable'"
                ),
                'BLE Commissioning and Thread Provisioning succeeded',
            ],
        ),
        'MobileDevice log missing expected BLE GATT / BTP / ThreadNetworkSetup'
        ' sequence',
    )

    # Verify CHIPEndDevice joined the Thread network as leader with expected
    # Extended PAN ID.
    self.check_device_thread_state(
        server_ids[0], expected_role=['leader'], timeout=5
    )
    for device_id in server_ids:
      reply = self.execute_device_cmd(device_id, 'ot-ctl extpanid')
      self.assertEqual(reply['output'].split()[0].strip(), TEST_EXTPANID)

    # Audit CHIPEndDevice log for BlueZ GATT C1/C2 BTP handshake and
    # post-commissioning cluster commands.
    for device_id in server_ids:
      end_device_log = self.get_device_log(device_id).decode(
          'utf-8', errors='replace'
      )
      self.assertTrue(
          self.sequenceMatch(
              end_device_log,
              [
                  'New BLE connection: conn=',
                  'C1 WriteHandlerCallback received 9 bytes',
                  'selected BTP version 4',
                  'CHIPoBLE subscribe received',
                  'Receive kCHIPoBLEConnectionEstablished',
                  (
                      'Received command for Endpoint=1 Cluster=0x0000_0006'
                      ' Command=0x0000_0001'
                  ),
                  'Toggle ep1 on/off from state 0 to 1',
                  (
                      'Received command for Endpoint=1 Cluster=0x0000_0006'
                      ' Command=0x0000_0000'
                  ),
                  'Toggle ep1 on/off from state 1 to 0',
                  (
                      'No command 0x0000_0001 in Cluster 0x0000_0006 on'
                      ' Endpoint 233'
                  ),
              ],
          ),
          f'CHIPEndDevice {device_id} log missing BLE GATT C1/C2 or OnOff'
          ' cluster sequence',
      )


if __name__ == '__main__':
  sys.exit(TestBleMobileDevice(DEVICE_CONFIG).run_test())
