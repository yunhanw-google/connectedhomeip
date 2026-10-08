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
import os
import shlex
import sys
import time

from helper.CHIPTestBase import CHIPVirtualHome
from helper.paths import (
    CHIP_ALL_CLUSTERS_APP_ESC,
    CHIP_REPO_STR,
    CONTROLLER_TEST_SCRIPTS_DIR_PATH,
    MATTER_CONTROLLER_INSTALL_WHEELS,
    MATTER_DEVELOPMENT_PAA_ROOT_CERTS_ESC,
)
from helper.wifi_lease import (
    get_device_wlan0_ipv6,
    next_pool_address,
    wait_for_device_wlan0_ipv4,
)

logger = logging.getLogger('BleWiFiMobileDeviceTest')
logger.setLevel(logging.INFO)

sh = logging.StreamHandler()
sh.setFormatter(
    logging.Formatter('%(asctime)s [%(name)s] %(levelname)s %(message)s')
)
logger.addHandler(sh)

CIRQUE_URL = 'http://localhost:5000'
TEST_WIFI_SSID = 'CHIP-VirtualWiFi-AP'
TEST_WIFI_PSK = 'ChipWiFiPassword123'
TEST_DISCRIMINATOR = 3840
TEST_SCRIPT_ESC = shlex.quote(
    str(CONTROLLER_TEST_SCRIPTS_DIR_PATH / 'mobile-device-ble-wifi-test.py')
)

DEVICE_CONFIG = {
    'device0': {
        'type': 'wifi_ap',
        'base_image': '@default',
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PSK,
        'capability': ['TrafficControl'],
        'traffic_control': {'latencyMs': 10},
    },
    'device1': {
        'type': 'CHIPEndDevice',
        'base_image': '@default',
        'capability': ['Bluetooth', 'WiFi', 'TrafficControl', 'Mount'],
        'wifi_auto_connect': False,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 10},
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
    'device2': {
        'type': os.environ.get('CHIP_CIRQUE_CONTROLLER_TYPE', 'MobileDevice'),
        'base_image': '@default',
        'capability': ['Bluetooth', 'WiFi', 'TrafficControl', 'Mount'],
        'wifi_auto_connect': True,
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PSK,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 10},
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
}


class TestBleWiFiMobileDevice(CHIPVirtualHome):

  def __init__(self, device_config):
    super().__init__(CIRQUE_URL, device_config)
    self.logger = logger

  def setup(self):
    self.initialize_home()

  def test_routine(self):
    self.run_ble_commissioning_and_wifi_provisioning_test()

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

  def _isolate_eth0_and_start_avahi(self, req_device_id, server_ids):
    self.logger.info(
        'Isolating eth0 on MobileDevice and CHIPEndDevice to enforce pure'
        ' Virtual BLE + Virtual Wi-Fi (wlan0) transport'
    )
    self.execute_device_cmd(
        req_device_id,
        'sh -c "ip addr flush dev eth0 || true; ip link set eth0 down || true"',
    )
    for server_id in server_ids:
      self.execute_device_cmd(
          server_id,
          'sh -c "ip addr flush dev eth0 || true; ip link set eth0 down ||'
          ' true"',
      )
      self.execute_device_cmd(
          server_id,
          'sh -c "service dbus start || true; service avahi-daemon restart ||'
          ' avahi-daemon -D || true"',
      )
    self.execute_device_cmd(
        req_device_id,
        'sh -c "service dbus start || true; service avahi-daemon restart ||'
        ' avahi-daemon -D || true"',
    )

    # Positive precondition: MobileDevice's wlan0 station must have a valid
    # 10.0.1.x DHCP lease from the AP before conducting negative pre-ping.
    # The station associates at node creation, but the DHCPv4 DORA over the
    # virtual bridge may still be in flight; poll until leased or timeout.
    mobile_wlan0_ip = wait_for_device_wlan0_ipv4(self, req_device_id)
    self.logger.info(
        'MobileDevice (%s) wlan0 IPv4 address: %s',
        self.get_device_pretty_id(req_device_id),
        mobile_wlan0_ip,
    )
    self.assertTrue(
        bool(mobile_wlan0_ip and mobile_wlan0_ip.startswith('10.0.1.')),
        f'MobileDevice {self.get_device_pretty_id(req_device_id)} station'
        ' not associated / no DHCP lease on wlan0 (expected 10.0.1.x)!',
    )

    # Derive pre-ping target: pool starts at 10.0.1.10; mobile leases first,
    # so target the next pool address to avoid pinging self or gateway.
    pre_ping_target = next_pool_address(mobile_wlan0_ip)

    pre_ping = self.execute_device_cmd(
        req_device_id, f'ping -c 1 -W 1 {pre_ping_target}'
    )
    self.logger.info(
        'Pre-commissioning wlan0 ping check to %s (expected to fail before WPA2'
        ' handshake): return_code=%s',
        pre_ping_target,
        pre_ping.get('return_code'),
    )
    self.assertNotEqual(
        pre_ping.get('return_code'),
        '0',
        'Expected wlan0 ping to CHIPEndDevice to fail BEFORE BLE Wi-Fi'
        ' provisioning!',
    )
    return mobile_wlan0_ip

  def _start_wifi_end_devices(self, server_devices, host_libs_dir):
    for device in server_devices:
      server_id = device['id']
      ble_adapt_id = self._extract_hci_index(device, default_index=1)
      self.logger.info(
          'Starting chip-all-clusters-app on CHIPEndDevice %s with'
          ' --wifi --ble-controller %d',
          self.get_device_pretty_id(server_id),
          ble_adapt_id,
      )
      self.execute_device_cmd(
          server_id,
          f'CHIPCirqueDaemon.py -- run env LD_LIBRARY_PATH={host_libs_dir}'
          ' gdb -batch -return-child-result -q -ex'
          ' "set pagination off" -ex run -ex "thread apply all bt" --args'
          f' {CHIP_ALL_CLUSTERS_APP_ESC} --wifi --ble-controller'
          f' {ble_adapt_id} --discriminator {TEST_DISCRIMINATOR}',
      )

  def _assert_ble_wifi_mobile_log(self, mobile_output):
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
                    " 'WiFiNetworkSetup'"
                ),
                (
                    "Commissioning stage next step: 'WiFiNetworkSetup' ->"
                    " 'FailsafeBeforeWiFiEnable'"
                ),
                (
                    "Commissioning stage next step: 'FailsafeBeforeWiFiEnable'"
                    " -> 'WiFiNetworkEnable'"
                ),
                (
                    "Commissioning stage next step: 'WiFiNetworkEnable' ->"
                    " 'EvictPreviousCaseSessions'"
                ),
                (
                    'Performing next commissioning step'
                    " 'FindOperationalForStayActive'"
                ),
                (
                    'Commissioning stage next step:'
                    " 'FindOperationalForCommissioningComplete' ->"
                    " 'SendComplete'"
                ),
                (
                    'BLE Commissioning and Wi-Fi Provisioning succeeded for'
                    ' nodeId=1'
                ),
                'Testing on off cluster over Wi-Fi',
                'Test finished',
            ],
        ),
        'MobileDevice log is missing expected BLE Commissioning + Wi-Fi'
        ' Provisioning sequence markers',
    )

  def _assert_ble_wifi_device_logs(self, server_ids):
    for device_id in server_ids:
      end_device_log = self.get_device_log(device_id).decode(
          'utf-8', errors='replace'
      )
      self.assertTrue(
          self.sequenceMatch(
              end_device_log,
              [
                  'Got WiFi interface: wlan0',
                  'wpa_supplicant: connected to interface proxy',
                  'GATT application registered successfully',
                  'BLE advertisement started successfully',
                  'New BLE connection',
                  'selected BTP version 4',
                  'Receive kCHIPoBLEConnectionEstablished',
                  'Commissioning completed session establishment step',
                  "LinuxWiFiDriver: ConnectNetwork 'CHIP-VirtualWiFi-AP'",
                  'wpa_supplicant: Added network:',
                  (
                      'wpa_supplicant: Interface properties changed, state is'
                      " 'completed'"
                  ),
                  'Current connected network: "CHIP-VirtualWiFi-AP"',
                  'Toggle ep1 on/off from state 0 to 1',
              ],
          ),
          'CHIPEndDevice log is missing expected BlueZ BTP + wpa_supplicant1'
          ' Wi-Fi provisioning sequence markers',
      )

  def _run_mobile_ble_wifi_controller(
      self, req_device, host_libs_dir, server_ids
  ):
    req_device_id = req_device['id']
    req_ble_adapt_id = self._extract_hci_index(req_device, default_index=0)
    self.logger.info(
        'Configuring MobileDevice %s with --ble-adapter %d',
        self.get_device_pretty_id(req_device_id),
        req_ble_adapt_id,
    )
    ssid_esc = shlex.quote(TEST_WIFI_SSID)
    pwd_esc = shlex.quote(TEST_WIFI_PSK)
    command = (
        f'env LD_LIBRARY_PATH={host_libs_dir} '
        'gdb -batch -return-child-result -q -ex run -ex "thread apply all bt"'
        f' --args python3 {TEST_SCRIPT_ESC} -t 300 --ble-adapter'
        f' {req_ble_adapt_id} --ssid {ssid_esc} --wifi-password {pwd_esc}'
        f' --paa-trust-store-path {MATTER_DEVELOPMENT_PAA_ROOT_CERTS_ESC}'
    )
    ret = self.execute_device_cmd(req_device_id, command)
    mobile_output = ret.get('output', '')
    self.logger.info(
        '===== MobileDevice (%s) BLE + Wi-Fi Controller Output =====\n%s',
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
        'BleWiFiMobileDeviceTest failed: non-zero return code from'
        ' mobile-device-ble-wifi-test.py',
    )
    return mobile_output

  def run_ble_commissioning_and_wifi_provisioning_test(self):
    server_devices = [
        d for d in self.non_ap_devices if d['type'] == 'CHIPEndDevice'
    ]
    ctrl_type = os.environ.get('CHIP_CIRQUE_CONTROLLER_TYPE', 'MobileDevice')
    req_devices = [
        d for d in self.non_ap_devices if d['type'] == ctrl_type
    ]
    if not req_devices:
      raise RuntimeError(
          'Required controller device of type'
          f" '{ctrl_type}' not found in topology"
      )
    server_ids = [d['id'] for d in server_devices]
    req_device = req_devices[0]
    req_device_id = req_device['id']
    host_libs_dir = shlex.quote(f'{CHIP_REPO_STR}/out/host_libs')

    self.execute_device_cmd(req_device_id, MATTER_CONTROLLER_INSTALL_WHEELS)

    mobile_wlan0_ip = self._isolate_eth0_and_start_avahi(
        req_device_id, server_ids
    )
    self._start_wifi_end_devices(server_devices, host_libs_dir)
    time.sleep(2)

    mobile_output = self._run_mobile_ble_wifi_controller(
        req_device, host_libs_dir, server_ids
    )

    # Derive the end device's actual post-commissioning wlan0 IPv4 from lease.
    # VirtualDhcpServer DORA exchange over virtual bridge can take a few
    # seconds after commissioning completes; poll until leased or timeout.
    leased_ipv4 = wait_for_device_wlan0_ipv4(self, server_ids[0])
    leased_ipv6 = get_device_wlan0_ipv6(self, server_ids[0])
    self.logger.info(
        'CHIPEndDevice (%s) wlan0 leased IPv4: %s, IPv6: %s',
        self.get_device_pretty_id(server_ids[0]),
        leased_ipv4,
        leased_ipv6,
    )
    self.assertTrue(
        bool(leased_ipv4 and leased_ipv4.startswith('10.0.1.')),
        'CHIPEndDevice wlan0 lease not obtained after provisioning:'
        f' {leased_ipv4}',
    )
    self.assertNotEqual(
        leased_ipv4,
        mobile_wlan0_ip,
        f'CHIPEndDevice leased same IP as controller {mobile_wlan0_ip}',
    )

    post_ping = self.execute_device_cmd(
        req_device_id, f'ping -c 2 -W 2 {leased_ipv4}'
    )
    self.logger.info(
        'Post-commissioning wlan0 ping output to %s:\n%s',
        leased_ipv4,
        post_ping.get('output', ''),
    )
    self.assertEqual(
        post_ping.get('return_code'),
        '0',
        'Expected wlan0 ping to CHIPEndDevice to succeed AFTER BLE Wi-Fi'
        ' provisioning!',
    )
    self._assert_ble_wifi_mobile_log(mobile_output)
    self._assert_ble_wifi_device_logs(server_ids)


if __name__ == '__main__':
  sys.exit(TestBleWiFiMobileDevice(DEVICE_CONFIG).run_test())
