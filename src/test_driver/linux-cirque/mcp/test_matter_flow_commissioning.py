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
"""Matter commissioning flow regression tests (AndroidBleThreadMobileDevice).

Fixtures are short verbatim excerpts of one AndroidBleThreadMobileDeviceTest
CI run: UDP payload prefixes of the Sigma1/2/3 frames in wifi_medium.pcap
(Android 10.0.1.21 <-> border router proxy 10.0.1.20), CHIPEndDevice log
lines and Android logcat lines.
"""

import json
import os
import re
import shutil
import struct
import sys
import tempfile
import unittest

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CIRQUE_ROOT = os.path.abspath(
    os.path.join(
        SCRIPT_DIR, '..', '..', '..', '..', 'third_party', 'cirque', 'repo'
    )
)
if CIRQUE_ROOT not in sys.path:
  sys.path.insert(0, CIRQUE_ROOT)

from cirque.capabilities.pcapcapability import DLT_EN10MB, PcapWriter  # noqa: E402
from cirque.pcap import flow_visualizer as fv  # noqa: E402
import matter_flow_plugin  # noqa: E402

SIGMA1 = bytes.fromhex('040000000662620e6f2c963497cf112205304015')
SIGMA2 = bytes.fromhex('01000000772ebd0a6f2c963497cf112206314015')
SIGMA3 = bytes.fromhex('040000000862620e6f2c963497cf112207324015')

DEVICE_LOG = 'CHIPEndDevice-1791928996-12fa6692.log'
DEVICE_DIAG_LOG = 'CHIPEndDevice-1791928996-12fa6692-thread-diag.log'
TBR_DIAG_LOG = 'ThreadBorderRouter-1791928996-431c84d3-thread-diag.log'
LOGCAT_LOG = 'android_emulator-1791928996-e3795874-logcat.log'
DEVICE_LINES = [
    '[1791928973.761] [164:164] [DMG] Received command for Endpoint=0 '
    'Cluster=0x0000_0030 Command=0x0000_0000',
    '[1791928975.270] [164:164] [EM] >>> [E:5440r S:0 M:241328646] (U) Msg'
    ' RX from 0:2211CF9734962C6F [0000] to 0000000000000000 --- Type'
    ' 0000:30 (SecureChannel:CASE_Sigma1) (B:203)',
    '[1791928975.282] [164:164] [EM] <<< [E:5440r S:0 M:180170359 '
    '(Ack:241328646)] (U) Msg TX from 0000000000000000 to '
    '0:2211CF9734962C6F [0000] [UDP:[fd11:33::1]:5542] --- Type 0000:31 '
    '(SecureChannel:CASE_Sigma2) (B:524)',
    '[1791928975.339] [164:164] [EM] >>> [E:5440r S:0 M:241328648 '
    '(Ack:180170359)] (U) Msg RX from 0:2211CF9734962C6F [0000] to '
    '0000000000000000 --- Type 0000:32 (SecureChannel:CASE_Sigma3) (B:364)',
]
LOGCAT_LINES = [
    "10-13 22:02:53.706  4732  4895 I CTL     : Performing next"
    " commissioning step 'SendNOC'",
    '10-13 22:02:55.071  4732  4732 I EM      : <<< [E:5440i S:0 '
    'M:241328646] (U) Msg TX from 2211CF9734962C6F to 0:0000000000000000 '
    '[0000] [UDP:10.0.1.20:5540] --- Type 0000:30 '
    '(SecureChannel:CASE_Sigma1) (B:203)',
]
TBR_PROXY_LINES = [
    '=== cat /tmp/tbr_border_proxy.log ===',
    '[TBR-Proxy] 1791928975.263 FWD wlan0->wpan0 10.0.1.21:45633 ->'
    ' [fd11:33::2]:5540 len=211 ctr=241328646',
    '[TBR-Proxy] 1791928975.284 FWD wpan0->wlan0 [fd11:33::2]:5540 ->'
    ' 10.0.1.21:45633 len=532 ctr=180170359',
]


def _eth_ipv4_udp(src, sport, dst, dport, payload, src_mac):
  udp = struct.pack('>HHHH', sport, dport, 8 + len(payload), 0) + payload
  ip = struct.pack(
      '>BBHHHBBH4s4s',
      0x45,
      0,
      20 + len(udp),
      0,
      0,
      64,
      17,
      0,
      bytes(map(int, src.split('.'))),
      bytes(map(int, dst.split('.'))),
  )
  return bytes.fromhex('020000000101') + src_mac + b'\x08\x00' + ip + udp


def _write(path, lines):
  with open(path, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')


class MatterHeaderTest(unittest.TestCase):

  def test_unsecured_session_headers_decode(self):
    # Session 0 frames keep the exchange header in cleartext.
    for buf, opcode, counter in (
        (SIGMA1, 0x30, 241328646),
        (SIGMA2, 0x31, 180170359),
        (SIGMA3, 0x32, 241328648),
    ):
      msg = matter_flow_plugin.decode_matter_message(buf + bytes(8))
      self.assertEqual(msg['session_id'], 0)
      self.assertEqual(msg['message_counter'], counter)
      self.assertEqual((msg['protocol_id'], msg['opcode']), (0, opcode))
      self.assertEqual(msg['exchange_id'], 5440)
    sigma2 = matter_flow_plugin.decode_matter_message(SIGMA2 + bytes(8))
    self.assertEqual(sigma2['dst_node_id'], '2211CF9734962C6F')

  def test_secured_and_invalid_payloads(self):
    secured = struct.pack('<BHBI', 0x00, 0xEC5C, 0x00, 77) + bytes(16)
    msg = matter_flow_plugin.decode_matter_message(secured)
    self.assertEqual((msg['session_id'], msg['message_counter']), (60508, 77))
    self.assertNotIn('opcode', msg)
    self.assertIsNone(matter_flow_plugin.decode_matter_message(b'\x40' * 12))
    self.assertIsNone(matter_flow_plugin.decode_matter_message(b'\x04'))

  def test_pcap_udp_5540_is_named_from_the_header(self):
    raw = _eth_ipv4_udp(
        '10.0.1.21', 45633, '10.0.1.20', 5540, SIGMA1 + bytes(183),
        bytes.fromhex('0215b2000000'),
    )
    ev = fv.dissect_wifi_packet(1791928975.262721, raw, 'wifi_medium.pcap', 196)
    ev = matter_flow_plugin.matter_packet_dissector(ev)
    self.assertEqual(ev.layer, 'MATTER')
    self.assertEqual(ev.title, 'Matter SecureChannel:CASE_Sigma1')
    self.assertEqual(ev.stage, 'CASE_CONNECT')
    key = fv.matter_message_key(241328646, 5440, 0, 0x30)
    self.assertEqual(ev.details['matter_msg_key'], key)
    self.assertEqual(ev.details['dedup_key'], 'MATTER:' + key)

  def test_thread_hop_keeps_the_thread_layer(self):
    ev = fv.FlowEvent(
        event_id='t',
        timestamp=1.0,
        layer='THREAD',
        sublayer='UDP',
        stage='NETWORK_CONFIG',
        src='fd11:33::1',
        dst='fd11:33::2',
        title='Thread UDP :5542->:5540',
        summary='',
        details={
            'pcap_file': 'thread_0.pcap',
            'src_port': 5542,
            'dst_port': 5540,
            'udp_payload_hex': (SIGMA1 + bytes(8)).hex(),
        },
    )
    ev = matter_flow_plugin.matter_packet_dissector(ev)
    self.assertEqual(ev.layer, 'THREAD')
    self.assertEqual(ev.title, 'Matter SecureChannel:CASE_Sigma1 (Thread)')
    self.assertEqual(ev.details['evidence'], 'wire')

  def test_hci_captures_are_host_hci_and_medium_captures_are_wire(self):
    for pcap_file, tier in (
        ('bt_hci_hci0.pcap', 'host_hci'),
        ('bt_hci_android_hci0.pcap', 'host_hci'),
        ('/tmp/logs/bt_hci_hci1.pcap', 'host_hci'),
        ('bt_medium.pcap', 'wire'),
        ('wifi_medium.pcap', 'wire'),
        ('thread_0.pcap', 'wire'),
    ):
      ev = fv.FlowEvent(
          event_id='b',
          timestamp=1.0,
          layer='BLE',
          sublayer='ATT',
          stage='GATT_CONNECT',
          src='a',
          dst='b',
          title='ATT Write',
          summary='',
          details={
              'pcap_file': pcap_file,
              'att_opcode': '0x12',
              'att_value_hex': '656c04',
          },
      )
      ev = matter_flow_plugin.matter_packet_dissector(ev)
      self.assertEqual(ev.details['evidence'], tier, pcap_file)
      self.assertEqual(ev.title, 'Matter BTP v4 Handshake')


class MatterLogTest(unittest.TestCase):

  def setUp(self):
    self.temp_dir = tempfile.mkdtemp(prefix='matter_flow_commissioning_')

  def tearDown(self):
    shutil.rmtree(self.temp_dir, ignore_errors=True)

  def test_commissioning_commands_get_matter_stages(self):
    _write(os.path.join(self.temp_dir, DEVICE_LOG), DEVICE_LINES[:1])
    _write(os.path.join(self.temp_dir, LOGCAT_LOG), LOGCAT_LINES[:1])
    events = matter_flow_plugin.parse_log_directory(self.temp_dir)
    self.assertEqual(
        [(ev.layer, ev.stage, ev.title) for ev in events],
        [
            (
                'MATTER',
                'FAILSAFE_ARMED',
                'GeneralCommissioning:ArmFailSafe (RX)',
            ),
            (
                'MATTER',
                'OPERATIONAL_CERT_ISSUED',
                'OperationalCredentials:AddNOC (TX)',
            ),
        ],
    )

  def test_thread_diag_copy_and_full_logcat_are_not_duplicated(self):
    _write(os.path.join(self.temp_dir, DEVICE_LOG), DEVICE_LINES)
    _write(
        os.path.join(self.temp_dir, DEVICE_DIAG_LOG),
        ['=== cat /tmp/chip-all-clusters.log ==='] + DEVICE_LINES,
    )
    _write(os.path.join(self.temp_dir, LOGCAT_LOG), LOGCAT_LINES)
    full_logcat = LOGCAT_LOG.replace('logcat', 'logcat-full')
    _write(os.path.join(self.temp_dir, full_logcat), LOGCAT_LINES)
    events = matter_flow_plugin.parse_log_directory(self.temp_dir)
    self.assertEqual(len(events), len(DEVICE_LINES) + len(LOGCAT_LINES))
    self.assertEqual(
        {ev.details['log_file'] for ev in events}, {DEVICE_LOG, LOGCAT_LOG}
    )

  def test_tbr_proxy_forward_lines_become_relay_hops(self):
    _write(os.path.join(self.temp_dir, TBR_DIAG_LOG), TBR_PROXY_LINES)
    events = matter_flow_plugin.parse_log_directory(self.temp_dir)
    self.assertEqual(
        [(ev.layer, ev.dst, ev.title) for ev in events],
        [
            ('THREAD', 'fd11:33::2', 'TBR proxy wlan0->wpan0 (211B)'),
            ('WIFI', '10.0.1.21', 'TBR proxy wpan0->wlan0 (532B)'),
        ],
    )
    self.assertEqual(
        events[0].details['lane_aliases'], {'10.0.1.21': 0, 'fd11:33::2': 3}
    )
    self.assertEqual(events[0].details['matter_message_counter'], 241328646)
    self.assertEqual(events[0].details['evidence'], 'relay')


class CommissioningFlowEndToEndTest(unittest.TestCase):
  """Flow visualizer CLI on a minimal AndroidBleThread capture directory."""

  def setUp(self):
    self.temp_dir = tempfile.mkdtemp(prefix='matter_flow_e2e_')
    phone, ap = bytes.fromhex('0215b2000000'), bytes.fromhex('020000000101')
    frames = [
        (1791928975.2627, SIGMA1, '10.0.1.21', 45633, '10.0.1.20', 5540, phone),
        (1791928975.2629, SIGMA1, '10.0.1.21', 45633, '10.0.1.20', 5540, ap),
        (1791928975.2903, SIGMA2, '10.0.1.20', 5540, '10.0.1.21', 45633, ap),
        (1791928975.3341, SIGMA3, '10.0.1.21', 45633, '10.0.1.20', 5540, phone),
    ]
    for name in ('wifi_medium.pcap', 'wifi_wifi2.pcap'):
      writer = PcapWriter(os.path.join(self.temp_dir, name), dlt=DLT_EN10MB)
      for ts, head, src, sport, dst, dport, mac in frames:
        writer.write_frame(
            _eth_ipv4_udp(src, sport, dst, dport, head + bytes(60), mac), ts
        )
      writer.close()
    _write(os.path.join(self.temp_dir, DEVICE_LOG), DEVICE_LINES)
    _write(os.path.join(self.temp_dir, LOGCAT_LOG), LOGCAT_LINES)

  def tearDown(self):
    shutil.rmtree(self.temp_dir, ignore_errors=True)

  def test_sigma_messages_deduplicated_joined_and_in_default_view(self):
    out_html = os.path.join(self.temp_dir, 'studio.html')
    out_json = os.path.join(self.temp_dir, 'trace.json')
    fv.main([
        '--pcap-dir', self.temp_dir,
        '--output', out_html,
        '--json-output', out_json,
        '--plugin', matter_flow_plugin.__file__,
        '--require-non-empty',
    ])
    with open(out_json, encoding='utf-8') as f:
      trace = json.load(f)
    pcap_sigmas = [
        ev for ev in trace if ev['details'].get('pcap_file')
        and 'Sigma' in ev['title']
    ]
    # 8 capture copies (2 files x AP relay) collapse into one per message.
    self.assertEqual(
        [ev['title'] for ev in pcap_sigmas],
        [
            'Matter SecureChannel:CASE_Sigma1',
            'Matter SecureChannel:CASE_Sigma2',
            'Matter SecureChannel:CASE_Sigma3',
        ],
    )
    self.assertEqual(len(pcap_sigmas[0]['details']['duplicates']), 3)
    # The capture of Sigma1 is joined with the controller TX and device RX.
    self.assertEqual(len(pcap_sigmas[0]['details']['joined_events']), 2)
    with open(out_html, encoding='utf-8') as f:
      page = f.read()
    initial = int(re.search(r'const INITIAL_INDEX = (\d+);', page).group(1))
    self.assertIn(trace[initial]['stage'], fv.DEFAULT_FOCUS_STAGES)
    start = max(0, min(initial - 25, len(trace) - 60))
    window = trace[start : start + 60]
    self.assertTrue(any('Sigma1' in ev['title'] for ev in window))
    self.assertIn('"10.0.1.20": 2', page)
    self.assertIn('"10.0.1.21": 0', page)


if __name__ == '__main__':
  unittest.main()
