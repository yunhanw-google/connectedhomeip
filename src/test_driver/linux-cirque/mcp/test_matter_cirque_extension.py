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
"""Comprehensive Unit Tests for Matter Cirque MCP Extension."""

import json
import os
import shutil
import struct
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CIRQUE_ROOT = os.path.abspath(
    os.path.join(
        SCRIPT_DIR, '..', '..', '..', '..', 'third_party', 'cirque', 'repo'
    )
)
if CIRQUE_ROOT not in sys.path:
  sys.path.insert(0, CIRQUE_ROOT)

from cirque.mcp.server import CirqueMcpServer
import matter_cirque_extension as matter_ext
import matter_flow_plugin


class TestMatterCirqueExtension(unittest.TestCase):
  """Unit test suite for Matter Cirque MCP Extension."""

  def setUp(self):
    self.temp_dir = tempfile.mkdtemp(prefix='matter_ext_test_')
    self.mock_home = MagicMock()
    self.mock_home.home_id = 'home_matter_1'
    self.mock_home.devices = {}
    self.homes = {'home_matter_1': self.mock_home}
    self.server = CirqueMcpServer(
        homes_provider=lambda: self.homes, pcap_dir=self.temp_dir
    )
    matter_ext.register_cirque_extension(self.server)

  def tearDown(self):
    shutil.rmtree(self.temp_dir, ignore_errors=True)

  def test_registration_and_tools_list(self):
    t_list = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 1,
        'method': 'tools/list',
    })
    tools = t_list['result']['tools']
    tool_names = [t['name'] for t in tools]

    expected_matter_tools = [
        'matter_commission_over_ble',
        'matter_cluster_action',
        'matter_diagnose_commissioning_health',
        'matter_verify_ble_commissioning_waterfall',
        'matter_exercise_ble_brownout_recovery',
        'matter_audit_cross_layer_contract',
        'matter_generate_flow_visualization',
    ]
    for t in expected_matter_tools:
      self.assertIn(t, tool_names)

  def test_matter_commission_over_ble_wifi_and_thread(self):
    android_mock = MagicMock()
    android_mock.type = 'android_emulator'
    android_mock.commission_via_chiptool_ui.return_value = {
        'status': 'success', 'commissioned_node_id': 1
    }
    android_mock.commission_thread_via_chiptool_ui.return_value = {
        'status': 'success', 'commissioned_node_id': 1
    }
    android_mock.start_screen_recording.return_value = {'started': True}
    android_mock.stop_screen_recording.return_value = {'stopped': True}
    self.mock_home.devices['android_emu'] = android_mock

    # Wi-Fi commission
    res_wifi = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 2,
        'method': 'tools/call',
        'params': {
            'name': 'matter_commission_over_ble',
            'arguments': {
                'home_id': 'home_matter_1',
                'device_id': 'android_emu',
                'network_type': 'wifi',
                'record_video': True,
            },
        },
    })
    self.assertFalse(res_wifi['result']['isError'])
    wifi_data = json.loads(res_wifi['result']['content'][0]['text'])
    self.assertIn('status', wifi_data)
    self.assertTrue(wifi_data['status'] == 'success')
    android_mock.commission_via_chiptool_ui.assert_called_once()

    # Thread commission
    res_thread = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 3,
        'method': 'tools/call',
        'params': {
            'name': 'matter_commission_over_ble',
            'arguments': {
                'home_id': 'home_matter_1',
                'device_id': 'android_emu',
                'network_type': 'thread',
                'channel': 15,
                'pan_id': '0x1234',
                'record_video': True,
            },
        },
    })
    self.assertFalse(res_thread['result']['isError'])
    thread_data = json.loads(res_thread['result']['content'][0]['text'])
    self.assertIn('status', thread_data)
    self.assertTrue(thread_data['status'] == 'success')
    android_mock.commission_thread_via_chiptool_ui.assert_called_once()

  def test_matter_cluster_action_onoff_and_descriptor(self):
    android_mock = MagicMock()
    android_mock.toggle_onoff_via_chiptool_ui.return_value = {'toggled': True}
    android_mock.read_onoff_via_chiptool_ui.return_value = {'value': True}
    self.mock_home.devices['android_emu'] = android_mock

    # OnOff toggle
    res_toggle = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 4,
        'method': 'tools/call',
        'params': {
            'name': 'matter_cluster_action',
            'arguments': {
                'home_id': 'home_matter_1',
                'device_id': 'android_emu',
                'cluster': 'OnOff',
                'action': 'toggle_and_read',
            },
        },
    })
    self.assertFalse(res_toggle['result']['isError'])
    toggle_data = json.loads(res_toggle['result']['content'][0]['text'])
    self.assertIn('status', toggle_data)
    self.assertTrue(toggle_data['status'] == 'success')
    self.assertTrue(toggle_data['result']['toggle']['toggled'])

    # Descriptor read via fallback
    self.mock_home.execute_device_cmd.return_value = SimpleNamespace(
        exit_code=0, output=b'DeviceList: [1, 2]\n'
    )
    res_desc = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 5,
        'method': 'tools/call',
        'params': {
            'name': 'matter_cluster_action',
            'arguments': {
                'home_id': 'home_matter_1',
                'device_id': 'android_emu',
                'cluster': 'Descriptor',
                'action': 'devicelist',
            },
        },
    })
    self.assertFalse(res_desc['result']['isError'])

  def test_scenarios_waterfall_brownout_audit(self):
    android_mock = MagicMock()
    android_mock.commission_via_chiptool_ui.return_value = {'status': 'success'}
    android_mock.toggle_onoff_via_chiptool_ui.return_value = {'status': 'ok'}
    android_mock.read_onoff_via_chiptool_ui.return_value = {'status': 'ok'}
    android_mock._chiptool_logcat_contains = MagicMock(return_value=True)
    self.mock_home.devices['android_emu'] = android_mock

    # Scenario 1: Waterfall
    s1_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 10,
        'method': 'tools/call',
        'params': {
            'name': 'matter_verify_ble_commissioning_waterfall',
            'arguments': {
                'home_id': 'home_matter_1',
                'device_id': 'android_emu',
            },
        },
    })
    self.assertFalse(s1_res['result']['isError'])
    s1_data = json.loads(s1_res['result']['content'][0]['text'])
    self.assertIn('status', s1_data)
    self.assertTrue(s1_data['status'] == 'success')
    self.assertEqual(len(s1_data['waterfall_stages']), 7)

    # Scenario 2: Chaos Brownout
    s2_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 11,
        'method': 'tools/call',
        'params': {
            'name': 'matter_exercise_ble_brownout_recovery',
            'arguments': {
                'home_id': 'home_matter_1',
                'device_id': 'android_emu',
            },
        },
    })
    self.assertFalse(s2_res['result']['isError'])
    s2_data = json.loads(s2_res['result']['content'][0]['text'])
    self.assertEqual(s2_data['status'], 'completed')

    # Scenario 3: Cross-layer Contract Audit
    s3_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 12,
        'method': 'tools/call',
        'params': {
            'name': 'matter_audit_cross_layer_contract',
            'arguments': {
                'home_id': 'home_matter_1',
            },
        },
    })
    self.assertFalse(s3_res['result']['isError'])
    s3_data = json.loads(s3_res['result']['content'][0]['text'])
    self.assertIn('contract_checks', s3_data)

  def test_matter_resources_and_prompts(self):
    # Resource read
    r_stages = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 20,
        'method': 'resources/read',
        'params': {'uri': 'cirque://matter/commissioning/stages'},
    })
    stages_content = json.loads(r_stages['result']['contents'][0]['text'])
    self.assertEqual(len(stages_content['stages']), 7)

    # Resource template read
    r_wf = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 21,
        'method': 'resources/read',
        'params': {'uri': 'cirque://homes/home_matter_1/matter/waterfall'},
    })
    self.assertFalse(r_wf.get('error'))

    # Prompts get
    for p_name in [
        'matter_ble_commissioning_validation',
        'matter_ble_chaos_recovery_drill',
        'matter_android_certification',
        'matter_debug_commissioning_failure',
    ]:
      p_get = self.server.handle_message({
          'jsonrpc': '2.0',
          'id': 22,
          'method': 'prompts/get',
          'params': {
              'name': p_name,
              'arguments': {'home_id': 'h1', 'device_id': 'd1'},
          },
      })
      self.assertIn('messages', p_get['result'])

  def test_tautology_removal_waterfall_failure_when_logcat_missing(self):
    android_mock = MagicMock()
    android_mock.commission_via_chiptool_ui.return_value = {
        'status': 'success'
    }
    android_mock.toggle_onoff_via_chiptool_ui.return_value = {'status': 'ok'}
    android_mock.read_onoff_via_chiptool_ui.return_value = {'status': 'ok'}
    android_mock._chiptool_logcat_contains = MagicMock(return_value=False)
    self.mock_home.devices['android_emu_fail'] = android_mock

    res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 30,
        'method': 'tools/call',
        'params': {
            'name': 'matter_verify_ble_commissioning_waterfall',
            'arguments': {
                'home_id': 'home_matter_1',
                'device_id': 'android_emu_fail',
            },
        },
    })
    self.assertFalse(res['result']['isError'])
    data = json.loads(res['result']['content'][0]['text'])
    self.assertEqual(data['status'], 'failed')
    for stage in data['waterfall_stages']:
      self.assertFalse(
          stage['passed'], f"Stage {stage['name']} should fail without logcat"
      )

  def test_extract_hci_index_all_descriptor_formats(self):
    fmt1_0 = {'description': {'ble_adapt_id': 0, 'ble_adapt': 'hci0'}}
    fmt1_1 = {'description': {'ble_adapt_id': 1, 'ble_adapt': 'hci1'}}
    self.assertEqual(matter_ext.extract_hci_index(fmt1_0, default_index=9), 0)
    self.assertEqual(matter_ext.extract_hci_index(fmt1_1, default_index=9), 1)

    fmt2_0 = {
        'capability': {'Bluetooth': {'ble_adapt_id': 0, 'ble_adapt': 'hci0'}}
    }
    fmt2_1 = {
        'capability': {'Bluetooth': {'ble_adapt_id': 1, 'ble_adapt': 'hci1'}}
    }
    self.assertEqual(matter_ext.extract_hci_index(fmt2_0, default_index=9), 0)
    self.assertEqual(matter_ext.extract_hci_index(fmt2_1, default_index=9), 1)

    fmt3_0 = {'ble_adapt': 'hci0'}
    fmt3_1 = {'ble_adapt': 'hci1'}
    self.assertEqual(matter_ext.extract_hci_index(fmt3_0, default_index=9), 0)
    self.assertEqual(matter_ext.extract_hci_index(fmt3_1, default_index=9), 1)

    self.assertEqual(matter_ext.extract_hci_index('hci0', default_index=9), 0)
    self.assertEqual(matter_ext.extract_hci_index('hci1', default_index=9), 1)

    self.assertEqual(matter_ext.extract_hci_index({}, default_index=42), 42)

  def test_live_virtual_bt_ble_commissioning_harness_end_to_end(self):
    live_home_id = 'live_harness_home_test'
    comm_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 40,
        'method': 'tools/call',
        'params': {
            'name': 'matter_commission_over_ble',
            'arguments': {
                'home_id': live_home_id,
                'device_id': 'android_emu',
                'network_type': 'thread',
                'execution_mode': 'ble_virtual_harness',
            },
        },
    })
    self.assertFalse(comm_res['result']['isError'])
    comm_data = json.loads(comm_res['result']['content'][0]['text'])
    self.assertIn(comm_data['status'], ('success',))
    self.assertEqual(comm_data['execution_mode'], 'ble_virtual_harness')
    self.assertEqual(len(comm_data['waterfall_stages']), 7)
    for st in comm_data['waterfall_stages']:
      self.assertTrue(st['passed'])

    self.assertTrue(
        os.path.exists(os.path.join(self.temp_dir, 'bt_medium.pcap'))
    )
    self.assertTrue(
        os.path.exists(os.path.join(self.temp_dir, 'bt_hci_hci0.pcap'))
    )
    self.assertTrue(
        os.path.exists(os.path.join(self.temp_dir, 'bt_hci_hci1.pcap'))
    )

    self.assertIn(live_home_id, self.homes)

    wf_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 41,
        'method': 'tools/call',
        'params': {
            'name': 'matter_verify_ble_commissioning_waterfall',
            'arguments': {
                'home_id': live_home_id,
                'device_id': 'android_emu',
                'network_type': 'thread',
            },
        },
    })
    self.assertFalse(wf_res['result']['isError'])
    wf_data = json.loads(wf_res['result']['content'][0]['text'])
    self.assertIn(wf_data['status'], ('success',))
    for st in wf_data['waterfall_stages']:
      self.assertTrue(st['passed'])

    onoff_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 42,
        'method': 'tools/call',
        'params': {
            'name': 'matter_cluster_action',
            'arguments': {
                'home_id': live_home_id,
                'device_id': 'android_emu',
                'cluster': 'OnOff',
                'action': 'toggle_and_read',
            },
        },
    })
    self.assertFalse(onoff_res['result']['isError'])
    onoff_data = json.loads(onoff_res['result']['content'][0]['text'])
    self.assertIn(onoff_data['status'], ('success',))
    self.assertTrue(onoff_data['result']['toggle']['toggled'])

    inv_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 43,
        'method': 'tools/call',
        'params': {
            'name': 'matter_cluster_action',
            'arguments': {
                'home_id': live_home_id,
                'device_id': 'android_emu',
                'cluster': 'OnOff',
                'endpoint': 233,
                'action': 'toggle',
            },
        },
    })
    self.assertTrue(inv_res['result']['isError'])

    bo_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 44,
        'method': 'tools/call',
        'params': {
            'name': 'matter_exercise_ble_brownout_recovery',
            'arguments': {
                'home_id': live_home_id,
                'device_id': 'android_emu',
            },
        },
    })
    self.assertFalse(bo_res['result']['isError'])
    bo_data = json.loads(bo_res['result']['content'][0]['text'])
    self.assertEqual(bo_data['status'], 'completed')
    self.assertTrue(bo_data['dropped_increased'])
    self.assertTrue(bo_data['routed_increased'])
    self.assertTrue(bo_data['post_recovery_diagnostics']['healthy'])

    audit_res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 45,
        'method': 'tools/call',
        'params': {
            'name': 'matter_audit_cross_layer_contract',
            'arguments': {
                'home_id': live_home_id,
            },
        },
    })
    self.assertFalse(audit_res['result']['isError'])
    audit_data = json.loads(audit_res['result']['content'][0]['text'])
    self.assertEqual(audit_data['status'], 'fulfilled')
    checks = audit_data['contract_checks']
    self.assertTrue(checks['hci_medium_attached'])
    self.assertTrue(checks['wifi_ap_active'])
    self.assertTrue(checks['controller_isolation'])
    self.assertTrue(checks['descriptor_formats_audited'])
    self.assertTrue(checks['packet_captures_present'])
    self.assertTrue(checks['oracles_pass'])

  def test_matter_generate_flow_visualization_and_dissector(self):
    # Record mock events in server
    self.server.record_flow_event({
        'layer': 'BT',
        'src': 'HCI_HOST',
        'dst': 'HCI_CTRL',
        'summary': 'HCI_RESET Command',
        'details': {'opcode': '0x0C03'},
    })
    self.server.record_flow_event({
        'layer': 'MATTER',
        'src': 'Controller',
        'dst': 'Accessory',
        'summary': 'PBKDFParamRequest',
        'details': {'session_id': 1},
        'color': '#9333ea',
    })
    out_html = os.path.join(self.temp_dir, 'custom_studio.html')
    out_json = os.path.join(self.temp_dir, 'custom_trace.json')

    res = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 50,
        'method': 'tools/call',
        'params': {
            'name': 'matter_generate_flow_visualization',
            'arguments': {
                'output_path': out_html,
                'json_output_path': out_json,
                'title': 'Test Studio',
            },
        },
    })
    self.assertFalse(res['result']['isError'])
    data = json.loads(res['result']['content'][0]['text'])
    self.assertEqual(data['status'], 'success')
    self.assertTrue(os.path.exists(out_html))
    self.assertTrue(os.path.exists(out_json))
    self.assertGreaterEqual(data['total_events'], 2)
    self.assertIn('MATTER', data['layer_counts'])
    self.assertIn('BT', data['layer_counts'])
    with open(out_html, 'r', encoding='utf-8') as f:
      self.assertIn('id="filter-btn-MATTER"', f.read())

    # Test Matter dissector directly on FlowEvent
    from cirque.pcap.flow_visualizer import FlowEvent
    ev_test = FlowEvent(
        event_id='test_ev',
        timestamp=1.0,
        layer='BT',
        sublayer='ACL',
        stage='GATT_CONNECT',
        src='Controller',
        dst='Accessory',
        title='PBKDFParamRequest',
        summary='Matter PBKDFParamRequest over BLE',
        details={},
    )
    dissected = matter_flow_plugin.matter_stage_dissector(ev_test)
    self.assertIsNotNone(dissected)
    self.assertEqual(dissected.layer, 'MATTER')
    self.assertEqual(dissected.stage, 'PASE_HANDSHAKE')
    self.assertIn('PASE SPAKE2+', dissected.encapsulation)

  def test_brownout_recovery_uncommissioned_negative_control_and_duration(self):
    """Verifies Items 3, 4, 5: brownout logcat, negative control, duration."""
    # Item 3 & Item 5: Uncommissioned home + brownout_duration_sec=0.15
    t0 = time.monotonic()
    res_uncomm = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 901,
        'method': 'tools/call',
        'params': {
            'name': 'matter_exercise_ble_brownout_recovery',
            'arguments': {
                'home_id': 'home_uncommissioned_brownout',
                'device_id': 'android_emu',
                'brownout_duration_sec': 0.15,
            },
        },
    })
    elapsed = time.monotonic() - t0
    self.assertFalse(res_uncomm['result']['isError'])
    uncomm_data = json.loads(res_uncomm['result']['content'][0]['text'])
    self.assertTrue(uncomm_data['synthetic_home'])
    self.assertGreaterEqual(elapsed, 0.14)
    self.assertGreaterEqual(
        uncomm_data['measured_brownout_duration_sec'], 0.14
    )
    base_oracles = uncomm_data['baseline']['oracles']
    post_oracles = uncomm_data['post_recovery_diagnostics']['oracles']
    self.assertFalse(base_oracles['matter_pase_established'])
    self.assertFalse(base_oracles['matter_case_established'])
    self.assertFalse(post_oracles['matter_pase_established'])
    self.assertFalse(post_oracles['matter_case_established'])
    self.assertTrue(uncomm_data['dropped_increased'])
    self.assertTrue(uncomm_data['routed_increased'])
    self.assertTrue(uncomm_data['h4_exchange_ok'])

    # Item 4: Negative control with restore_after_brownout=False
    res_neg = self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 902,
        'method': 'tools/call',
        'params': {
            'name': 'matter_exercise_ble_brownout_recovery',
            'arguments': {
                'home_id': 'home_matter_1',
                'device_id': 'android_emu',
                'brownout_duration_sec': 0.02,
                'restore_after_brownout': False,
            },
        },
    })
    self.assertFalse(res_neg['result']['isError'])
    neg_data = json.loads(res_neg['result']['content'][0]['text'])
    self.assertTrue(neg_data['dropped_increased'])
    self.assertFalse(neg_data['h4_exchange_ok'])
    self.assertIsNotNone(neg_data['h4_error'])
    self.assertFalse(neg_data['routed_increased'])
    self.assertFalse(neg_data['post_recovery_diagnostics']['healthy'])
    self.assertFalse(neg_data['healthy'])

    # Restore clean state after negative control
    self.server.handle_message({
        'jsonrpc': '2.0',
        'id': 903,
        'method': 'tools/call',
        'params': {
            'name': 'cirque_inject_rf_fault',
            'arguments': {'recipe': 'clear_all_faults'},
        },
    })


class TestMatterFlowPlugin(unittest.TestCase):
  """Matter classification on top of the protocol-agnostic Cirque core."""

  def setUp(self):
    self.temp_dir = tempfile.mkdtemp(prefix='matter_flow_plugin_test_')
    self._write_pcaps()

  def tearDown(self):
    shutil.rmtree(self.temp_dir, ignore_errors=True)

  def _write_pcaps(self):
    from cirque.capabilities.pcapcapability import (
        DLT_BLUETOOTH_HCI_H4_WITH_PHDR,
        DLT_BLUETOOTH_LE_LL_WITH_PHDR,
        DLT_EN10MB,
        PcapWriter,
        build_le_ll_phdr_frame,
    )

    def acl(att_pdu):
      l2cap = struct.pack('<HH', len(att_pdu), 0x0004) + att_pdu
      return b'\x02' + struct.pack('<HH', 0x40, len(l2cap)) + l2cap

    writer = PcapWriter(
        os.path.join(self.temp_dir, 'bt_hci_0.pcap'),
        dlt=DLT_BLUETOOTH_HCI_H4_WITH_PHDR,
    )
    host, ctrl = struct.pack('>I', 0), struct.pack('>I', 1)
    writer.write_frame(host + acl(bytes.fromhex('120b00656c0400')), 10.0)
    writer.write_frame(ctrl + acl(bytes.fromhex('1d0d00656c04')), 10.1)
    writer.write_frame(host + acl(bytes.fromhex('120b000102')), 10.2)
    writer.write_frame(host + acl(bytes.fromhex('520b006c6505aa')), 10.3)
    writer.close()

    service_data = bytes.fromhex('0b16f6ff000f0ff1ff018000')
    writer = PcapWriter(
        os.path.join(self.temp_dir, 'bt_medium.pcap'),
        dlt=DLT_BLUETOOTH_LE_LL_WITH_PHDR,
    )
    writer.write_frame(
        build_le_ll_phdr_frame(
            'ADV_IND', '11:22:33:44:55:66', payload=service_data
        ),
        9.0,
    )
    writer.close()

    def ipv4_udp(sport, dport):
      udp = struct.pack('>HHHH', sport, dport, 8, 0)
      ip = (
          bytes([0x45, 0])
          + struct.pack('>H', 20 + len(udp))
          + bytes(4)
          + bytes([64, 17, 0, 0, 10, 0, 0, 1, 10, 0, 0, 2])
      )
      return bytes(12) + b'\x08\x00' + ip + udp

    writer = PcapWriter(
        os.path.join(self.temp_dir, 'wifi.pcap'), dlt=DLT_EN10MB
    )
    writer.write_frame(ipv4_udp(51000, 5540), 11.0)
    writer.write_frame(ipv4_udp(5353, 5353), 11.1)
    writer.close()

    # A Thread frame whose payload happens to contain the BTP magic.
    writer = PcapWriter(os.path.join(self.temp_dir, 'thread.pcap'), dlt=195)
    writer.write_frame(bytes.fromhex('61dc02656c0411'), 12.0)
    writer.close()

  def test_core_alone_emits_no_matter_layer(self):
    from cirque.pcap.flow_visualizer import dissect_pcap_directory

    events = dissect_pcap_directory(self.temp_dir)
    self.assertEqual(len(events), 8)
    self.assertNotIn('MATTER', {ev.layer for ev in events})

  def test_plugin_classifies_matter_traffic(self):
    from cirque.pcap.flow_visualizer import (
        dissect_pcap_directory,
        merge_and_sort_flow_events,
    )

    events = merge_and_sort_flow_events(
        dissect_pcap_directory(self.temp_dir),
        [],
        dissectors=matter_flow_plugin.FLOW_DISSECTORS,
    )
    titles = [(ev.layer, ev.title) for ev in events]
    self.assertEqual(
        titles,
        [
            ('MATTER', 'Matter BLE Adv (0xFFF6)'),
            ('MATTER', 'Matter BTP v4 Handshake'),
            ('BT', 'ATT Indication/Notify (0x000d)'),
            ('BT', 'ATT Write (Handle 0x000b)'),
            ('MATTER', 'Matter BTP v5 Handshake'),
            ('MATTER', 'Matter UDP :5540 Message'),
            ('WIFI', 'mDNS Service Discovery'),
            ('THREAD', 'Thread 802.15.4 Transmission'),
        ],
    )
    btp_v4, indication, _, btp_v5, udp, mdns, thread = events[1:]
    self.assertEqual(
        btp_v4.replay_mcp_call['arguments'], {'cmd': 'chiptool btp handshake'}
    )
    self.assertEqual(btp_v4.sublayer, 'BTP')
    self.assertEqual(btp_v4.encapsulation.count('Matter BTP v4'), 1)
    self.assertEqual(btp_v5.encapsulation.count('Matter BTP v5'), 1)
    self.assertIn('Matter BTP Indication', indication.encapsulation)
    self.assertEqual(udp.stage, 'CASE_CONNECT')
    self.assertEqual(mdns.stage, 'NETWORK_CONFIG')
    self.assertEqual(
        (thread.sublayer, thread.stage), ('802.15.4', 'NETWORK_CONFIG')
    )

  def test_packet_dissector_ignores_recorded_events(self):
    from cirque.pcap.flow_visualizer import merge_and_sort_flow_events

    recorded = [
        {
            'event_id': 'udp',
            'timestamp': 1.0,
            'layer': 'WIFI',
            'sublayer': 'UDP',
            'title': 'recorded udp',
            'details': {'src_port': 1234, 'dst_port': 5540},
        },
        {
            'event_id': 'att',
            'timestamp': 2.0,
            'layer': 'BT',
            'sublayer': 'ATT',
            'title': 'recorded att',
            'details': {'att_opcode': '0x12', 'att_value_hex': '656c04'},
        },
    ]
    events = merge_and_sort_flow_events(
        [], recorded, dissectors=matter_flow_plugin.FLOW_DISSECTORS
    )
    self.assertEqual(
        [(ev.layer, ev.sublayer, ev.title) for ev in events],
        [('WIFI', 'UDP', 'recorded udp'), ('BT', 'ATT', 'recorded att')],
    )

  def test_stage_dissector_keeps_recorded_btp_version(self):
    from cirque.pcap.flow_visualizer import merge_and_sort_flow_events

    def btp(event_id, title, details=None, encapsulation=None):
      return {
          'event_id': event_id,
          'timestamp': len(event_id),
          'layer': 'BT',
          'sublayer': 'BTP',
          'title': title,
          'details': details or {},
          'encapsulation': encapsulation or [],
      }

    events = merge_and_sort_flow_events(
        [],
        [
            btp('v5', 'BTP handshake', {'att_value_hex': '656c05'}),
            btp('title', 'BTP v6 handshake done'),
            btp('none', 'BTP session up'),
            btp('dup', 'BTP v5', {'version': 5}, ['Matter BTP v5']),
        ],
        dissectors=matter_flow_plugin.FLOW_DISSECTORS,
    )
    by_id = {ev.event_id: ev for ev in events}
    self.assertEqual(
        {k: (ev.layer, ev.title) for k, ev in by_id.items()},
        {
            'v5': ('MATTER', 'Matter BTP v5 Handshake'),
            'title': ('MATTER', 'Matter BTP v6 Handshake'),
            'none': ('MATTER', 'BTP session up'),
            'dup': ('MATTER', 'Matter BTP v5 Handshake'),
        },
    )
    self.assertEqual(by_id['v5'].encapsulation, ['Matter BTP v5'])
    self.assertEqual(by_id['none'].encapsulation, ['Matter BTP'])
    self.assertEqual(by_id['dup'].encapsulation, ['Matter BTP v5'])
    self.assertNotIn('v4', ' '.join(ev.title for ev in events))

  def test_malformed_att_value_does_not_break_later_events(self):
    from cirque.pcap.flow_visualizer import FlowEvent, merge_and_sort_flow_events

    def pcap_event(event_id, timestamp, sublayer, details):
      return FlowEvent(
          event_id=event_id,
          timestamp=timestamp,
          layer='BT',
          sublayer=sublayer,
          stage='GATT_CONNECT',
          src='a',
          dst='b',
          title=event_id,
          summary='',
          details=dict(details, pcap_file='x.pcap'),
      )

    events = merge_and_sort_flow_events(
        [
            pcap_event(
                'bad', 1.0, 'ATT', {'att_opcode': '0x12', 'att_value_hex': 'zz'}
            ),
            pcap_event('udp', 2.0, 'UDP', {'dst_port': 5540}),
        ],
        [],
        dissectors=matter_flow_plugin.FLOW_DISSECTORS,
    )
    self.assertEqual([ev.layer for ev in events], ['BT', 'MATTER'])
    self.assertEqual(
        events[1].summary, 'Operational Matter Message None:None -> None:5540'
    )

  def test_cirque_cli_plugin(self):
    from cirque.pcap import flow_visualizer

    out_html = os.path.join(self.temp_dir, 'studio.html')
    out_json = os.path.join(self.temp_dir, 'trace.json')
    flow_visualizer.main([
        '--pcap-dir',
        self.temp_dir,
        '--output',
        out_html,
        '--json-output',
        out_json,
        '--plugin',
        matter_flow_plugin.__file__,
    ])
    with open(out_json, 'r', encoding='utf-8') as f:
      layers = [ev['layer'] for ev in json.load(f)]
    self.assertEqual(layers.count('MATTER'), 4)
    with open(out_html, 'r', encoding='utf-8') as f:
      html_content = f.read()
    self.assertIn('id="filter-btn-MATTER"', html_content)
    self.assertIn('Matter (4)', html_content)

  def test_parse_log_directory_and_matter_log_dissector(self):
    from cirque.pcap import flow_visualizer

    log_dir = os.path.join(self.temp_dir, 'logs_only')
    os.makedirs(log_dir, exist_ok=True)
    log_path = os.path.join(log_dir, 'CHIPEndDevice-1791839340-96d0f9ac.log')
    with open(log_path, 'w', encoding='utf-8') as f:
      f.write(
          '2026-10-12 21:08:24,205 [root] INFO Will run command:'
          ' otbr-agent -I wpan0\n'
          'otbr-agent[46]: 00:00:08.532 [I] Mle-----------: Role detached ->'
          ' leader\n'
          '[1791839310.100] [64:64] [BLE] BLE advertisement started\n'
          '[1791839312.200] [64:64] [EM] <<< [E:20892r S:0 M:226165355] (U)'
          ' Msg RX from 0:B22D9B353E5F8AB1 [0000] to 0:0000000000000000 [BLE]'
          ' --- Type 0000:20 (SecureChannel:PBKDFParamRequest) (B:94)\n'
          '[1791839315.300] [64:64] [EM] <<< [E:20893r S:0 M:226165360] (U)'
          ' Msg RX from 0:B22D9B353E5F8AB1 [0000] to 0:0000000000000000 [UDP]'
          ' --- Type 0000:30 (SecureChannel:CASE_Sigma1) (B:196)\n'
          '[1791839318.400] [64:64] [EM] <<< [E:20894r S:1234 M:226165370]'
          ' (S) Msg RX from 1:000000000001B669 [4B53] to 1:0000000012344321'
          ' [UDP] --- Type 0001:08 (IM:InvokeCommandRequest) (B:64)\n'
          '[1791839319.500] [64:64] [SVR] Commissioning completed session'
          ' establishment step\n'
      )

    parsed = matter_flow_plugin.parse_log_directory(log_dir)
    self.assertEqual(len(parsed), 7)
    stages = {ev.stage for ev in parsed}
    self.assertIn('PASE_HANDSHAKE', stages)
    self.assertIn('CASE_CONNECT', stages)
    self.assertIn('CLUSTER_CONTROL', stages)
    self.assertIn('OPERATIONAL_CERT_ISSUED', stages)

    out_html = os.path.join(log_dir, 'studio.html')
    out_json = os.path.join(log_dir, 'trace.json')
    flow_visualizer.main([
        '--pcap-dir',
        log_dir,
        '--output',
        out_html,
        '--json-output',
        out_json,
        '--plugin',
        matter_flow_plugin.__file__,
        '--require-non-empty',
    ])
    with open(out_json, 'r', encoding='utf-8') as f:
      trace = json.load(f)
    self.assertEqual(len(trace), 7)
    self.assertEqual(sum(1 for ev in trace if ev['layer'] == 'MATTER'), 4)


if __name__ == '__main__':
  unittest.main()


