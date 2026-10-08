# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""End-to-End & Unit Tests for Android Emulator Virtual Home with CHIPTool.

Validates:
  1. Virtual Home Topology configuration for Android Emulator.
  2. CLI validation script (validate_virtual_android_home.sh).
  3. UI automation helpers for CHIPTool.apk (BLE PASE, Wi-Fi, Thread).
  4. PCAP inspection (summarize_pcap and tshark filters).
  5. Live KVM Android Emulator E2E commissioning (CIRQUE_ANDROID_E2E=1).
"""

import os
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CHIP_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, '..', '..', '..'))
CIRQUE_ROOT = os.path.join(CHIP_ROOT, 'third_party', 'cirque', 'repo')
if not os.path.isdir(os.path.join(CIRQUE_ROOT, 'cirque')):
  # Fallback for git worktrees where submodules exist in main repo.
  _fallback_cirque = os.path.expanduser(
      '~/connectedhomeip/third_party/cirque/repo'
  )
  if os.path.isdir(os.path.join(_fallback_cirque, 'cirque')):
    CIRQUE_ROOT = _fallback_cirque
if CIRQUE_ROOT not in sys.path:
  sys.path.insert(0, CIRQUE_ROOT)

from cirque.capabilities.pcapcapability import (  # noqa: E402
    DLT_EN10MB,
    PcapWriter,
)
from cirque.home.virtual_home_topology import (  # noqa: E402
    VirtualHomeTopology,
)
from cirque.nodes.androiddockernode import (  # noqa: E402
    UiAutomatorHelper,
)
from cirque.pcap.summarize_pcap import summarize_pcap  # noqa: E402

VALIDATE_SCRIPT = os.path.join(
    CIRQUE_ROOT, 'examples', 'validate_virtual_android_home.sh'
)


class TestVirtualAndroidHomeBleWiFiUnit(unittest.TestCase):
  """Fast, hermetic unit tests for Android Virtual Home + CHIPTool helpers."""

  def setUp(self):
    super().setUp()
    self.tmp_dir = tempfile.mkdtemp(prefix='cirque_android_unit_')

  def tearDown(self):
    if os.path.isdir(self.tmp_dir):
      shutil.rmtree(self.tmp_dir)
    super().tearDown()

  def test_00_test_drivers_import_without_cirque_pythonpath(self):
    check_script = os.path.join(self.tmp_dir, 'check_client_isolation.py')
    with open(check_script, 'w', encoding='utf-8') as f:
      f.write(
          'import importlib\n'
          'import sys\n'
          'from unittest.mock import MagicMock, patch\n'
          "sys.path = [p for p in sys.path if 'cirque/repo' not in p]\n"
          "sys.modules['cirque'] = None\n"
          "import helper.CHIPTestBase as base\n"
          'for mod_name in (\n'
          "    'EchoTest',\n"
          "    'IcdDeviceTest',\n"
          "    'BleMobileDeviceTest',\n"
          "    'BleWiFiMobileDeviceTest',\n"
          "    'AndroidBleWiFiMobileDeviceTest',\n"
          "    'AndroidBleThreadMobileDeviceTest',\n"
          '):\n'
          '  importlib.import_module(mod_name)\n'
          'class DummyHome(base.CHIPVirtualHome):\n'
          '  def __init__(self):\n'
          "    super().__init__('http://127.0.0.1:5000', {})\n"
          'home = DummyHome()\n'
          'home.logger = MagicMock()\n'
          'post_resp = MagicMock()\n'
          "post_resp.json.return_value = 'home_0'\n"
          'get_resp = MagicMock()\n'
          "get_resp.json.side_effect = [['home_0'], {}]\n"
          "with patch('requests.get', return_value=get_resp), \\\n"
          "     patch('requests.post', return_value=post_resp):\n"
          '  home.initialize_home()\n'
          "print('CLIENT_ISOLATION_OK')\n"
      )
    env = dict(os.environ)
    env['PYTHONPATH'] = SCRIPT_DIR
    proc = subprocess.run(
        [sys.executable, check_script],
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )
    self.assertEqual(proc.returncode, 0, msg=proc.stderr)
    self.assertIn('CLIENT_ISOLATION_OK', proc.stdout)

  def test_01_android_ble_wifi_topology_config(self):
    config = VirtualHomeTopology.default_android_emulator_ble_wifi_config(
        ssid='TEST_AP', wifi_psk='test_psk'
    )
    self.assertIn('wifi_ap', config)
    self.assertIn('android_emulator', config)
    self.assertIn('matter_device', config)

    emu_cfg = config['android_emulator']
    self.assertEqual(emu_cfg['type'], 'android_emulator')
    self.assertTrue(emu_cfg['is_tap_station'])
    self.assertEqual(emu_cfg['tap_interface'], 'cirque_tap0')
    self.assertEqual(emu_cfg['preferred_mode'], 'kvm_emulator')
    self.assertIn('Bluetooth', emu_cfg['capability'])
    self.assertIn('WiFi', emu_cfg['capability'])

    dev_cfg = config['matter_device']
    self.assertEqual(dev_cfg['type'], 'IoTEndDevice')
    self.assertIn('Bluetooth', dev_cfg['capability'])
    self.assertIn('WiFi', dev_cfg['capability'])

  def test_02_android_ble_thread_topology_config(self):
    config = VirtualHomeTopology.default_android_emulator_ble_thread_config(
        ssid='TEST_AP', wifi_psk='test_psk'
    )
    self.assertIn('wifi_ap', config)
    self.assertIn('thread_border_router', config)
    self.assertIn('android_emulator', config)
    self.assertIn('matter_device', config)

    tbr_cfg = config['thread_border_router']
    self.assertEqual(tbr_cfg['type'], 'ThreadBorderRouter')
    self.assertTrue(tbr_cfg.get('rcp_mode'))
    self.assertTrue(tbr_cfg.get('wifi_auto_connect'))
    self.assertIn('Thread', tbr_cfg['capability'])
    self.assertIn('WiFi', tbr_cfg['capability'])

    dev_cfg = config['matter_device']
    self.assertEqual(dev_cfg['type'], 'IoTEndDevice')
    self.assertTrue(dev_cfg.get('rcp_mode'))
    self.assertNotIn('WiFi', dev_cfg['capability'])
    self.assertIn('Thread', dev_cfg['capability'])
    self.assertIn('Bluetooth', dev_cfg['capability'])

  def test_03_validate_script_cli_help(self):
    proc = subprocess.run(
        [VALIDATE_SCRIPT, '--help'],
        capture_output=True,
        text=True,
        check=False,
    )
    self.assertEqual(proc.returncode, 0)
    self.assertIn('Usage:', proc.stdout)
    self.assertIn('--negative-psk', proc.stdout)
    self.assertIn('--negative-bt', proc.stdout)

  def test_04_validate_script_negative_bt(self):
    proc = subprocess.run(
        [VALIDATE_SCRIPT, '--negative-bt'],
        capture_output=True,
        text=True,
        check=False,
        env=dict(os.environ, PYTHONPATH=CIRQUE_ROOT),
    )
    self.assertEqual(proc.returncode, 0)
    self.assertIn(
        'SUCCESS: BT relay enable/disable negative control verified',
        proc.stdout,
    )

  def test_05_validate_script_negative_psk_missing_container(self):
    proc = subprocess.run(
        [VALIDATE_SCRIPT, '--negative-psk'],
        capture_output=True,
        text=True,
        check=False,
        env=dict(os.environ, CIRQUE_DISABLE_CONTAINER_AUTODISCOVERY='1'),
    )
    self.assertNotEqual(proc.returncode, 0)
    self.assertIn(
        'ERROR: IoTEndDevice container required for negative PSK test',
        proc.stderr,
    )

  def test_06_uiautomator_helper_parsing(self):
    xml_data = (
        "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>\n"
        '<hierarchy rotation="0">\n'
        '  <node bounds="[0,0][1080,2400]">\n'
        '    <node resource-id="'
        'com.google.chip.chiptool:id/provisionWiFiCredentialsBtn"\n'
        '      bounds="[42,443][714,569]"'
        ' text="PROVISION CHIP DEVICE WITH WI-FI" />\n'
        '    <node resource-id="'
        'com.google.chip.chiptool:id/provisionThreadCredentialsBtn"\n'
        '      bounds="[42,600][714,726]"'
        ' text="PROVISION CHIP DEVICE WITH THREAD" />\n'
        '  </node>\n'
        '</hierarchy>\n'
    )
    wifi_center = UiAutomatorHelper.get_element_center(
        xml_data, resource_id='provisionWiFiCredentialsBtn'
    )
    self.assertEqual(wifi_center, ((42 + 714) // 2, (443 + 569) // 2))

    thread_center = UiAutomatorHelper.get_element_center(
        xml_data, resource_id='provisionThreadCredentialsBtn'
    )
    self.assertEqual(thread_center, ((42 + 714) // 2, (600 + 726) // 2))

  def test_07_pcap_generation_and_summarize(self):
    pcap_path = os.path.join(self.tmp_dir, 'wifi_medium.pcap')
    writer = PcapWriter(pcap_path, dlt=DLT_EN10MB)

    # 1. Write an EAPOL packet (EtherType 0x888e)
    # Dst MAC: 6B, Src MAC: 6B, EtherType: 2B (0x888e), EAPOL body: 4B
    eapol_frame = (
        b'\x02\x00\x00\x00\x01\x01\x02\x15\xb2\x00\x00\x00\x88\x8e'
        b'\x01\x03\x00\x00'
    )
    writer.write_frame(eapol_frame)

    # 2. Write an IPv4 UDP 5540 packet
    # Ethernet (14B) + IPv4 (20B, proto=17, src=10.0.1.5, dst=10.0.1.10)
    # + UDP (8B, dport=5540) + payload (4B)
    udp5540_frame = (
        b'\x02\x00\x00\x00\x01\x02\x02\x15\xb2\x00\x00\x00\x08\x00'
        b'\x45\x00\x00\x20\x00\x01\x00\x00\x40\x11\x00\x00'
        b'\x0a\x00\x01\x05\x0a\x00\x01\x0a'
        b'\x15\xa4\x15\xa4\x00\x0c\x00\x00'  # src 5540, dst 5540, len 12
        b'\x00\x01\x02\x03'
    )
    writer.write_frame(udp5540_frame)
    writer.close()

    summary = summarize_pcap(pcap_path)
    self.assertEqual(summary['records'], 2)
    self.assertEqual(summary['dlt'], 1)
    self.assertIn('EN10MB', summary['dlt_name'])

    # Test directory summarization CLI
    proc = subprocess.run(
        [
            'python3',
            '-m',
            'cirque.pcap.summarize_pcap',
            '--dir',
            self.tmp_dir,
            '--json',
        ],
        capture_output=True,
        text=True,
        check=False,
        env=dict(os.environ, PYTHONPATH=CIRQUE_ROOT),
    )
    self.assertEqual(proc.returncode, 0)
    self.assertIn('wifi_medium.pcap', proc.stdout)

    # Test tshark filters if tshark is available
    if shutil.which('tshark'):
      tshark_eapol = subprocess.run(
          ['tshark', '-r', pcap_path, '-Y', 'eth.type == 0x888e'],
          capture_output=True,
          text=True,
          check=False,
      )
      self.assertEqual(tshark_eapol.returncode, 0)
      self.assertIn('EAPOL', tshark_eapol.stdout)

      tshark_udp = subprocess.run(
          ['tshark', '-r', pcap_path, '-Y', 'udp.port == 5540'],
          capture_output=True,
          text=True,
          check=False,
      )
      self.assertEqual(tshark_udp.returncode, 0)
      self.assertIn('5540', tshark_udp.stdout)

  def test_08_mocked_verify_android_ble_wifi_commissioning(self):
    mock_home = MagicMock()
    mock_android = MagicMock()
    mock_android.get_guest_wlan_mac.return_value = '02:15:b2:00:00:00'
    mock_android.get_wifi_station_id.return_value = 'android_emulator'
    mock_android.capabilities = []
    mock_android.commission_via_chiptool_ui.return_value = {
        'status': 'success',
        'commissioned_node_id': 1,
    }
    mock_android.toggle_onoff_via_chiptool_ui.return_value = {
        'status': 'success'
    }
    mock_android.read_onoff_via_chiptool_ui.return_value = {
        'status': 'success',
        'value': 'true',
    }
    mock_android.container = MagicMock()
    mock_android.container.exec_run.return_value = SimpleNamespace(
        exit_code=0, output=b'inet 10.0.1.5\n'
    )
    mock_home.home = {'devices': {'android_emulator': mock_android}}

    def fake_cmd(cmd, device_id, **kwargs):
      cmd_str = str(cmd)
      if 'iptables' in cmd_str:
        return SimpleNamespace(
            exit_code=0,
            output=(
                b'-A OUTPUT -o eth0 -p udp -m udp --dport 5353 -j DROP\n'
                b'-A INPUT -i eth0 -p udp -m udp --dport 5353 -j DROP\n'
            ),
        )
      if 'pidof chip-all-clusters-app' in cmd_str:
        return SimpleNamespace(exit_code=1, output=b'')
      if 'grep -E' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'CHIP:DL: BLE adv start\n')
      if 'ip -4 addr show dev wlan0' in cmd_str:
        return SimpleNamespace(
            exit_code=0, output=b'inet 10.0.1.10/24 brd 10.0.1.255 dev wlan0\n'
        )
      return SimpleNamespace(exit_code=0, output=b'ok\n')

    mock_home.execute_device_cmd = fake_cmd
    res = VirtualHomeTopology.verify_android_emulator_ble_wifi_commissioning(
        cirque_home=mock_home,
        controller_id='android_emulator',
        device_id='matter_device',
        timeout_sec=1.0,
        restart_app=True,
    )
    self.assertEqual(res.get('status'), 'success')
    self.assertEqual(res.get('controller_ip'), '10.0.1.5')
    self.assertEqual(res.get('device_ip'), '10.0.1.10')

  def test_09_mocked_verify_android_ble_thread_commissioning(self):
    mock_home = MagicMock()
    mock_android = MagicMock()
    mock_android.get_guest_wlan_mac.return_value = '02:15:b2:00:00:00'
    mock_android.get_wifi_station_id.return_value = 'android_emulator'
    mock_android.capabilities = []
    mock_android.commission_thread_via_chiptool_ui.return_value = {
        'status': 'success',
        'commissioned_node_id': 1,
    }
    mock_android.toggle_onoff_via_chiptool_ui.return_value = {
        'status': 'success'
    }
    mock_android.read_onoff_via_chiptool_ui.return_value = {
        'status': 'success',
        'value': 'true',
    }
    mock_android.container = MagicMock()
    mock_android.container.exec_run.return_value = SimpleNamespace(
        exit_code=0, output=b'inet 10.0.1.5\n'
    )
    mock_home.home = {'devices': {'android_emulator': mock_android}}

    def fake_cmd(cmd, device_id, **kwargs):
      cmd_str = str(cmd)
      if 'iptables' in cmd_str:
        return SimpleNamespace(
            exit_code=0,
            output=(
                b'-A OUTPUT -o eth0 -p udp -m udp --dport 5353 -j DROP\n'
                b'-A INPUT -i eth0 -p udp -m udp --dport 5353 -j DROP\n'
            ),
        )
      if 'pidof chip-all-clusters-app' in cmd_str:
        return SimpleNamespace(exit_code=1, output=b'')
      if 'ot-ctl state' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'leader\n')
      if 'ot-ctl extpanid' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'1111111122222222\n')
      if 'ot-ctl panid' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'0x1234\n')
      if 'ot-ctl channel' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'15\n')
      if 'grep -E' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'CHIP:DL: BLE adv start\n')
      if 'ip -4 addr show dev wlan0' in cmd_str:
        return SimpleNamespace(
            exit_code=0, output=b'inet 10.0.1.10/24 brd 10.0.1.255 dev wlan0\n'
        )
      return SimpleNamespace(exit_code=0, output=b'ok\n')

    mock_home.execute_device_cmd = fake_cmd
    res = VirtualHomeTopology.verify_android_emulator_ble_thread_commissioning(
        cirque_home=mock_home,
        controller_id='android_emulator',
        device_id='matter_device',
        timeout_sec=1.0,
        restart_app=True,
    )
    self.assertEqual(res.get('status'), 'success')
    self.assertEqual(res.get('thread_state'), 'leader')
    self.assertEqual(res.get('thread_extpanid'), '1111111122222222')
    self.assertEqual(res.get('thread_panid'), '0x1234')
    self.assertEqual(res.get('thread_channel'), '15')

  def test_10_unit_negative_controls(self):
    from cirque.capabilities.bluetoothcapability import BlueToothCapability

    bt_server = BlueToothCapability.get_or_start_virtual_server()
    bt_server.set_relay_enabled(False)
    self.assertFalse(bt_server.relay_enabled)
    bt_server.set_relay_enabled(True)
    self.assertTrue(bt_server.relay_enabled)

  def test_11_negative_commissioning_status_checks(self):
    mock_home = MagicMock()
    mock_android = MagicMock()
    mock_android.get_guest_wlan_mac.return_value = '02:15:b2:00:00:00'
    mock_android.get_wifi_station_id.return_value = 'android_emulator'
    mock_android.capabilities = []
    # Wi-Fi UI commissioning failure
    mock_android.commission_via_chiptool_ui.return_value = {
        'status': 'failed',
        'error': 'BLE discovery timeout',
    }
    mock_android.container = MagicMock()
    mock_android.container.exec_run.return_value = SimpleNamespace(
        exit_code=0, output=b'inet 10.0.1.5\n'
    )
    mock_home.home = {'devices': {'android_emulator': mock_android}}

    def fake_cmd_wifi_negative(cmd, device_id, **kwargs):
      cmd_str = str(cmd)
      if 'iptables' in cmd_str:
        return SimpleNamespace(
            exit_code=0,
            output=(
                b'-A OUTPUT -o eth0 -p udp -m udp --dport 5353 -j DROP\n'
                b'-A INPUT -i eth0 -p udp -m udp --dport 5353 -j DROP\n'
            ),
        )
      if 'pidof chip-all-clusters-app' in cmd_str:
        return SimpleNamespace(exit_code=1, output=b'')
      if 'grep -E' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'CHIP:DL: BLE adv start\n')
      if 'ip -4 addr show dev wlan0' in cmd_str:
        return SimpleNamespace(
            exit_code=0, output=b'inet 10.0.1.10/24 brd 10.0.1.255 dev wlan0\n'
        )
      return SimpleNamespace(exit_code=0, output=b'ok\n')

    mock_home.execute_device_cmd = fake_cmd_wifi_negative

    # Even though wlan0 has 10.0.1.10, comm_res failed -> overall status failed
    res_wifi = (
        VirtualHomeTopology.verify_android_emulator_ble_wifi_commissioning(
            cirque_home=mock_home,
            controller_id='android_emulator',
            device_id='matter_device',
            timeout_sec=1.0,
            restart_app=True,
        )
    )
    self.assertEqual(res_wifi.get('status'), 'failed')
    self.assertEqual(res_wifi.get('phase'), 'commissioning')
    mock_android.toggle_onoff_via_chiptool_ui.assert_not_called()
    mock_android.read_onoff_via_chiptool_ui.assert_not_called()

    # Thread Case A: commission_thread_via_chiptool_ui failed
    mock_android.commission_thread_via_chiptool_ui.return_value = {
        'status': 'failed',
        'error': 'Thread provisioning timed out',
    }

    def fake_cmd_thread_negative(cmd, device_id, **kwargs):
      cmd_str = str(cmd)
      if 'iptables' in cmd_str:
        return SimpleNamespace(
            exit_code=0,
            output=(
                b'-A OUTPUT -o eth0 -p udp -m udp --dport 5353 -j DROP\n'
                b'-A INPUT -i eth0 -p udp -m udp --dport 5353 -j DROP\n'
            ),
        )
      if 'pidof chip-all-clusters-app' in cmd_str:
        return SimpleNamespace(exit_code=1, output=b'')
      if 'ot-ctl state' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'leader\n')
      if 'ot-ctl extpanid' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'1111111122222222\n')
      if 'ot-ctl panid' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'0x1234\n')
      if 'ot-ctl channel' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'15\n')
      if 'grep -E' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'CHIP:DL: BLE adv start\n')
      if 'ip -4 addr show dev wlan0' in cmd_str:
        return SimpleNamespace(
            exit_code=0, output=b'inet 10.0.1.10/24 brd 10.0.1.255 dev wlan0\n'
        )
      return SimpleNamespace(exit_code=0, output=b'ok\n')

    mock_home.execute_device_cmd = fake_cmd_thread_negative
    res_thread_fail = (
        VirtualHomeTopology.verify_android_emulator_ble_thread_commissioning(
            cirque_home=mock_home,
            controller_id='android_emulator',
            device_id='matter_device',
            timeout_sec=1.0,
            restart_app=True,
        )
    )
    self.assertEqual(res_thread_fail.get('status'), 'failed')
    self.assertEqual(res_thread_fail.get('phase'), 'commissioning')
    mock_android.toggle_onoff_via_chiptool_ui.assert_not_called()

    # Thread Case B: commission succeeded, but ot-ctl state is detached
    mock_android.commission_thread_via_chiptool_ui.return_value = {
        'status': 'success',
        'commissioned_node_id': 1,
    }

    def fake_cmd_thread_detached(cmd, device_id, **kwargs):
      cmd_str = str(cmd)
      if 'iptables' in cmd_str:
        return SimpleNamespace(
            exit_code=0,
            output=(
                b'-A OUTPUT -o eth0 -p udp -m udp --dport 5353 -j DROP\n'
                b'-A INPUT -i eth0 -p udp -m udp --dport 5353 -j DROP\n'
            ),
        )
      if 'pidof chip-all-clusters-app' in cmd_str:
        return SimpleNamespace(exit_code=1, output=b'')
      if 'ot-ctl state' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'detached\n')
      if 'ot-ctl extpanid' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'1111111122222222\n')
      if 'ot-ctl panid' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'0x1234\n')
      if 'ot-ctl channel' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'15\n')
      if 'grep -E' in cmd_str:
        return SimpleNamespace(exit_code=0, output=b'CHIP:DL: BLE adv start\n')
      if 'ip -4 addr show dev wlan0' in cmd_str:
        return SimpleNamespace(
            exit_code=0, output=b'inet 10.0.1.10/24 brd 10.0.1.255 dev wlan0\n'
        )
      return SimpleNamespace(exit_code=0, output=b'ok\n')

    mock_home.execute_device_cmd = fake_cmd_thread_detached
    res_detached = (
        VirtualHomeTopology.verify_android_emulator_ble_thread_commissioning(
            cirque_home=mock_home,
            controller_id='android_emulator',
            device_id='matter_device',
            timeout_sec=1.0,
            restart_app=True,
        )
    )
    self.assertEqual(res_detached.get('status'), 'failed')
    self.assertEqual(res_detached.get('phase'), 'commissioning')
    mock_android.toggle_onoff_via_chiptool_ui.assert_not_called()


class TestVirtualAndroidHomeBleWiFiLiveE2E(unittest.TestCase):
  """Live E2E test running real KVM Android emulator + IoTEndDevice.

  Guarded by environment variable: CIRQUE_ANDROID_E2E=1.
  """

  @classmethod
  def setUpClass(cls):
    super().setUpClass()
    if os.environ.get('CIRQUE_ANDROID_E2E') != '1':
      raise unittest.SkipTest(
          'Set CIRQUE_ANDROID_E2E=1 to execute live Android emulator tests'
      )
    if not os.path.exists('/dev/kvm'):
      raise unittest.SkipTest('Host /dev/kvm not available')

    # Ensure no conflicting runner containers are active
    proc = subprocess.run(
        [
            'docker',
            'ps',
            '--filter',
            'ancestor=cirque-android-runner:latest',
            '-q',
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.stdout.strip():
      raise unittest.SkipTest(
          'Another cirque-android-runner container is already running: '
          f'{proc.stdout.strip()}'
      )

    from cirque.home.home import CirqueHome

    cls.pcap_dir = tempfile.mkdtemp(prefix='cirque_android_pcap_')
    os.environ['CIRQUE_PCAP_DIR'] = cls.pcap_dir

    cls.home = CirqueHome()
    cls.config = (
        VirtualHomeTopology.default_android_emulator_ble_thread_config(
            ssid='CIRQUE_HOME_AP',
            wifi_psk='cirque_home_psk',
            pcap_dir=cls.pcap_dir,
            wifi_auto_connect=False,
        )
    )
    try:
      cls.home.create_home(cls.config)
    except Exception:
      cls.home.destroy_home()
      raise

  @classmethod
  def tearDownClass(cls):
    if hasattr(cls, 'home') and cls.home is not None:
      cls.home.destroy_home()
    if hasattr(cls, 'pcap_dir') and os.path.isdir(cls.pcap_dir):
      shutil.rmtree(cls.pcap_dir, ignore_errors=True)
    super().tearDownClass()

  def test_01_live_ble_wifi_commissioning(self):
    res = VirtualHomeTopology.verify_android_emulator_ble_wifi_commissioning(
        cirque_home=self.home,
        controller_id='android_emulator',
        device_id='matter_device',
        timeout_sec=90.0,
    )
    self.assertEqual(res.get('status'), 'success')
    self.assertTrue(res.get('controller_ip', '').startswith('10.0.1.'))
    self.assertTrue(res.get('device_ip', '').startswith('10.0.1.'))

  def test_02_cli_validation_script_and_pcap_integrity(self):
    proc = subprocess.run(
        [
            VALIDATE_SCRIPT,
            '--pcap-dir',
            self.pcap_dir,
        ],
        capture_output=True,
        text=True,
        check=False,
        env=dict(os.environ, PYTHONPATH=CIRQUE_ROOT),
    )
    self.assertEqual(
        proc.returncode,
        0,
        f'validate_virtual_android_home.sh failed:\n'
        f'STDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}',
    )
    self.assertIn(
        'SUCCESS: Android Virtual Home (BLE + Wi-Fi Data Plane) Validated!',
        proc.stdout,
    )

  def test_03_live_ble_thread_commissioning(self):
    res = VirtualHomeTopology.verify_android_emulator_ble_thread_commissioning(
        cirque_home=self.home,
        controller_id='android_emulator',
        device_id='matter_device',
        timeout_sec=90.0,
    )
    self.assertEqual(res.get('status'), 'success')
    self.assertIn(res.get('thread_state', ''), ('leader', 'router', 'child'))

  def test_04_live_negative_controls(self):
    from cirque.capabilities.bluetoothcapability import BlueToothCapability

    # Negative control: wrong Wi-Fi PSK fails to commission
    res_psk = (
        VirtualHomeTopology.verify_android_emulator_ble_wifi_commissioning(
            cirque_home=self.home,
            controller_id='android_emulator',
            device_id='matter_device',
            wifi_psk='wrong_invalid_psk_1234',
            timeout_sec=15.0,
        )
    )
    self.assertEqual(res_psk.get('status'), 'failed')

    # Negative control: disabled BT relay prevents commissioning
    bt_server = BlueToothCapability.get_or_start_virtual_server()
    bt_server.set_relay_enabled(False)
    try:
      res_bt = (
          VirtualHomeTopology.verify_android_emulator_ble_wifi_commissioning(
              cirque_home=self.home,
              controller_id='android_emulator',
              device_id='matter_device',
              timeout_sec=15.0,
          )
      )
      self.assertEqual(res_bt.get('status'), 'failed')
    finally:
      bt_server.set_relay_enabled(True)


class TestAndroidBleThreadMobileDeviceTestUnit(unittest.TestCase):
  """Unit tests for the 4-node AndroidBleThreadMobileDeviceTest TBR flow."""

  def test_android_ble_thread_4_node_device_config(self):
    import AndroidBleThreadMobileDeviceTest as abt_mod

    test_inst = abt_mod.TestAndroidBleThreadMobileDevice()
    devices = test_inst.device_config

    self.assertEqual(len(devices), 4, f'Expected 4 devices, got {len(devices)}')
    self.assertIn('device0', devices)
    self.assertIn('device1', devices)
    self.assertIn('device2', devices)
    self.assertIn('device3', devices)

    self.assertEqual(devices['device0']['type'], 'wifi_ap')

    dev1_caps = devices['device1']['capability']
    self.assertEqual(devices['device1']['type'], 'ThreadBorderRouter')
    self.assertIn('WiFi', dev1_caps)
    self.assertIn('Thread', dev1_caps)

    dev2_caps = devices['device2']['capability']
    self.assertEqual(devices['device2']['type'], 'CHIPEndDevice')
    self.assertIn('Bluetooth', dev2_caps)
    self.assertIn('Thread', dev2_caps)
    self.assertNotIn('WiFi', dev2_caps)

    dev3_caps = devices['device3']['capability']
    self.assertEqual(devices['device3']['type'], 'android_emulator')
    self.assertIn('WiFi', dev3_caps)
    self.assertIn('Bluetooth', dev3_caps)

  def test_setup_thread_border_routers_forms_leader_and_extracts_dataset(self):
    import AndroidBleThreadMobileDeviceTest as abt_mod

    test_inst = abt_mod.TestAndroidBleThreadMobileDevice()
    test_inst.execute_device_cmd = MagicMock(
        return_value={'returncode': 0, 'output': 'leader\n'}
    )
    dataset_output = (
        'Active Timestamp: 1\n'
        'Channel: 15\n'
        'Channel Mask: 0x07fff800\n'
        'Ext PAN ID: 1111222233334444\n'
        'Mesh Local Prefix: fd11:22::/64\n'
        'Network Key: 00112233445566778899aabbccddeeff\n'
        'Network Name: OpenThread-Cirque\n'
        'PAN ID: 0x1234\n'
        'PSKc: 00112233445566778899aabbccddeeff\n'
        'Done\n'
    )
    test_inst._get_tbr_active_dataset = MagicMock(
        return_value={
            'channel': '15',
            'panid': '0x1234',
            'extpanid': '1111222233334444',
            'networkkey': '00112233445566778899aabbccddeeff',
            'networkname': 'OpenThread-Cirque',
            'raw': dataset_output,
        }
    )

    ds = test_inst._setup_thread_border_routers(['device1'])
    self.assertEqual(ds.get('channel'), '15')
    self.assertEqual(ds.get('panid'), '0x1234')
    self.assertEqual(ds.get('extpanid'), '1111222233334444')
    self.assertEqual(ds.get('networkkey'), '00112233445566778899aabbccddeeff')

    cmds = [
        call_args[0][1]
        for call_args in test_inst.execute_device_cmd.call_args_list
    ]
    self.assertTrue(any('ot-ctl dataset init new' in c for c in cmds))
    self.assertTrue(any('ot-ctl state' in c for c in cmds))
    self.assertTrue(any('ot-ctl prefix add fd11:33::/64' in c for c in cmds))
    self.assertTrue(any('tbr_border_proxy.py' in c for c in cmds))

  def test_commission_and_interact_chiptool_uses_tbr_dataset(self):
    import AndroidBleThreadMobileDeviceTest as abt_mod

    test_inst = abt_mod.TestAndroidBleThreadMobileDevice()
    test_inst.execute_device_cmd = MagicMock(return_value='success')
    test_inst.query_api = MagicMock(return_value=None)
    test_inst._provision_thread_network_via_adb_and_otctl = MagicMock(
        return_value=True
    )
    test_inst._trigger_ui_toggle_and_read = MagicMock(return_value=(True, True))

    tbr_dataset = {
        'channel': '15',
        'panid': '0x1234',
        'extpanid': '1111222233334444',
        'networkkey': '00112233445566778899aabbccddeeff',
    }

    test_inst._commission_and_interact_chiptool(
        'device3',
        tbr_dataset=tbr_dataset,
    )

    provision_mock = test_inst._provision_thread_network_via_adb_and_otctl
    provision_mock.assert_called_once_with(
        'device3', tbr_dataset=tbr_dataset
    )
    test_inst._trigger_ui_toggle_and_read.assert_called_once_with('device3')

  def test_verify_post_commissioning_thread_state_tbr_leader_and_end_device(
      self,
  ):
    import AndroidBleThreadMobileDeviceTest as abt_mod

    test_inst = abt_mod.TestAndroidBleThreadMobileDevice()

    def fake_cmd(dev_id, cmd):
      if dev_id == 'device1':
        if 'ot-ctl state' in cmd:
          return 'leader\n'
      elif dev_id == 'device2':
        if 'ot-ctl state' in cmd:
          return 'child\n'
        if 'ip -6 addr show dev wpan0' in cmd:
          return {'output': 'inet6 fd11:33::2/64 scope global nodad\n'}
      return ''

    test_inst.execute_device_cmd = MagicMock(side_effect=fake_cmd)
    test_inst._verify_post_commissioning_thread_state(['device1'], ['device2'])

    tbr_calls = [
        c[0][1]
        for c in test_inst.execute_device_cmd.call_args_list
        if c[0][0] == 'device1'
    ]
    self.assertTrue(any('ot-ctl state' in c for c in tbr_calls))

    ed_calls = [
        c[0][1]
        for c in test_inst.execute_device_cmd.call_args_list
        if c[0][0] == 'device2'
    ]
    self.assertTrue(any('ot-ctl state' in c for c in ed_calls))
    self.assertTrue(any('ip -6 addr show dev wpan0' in c for c in ed_calls))

  def test_isolate_eth0_and_start_avahi_tbr_and_end_device(self):
    from unittest.mock import patch
    import AndroidBleThreadMobileDeviceTest as abt_mod

    test_inst = abt_mod.TestAndroidBleThreadMobileDevice()
    test_inst.execute_device_cmd = MagicMock(return_value='')

    with patch(
        'AndroidBleThreadMobileDeviceTest.wait_for_device_wlan0_ipv4',
        return_value='10.0.1.2',
    ):
      test_inst._isolate_eth0_and_start_avahi(['device1', 'device2'])

    d1_cmds = [
        c[0][1]
        for c in test_inst.execute_device_cmd.call_args_list
        if c[0][0] == 'device1'
    ]
    self.assertTrue(any('wlan0' in c for c in d1_cmds))
    self.assertTrue(any('enable-reflector=yes' in c for c in d1_cmds))

    d2_cmds = [
        c[0][1]
        for c in test_inst.execute_device_cmd.call_args_list
        if c[0][0] == 'device2'
    ]
    self.assertTrue(any('ip link add mdns0 type dummy' in c for c in d2_cmds))
    self.assertTrue(any('allow-interfaces=mdns0' in c for c in d2_cmds))

  def test_start_ble_thread_end_devices_tees_chip_all_clusters_log(self):
    import AndroidBleThreadMobileDeviceTest as abt_mod

    test_inst = abt_mod.TestAndroidBleThreadMobileDevice()
    test_inst.execute_device_cmd = MagicMock(return_value='')
    test_inst.get_device_pretty_id = MagicMock(return_value='CHIPEndDevice')

    test_inst._start_ble_thread_end_devices(['device2'])

    d2_cmds = [
        c[0][1]
        for c in test_inst.execute_device_cmd.call_args_list
        if c[0][0] == 'device2'
    ]
    self.assertTrue(any(': > /tmp/chip-all-clusters.log' in c for c in d2_cmds))
    launch_cmds = [
        c for c in d2_cmds if 'CHIPCirqueDaemon.py -- run sh -c' in c
    ]
    self.assertEqual(len(launch_cmds), 1)
    launch_cmd = launch_cmds[0]
    self.assertIn('2>&1 | tee -a /tmp/chip-all-clusters.log', launch_cmd)
    self.assertIn('--thread', launch_cmd)
    self.assertIn('--discriminator 3840', launch_cmd)

  def test_tbr_border_proxy_helper_removes_probe_end_device_mdns(self):
    import AndroidBleThreadMobileDeviceTest as abt_mod

    helper_code = abt_mod.TBR_BORDER_PROXY_HELPER
    self.assertNotIn('def probe_end_device_mdns', helper_code)
    self.assertNotIn('probe_end_device_mdns(', helper_code)
    self.assertIn('now - last_periodic >= 1.0', helper_code)
    self.assertIn('announce_all_instances()', helper_code)
    self.assertIn('REGISTER', helper_code)
    self.assertIn('5541', helper_code)

  def test_thread_end_device_joiner_helper_extracts_operational_instance(self):
    import re
    import AndroidBleThreadMobileDeviceTest as abt_mod

    helper_code = abt_mod.THREAD_END_DEVICE_JOINER_HELPER
    self.assertIn('/tmp/chip-all-clusters.log', helper_code)
    self.assertNotIn('/var/log/syslog', helper_code)
    self.assertNotIn('glob.glob("/tmp/*.log")', helper_code)
    self.assertNotIn('import glob', helper_code)

    op_pat = re.search(
        r'op_pat = re\.compile\(\s*(?:r"[^"]+"\s*)+\)', helper_code
    )
    self.assertIsNotNone(op_pat)
    raw_pat = (
        r'(?:Advertise operational node|instance name:)\s*'
        r'([0-9A-Fa-f]{16}-[0-9A-Fa-f]{16})'
    )
    compiled_pat = re.compile(raw_pat)

    sample_log = (
        '[1730000000.123] [1:1] [DIS] Advertise commission parameter'
        ' vendorID=0xFFF1 productID=0x8000 discriminator=3840\n'
        '[1730000005.456] [1:1] [DIS] CHIP minimal mDNS configured as'
        " 'Commissionable node'; instance name: 96D4D1525EDFF799\n"
        '[1730000010.789] [1:1] [DIS] Advertise operational node'
        ' 6214DC2C77097B8D-0000000000000001\n'
        '[1730000010.790] [1:1] [DIS] CHIP minimal mDNS configured as'
        " 'Operational device'; instance name:"
        ' 6214DC2C77097B8D-0000000000000001\n'
    )
    matches = compiled_pat.findall(sample_log)
    self.assertEqual(len(matches), 2)
    self.assertEqual(matches[-1].upper(), '6214DC2C77097B8D-0000000000000001')

    # Commissionable only log produces zero operational matches
    comm_only_log = (
        '[1730000000.123] [1:1] [DIS] Advertise commission parameter'
        ' vendorID=0xFFF1 productID=0x8000 discriminator=3840\n'
        '[1730000005.456] [1:1] [DIS] CHIP minimal mDNS configured as'
        " 'Commissionable node'; instance name: 96D4D1525EDFF799\n"
    )
    self.assertEqual(compiled_pat.findall(comm_only_log), [])

  def test_thread_end_device_joiner_helper_probes_dummy_mdns0_targets(self):
    import AndroidBleThreadMobileDeviceTest as abt_mod

    helper_code = abt_mod.THREAD_END_DEVICE_JOINER_HELPER
    self.assertIn('"169.254.169.254"', helper_code)
    self.assertIn('"fd11:33::fe"', helper_code)
    self.assertNotIn('"::1"', helper_code)
    self.assertNotIn('"127.0.0.1"', helper_code)
    self.assertIn('found_logged', helper_code)
    self.assertIn('sent_logged', helper_code)

  def test_wifi_fallback_commissioning_runs_rest_toggle_and_verified_ui_coords(
      self,
  ):
    from unittest.mock import patch
    import AndroidBleWiFiMobileDeviceTest as abw_mod

    test_inst = abw_mod.TestAndroidBleWiFiMobileDevice()

    def fake_cmd(dev_id, cmd):
      if 'logcat -d' in cmd and 'onCommissioningComplete' in cmd:
        return {
            'output': (
                'I CTL: Commissioning complete for node ID 0x01: success\n'
                'D OnOffClientFragment: Toggle command success\n'
                'D OnOffClientFragment: On/Off attribute value: true\n'
            )
        }
      return {'output': ''}

    test_inst.execute_device_cmd = MagicMock(side_effect=fake_cmd)

    def fake_query_api(endpoint, *args, **kwargs):
      if endpoint == 'commission_chiptool':
        return None
      return {'status': 'failed'}

    test_inst.query_api = MagicMock(side_effect=fake_query_api)

    with (
        patch.object(abw_mod.time, 'sleep', return_value=None),
        patch(
            'AndroidBleWiFiMobileDeviceTest.wait_for_device_wlan0_ipv4',
            return_value='10.0.1.2',
        ) as mock_wait_v4,
        patch(
            'AndroidBleWiFiMobileDeviceTest.get_device_wlan0_ipv6',
            return_value=['fe80::1'],
        ),
    ):
      comm_ok, toggle_ok, read_ok = (
          test_inst._commission_and_interact_chiptool(
              'device0', server_ids=['device2']
          )
      )

    self.assertTrue(comm_ok)
    self.assertTrue(toggle_ok)
    self.assertTrue(read_ok)
    mock_wait_v4.assert_called_once()
    api_endpoints = [
        c[0][0] for c in test_inst.query_api.call_args_list
    ]
    self.assertIn('toggle_chiptool', api_endpoints)
    self.assertIn('read_chiptool', api_endpoints)

    executed_cmds = [
        c[0][1] for c in test_inst.execute_device_cmd.call_args_list
    ]
    self.assertTrue(any('input tap 330 947' in c for c in executed_cmds))
    self.assertTrue(any('input tap 537 730' in c for c in executed_cmds))
    self.assertTrue(any('input tap 166 884' in c for c in executed_cmds))
    self.assertFalse(any('input tap 350 1100' in c for c in executed_cmds))

  def test_cirque_ci_acceleration_wheel_paths_and_install_guard(self):
    from helper import paths as cirque_paths

    self.assertIn('matter_clusters', cirque_paths.MATTER_CONTROLLER_WHEELS)
    self.assertIn('matter_core', cirque_paths.MATTER_CONTROLLER_WHEELS)
    self.assertNotIn('matter_repl', cirque_paths.MATTER_CONTROLLER_WHEELS)

    install_cmd = cirque_paths.MATTER_CONTROLLER_INSTALL_WHEELS
    self.assertIn('import matter.ChipDeviceCtrl, matter.clusters', install_cmd)
    self.assertIn('--break-system-packages', install_cmd)
    self.assertIn('--no-cache-dir', install_cmd)
    self.assertNotIn('matter_repl', install_cmd)

  def test_cirque_ci_acceleration_gn_gen_cirque_targets_and_ccache(self):
    gn_script = os.path.join(CHIP_ROOT, 'scripts', 'build', 'gn_gen_cirque.sh')
    with open(gn_script, 'r', encoding='utf-8') as f:
      content = f.read()

    self.assertIn('enable_standalone_chip_tool_build=false', content)
    self.assertIn('pw_command_launcher=\\"ccache\\"', content)
    self.assertIn('linux_x64_gcc/chip-echo-requester', content)
    self.assertIn('linux_x64_gcc/chip-echo-responder', content)
    self.assertIn('linux_x64_gcc/chip-im-initiator', content)
    self.assertIn('linux_x64_gcc/chip-im-responder', content)
    self.assertIn(
        'linux_x64_gcc/gen/src/controller/python/'
        'matter-controller-wheels.pw_pystamp',
        content,
    )
    self.assertIn('linux_lit_icd_app', content)
    self.assertNotIn('time ninja -C out/debug all check', content)

  def test_cirque_ci_acceleration_cirque_tests_prewarm_and_docker_stop(self):
    cirque_tests_script = os.path.join(
        CHIP_ROOT, 'scripts', 'tests', 'cirque_tests.sh'
    )
    with open(cirque_tests_script, 'r', encoding='utf-8') as f:
      content = f.read()

    self.assertIn('prewarm_cirque_device_base_wheels', content)
    self.assertIn('--entrypoint /bin/bash', content)
    self.assertIn('timeout 180s docker run', content)
    self.assertIn(
        "--change 'ENTRYPOINT [\"/opt/entrypoint.sh\"]' --change 'CMD []'",
        content,
    )
    self.assertNotIn('docker wait', content)
    self.assertIn('docker stop -t 0', content)
    self.assertIn('http://127.0.0.1:5000/get_homes', content)

  def test_cirque_ci_acceleration_subscription_resumption_polling(self):
    test_file = os.path.join(
        SCRIPT_DIR, 'SubscriptionResumptionTimeoutTest.py'
    )
    with open(test_file, 'r', encoding='utf-8') as f:
      content = f.read()

    self.assertIn('Retries: 1', content)
    self.assertIn('Retries: 2', content)
    self.assertIn('resumption_matched', content)
    self.assertNotIn('time.sleep(120)', content)


if __name__ == '__main__':
  unittest.main()

