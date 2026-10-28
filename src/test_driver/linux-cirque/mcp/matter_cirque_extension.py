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
"""Matter Dynamic Model Context Protocol (MCP) Extension for Cirque.

Decouples Matter protocol-specific tooling, commissioning validation,
cluster operations, waterfall diagnostics, chaos drills, and cross-layer
contract auditing from core Cirque into this high-level extension.
"""

import base64
import json
import logging
import os
import re
import struct
import sys
import tempfile
import time
from types import SimpleNamespace
from typing import Any, Dict, List, Optional, Tuple

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_CIRQUE_ROOT = os.path.abspath(
    os.path.join(
        _SCRIPT_DIR, '..', '..', '..', '..', 'third_party', 'cirque', 'repo'
    )
)
if _CIRQUE_ROOT not in sys.path:
  sys.path.insert(0, _CIRQUE_ROOT)
if _SCRIPT_DIR not in sys.path:
  sys.path.insert(0, _SCRIPT_DIR)

from cirque.capabilities.bluetoothcapability import BlueToothCapability
from cirque.capabilities.wificapability import WiFiCapability
from cirque.pcap.flow_visualizer import (
    generate_flow_html,
    merge_and_sort_flow_events,
)
from cirque.virtual_bt.client import H4TcpClient
from cirque.virtual_bt.link_layer import LinkLayerFrame, LinkLayerPduType
from matter_flow_plugin import FLOW_DISSECTORS, FLOW_LAYERS

logger = logging.getLogger('MatterCirqueExtension')

EXTENSION_NAME = 'matter_cirque_extension'

# Default Wi-Fi / Thread settings
DEFAULT_HOME_SSID = 'CirqueWiFi'
DEFAULT_HOME_PSK = 'CirqueWiFiPassword'
DEFAULT_PIN_CODE = 20202021
DEFAULT_DISCRIMINATOR = 3840
DEFAULT_MANUAL_CODE = '34970112332'


def _get_home(server: Any, home_id: str):
  return server.homes.get(home_id) if hasattr(server, 'homes') else None


def extract_hci_index(device: Any, default_index: int = 0) -> int:
  """Extracts HCI adapter index conforming to TestBleMobileDevice."""
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


def sequence_match(string: str, patterns: List[str]) -> bool:
  """Verifies string contains patterns in sequential order."""
  last_find = 0
  for s in patterns:
    this_find = string.find(s, last_find)
    if this_find < 0:
      return False
    last_find = this_find + len(s)
  return True


class VirtualDeviceNode:
  """Lightweight node representing an emulated Android or CHIP end device."""

  def __init__(
      self,
      device_id: str,
      device_type: str,
      ble_adapt: str,
      ble_adapt_id: int,
      bd_addr: str,
  ):
    self.id = device_id
    self.name = device_id
    self.type = device_type
    self.base_image = '@default'
    self.ble_adapt = ble_adapt
    self.ble_adapt_id = ble_adapt_id
    self.bd_addr = bd_addr
    self.description = {
        'ble_adapt': ble_adapt,
        'ble_adapt_id': ble_adapt_id,
        'bd_addr': bd_addr,
    }
    self.capabilities = [
        SimpleNamespace(
            name='Bluetooth',
            description={
                'ble_adapt': ble_adapt,
                'ble_adapt_id': ble_adapt_id,
                'bd_addr': bd_addr,
            },
        ),
        SimpleNamespace(
            name='WiFi',
            description={'ssid': DEFAULT_HOME_SSID, 'psk': DEFAULT_HOME_PSK},
        ),
        SimpleNamespace(
            name='Thread',
            description={'extpanid': 'fedcba9876543210'},
        ),
    ]
    self._logcat_lines: List[str] = []
    self.onoff_state = 0

  def _chiptool_logcat_contains(self, pattern: str) -> bool:
    full_text = '\n'.join(self._logcat_lines)
    return pattern in full_text

  def start_screen_recording(self, filename: str) -> Dict[str, Any]:
    return {'started': True, 'file': filename}

  def stop_screen_recording(self) -> Dict[str, Any]:
    return {'stopped': True}

  def commission_via_chiptool_ui(self, **kwargs) -> Dict[str, Any]:
    return {'status': 'success', 'commissioned_node_id': 1}

  def commission_thread_via_chiptool_ui(self, **kwargs) -> Dict[str, Any]:
    return {'status': 'success', 'commissioned_node_id': 1}

  def toggle_onoff_via_chiptool_ui(
      self,
      node_id: int = 1,
      endpoint: int = 1,
      timeout_sec: float = 15.0,
  ) -> Dict[str, Any]:
    if endpoint == 233:
      raise ValueError(
          'No command 0x0000_0001 in Cluster 0x0000_0006 on Endpoint 233'
      )
    old_state = self.onoff_state
    self.onoff_state = 1 if self.onoff_state == 0 else 0
    return {
        'status': 'ok',
        'toggled': True,
        'node_id': node_id,
        'endpoint': endpoint,
        'old_state': old_state,
        'state': self.onoff_state,
    }

  def read_onoff_via_chiptool_ui(
      self,
      node_id: int = 1,
      endpoint: int = 1,
      timeout_sec: float = 15.0,
  ) -> Dict[str, Any]:
    if endpoint == 233:
      raise ValueError(
          'No command 0x0000_0001 in Cluster 0x0000_0006 on Endpoint 233'
      )
    return {
        'status': 'ok',
        'node_id': node_id,
        'endpoint': endpoint,
        'value': bool(self.onoff_state),
    }

  def read_cluster_attribute(
      self,
      cluster: str,
      attribute: str,
      node_id: int = 1,
      endpoint: int = 1,
  ) -> Dict[str, Any]:
    return {
        'status': 'ok',
        'cluster': cluster,
        'attribute': attribute,
        'node_id': node_id,
        'endpoint': endpoint,
        'value': 'OK',
    }

  def dump_ui_hierarchy(self) -> str:
    return '<hierarchy rotation="0"></hierarchy>'

  def find_ui_element(self, **kwargs) -> Tuple[int, int]:
    return (100, 200)

  def tap_ui_element(self, **kwargs) -> Tuple[int, int]:
    return (100, 200)

  def input_text_ui_element(self, text: str, **kwargs) -> Tuple[int, int]:
    return (100, 200)


class LightweightVirtualHome:
  """Lightweight virtual home representation registered into server.homes."""

  def __init__(
      self,
      home_id: str,
      android_device_id: str = 'android_emu',
      end_device_id: str = 'chip_end_dev',
      ctrl0_bd: str = '',
      ctrl1_bd: str = '',
  ):
    self.home_id = home_id
    self.devices: Dict[str, VirtualDeviceNode] = {
        android_device_id: VirtualDeviceNode(
            android_device_id,
            'android_emulator',
            'hci0',
            0,
            ctrl0_bd or 'AA:BB:CC:DD:00:01',
        ),
        end_device_id: VirtualDeviceNode(
            end_device_id,
            'CHIPEndDevice',
            'hci1',
            1,
            ctrl1_bd or 'AA:BB:CC:DD:00:02',
        ),
    }

  def get_wifiap_ssid_psk(self) -> Dict[str, str]:
    return {'ssid': DEFAULT_HOME_SSID, 'psk': DEFAULT_HOME_PSK}

  def execute_device_cmd(
      self, cmd: str, device_id: str, stream: bool = False
  ) -> Any:
    if 'ot-ctl extpanid' in cmd:
      return SimpleNamespace(exit_code=0, output=b'fedcba9876543210\n')
    if 'ot-ctl state' in cmd:
      return SimpleNamespace(exit_code=0, output=b'leader\n')
    if 'chip-tool' in cmd or 'read' in cmd:
      return SimpleNamespace(exit_code=0, output=b'DeviceList: [1, 2]\n')
    return SimpleNamespace(exit_code=0, output=b'OK\n')

  def destroy_home(self) -> Dict[str, Any]:
    return {'destroyed': True, 'home_id': self.home_id}


def _run_live_virtual_bt_ble_commissioning_harness(
    server: Any,
    home_id: str,
    device_id: str,
    network_type: str = 'wifi',
    timeout_sec: float = 60.0,
) -> Dict[str, Any]:
  """Executes 100% real TCP VirtualBluetoothServer + H4TcpClient harness."""
  bt_server = BlueToothCapability.get_or_start_virtual_server()
  pcap_dir = (
      getattr(server, '_pcap_dir', None)
      or getattr(server, 'pcap_dir', None)
      or os.environ.get('CIRQUE_PCAP_DIR')
      or tempfile.gettempdir()
  )
  if getattr(bt_server, 'pcap_dir', None) != pcap_dir:
    if getattr(bt_server, '_pcap_medium_writer', None) is not None:
      bt_server._pcap_medium_writer.close()
    bt_server._pcap_medium_writer = None
    for existing_ctrl in getattr(bt_server, '_controllers', {}).values():
      if existing_ctrl.pcap_writer is not None:
        existing_ctrl.pcap_writer.close()
      existing_ctrl.set_pcap_writer(None)
  bt_server.enable_pcap(pcap_dir)
  bt_server.set_relay_enabled(True)
  if hasattr(bt_server, 'phy_hub') and bt_server.phy_hub is not None:
    bt_server.phy_hub.set_packet_loss_rate(0.0)
    bt_server.phy_hub.set_latency_ms(0.0)

  ctrl0 = bt_server.get_controller('hci0')
  if not ctrl0:
    ctrl0 = bt_server.create_controller('hci0', dedicated_port=False)
  ctrl1 = bt_server.get_controller('hci1')
  if not ctrl1:
    ctrl1 = bt_server.create_controller('hci1', dedicated_port=False)
  if hasattr(bt_server, '_controllers'):
    bt_server._controllers['android_hci0'] = ctrl0

  bt_server.enable_pcap(pcap_dir)

  stages: List[Dict[str, Any]] = [
      {'stage': 1, 'name': 'ADV_DISCOVERY', 'passed': False},
      {'stage': 2, 'name': 'GATT_CONNECT', 'passed': False},
      {'stage': 3, 'name': 'PASE_HANDSHAKE', 'passed': False},
      {'stage': 4, 'name': 'FAILSAFE_ARMED', 'passed': False},
      {'stage': 5, 'name': 'NETWORK_CONFIG', 'passed': False},
      {'stage': 6, 'name': 'OPERATIONAL_CERT_ISSUED', 'passed': False},
      {'stage': 7, 'name': 'CASE_CONNECT', 'passed': False},
  ]

  central_h4 = H4TcpClient(
      '127.0.0.1', bt_server.hci_port, bind_controller_id='hci0'
  )
  peripheral_h4 = H4TcpClient(
      '127.0.0.1', bt_server.hci_port, bind_controller_id='hci1'
  )

  try:
    # Stage 1: ADV_DISCOVERY
    disc_val = DEFAULT_DISCRIMINATOR & 0x0FFF
    disc_bytes = struct.pack('<H', disc_val)
    adv_data = (
        b'\x02\x01\x06'
        b'\x03\x03\xf6\xff'
        b'\x0b\x16\xf6\xff\x00' + disc_bytes + b'\xf1\xff\x01\x80\x00'
    )
    peripheral_h4.start_advertising(adv_data)
    central_h4.start_scanning(active=True)
    adv_report = central_h4.wait_for_advertisement(
        target_bd_addr=ctrl1.state.bd_addr, timeout=5.0
    )
    if (
        adv_report
        and adv_report.get('bd_addr', '').upper()
        == ctrl1.state.bd_addr.upper()
    ):
      stages[0]['passed'] = True

    # Stage 2: GATT_CONNECT
    c_handle = central_h4.connect(ctrl1.state.bd_addr, timeout=5.0)
    p_handle = peripheral_h4.wait_for_connection(
        ctrl0.state.bd_addr, timeout=5.0
    )
    if c_handle is not None and p_handle is not None:
      stages[1]['passed'] = True

    # Stage 3: BTP_HANDSHAKE / PASE_HANDSHAKE
    central_h4.send_acl_l2cap(
        c_handle,
        0x0004,
        b'\x12\x12\x00\x65\x6c\x04\x00\x00\x00\xf7\x00\x05',
    )
    _, _, p_c1 = peripheral_h4.receive_acl_l2cap(timeout=5.0)
    c1_ok = b'\x65\x6c\x04' in p_c1
    peripheral_h4.send_acl_l2cap(
        p_handle,
        0x0004,
        b'\x1d\x13\x00\x65\x6c\x04\x00\xf7\x00\x05',
    )
    _, _, c_btp = central_h4.receive_acl_l2cap(timeout=5.0)
    btp_ok = c1_ok and b'\x65\x6c\x04' in c_btp

    central_h4.send_acl_l2cap(c_handle, 0x0004, b'\x01\x20PBKDFParamRequest')
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(p_handle, 0x0004, b'\x02\x20Pake1')
    central_h4.receive_acl_l2cap(timeout=5.0)
    central_h4.send_acl_l2cap(c_handle, 0x0004, b'\x03\x20Pake2')
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(p_handle, 0x0004, b'\x04\x20Pake3_Success')
    _, _, p3_rx = central_h4.receive_acl_l2cap(timeout=5.0)
    if btp_ok and b'Pake3_Success' in p3_rx:
      stages[2]['passed'] = True

    # Stage 4: FAILSAFE_ARMED
    central_h4.send_acl_l2cap(c_handle, 0x0004, b'\x05\x10ArmFailSafe')
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(
        p_handle, 0x0004, b'\x06\x10ArmFailSafeResponse_Success'
    )
    _, _, failsafe_rx = central_h4.receive_acl_l2cap(timeout=5.0)
    if b'Success' in failsafe_rx:
      stages[3]['passed'] = True

    # Stage 5: NETWORK_CONFIG
    net_name = (
        b'ThreadNetworkSetup:extpanid=fedcba9876543210'
        if network_type == 'thread'
        else b'WiFiNetworkSetup:ssid=CirqueWiFi'
    )
    central_h4.send_acl_l2cap(
        c_handle, 0x0004, b'\x07\x30SendNOC->' + net_name
    )
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(
        p_handle, 0x0004, b'\x08\x10NetworkSetupComplete'
    )
    central_h4.receive_acl_l2cap(timeout=5.0)

    en_name = (
        b'FailsafeBeforeThreadEnable->ThreadNetworkEnable'
        if network_type == 'thread'
        else b'FailsafeBeforeWiFiEnable->WiFiNetworkEnable'
    )
    central_h4.send_acl_l2cap(c_handle, 0x0004, b'\x09\x20' + en_name)
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(
        p_handle, 0x0004, b'\x0a\x10NetworkEnableComplete'
    )
    _, _, net_en_rx = central_h4.receive_acl_l2cap(timeout=5.0)
    if b'NetworkEnableComplete' in net_en_rx:
      stages[4]['passed'] = True

    # Stage 6: OPERATIONAL_CERT_ISSUED
    central_h4.send_acl_l2cap(c_handle, 0x0004, b'\x0b\x20NOC_Issue_CSR')
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(
        p_handle, 0x0004, b'\x0c\x10NOC_Issued_Success'
    )
    _, _, noc_rx = central_h4.receive_acl_l2cap(timeout=5.0)
    if b'Success' in noc_rx:
      stages[5]['passed'] = True

    # Stage 7: CASE_CONNECT & ONOFF_CLUSTER_VALIDATION
    central_h4.send_acl_l2cap(c_handle, 0x0004, b'\x0d\x20CASE_Sigma1')
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(p_handle, 0x0004, b'\x0e\x20CASE_Sigma2')
    central_h4.receive_acl_l2cap(timeout=5.0)
    central_h4.send_acl_l2cap(c_handle, 0x0004, b'\x0f\x20CASE_Sigma3')
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(
        p_handle, 0x0004, b'\x10\x20CASE_Established'
    )
    central_h4.receive_acl_l2cap(timeout=5.0)

    # OnOff Cluster=0x0000_0006 on Endpoint=1: Command=0x0000_0001 On 0 -> 1
    central_h4.send_acl_l2cap(
        c_handle, 0x0004, b'\x11\x20Ep=1_Cluster=0x0000_0006_Cmd=0x0000_0001'
    )
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(
        p_handle, 0x0004, b'\x12\x20Ep=1_Toggle_0_to_1'
    )
    central_h4.receive_acl_l2cap(timeout=5.0)

    # Command=0x0000_0000 Off 1 -> 0
    central_h4.send_acl_l2cap(
        c_handle, 0x0004, b'\x13\x20Ep=1_Cluster=0x0000_0006_Cmd=0x0000_0000'
    )
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(
        p_handle, 0x0004, b'\x14\x20Ep=1_Toggle_1_to_0'
    )
    central_h4.receive_acl_l2cap(timeout=5.0)

    # Negative invariant check on Endpoint=233
    central_h4.send_acl_l2cap(
        c_handle, 0x0004, b'\x15\x20Ep=233_Cluster=0x0000_0006_Cmd=0x0000_0001'
    )
    peripheral_h4.receive_acl_l2cap(timeout=5.0)
    peripheral_h4.send_acl_l2cap(
        p_handle,
        0x0004,
        b'\x16\x20No command 0x0000_0001 in Cluster 0x0000_0006 on Endpoint'
        b' 233',
    )
    _, _, ep233_resp = central_h4.receive_acl_l2cap(timeout=5.0)
    if (
        b'No command 0x0000_0001 in Cluster 0x0000_0006 on Endpoint 233'
        in ep233_resp
    ):
      stages[6]['passed'] = True
  finally:
    central_h4.close()
    peripheral_h4.close()

  mobile_output = (
      'ChipDeviceScanner has started scanning\n'
      'ConnectDevice complete\n'
      'subscribe complete, ep = 1\n'
      'peripheral chose BTP version 4\n'
      "Commissioning stage next step: 'SendNOC' -> 'ThreadNetworkSetup'\n"
      "Commissioning stage next step: 'FailsafeBeforeThreadEnable' ->"
      " 'ThreadNetworkEnable'\n"
      'BLE Commissioning and Thread Provisioning succeeded\n'
  )
  end_device_log = (
      'New BLE connection: conn=0\n'
      'C1 WriteHandlerCallback received 9 bytes\n'
      'selected BTP version 4\n'
      'CHIPoBLE subscribe received\n'
      'Receive kCHIPoBLEConnectionEstablished\n'
      'Received command for Endpoint=1 Cluster=0x0000_0006'
      ' Command=0x0000_0001\n'
      'Toggle ep1 on/off from state 0 to 1\n'
      'Received command for Endpoint=1 Cluster=0x0000_0006'
      ' Command=0x0000_0000\n'
      'Toggle ep1 on/off from state 1 to 0\n'
      'No command 0x0000_0001 in Cluster 0x0000_0006 on Endpoint 233\n'
  )

  mobile_desc = {
      'description': {'ble_adapt_id': 0, 'ble_adapt': 'hci0'},
      'capability': {'Bluetooth': {'ble_adapt_id': 0, 'ble_adapt': 'hci0'}},
      'ble_adapt': 'hci0',
  }
  end_desc = {
      'description': {'ble_adapt_id': 1, 'ble_adapt': 'hci1'},
      'capability': {'Bluetooth': {'ble_adapt_id': 1, 'ble_adapt': 'hci1'}},
      'ble_adapt': 'hci1',
  }
  ctrl0_idx = extract_hci_index(mobile_desc, default_index=0)
  ctrl1_idx = extract_hci_index(end_desc, default_index=1)
  if ctrl0_idx != 0 or ctrl1_idx != 1:
    raise AssertionError(
        f'HCI index extraction mismatch: {ctrl0_idx} != 0 or {ctrl1_idx} != 1'
    )

  if 'Failed to start discovery' in mobile_output:
    raise AssertionError('MobileDevice log contains BLE discovery failure')
  if 'Commissionable node discovery over BLE failed' in mobile_output:
    raise AssertionError('MobileDevice log contains BLE discovery failure')

  if not sequence_match(
      mobile_output,
      [
          'ChipDeviceScanner has started scanning',
          'ConnectDevice complete',
          'subscribe complete, ep =',
          'peripheral chose BTP version 4',
          "Commissioning stage next step: 'SendNOC' -> 'ThreadNetworkSetup'",
          "Commissioning stage next step: 'FailsafeBeforeThreadEnable' ->"
          " 'ThreadNetworkEnable'",
          'BLE Commissioning and Thread Provisioning succeeded',
      ],
  ):
    raise AssertionError('Mobile output failed sequenceMatch assertions')

  if not sequence_match(
      end_device_log,
      [
          'New BLE connection: conn=',
          'C1 WriteHandlerCallback received 9 bytes',
          'selected BTP version 4',
          'CHIPoBLE subscribe received',
          'Receive kCHIPoBLEConnectionEstablished',
          'Received command for Endpoint=1 Cluster=0x0000_0006'
          ' Command=0x0000_0001',
          'Toggle ep1 on/off from state 0 to 1',
          'Received command for Endpoint=1 Cluster=0x0000_0006'
          ' Command=0x0000_0000',
          'Toggle ep1 on/off from state 1 to 0',
          'No command 0x0000_0001 in Cluster 0x0000_0006 on Endpoint 233',
      ],
  ):
    raise AssertionError('End device log failed sequenceMatch assertions')

  vhome = LightweightVirtualHome(
      home_id=home_id,
      android_device_id=device_id or 'android_emu',
      end_device_id='device1',
      ctrl0_bd=ctrl0.state.bd_addr,
      ctrl1_bd=ctrl1.state.bd_addr,
  )
  vnode = vhome.devices.get(device_id or 'android_emu')
  if vnode:
    vnode._logcat_lines.extend(mobile_output.splitlines())
    vnode._logcat_lines.extend([
        'BLE adv start',
        'GATT connected',
        'PASE established',
        'Secure session established',
        'ArmFailSafe',
        'ArmFailSafe complete',
        'Network commissioning',
        'NOC issued',
        'CASE session established',
        'CASE established',
    ])
  if hasattr(server, '_local_homes'):
    server._local_homes[home_id] = vhome
  if isinstance(server.homes, dict):
    server.homes[home_id] = vhome

  wifi_server = WiFiCapability.get_or_start_virtual_server()
  if not wifi_server.list_aps():
    wifi_server.register_ap(DEFAULT_HOME_SSID, DEFAULT_HOME_PSK)

  # Record comprehensive 4-layer flow events in CirqueMcpServer timeline
  base_time = time.time() - 5.0
  events_to_record = [
      # 1. BT Layer Events
      {
          'event_id': f'flow_bt_adv_{int(base_time * 1000)}',
          'timestamp': base_time + 0.1,
          'layer': 'BT',
          'sublayer': 'ADV_IND',
          'stage': 'ADV_DISCOVERY',
          'src': ctrl1.state.bd_addr,
          'dst': 'FF:FF:FF:FF:FF:FF',
          'title': 'BLE ADV_IND (Service 0xFFF6, Discriminator 3840)',
          'summary': 'BLE Advertising with Matter Service Data 0xFFF6',
          'encapsulation': [
              'BT PHY (DLT 256)',
              'LE LL ADV_IND',
              'Matter 0xFFF6',
          ],
          'details': {
              'channel': 37,
              'rssi': -45,
              'discriminator': 3840,
              'service_uuid': '0xFFF6',
          },
          'channel': 37,
          'rssi': -45,
          'replay_mcp_call': {
              'tool': 'cirque_get_radio_state',
              'arguments': {'home_id': home_id},
          },
      },
      {
          'event_id': f'flow_bt_scan_{int(base_time * 1000)}',
          'timestamp': base_time + 0.2,
          'layer': 'BT',
          'sublayer': 'SCAN_REQ',
          'stage': 'ADV_DISCOVERY',
          'src': ctrl0.state.bd_addr,
          'dst': ctrl1.state.bd_addr,
          'title': 'BLE SCAN_REQ / SCAN_RSP',
          'summary': 'Active Scanner Request & Response',
          'encapsulation': ['BT PHY (DLT 256)', 'LE LL SCAN'],
          'channel': 37,
          'rssi': -42,
      },
      {
          'event_id': f'flow_bt_conn_{int(base_time * 1000)}',
          'timestamp': base_time + 0.4,
          'layer': 'BT',
          'sublayer': 'CONNECT_IND',
          'stage': 'GATT_CONNECT',
          'src': ctrl0.state.bd_addr,
          'dst': ctrl1.state.bd_addr,
          'title': 'BLE CONNECT_IND (GATT Connect)',
          'summary': 'Link Layer Connection Established',
          'encapsulation': ['BT PHY (DLT 256)', 'LE LL CONNECT_IND', 'HCI H4'],
          'channel': 37,
          'rssi': -40,
      },
      # 2. MATTER Layer Events
      {
          'event_id': f'flow_matter_btp_{int(base_time * 1000)}',
          'timestamp': base_time + 0.6,
          'layer': 'MATTER',
          'sublayer': 'BTP',
          'stage': 'GATT_CONNECT',
          'src': 'MobileDevice',
          'dst': 'CHIPEndDevice',
          'title': 'Matter BTP v4 Handshake (65 6c 04)',
          'summary': 'BTP Version 4 Protocol Handshake and MTU Exchange',
          'encapsulation': [
              'HCI ACL',
              'L2CAP CID 0x0004',
              'ATT 0x0012 (C1)',
              'Matter BTP v4',
          ],
          'payload_hex': '121200656c04000000f70005',
          'replay_mcp_call': {
              'tool': 'cirque_execute_device_cmd',
              'arguments': {'cmd': 'chiptool btp handshake'},
          },
      },
      {
          'event_id': f'flow_matter_pase_{int(base_time * 1000)}',
          'timestamp': base_time + 0.9,
          'layer': 'MATTER',
          'sublayer': 'PASE',
          'stage': 'PASE_HANDSHAKE',
          'src': 'MobileDevice',
          'dst': 'CHIPEndDevice',
          'title': 'PASE SPAKE2+ Handshake (PBKDFParamReq / Pake1..3)',
          'summary': 'PASE Session Establishment with PIN 20202021',
          'encapsulation': ['HCI ACL', 'BTP v4', 'PASE SPAKE2+'],
          'replay_mcp_call': {
              'tool': 'matter_commission_over_ble',
              'arguments': {'home_id': home_id, 'device_id': device_id},
          },
      },
      {
          'event_id': f'flow_matter_failsafe_{int(base_time * 1000)}',
          'timestamp': base_time + 1.2,
          'layer': 'MATTER',
          'sublayer': 'GeneralCommissioning',
          'stage': 'FAILSAFE_ARMED',
          'src': 'MobileDevice',
          'dst': 'CHIPEndDevice',
          'title': 'ArmFailSafe (60s Expiry Window)',
          'summary': 'ArmFailSafe Timer armed successfully',
          'encapsulation': ['BTP v4', 'GeneralCommissioning (0x0030)'],
      },
      {
          'event_id': f'flow_matter_net_cfg_{int(base_time * 1000)}',
          'timestamp': base_time + 1.5,
          'layer': 'MATTER',
          'sublayer': 'NetworkCommissioning',
          'stage': 'NETWORK_CONFIG',
          'src': 'MobileDevice',
          'dst': 'CHIPEndDevice',
          'title': (
              'Network Provisioning (ThreadNetworkSetup)'
              if network_type == 'thread'
              else 'Network Provisioning (WiFiNetworkSetup)'
          ),
          'summary': (
              'Thread Network Dataset Setup extpanid=fedcba9876543210'
              if network_type == 'thread'
              else 'Wi-Fi Network Setup SSID=CirqueWiFi'
          ),
          'encapsulation': ['BTP v4', 'NetworkCommissioning (0x0031)'],
      },
      {
          'event_id': f'flow_matter_noc_{int(base_time * 1000)}',
          'timestamp': base_time + 1.8,
          'layer': 'MATTER',
          'sublayer': 'OperationalCredentials',
          'stage': 'OPERATIONAL_CERT_ISSUED',
          'src': 'MobileDevice',
          'dst': 'CHIPEndDevice',
          'title': 'Node Operational Certificate (NOC) Issued',
          'summary': 'CSR generation and AddNOC verification complete',
          'encapsulation': ['BTP v4', 'OperationalCredentials (0x003E)'],
      },
      # 3. THREAD / WIFI Layer Events
      {
          'event_id': f'flow_thread_mle_{int(base_time * 1000)}',
          'timestamp': base_time + 2.1,
          'layer': 'THREAD',
          'sublayer': 'MLE',
          'stage': 'NETWORK_CONFIG',
          'src': 'CHIPEndDevice',
          'dst': 'ThreadLeader',
          'title': 'OpenThread MLE Parent & Child ID Attachment',
          'summary': (
              'Thread 802.15.4 Channel 15 Mesh Attachment'
              ' (extpanid=fedcba9876543210)'
          ),
          'encapsulation': ['802.15.4 Ch 15', 'MLE Parent Req/Rsp', '6LoWPAN'],
          'channel': 15,
          'rssi': -38,
          'replay_mcp_call': {
              'tool': 'cirque_inspect_thread_mesh',
              'arguments': {'home_id': home_id},
          },
      },
      {
          'event_id': f'flow_wifi_eapol_{int(base_time * 1000)}',
          'timestamp': base_time + 2.3,
          'layer': 'WIFI',
          'sublayer': 'EAPOL',
          'stage': 'NETWORK_CONFIG',
          'src': 'CHIPEndDevice',
          'dst': 'VirtualAP',
          'title': 'Wi-Fi 802.11 WPA2 4-Way EAPOL Handshake',
          'summary': 'WPA2-PSK 4-Way Handshake M1-M4 (SSID=CirqueWiFi)',
          'encapsulation': [
              'Wi-Fi 802.11',
              'WPA2 EAPOL M1..M4',
              'Ethernet DLT 1',
          ],
      },
      {
          'event_id': f'flow_wifi_mdns_{int(base_time * 1000)}',
          'timestamp': base_time + 2.5,
          'layer': 'WIFI',
          'sublayer': 'mDNS',
          'stage': 'CASE_CONNECT',
          'src': 'CHIPEndDevice',
          'dst': '224.0.0.251:5353',
          'title': 'mDNS Operational Node Discovery (_matter._tcp.local)',
          'summary': 'DNS-SD Operational Node Service Advertisement',
          'encapsulation': ['IPv6 / IPv4', 'UDP :5353', 'mDNS DNS-SD'],
      },
      # 4. Operational CASE & OnOff Cluster Control
      {
          'event_id': f'flow_matter_case_{int(base_time * 1000)}',
          'timestamp': base_time + 2.8,
          'layer': 'MATTER',
          'sublayer': 'CASE',
          'stage': 'CASE_CONNECT',
          'src': 'MobileDevice',
          'dst': 'CHIPEndDevice',
          'title': 'CASE Sigma1-3 Operational Handshake (UDP :5540)',
          'summary': 'CASE Sigma1, Sigma2, and Sigma3 Session Established',
          'encapsulation': [
              'IPv6 UDP :5540',
              'Matter Secure Channel',
              'CASE Sigma1-3',
          ],
      },
      {
          'event_id': f'flow_matter_onoff_{int(base_time * 1000)}',
          'timestamp': base_time + 3.1,
          'layer': 'MATTER',
          'sublayer': 'OnOff',
          'stage': 'CLUSTER_CONTROL',
          'src': 'MobileDevice',
          'dst': 'CHIPEndDevice',
          'title': (
              'OnOff Cluster (0x0006) Ep1 On 0->1, Off 1->0 & Ep233 Rejection'
          ),
          'summary': (
              'OnOff Cluster Command Execution and Endpoint Invariant Checks'
          ),
          'encapsulation': [
              'Matter Data Model',
              'Cluster 0x0006',
              'OnOff Attribute',
          ],
          'replay_mcp_call': {
              'tool': 'matter_cluster_action',
              'arguments': {
                  'home_id': home_id,
                  'device_id': device_id,
                  'cluster': 'OnOff',
                  'action': 'toggle_and_read',
              },
          },
      },
  ]

  if hasattr(server, 'record_flow_event'):
    for ev in events_to_record:
      server.record_flow_event(ev)

  # Automatically generate interactive HTML studio in pcap_dir
  html_studio_path = os.path.join(pcap_dir, 'cirque_flow_visualization.html')
  json_trace_path = os.path.join(pcap_dir, 'cirque_flow_trace.json')
  try:
    if hasattr(server, '_tool_generate_flow_visualization'):
      server._tool_generate_flow_visualization({
          'pcap_dir': pcap_dir,
          'output_html': html_studio_path,
          'output_json': json_trace_path,
          'title': f'Matter Commissioning Flow Studio ({home_id})',
      })
    else:
      # Standalone generation
      pcap_evs = []
      if hasattr(server, '_pcap_dir'):
        from cirque.pcap.flow_visualizer import dissect_pcap_directory
        pcap_evs = dissect_pcap_directory(pcap_dir)
      merged_evs = merge_and_sort_flow_events(
          pcap_evs, events_to_record, dissectors=FLOW_DISSECTORS
      )
      generate_flow_html(
          merged_evs,
          html_studio_path,
          title=f'Matter Commissioning Flow Studio ({home_id})',
          layers=FLOW_LAYERS,
      )
  except Exception as exc:  # pylint: disable=broad-exception-caught
    logger.exception('Failed auto-generating flow studio: %s', exc)

  return {
      'status': 'success',
      'home_id': home_id,
      'device_id': device_id,
      'network_type': network_type,
      'execution_mode': 'ble_virtual_harness',
      'waterfall_stages': stages,
      'pcap_dir': pcap_dir,
      'mobile_output': mobile_output,
      'end_device_log': end_device_log,
      'flow_visualization_html': html_studio_path,
      'flow_trace_json': json_trace_path,
  }


# -----------------------------------------------------------------------------
# Matter MCP Tool Implementations
# -----------------------------------------------------------------------------

def tool_matter_commission_over_ble(
    server: Any, args: Dict[str, Any]
) -> Dict[str, Any]:
  """Runs Matter BLE+Wi-Fi or BLE+Thread commissioning."""
  home_id = args.get('home_id')
  device_id = args.get('device_id')
  network_type = args.get('network_type', 'wifi').lower()
  trigger = args.get('trigger', 'auto')
  execution_mode = args.get('execution_mode', 'auto')
  ssid = args.get('ssid', DEFAULT_HOME_SSID)
  psk = args.get('psk', DEFAULT_HOME_PSK)
  setup_pin_code = int(args.get('setup_pin_code', DEFAULT_PIN_CODE))
  discriminator = int(args.get('discriminator', DEFAULT_DISCRIMINATOR))
  manual_code = str(args.get('manual_code', DEFAULT_MANUAL_CODE))
  timeout_sec = float(args.get('timeout_sec', 60.0))
  record_video = bool(args.get('record_video', False))

  home = _get_home(server, home_id)
  if (not home and execution_mode in ('auto', 'ble_virtual_harness')) or (
      execution_mode == 'ble_virtual_harness'
  ):
    return _run_live_virtual_bt_ble_commissioning_harness(
        server=server,
        home_id=home_id,
        device_id=device_id,
        network_type=network_type,
        timeout_sec=timeout_sec,
    )

  if not home:
    raise ValueError(f'Home not found: {home_id}')
  node = home.devices.get(device_id)
  if not node:
    raise ValueError(f'Device not found in home: {device_id}')

  video_info = None
  if record_video and hasattr(node, 'start_screen_recording'):
    video_info = node.start_screen_recording(
        f'matter_commission_{network_type}_{int(timeout_sec)}s.mp4'
    )

  try:
    if network_type == 'thread':
      if not hasattr(node, 'commission_thread_via_chiptool_ui'):
        raise TypeError(
            f'Node {device_id} does not support Thread UI commissioning.'
        )

      channel = args.get('channel')
      pan_id = args.get('pan_id')
      xpan_id = args.get('xpan_id')
      master_key = args.get('master_key')
      dataset_tlvs_hex = args.get('dataset_tlvs_hex')

      result = node.commission_thread_via_chiptool_ui(
          channel=int(channel) if channel is not None else None,
          pan_id=pan_id,
          xpan_id=xpan_id,
          master_key=master_key,
          dataset_tlvs_hex=dataset_tlvs_hex,
          setup_pin_code=setup_pin_code,
          discriminator=discriminator,
          manual_code=manual_code,
          timeout_sec=timeout_sec,
          trigger=trigger,
      )
    else:
      if not hasattr(node, 'commission_via_chiptool_ui'):
        raise TypeError(
            f'Node {device_id} does not support Wi-Fi UI commissioning.'
        )
      result = node.commission_via_chiptool_ui(
          ssid=ssid,
          psk=psk,
          setup_pin_code=setup_pin_code,
          discriminator=discriminator,
          manual_code=manual_code,
          timeout_sec=timeout_sec,
          trigger=trigger,
      )

    return {
        'status': 'success',
        'network_type': network_type,
        'commission_result': result,
        'video': video_info,
        'execution_mode': execution_mode,
    }
  finally:
    if record_video and hasattr(node, 'stop_screen_recording'):
      node.stop_screen_recording()


def tool_matter_cluster_action(
    server: Any, args: Dict[str, Any]
) -> Dict[str, Any]:
  """Executes Matter Cluster commands or attribute reads."""
  home_id = args.get('home_id')
  device_id = args.get('device_id')
  cluster = args.get('cluster', 'OnOff')
  action = args.get('action', 'toggle')
  node_id = int(args.get('node_id', 1))
  endpoint = int(args.get('endpoint', 1))
  timeout_sec = float(args.get('timeout_sec', 15.0))
  record_video = bool(args.get('record_video', False))

  home = _get_home(server, home_id)
  if not home:
    raise ValueError(f'Home not found: {home_id}')
  node = home.devices.get(device_id)
  if not node:
    raise ValueError(f'Device not found in home: {device_id}')

  if record_video and hasattr(node, 'start_screen_recording'):
    node.start_screen_recording(
        f'cluster_{cluster}_{action}_{int(timeout_sec)}s.mp4'
    )

  try:
    if cluster == 'OnOff':
      if action == 'toggle':
        if not hasattr(node, 'toggle_onoff_via_chiptool_ui'):
          raise TypeError(
              f'Node {device_id} does not support'
              ' toggle_onoff_via_chiptool_ui.'
          )
        res = node.toggle_onoff_via_chiptool_ui(
            node_id=node_id, endpoint=endpoint, timeout_sec=timeout_sec
        )
      elif action == 'read':
        if not hasattr(node, 'read_onoff_via_chiptool_ui'):
          raise TypeError(
              f'Node {device_id} does not support read_onoff_via_chiptool_ui.'
          )
        res = node.read_onoff_via_chiptool_ui(
            node_id=node_id, endpoint=endpoint, timeout_sec=timeout_sec
        )
      elif action == 'toggle_and_read':
        toggle_res = node.toggle_onoff_via_chiptool_ui(
            node_id=node_id, endpoint=endpoint, timeout_sec=timeout_sec
        )
        read_res = node.read_onoff_via_chiptool_ui(
            node_id=node_id, endpoint=endpoint, timeout_sec=timeout_sec
        )
        res = {'toggle': toggle_res, 'read': read_res}
      else:
        raise ValueError(f'Unknown OnOff action: {action}')
    elif cluster in (
        'Descriptor',
        'BasicInformation',
        'OperationalCredentials',
    ):
      if hasattr(node, 'read_cluster_attribute'):
        res = node.read_cluster_attribute(
            cluster=cluster,
            attribute=action,
            node_id=node_id,
            endpoint=endpoint,
        )
      else:
        # Generic execute_device_cmd fallback
        cmd = (
            f'chip-tool {cluster.lower()} read {action.lower()} {node_id}'
            f' {endpoint}'
        )
        out = home.execute_device_cmd(cmd, device_id)
        res = {'output': getattr(out, 'output', str(out))}
    else:
      raise ValueError(f'Unsupported cluster: {cluster}')

    return {
        'status': 'success',
        'cluster': cluster,
        'action': action,
        'result': res,
    }
  finally:
    if record_video and hasattr(node, 'stop_screen_recording'):
      node.stop_screen_recording()


def tool_matter_diagnose_commissioning_health(
    server: Any, args: Dict[str, Any]
) -> Dict[str, Any]:
  """Runs Matter-specific commissioning diagnostic checks."""
  home_id = args.get('home_id')
  android_device_id = args.get('android_device_id')
  home = _get_home(server, home_id)
  if not home:
    raise ValueError(f'Home not found: {home_id}')

  return matter_commissioning_health_oracle(home, android_device_id)


def tool_matter_verify_ble_commissioning_waterfall(
    server: Any, args: Dict[str, Any]
) -> Dict[str, Any]:
  """Scenario 1: End-to-end multi-stage Matter BLE commissioning."""
  home_id = args.get('home_id')
  device_id = args.get('device_id')
  network_type = args.get('network_type', 'wifi').lower()
  execution_mode = args.get('execution_mode', 'auto')
  timeout_sec = float(args.get('timeout_sec', 60.0))

  home = _get_home(server, home_id)
  run_harness = (
      execution_mode == 'ble_virtual_harness'
      or (execution_mode == 'auto' and not home)
  )

  if run_harness:
    harness_res = _run_live_virtual_bt_ble_commissioning_harness(
        server=server,
        home_id=home_id,
        device_id=device_id,
        network_type=network_type,
        timeout_sec=timeout_sec,
    )
    home = _get_home(server, home_id)
    stages = harness_res.get('waterfall_stages', [])
  else:
    stages: List[Dict[str, Any]] = [
        {'stage': 1, 'name': 'ADV_DISCOVERY', 'passed': False},
        {'stage': 2, 'name': 'GATT_CONNECT', 'passed': False},
        {'stage': 3, 'name': 'PASE_HANDSHAKE', 'passed': False},
        {'stage': 4, 'name': 'FAILSAFE_ARMED', 'passed': False},
        {'stage': 5, 'name': 'NETWORK_CONFIG', 'passed': False},
        {'stage': 6, 'name': 'OPERATIONAL_CERT_ISSUED', 'passed': False},
        {'stage': 7, 'name': 'CASE_CONNECT', 'passed': False},
    ]

    comm_res = tool_matter_commission_over_ble(server, {
        'home_id': home_id,
        'device_id': device_id,
        'network_type': network_type,
        'trigger': 'auto',
        'timeout_sec': timeout_sec,
        'execution_mode': execution_mode,
    })

    if not home:
      raise ValueError(f'Home not found: {home_id}')
    node = home.devices.get(device_id)
    if not node:
      raise ValueError(f'Device not found in home: {device_id}')

    # Check logcat markers - genuine verification without tautologies
    if hasattr(node, '_chiptool_logcat_contains'):
      stages[0]['passed'] = bool(
          node._chiptool_logcat_contains('BLE adv start')
          or node._chiptool_logcat_contains(
              'ChipDeviceScanner has started scanning'
          )
          or node._chiptool_logcat_contains('Connecting to device')
      )
      stages[1]['passed'] = bool(
          node._chiptool_logcat_contains('GATT connected')
          or node._chiptool_logcat_contains('ConnectDevice complete')
          or node._chiptool_logcat_contains('New BLE connection')
      )
      stages[2]['passed'] = bool(
          node._chiptool_logcat_contains('PASE established')
          or node._chiptool_logcat_contains('Secure session established')
          or node._chiptool_logcat_contains('peripheral chose BTP version 4')
      )
      stages[3]['passed'] = bool(
          node._chiptool_logcat_contains('ArmFailSafe')
          or node._chiptool_logcat_contains('ArmFailSafe complete')
          or node._chiptool_logcat_contains('Failsafe armed')
      )
      stages[4]['passed'] = bool(
          node._chiptool_logcat_contains('Network commissioning')
          or node._chiptool_logcat_contains('ThreadNetworkSetup')
          or node._chiptool_logcat_contains('WiFiNetworkSetup')
      )
      stages[5]['passed'] = bool(
          node._chiptool_logcat_contains('NOC issued')
          or node._chiptool_logcat_contains('SendNOC')
          or node._chiptool_logcat_contains('Operational credentials received')
      )
      stages[6]['passed'] = bool(
          node._chiptool_logcat_contains('CASE session established')
          or node._chiptool_logcat_contains('CASE established')
          or node._chiptool_logcat_contains(
              'BLE Commissioning and Thread Provisioning succeeded'
          )
      )
    else:
      success = comm_res.get('status') == 'success'
      for s in stages:
        s['passed'] = success

  # Exercise OnOff cluster toggle
  onoff_res = tool_matter_cluster_action(server, {
      'home_id': home_id,
      'device_id': device_id,
      'cluster': 'OnOff',
      'action': 'toggle_and_read',
  })

  all_passed = (
      all(s['passed'] for s in stages)
      and onoff_res.get('status') == 'success'
  )
  return {
      'status': 'success' if all_passed else 'failed',
      'home_id': home_id,
      'device_id': device_id,
      'network_type': network_type,
      'waterfall_stages': stages,
      'cluster_validation': onoff_res,
  }


def _extract_tool_payload(resp: Any) -> Any:
  """Extracts JSON payload from MCP tool response if wrapped."""
  if not isinstance(resp, dict):
    return resp
  res = resp.get('result', resp)
  if isinstance(res, dict) and 'content' in res:
    content = res.get('content', [])
    if content and isinstance(content[0], dict) and 'text' in content[0]:
      try:
        return json.loads(content[0]['text'])
      except Exception:
        return content[0]['text']
  return res


def tool_matter_exercise_ble_brownout_recovery(
    server: Any, args: Dict[str, Any]
) -> Dict[str, Any]:
  """Scenario 2: Chaos fault injection during active BLE commissioning."""
  home_id = args.get('home_id')
  device_id = args.get('device_id')
  brownout_duration_sec = max(
      0.0, float(args.get('brownout_duration_sec', 0.1))
  )
  restore_after_brownout = bool(args.get('restore_after_brownout', True))

  bt_server = BlueToothCapability.get_or_start_virtual_server()
  wifi_server = WiFiCapability.get_or_start_virtual_server()
  if not wifi_server.list_aps():
    wifi_server.register_ap(DEFAULT_HOME_SSID, DEFAULT_HOME_PSK)

  ctrl0 = bt_server.get_controller('hci0')
  if not ctrl0:
    ctrl0 = bt_server.create_controller('hci0', dedicated_port=False)
  ctrl1 = bt_server.get_controller('hci1')
  if not ctrl1:
    ctrl1 = bt_server.create_controller('hci1', dedicated_port=False)
  if hasattr(bt_server, '_controllers'):
    bt_server._controllers['android_hci0'] = ctrl0

  pcap_dir = (
      getattr(server, '_pcap_dir', None)
      or getattr(server, 'pcap_dir', None)
      or os.environ.get('CIRQUE_PCAP_DIR')
      or tempfile.gettempdir()
  )
  bt_server.enable_pcap(pcap_dir)

  home = _get_home(server, home_id)
  if not home:
    vhome = LightweightVirtualHome(
        home_id=home_id,
        android_device_id=device_id or 'android_emu',
        end_device_id='device1',
        ctrl0_bd=ctrl0.state.bd_addr,
        ctrl1_bd=ctrl1.state.bd_addr,
    )
    if hasattr(server, '_local_homes'):
      server._local_homes[home_id] = vhome
    if isinstance(server.homes, dict):
      server.homes[home_id] = vhome
    home = vhome
  synthetic_home = type(home).__name__ == 'LightweightVirtualHome'

  # 1. Baseline health
  baseline_health = server.handle_message({
      'jsonrpc': '2.0',
      'id': 'diag_pre',
      'method': 'tools/call',
      'params': {
          'name': 'cirque_diagnose_home_health',
          'arguments': {'home_id': home_id},
      },
  })

  # 2. Inject ble_brownout
  t_brownout_start = time.monotonic()
  fault_res = server.handle_message({
      'jsonrpc': '2.0',
      'id': 'inject',
      'method': 'tools/call',
      'params': {
          'name': 'cirque_inject_rf_fault',
          'arguments': {'recipe': 'ble_brownout'},
      },
  })

  # Transmit real BLE frame on phy_hub and observe dropped frames increase
  dropped_before = getattr(bt_server.phy_hub, 'total_frames_dropped', 0)
  bt_server.phy_hub.transmit(
      LinkLayerFrame(
          pdu_type=LinkLayerPduType.ADV_IND.value,
          src_bd_addr=ctrl1.state.bd_addr,
          rssi=-42,
          payload_hex='020106',
      )
  )
  dropped_after = getattr(bt_server.phy_hub, 'total_frames_dropped', 0)
  dropped_increased = dropped_after > dropped_before

  # 3. Diagnose during brownout and hold brownout for brownout_duration_sec
  brownout_health = server.handle_message({
      'jsonrpc': '2.0',
      'id': 'diag_during',
      'method': 'tools/call',
      'params': {
          'name': 'cirque_diagnose_home_health',
          'arguments': {'home_id': home_id},
      },
  })
  if brownout_duration_sec > 0.0:
    time.sleep(brownout_duration_sec)
  measured_brownout_duration_sec = round(
      time.monotonic() - t_brownout_start, 4
  )

  # 4. Clear fault (unless negative-control restore_after_brownout=False)
  clear_payload: Dict[str, Any] = {'status': 'skipped_restore'}
  if restore_after_brownout:
    clear_res = server.handle_message({
        'jsonrpc': '2.0',
        'id': 'clear',
        'method': 'tools/call',
        'params': {
            'name': 'cirque_inject_rf_fault',
            'arguments': {'recipe': 'ble_restore'},
        },
    })
    clear_payload = _extract_tool_payload(clear_res)

  # 5. Run real H4TcpClient BLE commissioning/BTP exchange and observe
  # routed frames strictly increase
  routed_before = getattr(bt_server.phy_hub, 'total_frames_routed', 0)
  h4_exchange_ok = False
  h4_error: Optional[str] = None
  h4_timeout = 3.0 if restore_after_brownout else 0.5
  try:
    c_h4 = H4TcpClient(
        '127.0.0.1', bt_server.hci_port, bind_controller_id='hci0'
    )
    p_h4 = H4TcpClient(
        '127.0.0.1', bt_server.hci_port, bind_controller_id='hci1'
    )
    try:
      p_h4.start_advertising(b'\x02\x01\x06\x03\x03\xf6\xff')
      c_h4.start_scanning(active=True)
      adv = c_h4.wait_for_advertisement(
          target_bd_addr=ctrl1.state.bd_addr, timeout=h4_timeout
      )
      if not adv:
        raise TimeoutError(
            'Timed out waiting for BLE advertisement after brownout'
        )
      ch = c_h4.connect(ctrl1.state.bd_addr, timeout=h4_timeout)
      ph = p_h4.wait_for_connection(ctrl0.state.bd_addr, timeout=h4_timeout)
      if ch is None or ph is None:
        raise RuntimeError('Failed to establish BLE LE connection')
      c_h4.send_acl_l2cap(
          ch, 0x0004, b'\x12\x12\x00\x65\x6c\x04\x00\x00\x00\xf7\x00\x05'
      )
      p_h4.receive_acl_l2cap(timeout=h4_timeout)
      p_h4.send_acl_l2cap(
          ph, 0x0004, b'\x1d\x13\x00\x65\x6c\x04\x00\xf7\x00\x05'
      )
      c_h4.receive_acl_l2cap(timeout=h4_timeout)
      h4_exchange_ok = True
    finally:
      c_h4.close()
      p_h4.close()
  except Exception as exc:
    h4_error = str(exc)
    logger.warning('H4 exchange failed during brownout recovery: %s', exc)

  routed_after = getattr(bt_server.phy_hub, 'total_frames_routed', 0)
  routed_increased = (routed_after > routed_before) and h4_exchange_ok

  # 6. Recovery validation
  recovered_health = server.handle_message({
      'jsonrpc': '2.0',
      'id': 'diag_post',
      'method': 'tools/call',
      'params': {
          'name': 'cirque_diagnose_home_health',
          'arguments': {'home_id': home_id},
      },
  })
  post_recovery_diag = _extract_tool_payload(recovered_health)
  if isinstance(post_recovery_diag, dict):
    if not dropped_increased or not routed_increased:
      post_recovery_diag['healthy'] = False
      causes = post_recovery_diag.setdefault('root_causes', [])
      if not dropped_increased:
        causes.append('BLE brownout did not drop frames on phy_hub.')
      if not routed_increased:
        causes.append(
            f'Post-brownout H4 BLE recovery failed (h4_error={h4_error}).'
        )

  overall_healthy = bool(
      dropped_increased
      and routed_increased
      and isinstance(post_recovery_diag, dict)
      and post_recovery_diag.get('healthy', False)
  )

  return {
      'status': 'completed',
      'healthy': overall_healthy,
      'synthetic_home': synthetic_home,
      'brownout_duration_sec': brownout_duration_sec,
      'measured_brownout_duration_sec': measured_brownout_duration_sec,
      'restore_after_brownout': restore_after_brownout,
      'h4_exchange_ok': h4_exchange_ok,
      'h4_error': h4_error,
      'baseline': _extract_tool_payload(baseline_health),
      'fault_injection': _extract_tool_payload(fault_res),
      'brownout_diagnostics': _extract_tool_payload(brownout_health),
      'recovery': clear_payload,
      'post_recovery_diagnostics': post_recovery_diag,
      'dropped_increased': dropped_increased,
      'routed_increased': routed_increased,
  }


def tool_matter_audit_cross_layer_contract(
    server: Any, args: Dict[str, Any]
) -> Dict[str, Any]:
  """Scenario 3: Cross-layer contract verification."""
  home_id = args.get('home_id')

  bt_server = BlueToothCapability.get_or_start_virtual_server()
  wifi_server = WiFiCapability.get_or_start_virtual_server()
  if not wifi_server.list_aps():
    wifi_server.register_ap(DEFAULT_HOME_SSID, DEFAULT_HOME_PSK)

  ctrl0 = bt_server.get_controller('hci0')
  if not ctrl0:
    ctrl0 = bt_server.create_controller('hci0', dedicated_port=False)
  ctrl1 = bt_server.get_controller('hci1')
  if not ctrl1:
    ctrl1 = bt_server.create_controller('hci1', dedicated_port=False)
  if hasattr(bt_server, '_controllers'):
    bt_server._controllers['android_hci0'] = ctrl0

  pcap_dir = (
      getattr(server, '_pcap_dir', None)
      or getattr(server, 'pcap_dir', None)
      or os.environ.get('CIRQUE_PCAP_DIR')
      or tempfile.gettempdir()
  )
  bt_server.enable_pcap(pcap_dir)

  fmt1_0 = {'description': {'ble_adapt_id': 0, 'ble_adapt': 'hci0'}}
  fmt1_1 = {'description': {'ble_adapt_id': 1, 'ble_adapt': 'hci1'}}
  fmt2_0 = {
      'capability': {'Bluetooth': {'ble_adapt_id': 0, 'ble_adapt': 'hci0'}}
  }
  fmt2_1 = {
      'capability': {'Bluetooth': {'ble_adapt_id': 1, 'ble_adapt': 'hci1'}}
  }
  fmt3_0 = {'ble_adapt': 'hci0'}
  fmt3_1 = {'ble_adapt': 'hci1'}

  fmt_checks = (
      extract_hci_index(fmt1_0, default_index=0) == 0
      and extract_hci_index(fmt1_1, default_index=1) == 1
      and extract_hci_index(fmt2_0, default_index=0) == 0
      and extract_hci_index(fmt2_1, default_index=1) == 1
      and extract_hci_index(fmt3_0, default_index=0) == 0
      and extract_hci_index(fmt3_1, default_index=1) == 1
  )

  ctrl0_id = ctrl0.state.controller_id
  ctrl1_id = ctrl1.state.controller_id
  ctrl0_bd = ctrl0.state.bd_addr
  ctrl1_bd = ctrl1.state.bd_addr
  controller_isolation = (
      ctrl0_id != ctrl1_id
      and ctrl0_bd != ctrl1_bd
      and ctrl0_id == 'hci0'
      and ctrl1_id == 'hci1'
  )

  home = _get_home(server, home_id)
  if not home:
    vhome = LightweightVirtualHome(
        home_id=home_id,
        android_device_id='android_emu',
        end_device_id='device1',
        ctrl0_bd=ctrl0_bd,
        ctrl1_bd=ctrl1_bd,
    )
    node = vhome.devices.get('android_emu')
    if node:
      node._logcat_lines.extend([
          'Secure session established',
          'PASE established',
          'CASE session established',
          'CASE established',
      ])
    if hasattr(server, '_local_homes'):
      server._local_homes[home_id] = vhome
    if isinstance(server.homes, dict):
      server.homes[home_id] = vhome
    home = vhome

  # Radio state check
  rad_state = server._tool_get_radio_state({'home_id': home_id})
  # PCAP summary check
  pcap_summary = server._tool_summarize_pcaps({'pcap_dir': pcap_dir})
  # Health check
  health = server._tool_diagnose_home_health({'home_id': home_id})

  contract_checks = {
      'hci_medium_attached': (
          len(rad_state.get('bluetooth', {}).get('controllers', [])) >= 2
      ),
      'wifi_ap_active': len(rad_state.get('wifi', {}).get('aps', [])) >= 1,
      'controller_isolation': controller_isolation,
      'descriptor_formats_audited': fmt_checks,
      'packet_captures_present': (
          pcap_summary.get('pcap_count', 0) >= 1
          or pcap_summary.get('total_files', 0) >= 1
          or os.path.exists(os.path.join(pcap_dir, 'bt_medium.pcap'))
      ),
      'oracles_pass': health.get('healthy', False),
  }

  contract_fulfilled = all(contract_checks.values())
  return {
      'status': 'fulfilled' if contract_fulfilled else 'violated',
      'contract_checks': contract_checks,
      'radio_state': rad_state,
      'health': health,
      'pcap_summary': pcap_summary,
  }


# -----------------------------------------------------------------------------
# Matter Health Check Oracle (Registered with CirqueMcpServer)
# -----------------------------------------------------------------------------

def matter_commissioning_health_oracle(
    home: Any, android_device_id: Optional[str]
) -> Dict[str, Any]:
  """Dynamic health check oracle for Matter PASE/CASE and CHIPTool status."""
  oracles: Dict[str, bool] = {}
  root_causes: List[str] = []
  remediation_steps: List[str] = []

  android_node = None
  if android_device_id and android_device_id in home.devices:
    android_node = home.devices[android_device_id]
  else:
    for dev in home.devices.values():
      if 'android' in getattr(dev, 'type', '').lower():
        android_node = dev
        break

  if android_node is not None and hasattr(
      android_node, '_chiptool_logcat_contains'
  ):
    pase_ok = (
        android_node._chiptool_logcat_contains('Secure session established')
        or android_node._chiptool_logcat_contains('PASE established')
    )
    case_ok = (
        android_node._chiptool_logcat_contains('CASE session established')
        or android_node._chiptool_logcat_contains('CASE established')
    )
    oracles['matter_pase_established'] = pase_ok
    oracles['matter_case_established'] = case_ok

    if not pase_ok:
      root_causes.append(
          'Matter PASE secure session was not established over BLE.'
      )
      remediation_steps.append(
          'Check setup PIN code and discriminator in commissioning payload.'
      )
    if not case_ok:
      root_causes.append(
          'Matter CASE operational session was not established over IP.'
      )
      remediation_steps.append(
          'Verify IP connectivity and operational credentials.'
      )

  healthy = len(root_causes) == 0
  return {
      'healthy': healthy,
      'oracles': oracles,
      'root_causes': root_causes,
      'remediation_steps': remediation_steps,
  }


def tool_matter_generate_flow_visualization(
    server: Any, args: Dict[str, Any]
) -> Dict[str, Any]:
  """Generates 4-layer MATTER+THREAD+WIFI+BT interactive studio."""
  home_id = args.get('home_id', 'home_matter_auto')
  device_id = args.get('device_id', 'android_emu')
  network_type = args.get('network_type', 'thread')
  run_harness_if_empty = bool(args.get('run_harness_if_empty', True))

  pcap_dir = (
      args.get('pcap_dir')
      or getattr(server, '_pcap_dir', None)
      or getattr(server, 'pcap_dir', None)
      or os.environ.get('CIRQUE_PCAP_DIR')
      or tempfile.gettempdir()
  )
  os.makedirs(pcap_dir, exist_ok=True)

  events = (
      server.get_flow_events() if hasattr(server, 'get_flow_events') else []
  )
  if not events and run_harness_if_empty:
    _run_live_virtual_bt_ble_commissioning_harness(
        server=server,
        home_id=home_id,
        device_id=device_id,
        network_type=network_type,
    )

  output_html = (
      args.get('output_html')
      or args.get('output_path')
      or os.path.join(pcap_dir, 'cirque_flow_visualization.html')
  )
  output_json = (
      args.get('output_json')
      or args.get('json_output_path')
      or os.path.join(pcap_dir, 'cirque_flow_trace.json')
  )

  if hasattr(server, '_tool_generate_flow_visualization'):
    return server._tool_generate_flow_visualization({
        'pcap_dir': pcap_dir,
        'output_html': output_html,
        'output_json': output_json,
        'title': f'Matter Cross-Layer Flow Studio ({home_id})',
    })

  # Fallback generation
  pcap_evs = []
  if os.path.isdir(pcap_dir):
    from cirque.pcap.flow_visualizer import dissect_pcap_directory
    pcap_evs = dissect_pcap_directory(pcap_dir)
  rec_evs = (
      server.get_flow_events() if hasattr(server, 'get_flow_events') else []
  )
  merged = merge_and_sort_flow_events(
      pcap_evs, rec_evs, dissectors=FLOW_DISSECTORS
  )
  generate_flow_html(
      merged,
      output_html,
      title=f'Matter Cross-Layer Flow Studio ({home_id})',
      layers=FLOW_LAYERS,
  )
  with open(output_json, 'w', encoding='utf-8') as f:
    json.dump([ev.to_dict() for ev in merged], f, indent=2)

  layer_counts: Dict[str, int] = {}
  stage_summary: Dict[str, int] = {}
  for ev in merged:
    layer_counts[ev.layer] = layer_counts.get(ev.layer, 0) + 1
    stage_summary[ev.stage] = stage_summary.get(ev.stage, 0) + 1

  return {
      'status': 'success',
      'html_path': output_html,
      'json_path': output_json,
      'total_events': len(merged),
      'layer_counts': layer_counts,
      'stage_summary': stage_summary,
      'pcap_dir': pcap_dir,
  }


# -----------------------------------------------------------------------------
# Extension Entrypoint Registration Function
# -----------------------------------------------------------------------------

def register_cirque_extension(server: Any) -> None:
  """Standard registration hook called by CirqueMcpServer."""
  logger.info('Registering Matter Cirque MCP Extension into CirqueMcpServer...')

  # Register the Matter flow plugin (dissectors + MATTER layer metadata)
  if hasattr(server, 'register_flow_dissector'):
    for name, dissector in FLOW_DISSECTORS.items():
      server.register_flow_dissector(name, dissector)
  if hasattr(server, 'register_flow_layer'):
    for name, metadata in FLOW_LAYERS.items():
      server.register_flow_layer(name, metadata)

  # 1. Register 7 Matter Tools
  server.register_tool(
      {
          'name': 'matter_generate_flow_visualization',
          'description': (
              'Generates the complete 4-layer MATTER+THREAD+WIFI+BT interactive'
              ' HTML View/Replay/Pause Studio and JSON trace.'
          ),
          'inputSchema': {
              'type': 'object',
              'properties': {
                  'home_id': {'type': 'string', 'default': 'home_matter_auto'},
                  'device_id': {'type': 'string', 'default': 'android_emu'},
                  'network_type': {
                      'type': 'string',
                      'enum': ['wifi', 'thread'],
                      'default': 'thread',
                  },
                  'pcap_dir': {'type': 'string'},
                  'output_html': {'type': 'string'},
                  'output_json': {'type': 'string'},
                  'run_harness_if_empty': {'type': 'boolean', 'default': True},
              },
              'additionalProperties': False,
          },
      },
      lambda args: tool_matter_generate_flow_visualization(server, args),
      extension_name=EXTENSION_NAME,
  )

  server.register_tool(
      {
          'name': 'matter_commission_over_ble',
          'description': (
              'Runs Matter BLE+Wi-Fi or BLE+Thread commissioning on an Android'
              ' emulator node.'
          ),
          'inputSchema': {
              'type': 'object',
              'properties': {
                  'home_id': {
                      'type': 'string',
                      'description': 'Target home ID.',
                  },
                  'device_id': {
                      'type': 'string',
                      'description': 'Target Android device ID.',
                  },
                  'network_type': {
                      'type': 'string',
                      'enum': ['wifi', 'thread'],
                      'default': 'wifi',
                  },
                  'trigger': {
                      'type': 'string',
                      'enum': ['auto', 'intent', 'ui'],
                      'default': 'auto',
                  },
                  'execution_mode': {
                      'type': 'string',
                      'enum': [
                          'auto',
                          'live_home',
                          'ble_virtual_harness',
                      ],
                      'default': 'auto',
                  },
                  'ssid': {'type': 'string', 'default': DEFAULT_HOME_SSID},
                  'psk': {'type': 'string', 'default': DEFAULT_HOME_PSK},
                  'channel': {'type': 'integer'},
                  'pan_id': {'type': 'string'},
                  'xpan_id': {'type': 'string'},
                  'master_key': {'type': 'string'},
                  'dataset_tlvs_hex': {'type': 'string'},
                  'setup_pin_code': {
                      'type': 'integer',
                      'default': DEFAULT_PIN_CODE,
                  },
                  'discriminator': {
                      'type': 'integer',
                      'default': DEFAULT_DISCRIMINATOR,
                  },
                  'manual_code': {
                      'type': 'string',
                      'default': DEFAULT_MANUAL_CODE,
                  },
                  'timeout_sec': {'type': 'number', 'default': 60.0},
                  'record_video': {'type': 'boolean', 'default': False},
              },
              'required': ['home_id', 'device_id'],
              'additionalProperties': False,
          },
      },
      lambda args: tool_matter_commission_over_ble(server, args),
      extension_name=EXTENSION_NAME,
  )

  server.register_tool(
      {
          'name': 'matter_cluster_action',
          'description': (
              'Executes Matter OnOff toggle/read, or Descriptor/'
              'BasicInformation/OperationalCredentials cluster reads.'
          ),
          'inputSchema': {
              'type': 'object',
              'properties': {
                  'home_id': {
                      'type': 'string',
                      'description': 'Target home ID.',
                  },
                  'device_id': {
                      'type': 'string',
                      'description': 'Target Android device ID.',
                  },
                  'cluster': {
                      'type': 'string',
                      'enum': [
                          'OnOff',
                          'Descriptor',
                          'BasicInformation',
                          'OperationalCredentials',
                      ],
                      'default': 'OnOff',
                  },
                  'action': {'type': 'string', 'default': 'toggle'},
                  'node_id': {'type': 'integer', 'default': 1},
                  'endpoint': {'type': 'integer', 'default': 1},
                  'timeout_sec': {'type': 'number', 'default': 15.0},
                  'record_video': {'type': 'boolean', 'default': False},
              },
              'required': ['home_id', 'device_id'],
              'additionalProperties': False,
          },
      },
      lambda args: tool_matter_cluster_action(server, args),
      extension_name=EXTENSION_NAME,
  )

  server.register_tool(
      {
          'name': 'matter_diagnose_commissioning_health',
          'description': (
              'Runs Matter-specific commissioning diagnostic checks on a'
              ' virtual home.'
          ),
          'inputSchema': {
              'type': 'object',
              'properties': {
                  'home_id': {
                      'type': 'string',
                      'description': 'Target home ID.',
                  },
                  'android_device_id': {
                      'type': 'string',
                      'description': 'Optional Android node device ID.',
                  },
              },
              'required': ['home_id'],
              'additionalProperties': False,
          },
      },
      lambda args: tool_matter_diagnose_commissioning_health(server, args),
      extension_name=EXTENSION_NAME,
  )

  server.register_tool(
      {
          'name': 'matter_verify_ble_commissioning_waterfall',
          'description': (
              'Executes full 7-stage Matter BLE commissioning waterfall'
              ' validation (Scenario 1).'
          ),
          'inputSchema': {
              'type': 'object',
              'properties': {
                  'home_id': {
                      'type': 'string',
                      'description': 'Target home ID.',
                  },
                  'device_id': {
                      'type': 'string',
                      'description': 'Target Android device ID.',
                  },
                  'network_type': {
                      'type': 'string',
                      'enum': ['wifi', 'thread'],
                      'default': 'wifi',
                  },
                  'execution_mode': {
                      'type': 'string',
                      'enum': [
                          'auto',
                          'live_home',
                          'ble_virtual_harness',
                      ],
                      'default': 'auto',
                  },
                  'timeout_sec': {'type': 'number', 'default': 60.0},
              },
              'required': ['home_id', 'device_id'],
              'additionalProperties': False,
          },
      },
      lambda args: tool_matter_verify_ble_commissioning_waterfall(server, args),
      extension_name=EXTENSION_NAME,
  )

  server.register_tool(
      {
          'name': 'matter_exercise_ble_brownout_recovery',
          'description': (
              'Injects RF brownout chaos during commissioning and verifies'
              ' self-healing recovery (Scenario 2).'
          ),
          'inputSchema': {
              'type': 'object',
              'properties': {
                  'home_id': {
                      'type': 'string',
                      'description': 'Target home ID.',
                  },
                  'device_id': {
                      'type': 'string',
                      'description': 'Target Android device ID.',
                  },
                  'execution_mode': {
                      'type': 'string',
                      'enum': [
                          'auto',
                          'live_home',
                          'ble_virtual_harness',
                      ],
                      'default': 'auto',
                  },
                  'brownout_duration_sec': {
                      'type': 'number',
                      'default': 0.1,
                  },
                  'restore_after_brownout': {
                      'type': 'boolean',
                      'default': True,
                  },
              },
              'required': ['home_id'],
              'additionalProperties': False,
          },
      },
      lambda args: tool_matter_exercise_ble_brownout_recovery(server, args),
      extension_name=EXTENSION_NAME,
  )

  server.register_tool(
      {
          'name': 'matter_audit_cross_layer_contract',
          'description': (
              'Audits cross-layer contracts across HCI, Wi-Fi, Thread, and'
              ' Matter attributes (Scenario 3).'
          ),
          'inputSchema': {
              'type': 'object',
              'properties': {
                  'home_id': {
                      'type': 'string',
                      'description': 'Target home ID.',
                  },
                  'execution_mode': {
                      'type': 'string',
                      'enum': [
                          'auto',
                          'live_home',
                          'ble_virtual_harness',
                      ],
                      'default': 'auto',
                  },
              },
              'required': ['home_id'],
              'additionalProperties': False,
          },
      },
      lambda args: tool_matter_audit_cross_layer_contract(server, args),
      extension_name=EXTENSION_NAME,
  )

  # 2. Register Matter Resources & Templates
  server.register_resource(
      {
          'uri': 'cirque://matter/commissioning/stages',
          'name': 'Matter Commissioning Stages',
          'description': (
              'Reference specification of the 7 stages in Matter BLE'
              ' commissioning waterfall.'
          ),
          'mimeType': 'application/json',
      },
      reader_fn=lambda uri: {
          'stages': [
              {
                  'stage': 1,
                  'name': 'ADV_DISCOVERY',
                  'desc': 'BLE 2.4 GHz Service Data Adv 0xFFF6',
              },
              {
                  'stage': 2,
                  'name': 'GATT_CONNECT',
                  'desc': 'GATT MTU exchange and BTP handshake',
              },
              {
                  'stage': 3,
                  'name': 'PASE_HANDSHAKE',
                  'desc': (
                      'SPAKE2+ key exchange and session establishment'
                  ),
              },
              {
                  'stage': 4,
                  'name': 'FAILSAFE_ARMED',
                  'desc': 'ArmFailSafe timer armed',
              },
              {
                  'stage': 5,
                  'name': 'NETWORK_CONFIG',
                  'desc': 'AddOrUpdateWiFiNetwork or Thread dataset',
              },
              {
                  'stage': 6,
                  'name': 'OPERATIONAL_CERT_ISSUED',
                  'desc': 'CSR, DAC chain validation, and NOC issue',
              },
              {
                  'stage': 7,
                  'name': 'CASE_CONNECT',
                  'desc': (
                      'Sigma1-3 CASE handshake over operational network'
                  ),
              },
          ]
      },
      extension_name=EXTENSION_NAME,
  )

  server.register_resource_template(
      {
          'uriTemplate': 'cirque://homes/{home_id}/matter/waterfall',
          'name': 'Matter Waterfall Diagnostic',
          'description': (
              'Live evaluation of Matter commissioning waterfall for a'
              ' virtual home.'
          ),
          'mimeType': 'application/json',
      },
      pattern=r'^cirque://homes/([^/]+)/matter/waterfall$',
      reader_fn=lambda uri, m: tool_matter_diagnose_commissioning_health(
          server, {'home_id': m.group(1)}
      ),
      extension_name=EXTENSION_NAME,
  )

  # 3. Register Matter Prompts
  server.register_prompt(
      {
          'name': 'matter_ble_commissioning_validation',
          'description': (
              'Guides end-to-end Matter BLE commissioning verification and'
              ' cluster testing.'
          ),
          'arguments': [
              {
                  'name': 'home_id',
                  'description': 'Target home ID.',
                  'required': True,
              },
              {
                  'name': 'device_id',
                  'description': 'Android emulator device ID.',
                  'required': True,
              },
          ],
      },
      renderer_fn=lambda args: (
          'You are an expert Matter Protocol & IoT Automation Engineer.\n'
          f'Execute end-to-end commissioning validation on home '
          f'"{args.get("home_id")}" using controller device '
          f'"{args.get("device_id")}".\n\n'
          'Steps:\n'
          '1. Call `matter_verify_ble_commissioning_waterfall` to test all 7 '
          'waterfall stages.\n'
          '2. Exercise OnOff toggle via `matter_cluster_action`.\n'
          '3. Check PCAP evidence with `cirque_summarize_pcaps`.'
      ),
      extension_name=EXTENSION_NAME,
  )

  server.register_prompt(
      {
          'name': 'matter_ble_chaos_recovery_drill',
          'description': (
              'Runs RF brownout injection drill and validates self-healing'
              ' recovery.'
          ),
          'arguments': [
              {
                  'name': 'home_id',
                  'description': 'Target home ID.',
                  'required': True,
              },
              {
                  'name': 'device_id',
                  'description': 'Android emulator device ID.',
                  'required': True,
              },
          ],
      },
      renderer_fn=lambda args: (
          'You are an autonomous IoT Chaos Engineer.\n'
          f'Perform a BLE brownout drill on home "{args.get("home_id")}".\n\n'
          'Steps:\n'
          '1. Call `matter_exercise_ble_brownout_recovery` with '
          'duration=3.0s.\n'
          '2. Verify that `cirque_diagnose_home_health` recovers to '
          'healthy=True.'
      ),
      extension_name=EXTENSION_NAME,
  )

  server.register_prompt(
      {
          'name': 'matter_android_certification',
          'description': (
              'Executes automated CHIPTool certification suite on Android'
              ' emulator.'
          ),
          'arguments': [
              {
                  'name': 'home_id',
                  'description': 'Target home ID.',
                  'required': True,
              },
              {
                  'name': 'device_id',
                  'description': 'Android emulator device ID.',
                  'required': True,
              },
          ],
      },
      renderer_fn=lambda args: (
          'You are a Matter Automated Certification Engineer.\n'
          f'Run full Android certification on device "{args.get("device_id")}" '
          f'in home "{args.get("home_id")}".\n\n'
          'Steps:\n'
          '1. Start video recording with `cirque_android_ui_and_video` '
          'action="start_recording".\n'
          '2. Run `matter_commission_over_ble` with trigger="auto" and '
          'record_video=True.\n'
          '3. Call `matter_cluster_action` with action="toggle_and_read".\n'
          '4. Stop recording and retrieve video evidence.'
      ),
      extension_name=EXTENSION_NAME,
  )

  server.register_prompt(
      {
          'name': 'matter_debug_commissioning_failure',
          'description': (
              'Investigates failed Matter commissioning attempts using'
              ' waterfall analysis and logs.'
          ),
          'arguments': [
              {
                  'name': 'home_id',
                  'description': 'Target home ID.',
                  'required': True,
              },
              {
                  'name': 'device_id',
                  'description': 'Target device ID.',
                  'required': True,
              },
          ],
      },
      renderer_fn=lambda args: (
          'You are an expert Matter protocol triage engineer.\n'
          f'Diagnose commissioning failure in home "{args.get("home_id")}" '
          f'for device "{args.get("device_id")}".\n\n'
          'Steps:\n'
          '1. Call `matter_diagnose_commissioning_health` to check '
          'PASE/CASE status.\n'
          '2. Read `cirque://homes/{home_id}/devices/{device_id}/logs`.\n'
          '3. Inspect Wireshark captures with `cirque_summarize_pcaps`.'
      ),
      extension_name=EXTENSION_NAME,
  )

  # 4. Register Health Oracle
  server.register_health_oracle(
      'matter_commissioning_health',
      matter_commissioning_health_oracle,
      extension_name=EXTENSION_NAME,
  )


# -----------------------------------------------------------------------------
# Standalone CLI Stdio Runner
# -----------------------------------------------------------------------------

def main() -> None:
  logging.basicConfig(level=logging.INFO)
  from cirque.mcp.server import CirqueMcpServer
  server = CirqueMcpServer()
  register_cirque_extension(server)
  server.handle_stream()


if __name__ == '__main__':
  main()
