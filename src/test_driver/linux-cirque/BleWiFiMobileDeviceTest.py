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
import time

from helper.CHIPTestBase import CHIPVirtualHome
from helper.paths import (
    CHIP_ALL_CLUSTERS_APP_ESC,
    CHIP_REPO_STR,
    CONTROLLER_TEST_SCRIPTS_DIR_PATH,
    MATTER_CONTROLLER_INSTALL_WHEELS,
    MATTER_DEVELOPMENT_PAA_ROOT_CERTS_ESC,
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
TEST_WIFI_PASSWORD = 'ChipWiFiPassword123'
TEST_DISCRIMINATOR = 3840
TEST_SCRIPT_ESC = shlex.quote(
    str(CONTROLLER_TEST_SCRIPTS_DIR_PATH / 'mobile-device-ble-wifi-test.py')
)

DEVICE_CONFIG = {
    'device0': {
        'type': 'MobileDevice',
        'base_image': '@default',
        'capability': ['Bluetooth', 'WiFi', 'TrafficControl', 'Mount'],
        'wifi_auto_connect': True,
        'ssid': TEST_WIFI_SSID,
        'password': TEST_WIFI_PASSWORD,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 10},
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
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
        'type': 'wifi_ap',
        'base_image': '@default',
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PASSWORD,
        'capability': ['TrafficControl'],
        'traffic_control': {'latencyMs': 10},
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

  def run_ble_commissioning_and_wifi_provisioning_test(self):
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
    req_device = req_devices[0]
    req_device_id = req_device['id']
    host_libs_dir = shlex.quote(f'{CHIP_REPO_STR}/out/host_libs')

    # Install controller Python wheels on MobileDevice before disabling eth0.
    self.execute_device_cmd(req_device_id, MATTER_CONTROLLER_INSTALL_WHEELS)

    # Bring down eth0 on both MobileDevice and CHIPEndDevice so that ZERO
    # IP traffic can traverse Docker's default bridge. The ONLY active network
    # interface is wlan0 (Virtual Wi-Fi L2 switch).
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
      # Ensure avahi-daemon is running on CHIPEndDevice so mDNS operational
      # discovery (_matter._tcp) is broadcast over wlan0 once associated.
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

    # Negative-invariant check: before BLE Wi-Fi provisioning, CHIPEndDevice
    # wlan0 is NOT authenticated to VirtualWiFiServer, so ping over wlan0 MUST
    # fail (100% packet loss).
    end_device_wifi_ipv4 = (
        server_devices[0].get('description', {}).get('wifi_ipv4', '10.0.1.11')
    )
    pre_ping = self.execute_device_cmd(
        req_device_id, f'ping -c 1 -W 1 {end_device_wifi_ipv4}'
    )
    self.logger.info(
        'Pre-commissioning wlan0 ping check to %s (expected to fail before WPA2'
        ' handshake): return_code=%s',
        end_device_wifi_ipv4,
        pre_ping.get('return_code'),
    )
    self.assertNotEqual(
        pre_ping.get('return_code'),
        '0',
        'Expected wlan0 ping to CHIPEndDevice to fail BEFORE BLE Wi-Fi'
        ' provisioning!',
    )

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

    time.sleep(2)

    req_ble_adapt_id = self._extract_hci_index(req_device, default_index=0)
    self.logger.info(
        'Configuring MobileDevice %s with --ble-adapter %d',
        self.get_device_pretty_id(req_device_id),
        req_ble_adapt_id,
    )

    ssid_esc = shlex.quote(TEST_WIFI_SSID)
    pwd_esc = shlex.quote(TEST_WIFI_PASSWORD)
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

    # Positive L3 check: after BLE Wi-Fi provisioning, wlan0 L2 switch gate is
    # open and IPv4/IPv6 ping over wlan0 succeeds.
    post_ping = self.execute_device_cmd(
        req_device_id, f'ping -c 2 -W 2 {end_device_wifi_ipv4}'
    )
    self.logger.info(
        'Post-commissioning wlan0 ping output to %s:\n%s',
        end_device_wifi_ipv4,
        post_ping.get('output', ''),
    )
    self.assertEqual(
        post_ping.get('return_code'),
        '0',
        'Expected wlan0 ping to CHIPEndDevice to succeed AFTER BLE Wi-Fi'
        ' provisioning!',
    )

    # Audit MobileDevice log for pure BLE commissioning and Wi-Fi provisioning
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
                "Commissioning stage next step: 'SendNOC' ->"
                " 'WiFiNetworkSetup'",
                "Commissioning stage next step: 'WiFiNetworkSetup' ->"
                " 'FailsafeBeforeWiFiEnable'",
                "Commissioning stage next step: 'FailsafeBeforeWiFiEnable' ->"
                " 'WiFiNetworkEnable'",
                "Commissioning stage next step: 'WiFiNetworkEnable' ->"
                " 'EvictPreviousCaseSessions'",
                "Performing next commissioning step"
                " 'FindOperationalForStayActive'",
                "Commissioning stage next step:"
                " 'FindOperationalForCommissioningComplete' -> 'SendComplete'",
                'BLE Commissioning and Wi-Fi Provisioning succeeded for'
                ' nodeId=1',
                'Testing on off cluster over Wi-Fi',
                'Test finished',
            ],
        ),
        'MobileDevice log is missing expected BLE Commissioning + Wi-Fi'
        ' Provisioning sequence markers',
    )

    # Audit CHIPEndDevice log for BlueZ BLE peripheral + wpa_supplicant1 Wi-Fi
    # station provisioning markers.
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
                  "wpa_supplicant: Interface properties changed, state is 'completed'",
                  'Current connected network: "CHIP-VirtualWiFi-AP"',
                  'Toggle ep1 on/off from state 0 to 1',
              ],
          ),
          'CHIPEndDevice log is missing expected BlueZ BTP + wpa_supplicant1'
          ' Wi-Fi provisioning sequence markers',
      )


if __name__ == '__main__':
  sys.exit(TestBleWiFiMobileDevice(DEVICE_CONFIG).run_test())
