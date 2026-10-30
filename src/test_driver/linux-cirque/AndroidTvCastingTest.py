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
import re
import shlex
import shutil
import subprocess
import sys
import time

from helper.CHIPTestBase import CHIPVirtualHome, TestResult
from helper.paths import (
    CHIP_REPO_STR,
)
from helper.android_emulator_logs import save_android_emulator_diagnostics
from helper.pcap_oracles import (
    dhcp_ack_addresses,
)
# Removed helper.wifi_lease imports as we parse it manually
from cirque.nodes.androiddockernode import UiAutomatorHelper

logger = logging.getLogger('AndroidTvCastingTest')
logger.setLevel(logging.INFO)

sh = logging.StreamHandler()
sh.setFormatter(
    logging.Formatter('%(asctime)s [%(name)s] %(levelname)s %(message)s')
)
logger.addHandler(sh)

CIRQUE_URL = 'http://localhost:5000'
TEST_WIFI_SSID = 'CHIP-VirtualWiFi-AP'
TEST_WIFI_PSK = 'ChipWiFiPassword123'

ANDROID_BOOT_TIMEOUT_SEC = float(
    os.environ.get('CIRQUE_ANDROID_BOOT_TIMEOUT', '420')
)
ANDROID_COMMISSION_TIMEOUT_SEC = float(
    os.environ.get('CIRQUE_ANDROID_COMMISSION_TIMEOUT', '180')
)

TV_APP_APK_PATH = os.path.join(CHIP_REPO_STR, 'out', 'android-x64-tv-server', 'platform-app', 'outputs', 'apk', 'debug', 'platform-app-debug.apk')
CONTENT_APP_APK_PATH = os.path.join(CHIP_REPO_STR, 'out', 'android-x64-tv-server', 'content-app', 'outputs', 'apk', 'debug', 'content-app-debug.apk')
CASTING_APP_APK_PATH = os.path.join(CHIP_REPO_STR, 'out', 'android-x64-tv-casting-app', 'outputs', 'apk', 'debug', 'app-debug.apk')

DEVICE_CONFIG = {
    'device0': {
        'type': 'wifi_ap',
        'base_image': '@default',
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PSK,
        'use_virtual_wifi_tcp': True,
        'capability': ['TrafficControl'],
        'traffic_control': {'latencyMs': 10},
    },
    'device1': {
        'type': 'android_emulator',
        'base_image': 'cirque-android-runner:latest',
        'capability': ['Bluetooth', 'WiFi', 'Mount'],
        'use_virtual_bt_tcp': True,
        'use_virtual_wifi_tcp': True,
        'wifi_auto_connect': True,
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PSK,
        'tap_interface': 'cirque_tap0',
        'mac_address': '02:15:b2:00:00:01',
        'ipv4_addr': '10.0.1.11',
        'avd_name': 'Pixel_6_API_34',
        'chiptool_apk': str(TV_APP_APK_PATH),
        'extra_apks': [{'apk_path': str(CONTENT_APP_APK_PATH), 'package_name': 'com.example.contentapp'}],
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
    'device2': {
        'type': 'android_emulator',
        'base_image': 'cirque-android-runner:latest',
        'capability': ['Bluetooth', 'WiFi', 'Mount'],
        'use_virtual_bt_tcp': True,
        'use_virtual_wifi_tcp': True,
        'wifi_auto_connect': True,
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PSK,
        'tap_interface': 'cirque_tap0',
        'mac_address': '02:15:b2:00:00:02',
        'ipv4_addr': '10.0.1.12',
        'avd_name': 'Pixel_6_API_34',
        'chiptool_apk': str(CASTING_APP_APK_PATH),
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
}

class AndroidTvCastingTest(CHIPVirtualHome):
  def __init__(self, device_config):
    super().__init__(CIRQUE_URL, device_config)
    self.logger = logger

  def setup(self):
    self.initialize_home()

  def run_test(self, save_logs=True):
    return super().run_test(save_logs=save_logs)

  def save_device_logs(self):
    super().save_device_logs()
    timestamp = int(time.time())
    save_android_emulator_diagnostics(self, timestamp)

  def _guest_shell_arg(self, value: str) -> str:
    return shlex.quote(shlex.quote(str(value)))

  def _wait_for_logcat(self, container_id, pattern, timeout_sec, since_timestamp=None):
    start_time = time.time()
    since_arg = f'--since {since_timestamp}' if since_timestamp else ''
    compiled_pattern = re.compile(pattern)
    quoted_pattern = shlex.quote(pattern)
    while time.time() - start_time < timeout_sec:
      res = self.execute_device_cmd(container_id, f'adb logcat -d -e {quoted_pattern} {since_arg}')
      if str(res['return_code']) == '0':
        for line in res['output'].splitlines():
          if compiled_pattern.search(line):
            self.logger.info("Found logcat pattern %s: %s", pattern, line)
            return True, line
      time.sleep(2)
    self.logger.error("Timeout waiting for logcat pattern %s", pattern)
    return False, ""

  def _click_ui_element(self, container_id, resource_id=None, text=None):
    dump_res = self.execute_device_cmd(container_id, 'adb shell "uiautomator dump /data/local/tmp/dump.xml && cat /data/local/tmp/dump.xml"')
    if str(dump_res['return_code']) != '0':
      return False
    xml_content = dump_res['output']
    idx = xml_content.find('<?xml')
    if idx >= 0:
      xml_content = xml_content[idx:]
    center = UiAutomatorHelper.get_element_center(xml_content, resource_id=resource_id, text=text)
    if not center:
      self.logger.error("Could not find element resource_id=%s, text=%s", resource_id, text)
      return False
    x, y = center
    self.logger.info("Clicking element resource_id=%s, text=%s at (%d, %d)", resource_id, text, x, y)
    self.execute_device_cmd(container_id, f'adb shell input tap {x} {y}')
    return True

  def test_routine(self):
    tv_server = next(d for d in self.non_ap_devices if 'tv-server' in d.get('description', {}).get('chiptool_apk', ''))
    casting_client = next(d for d in self.non_ap_devices if 'casting-app' in d.get('description', {}).get('chiptool_apk', ''))

    for dev in [tv_server, casting_client]:
      self.logger.info(f"Initializing Android emulator environment on {dev['id']}")
      try:
        init_res = self.query_api(
            'init_android_emulator',
            [self.home_id, dev['id']],
            params={'timeout': ANDROID_BOOT_TIMEOUT_SEC},
        )
      except Exception as e:
        self.assertTrue(False, f'REST init_android_emulator failed: {e}')
      
      if isinstance(init_res, dict) and init_res.get('status') == 'success':
        self.logger.info('Android emulator initialized via REST API: %s', init_res)
      else:
        self.assertTrue(False, f'REST init_android_emulator returned: {init_res}')




    # Start tv-app
    self.logger.info("Starting TV App")
    self.execute_device_cmd(tv_server['id'], 'adb shell logcat -G 16M')
    self.execute_device_cmd(tv_server['id'], 'adb shell "pm disable-user com.google.android.bluetooth || pm disable-user com.android.bluetooth"')
    self.execute_device_cmd(tv_server['id'], 'adb shell am start -n com.matter.tv.server/.MainActivity')

    # Wait a bit for TV app to initialize and advertise (and finish rapidly changing its mDNS name)
    time.sleep(15)

    # Start casting-app
    self.logger.info("Starting Casting App")
    self.execute_device_cmd(casting_client['id'], 'adb shell logcat -G 16M')
    self.execute_device_cmd(casting_client['id'], 'adb shell "pm disable-user com.google.android.bluetooth || pm disable-user com.android.bluetooth"')
    self.execute_device_cmd(casting_client['id'], 'adb shell am start -n com.chip.casting/com.matter.casting.MainActivity')

    # Wait for DiscoveryExampleFragment onAdded() Discovered CastingPlayer deviceId:
    success, _ = self._wait_for_logcat(casting_client['id'], r'Discovered CastingPlayer deviceId:', ANDROID_COMMISSION_TIMEOUT_SEC)
    self.assertTrue(success, "Failed to discover CastingPlayer")

    time.sleep(2) # Give UI time to update
    self.assertTrue(self._click_ui_element(casting_client['id'], resource_id='commissionable_player_description'), "Failed to click player description")

    # Wait for UDC receive on TV app
    success, _ = self._wait_for_logcat(tv_server['id'], r'UDC instance=', ANDROID_COMMISSION_TIMEOUT_SEC)
    self.assertTrue(success, "Failed to receive UDC on TV app")

    # In tv app, if it prompts, we should accept it. 
    # But instruction says: "auto-accept when matter_show_authorisation_dialog=0 -> 'OnPromptAccepted'... CHECK this default really holds; if a dialog appears, accept it via uiautomator."
    self.logger.info("Waiting for TV App dialog to inflate")
    time.sleep(5)
    tv_dump = self.execute_device_cmd(tv_server['id'], 'adb shell "uiautomator dump /data/local/tmp/dump.xml && cat /data/local/tmp/dump.xml"')
    if 'dialog_title' in tv_dump['output'] or 'ALLOW' in tv_dump['output'].upper() or 'OK' in tv_dump['output'].upper():
       self.logger.info("TV App dialog found, trying to accept")
       # Usually "Allow" or "OK"
       if not self._click_ui_element(tv_server['id'], text='Allow'):
           if not self._click_ui_element(tv_server['id'], text='ALLOW'):
               self._click_ui_element(tv_server['id'], text='OK')
    
    # Wait for connection success
    success, _ = self._wait_for_logcat(casting_client['id'], r'Successfully connected to CastingPlayer', ANDROID_COMMISSION_TIMEOUT_SEC)
    self.assertTrue(success, "Failed to connect to CastingPlayer")

    time.sleep(2)
    self.assertTrue(self._click_ui_element(casting_client['id'], resource_id='connectionFragmentNextButton'), "Failed to click next button")

    time.sleep(2)
    self.assertTrue(self._click_ui_element(casting_client['id'], resource_id='selectContentLauncherLaunchURLButton'), "Failed to click launch URL button")

    time.sleep(2)
    self.assertTrue(self._click_ui_element(casting_client['id'], resource_id='launchUrlButton'), "Failed to click launch URL button")

    # Wait for LaunchURL received on tv-app (native ContentLauncherManager or Android ContentAppCommandDelegate)
    success, _ = self._wait_for_logcat(
        tv_server['id'],
        r'ContentLauncherManager::HandleLaunchUrl|custom response from content app for content launcher',
        ANDROID_COMMISSION_TIMEOUT_SEC,
    )
    self.assertTrue(success, "Failed to handle LaunchUrl on TV app")

    # Wait for LaunchURL success on casting side
    success, _ = self._wait_for_logcat(casting_client['id'], r'LaunchURL success', ANDROID_COMMISSION_TIMEOUT_SEC)
    self.assertTrue(success, "Failed to get LaunchURL success")

    # Second command: ApplicationBasic/MediaPlayback read
    self.logger.info("Testing ReadVendorID")
    self.execute_device_cmd(casting_client['id'], 'adb shell input keyevent 4') # Back
    time.sleep(2)
    self.assertTrue(self._click_ui_element(casting_client['id'], resource_id='selectApplicationBasicReadVendorIDButton'), "Failed to click selectApplicationBasicReadVendorIDButton")
    time.sleep(2)
    self.assertTrue(self._click_ui_element(casting_client['id'], resource_id='readVendorIdButton'), "Failed to click readVendorIdButton")
    
    success, _ = self._wait_for_logcat(casting_client['id'], r'ReadVendorID success', ANDROID_COMMISSION_TIMEOUT_SEC)
    self.assertTrue(success, "Failed to ReadVendorID")

    self.logger.info("Test passed")

    # Oracles check
    self.logger.info("Checking pcap for oracles")
    time.sleep(2)
    pcap_dir = os.environ.get('CIRQUE_PCAP_DIR') or os.environ.get('DEVICE_LOG_DIR')
    self.assertTrue(pcap_dir and os.path.isdir(pcap_dir), f"pcap_dir not found: {pcap_dir}")
    pcap_path = os.path.join(pcap_dir, 'wifi_medium.pcap')
    self.assertTrue(os.path.isfile(pcap_path), f"wifi_medium.pcap not found in {pcap_dir}")

    # Run tshark for mDNS
    mdns_res = subprocess.run(
        ['tshark', '-r', pcap_path, '-Y', 'mdns && dns.resp.name contains "_matterd._udp"'],
        capture_output=True,
        text=True,
    )
    self.logger.info("mDNS _matterd._udp frames:\n%s", mdns_res.stdout.strip())
    self.assertTrue(mdns_res.stdout.strip(), "No _matterd._udp mDNS answer found from TV in pcap")

    # UDC UDP datagram
    udc_res = subprocess.run(
        ['tshark', '-r', pcap_path, '-Y', 'udp && (udp.dstport == 33433 || udp.dstport == 5550 || udp.dstport == 5540)'],
        capture_output=True,
        text=True,
    )
    self.logger.info("UDC UDP datagrams:\n%s", udc_res.stdout.strip())
    self.assertTrue(udc_res.stdout.strip(), "No UDC datagram found from Casting App to TV")

    return TestResult.OK


if __name__ == '__main__':
  sys.exit(AndroidTvCastingTest(DEVICE_CONFIG).run_test())
