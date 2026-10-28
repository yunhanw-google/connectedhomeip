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
"""Matter flow plugin for the Cirque cross-layer flow visualizer.

The Cirque core only dissects Bluetooth, Wi-Fi and Thread link/transport
layers. This plugin classifies Matter traffic from the raw facts the core
keeps on each FlowEvent:
  - BTP handshake in ATT writes (`details['att_value_hex']` starting with
    65 6c / 6c 65) and BTP on ATT indications/notifications,
  - commissionable-node advertisements (`details['ad_service_uuids']`
    containing 0xFFF6),
  - operational messages on UDP port 5540 (`details['src_port']` /
    `details['dst_port']`),
and maps recorded events onto the Matter commissioning stages:
ADV_DISCOVERY, GATT_CONNECT, PASE_HANDSHAKE, FAILSAFE_ARMED, NETWORK_CONFIG,
OPERATIONAL_CERT_ISSUED, CASE_CONNECT, CLUSTER_CONTROL.

Two dissectors keep these concerns apart:
  - matter_packet_dissector only touches events dissected from a capture
    (`'pcap_file' in details`) and classifies them from the raw facts above;
  - matter_stage_dissector only touches recorded (non-capture) events and
    maps them onto stages from their titles.

It follows the Cirque flow plugin contract (FLOW_DISSECTORS / FLOW_LAYERS), so
it works from the Cirque CLI:

  python3 -m cirque.pcap.flow_visualizer --pcap-dir DIR --output OUT.html \
      --plugin src/test_driver/linux-cirque/mcp/matter_flow_plugin.py

and through the Cirque MCP server, where register_cirque_extension() in
matter_cirque_extension.py registers the same dissectors and layers.
"""

import os
import re
import struct
from typing import Any, Dict, List, Optional

from cirque.pcap.flow_visualizer import (
    FlowEvent,
    dissect_log_file as _cirque_dissect_log_file,
    list_log_files,
    matter_message_key,
)

MATTER_LAYER = 'MATTER'
MATTER_BLE_SERVICE_UUID = '0xfff6'
MATTER_UDP_PORT = 5540
BTP_HANDSHAKE_MAGIC = (b'\x65\x6c', b'\x6c\x65')

# SecureChannel protocol opcodes (src/protocols/secure_channel/Constants.h).
SECURE_CHANNEL_OPCODES = {
    0x00: 'MsgCounterSyncReq',
    0x01: 'MsgCounterSyncRsp',
    0x10: 'StandaloneAck',
    0x20: 'PBKDFParamRequest',
    0x21: 'PBKDFParamResponse',
    0x22: 'PASE_Pake1',
    0x23: 'PASE_Pake2',
    0x24: 'PASE_Pake3',
    0x30: 'CASE_Sigma1',
    0x31: 'CASE_Sigma2',
    0x32: 'CASE_Sigma3',
    0x33: 'CASE_Sigma2Resume',
    0x40: 'StatusReport',
    0x50: 'ICD_CheckIn',
}
# Commissioning stage of each commissioning-cluster command.
COMMAND_STAGES = {
    'ArmFailSafe': 'FAILSAFE_ARMED',
    'SetRegulatoryConfig': 'FAILSAFE_ARMED',
    'CommissioningComplete': 'CASE_CONNECT',
    'AddOrUpdateWiFiNetwork': 'WIFI_PROVISIONED',
    'AddOrUpdateThreadNetwork': 'THREAD_PROVISIONED',
    'ConnectNetwork': 'THREAD_PROVISIONED',
    'AttestationRequest': 'OPERATIONAL_CERT_ISSUED',
    'CertificateChainRequest': 'OPERATIONAL_CERT_ISSUED',
    'CSRRequest': 'OPERATIONAL_CERT_ISSUED',
    'AddNOC': 'OPERATIONAL_CERT_ISSUED',
    'AddTrustedRootCertificate': 'OPERATIONAL_CERT_ISSUED',
}
# Forwarding lines of the AndroidBleThreadMobileDeviceTest border router
# proxy: '[TBR-Proxy] <epoch> FWD <in>-><out> <src> -> <dst> len=<n> ctr=<c>'.
_TBR_PROXY_FWD_RE = re.compile(
    r'\[TBR-Proxy\] (\d+\.\d+) FWD (\w+)->(\w+) (\S+) -> (\S+) len=(\d+)'
    r'(?: ctr=(\d+))?'
)


def _hex_to_bytes(value: Any) -> bytes:
  """Decodes a hex string, treating malformed input as empty."""
  try:
    return bytes.fromhex(str(value or ''))
  except ValueError:
    return b''


def _recorded_btp_version(event: FlowEvent) -> Optional[int]:
  """Returns the BTP version a recorded event carries, or None if unknown.

  Looks at `details['btp_version']` / `details['version']`, then a BTP
  handshake in `details['att_value_hex']`, then a 'vN' token in the title.
  """
  details = event.details
  for key in ('btp_version', 'version'):
    try:
      return int(details[key])
    except (KeyError, TypeError, ValueError):
      pass
  att_value = _hex_to_bytes(details.get('att_value_hex'))
  if len(att_value) >= 3 and att_value[:2] in BTP_HANDSHAKE_MAGIC:
    return att_value[2]
  match = re.search(r'\bv(\d+)\b', event.title or '')
  return int(match.group(1)) if match else None


def capture_evidence_tier(pcap_file: str) -> str:
  """Returns the evidence tier of an event dissected from `pcap_file`.

  bt_hci_<adapter>.pcap files are host-side HCI captures (host <-> virtual
  controller), so they are 'host_hci'. Medium and sniffer captures
  (bt_medium.pcap, wifi_medium.pcap, Thread/Ethernet captures, ...) are
  'wire'.
  """
  if os.path.basename(str(pcap_file)).startswith('bt_hci_'):
    return 'host_hci'
  return 'wire'


def matter_packet_dissector(event: FlowEvent) -> Optional[FlowEvent]:
  """Re-labels captured link/transport events that carry Matter traffic."""
  details = event.details
  if 'pcap_file' not in details:
    return None
  details['evidence'] = capture_evidence_tier(details['pcap_file'])
  att_opcode = details.get('att_opcode')
  if att_opcode in ('0x12', '0x52'):  # ATT Write Req / Write Cmd
    att_value = _hex_to_bytes(details.get('att_value_hex'))
    if len(att_value) >= 3 and att_value[:2] in BTP_HANDSHAKE_MAGIC:
      version = att_value[2]
      event.layer = MATTER_LAYER
      event.sublayer = 'BTP'
      event.title = f'Matter BTP v{version} Handshake'
      event.summary = f'Matter BTP handshake protocol version {version}'
      event.stage = 'GATT_CONNECT'
      event.encapsulation.append(f'Matter BTP v{version}')
      event.replay_mcp_call = {
          'tool': 'cirque_execute_device_cmd',
          'arguments': {'cmd': 'chiptool btp handshake'},
      }
  elif att_opcode in ('0x1d', '0x1b'):  # ATT Indication / Notification
    event.encapsulation.append('Matter BTP Indication')

  if details.get('pdu_type') == 'ADV_IND' and (
      MATTER_BLE_SERVICE_UUID in details.get('ad_service_uuids', [])
  ):
    event.layer = MATTER_LAYER
    event.title = 'Matter BLE Adv (0xFFF6)'
    event.summary = (
        'Matter Commissionable Node Advertisement (0xFFF6) on Ch'
        f' {event.channel}'
    )
    event.encapsulation.append('Matter Service 0xFFF6')
    event.replay_mcp_call = {
        'tool': 'cirque_get_radio_state',
        'arguments': {'home_id': 'current'},
    }

  if event.sublayer == 'UDP' and MATTER_UDP_PORT in (
      details.get('src_port'),
      details.get('dst_port'),
  ):
    _classify_matter_udp(event)
  return event


def decode_matter_message(payload: bytes) -> Optional[Dict[str, Any]]:
  """Decodes a Matter message header (Matter Core spec 4.4).

  For the unsecured session (session ID 0, unicast) the exchange header is
  cleartext too, so the protocol, opcode and exchange ID are returned.
  Returns None if `payload` is not a version 0 Matter message.
  """
  if len(payload) < 8:
    return None
  flags, session_id, sec_flags, counter = struct.unpack_from('<BHBI', payload)
  if flags >> 4 or flags & 0x03 == 0x03 or sec_flags & 0x80:
    return None  # unknown version, reserved DSIZ or privacy-obfuscated
  info: Dict[str, Any] = {'session_id': session_id, 'message_counter': counter}
  pos = 8
  try:
    if flags & 0x04:
      info['src_node_id'] = f'{struct.unpack_from("<Q", payload, pos)[0]:016X}'
      pos += 8
    if flags & 0x03 == 1:
      info['dst_node_id'] = f'{struct.unpack_from("<Q", payload, pos)[0]:016X}'
      pos += 8
    elif flags & 0x03 == 2:
      info['dst_group_id'] = struct.unpack_from('<H', payload, pos)[0]
      pos += 2
    if sec_flags & 0x20:  # Message extensions
      pos += 2 + struct.unpack_from('<H', payload, pos)[0]
    if session_id != 0 or sec_flags & 0x03:
      return info  # The exchange header is encrypted.
    ex_flags, opcode, exchange_id = struct.unpack_from('<BBH', payload, pos)
    pos += 4
    if ex_flags & 0x10:  # Vendor ID present
      pos += 2
    info['protocol_id'] = struct.unpack_from('<H', payload, pos)[0]
    info.update(opcode=opcode, exchange_id=exchange_id)
    if ex_flags & 0x02:
      info['ack_counter'] = struct.unpack_from('<I', payload, pos + 2)[0]
  except struct.error:  # Truncated capture: keep the fields decoded so far.
    info['truncated'] = True
  return info


def _udp_payload(event: FlowEvent) -> bytes:
  """Returns the UDP payload the core recorded for a capture event."""
  details = event.details
  if 'udp_payload_hex' in details:
    return _hex_to_bytes(details['udp_payload_hex'])
  offset = details.get('udp_payload_offset')
  if isinstance(offset, int):
    return _hex_to_bytes(event.payload_hex)[offset:]
  return b''


def _classify_matter_udp(event: FlowEvent) -> None:
  """Names a captured UDP 5540 message from its Matter headers."""
  if event.details.get('decrypted'):
    return
  details = event.details
  on_thread = event.layer == 'THREAD'
  if not on_thread:
    event.layer = MATTER_LAYER
  event.sublayer = 'CASE'
  event.stage = 'CASE_CONNECT'
  if 'src_ip6' in details:
    event.title = 'Matter IPv6 Operational UDP :5540'
    event.summary = 'Matter Secure Message over IPv6 UDP :5540'
  else:
    event.title = 'Matter UDP :5540 Message'
    event.summary = (
        f'Operational Matter Message {details.get("src_ip")}:'
        f'{details.get("src_port")} -> {details.get("dst_ip")}:'
        f'{details.get("dst_port")}'
    )
  if 'Matter Operational (UDP 5540)' not in event.encapsulation:
    event.encapsulation.append('Matter Operational (UDP 5540)')
  msg = decode_matter_message(_udp_payload(event))
  if msg is None:
    return
  for name, value in msg.items():
    details['matter_' + name] = value
  where = f'{event.src} -> {event.dst}'
  if 'opcode' not in msg:
    sid, ctr = msg['session_id'], msg['message_counter']
    event.sublayer = 'Secure'
    event.stage = 'CLUSTER_CONTROL'
    event.title = f'Matter Secure S:{sid} M:{ctr}'
    event.summary = f'Encrypted Matter message S:{sid} M:{ctr} {where}'
    details['dedup_key'] = f'{event.layer}:secure:{sid}:{ctr}:{event.src}'
    return
  if not on_thread:
    event.layer = MATTER_LAYER
  event.sublayer = 'CASE'
  event.stage = 'CASE_CONNECT'
  if 'src_ip6' in details:
    event.title = 'Matter IPv6 Operational UDP :5540'
    event.summary = 'Matter Secure Message over IPv6 UDP :5540'
  else:
    event.title = 'Matter UDP :5540 Message'
    event.summary = (
        f'Operational Matter Message {details.get("src_ip")}:'
        f'{details.get("src_port")} -> {details.get("dst_ip")}:'
        f'{details.get("dst_port")}'
    )
  if 'Matter Operational (UDP 5540)' not in event.encapsulation:
    event.encapsulation.append('Matter Operational (UDP 5540)')
  proto, opcode = msg['protocol_id'], msg['opcode']
  if proto == 0:
    name = 'SecureChannel:' + SECURE_CHANNEL_OPCODES.get(
        opcode, f'0x{opcode:02x}'
    )
  else:
    name = f'Protocol 0x{proto:04x}:0x{opcode:02x}'
  if any(k in name for k in ('PBKDF', 'Pake')):
    event.sublayer, event.stage = 'PASE', 'PASE_HANDSHAKE'
  elif 'StandaloneAck' in name:
    event.sublayer = 'MRP'
  key = matter_message_key(
      msg['message_counter'], msg['exchange_id'], proto, opcode
  )
  details['matter_msg_key'] = key
  details['dedup_key'] = f'{event.layer}:{key}'
  event.title = f'Matter {name}' + (' (Thread)' if on_thread else '')
  event.summary = (
      f'{name} E:{msg["exchange_id"]} M:{msg["message_counter"]} {where}'
  )


# ConnectNetwork carries no network type, so it takes the stage of the last
# AddOrUpdate*Network seen in the same log (Wi-Fi or Thread).
_ADD_NETWORK_STAGES = {
    'AddOrUpdateWiFiNetwork': 'WIFI_PROVISIONED',
    'AddOrUpdateThreadNetwork': 'THREAD_PROVISIONED',
}
_last_network_stage_by_log: Dict[str, str] = {}


def _command_stage(command: str, log_file: str) -> str:
  if command in _ADD_NETWORK_STAGES:
    _last_network_stage_by_log[log_file] = _ADD_NETWORK_STAGES[command]
  elif command == 'ConnectNetwork' and log_file in _last_network_stage_by_log:
    return _last_network_stage_by_log[log_file]
  return COMMAND_STAGES.get(command, 'CLUSTER_CONTROL')


def matter_log_dissector(event: FlowEvent) -> Optional[FlowEvent]:
  """Classifies container .log events that carry Matter exchange or session traffic."""
  details = event.details
  if 'log_file' not in details:
    return None

  chip_tag = details.get('chip_tag')
  command = details.get('matter_command')
  if command:
    event.layer = MATTER_LAYER
    event.stage = _command_stage(command, str(details['log_file']))
    if 'Matter Interaction Model' not in event.encapsulation:
      event.encapsulation.append('Matter Interaction Model')
    return event

  if chip_tag == 'EM':
    proto = str(details.get('matter_protocol') or '')
    msg_type = str(details.get('matter_msg_type') or '')
    event.layer = MATTER_LAYER
    if any(k in msg_type for k in ('PBKDF', 'PASE', 'Pake')):
      event.sublayer = 'PASE'
      event.stage = 'PASE_HANDSHAKE'
      if 'PASE SPAKE2+' not in event.encapsulation:
        event.encapsulation.append('PASE SPAKE2+')
    elif any(k in msg_type for k in ('Sigma', 'CASE')):
      event.sublayer = 'CASE'
      event.stage = 'CASE_CONNECT'
      if 'CASE Sigma1-3' not in event.encapsulation:
        event.encapsulation.append('CASE Sigma1-3')
    elif proto == 'IM' or any(
        k in msg_type
        for k in (
            'Invoke',
            'Read',
            'Write',
            'Subscribe',
            'ReportData',
            'StatusResponse',
            'TimedRequest',
        )
    ):
      event.sublayer = 'IM'
      event.stage = 'CLUSTER_CONTROL'
      if 'Matter Interaction Model' not in event.encapsulation:
        event.encapsulation.append('Matter Interaction Model')
    elif 'StandaloneAck' in msg_type:
      event.sublayer = 'MRP'
    return event

  if chip_tag in ('SC', 'PAS', 'CAS', 'IN', 'SVR'):
    event.layer = MATTER_LAYER
    title_text = f'{event.title} {event.summary}'
    if any(k in title_text for k in ('PBKDF', 'spake2p', 'PASE')):
      event.sublayer = 'PASE'
      event.stage = 'PASE_HANDSHAKE'
      if 'PASE SPAKE2+' not in event.encapsulation:
        event.encapsulation.append('PASE SPAKE2+')
    elif any(k in title_text for k in ('Sigma', 'CASE')):
      event.sublayer = 'CASE'
      event.stage = 'CASE_CONNECT'
      if 'CASE Sigma1-3' not in event.encapsulation:
        event.encapsulation.append('CASE Sigma1-3')
    elif any(k in title_text for k in ('Commissioning', 'Rendezvous')):
      event.sublayer = 'COMMISSIONING'
      event.stage = 'OPERATIONAL_CERT_ISSUED'
      if 'Matter Commissioning' not in event.encapsulation:
        event.encapsulation.append('Matter Commissioning')
    return event

  return event


def matter_stage_dissector(event: FlowEvent) -> Optional[FlowEvent]:
  """Maps recorded (non-capture) events onto Matter commissioning stages."""
  if 'pcap_file' in event.details or 'log_file' in event.details:
    return None

  # Enrich BLE Advertising with 0xFFF6
  if event.layer == 'BT' and '0xFFF6' in event.title:
    event.layer = 'MATTER'
    event.stage = 'ADV_DISCOVERY'
    event.encapsulation.append('Matter Service 0xFFF6')
    event.details['matter_service'] = '0xFFF6'

  # Enrich BTP Handshake
  if event.sublayer == 'BTP':
    event.layer = 'MATTER'
    event.stage = 'GATT_CONNECT'
    version = _recorded_btp_version(event)
    if version is not None:
      event.title = f'Matter BTP v{version} Handshake'
    if not any(e.startswith('Matter BTP') for e in event.encapsulation):
      event.encapsulation.append(
          'Matter BTP' if version is None else f'Matter BTP v{version}'
      )

  # Enrich PASE SPAKE2+
  if 'PBKDF' in event.title or 'Pake' in event.title or 'PASE' in event.title:
    event.layer = 'MATTER'
    event.stage = 'PASE_HANDSHAKE'
    event.encapsulation.append('PASE SPAKE2+')

  # Enrich FailSafe
  if 'ArmFailSafe' in event.title or 'Failsafe' in event.title:
    event.layer = 'MATTER'
    event.stage = 'FAILSAFE_ARMED'
    event.encapsulation.append('GeneralCommissioning')

  # Enrich Network Provisioning
  if 'ThreadNetwork' in event.title:
    event.layer = 'MATTER'
    event.stage = 'THREAD_PROVISIONED'
    event.encapsulation.append('ThreadNetworkSetup')
  elif 'WiFiNetwork' in event.title:
    event.layer = 'MATTER'
    event.stage = 'WIFI_PROVISIONED'
    event.encapsulation.append('WiFiNetworkSetup')

  # Enrich NOC
  if 'NOC' in event.title or 'CSR' in event.title:
    event.layer = 'MATTER'
    event.stage = 'OPERATIONAL_CERT_ISSUED'
    event.encapsulation.append('OperationalCredentials')

  # Enrich CASE
  if 'CASE' in event.title or 'Sigma' in event.title:
    event.layer = 'MATTER'
    event.stage = 'CASE_CONNECT'
    event.encapsulation.append('CASE Sigma1-3')

  # Enrich OnOff
  if 'OnOff' in event.title or '0x0000_0006' in event.title:
    event.layer = 'MATTER'
    event.stage = 'CLUSTER_CONTROL'
    event.encapsulation.append('Cluster 0x0006')

  # Enrich Thread MLE / 802.15.4
  if 'Thread' in event.title and event.layer != 'MATTER':
    event.layer = 'THREAD'
    event.sublayer = 'MLE'
    event.stage = 'NETWORK_CONFIG'

  # Enrich Wi-Fi EAPOL / mDNS
  if 'EAPOL' in event.title or 'WPA2' in event.title:
    event.layer = 'WIFI'
    event.sublayer = 'EAPOL'
    event.stage = 'NETWORK_CONFIG'
  elif 'mDNS' in event.title:
    event.layer = 'WIFI'
    event.sublayer = 'mDNS'
    event.stage = 'CASE_CONNECT'

  return event


def _split_addr(addr: str) -> str:
  """Drops the port from '10.0.1.21:45633' or '[fd11:33::2]:5540'."""
  m = re.match(r'^\[([^\]]+)\]:\d+$', addr) or re.match(r'^([^:]+):\d+$', addr)
  return m.group(1) if m else addr


def _parse_tbr_proxy_lines(log_path: str) -> List[FlowEvent]:
  """Turns TBR-proxy forwarding lines into relay-hop events."""
  fname = os.path.basename(log_path)
  events: List[FlowEvent] = []
  try:
    with open(log_path, 'r', encoding='utf-8', errors='replace') as f:
      lines = f.readlines()
  except OSError:
    return events
  for line_no, line in enumerate(lines, start=1):
    m = _TBR_PROXY_FWD_RE.search(line)
    if not m:
      continue
    ts, in_if, out_if, src, dst, length, counter = m.groups()
    src, dst = _split_addr(src), _split_addr(dst)
    to_thread = out_if.startswith('wpan')
    details: Dict[str, Any] = {
        'log_file': fname,
        'line_number': line_no,
        'proxy_in': in_if,
        'proxy_out': out_if,
        'bytes': int(length),
        'lane_aliases': {src: 0, dst: 3} if to_thread else {src: 3, dst: 0},
        'evidence': 'relay',
    }
    if counter:
      details['matter_message_counter'] = int(counter)
    events.append(
        FlowEvent(
            event_id=f'tbr_fwd_{fname}_{line_no}',
            timestamp=float(ts),
            layer='THREAD' if to_thread else 'WIFI',
            sublayer='Proxy',
            stage='CASE_CONNECT',
            src='ThreadBorderRouter (proxy)',
            dst=dst,
            title=f'TBR proxy {in_if}->{out_if} ({length}B)',
            summary=line.strip(),
            details=details,
            encapsulation=['TBR UDP proxy', f'{in_if} -> {out_if}'],
        )
    )
  return events


def parse_log_file(log_path: str) -> List[FlowEvent]:
  """Extracts and Matter-classifies FlowEvents from a Cirque container .log file."""
  raw_events = _cirque_dissect_log_file(log_path)
  raw_events.extend(_parse_tbr_proxy_lines(log_path))
  classified: List[FlowEvent] = []
  for ev in raw_events:
    res = matter_log_dissector(ev)
    classified.append(res if res is not None else ev)
  return classified


def parse_log_directory(log_dir: str) -> List[FlowEvent]:
  """Extracts and Matter-classifies FlowEvents from all .log files in `log_dir`.

  A '*-thread-diag.log' embeds a copy of the node's own log, so its events
  are parsed after the node log and dropped when the same node already
  produced an event with the same timestamp and title.
  """
  events: List[FlowEvent] = []
  if not os.path.isdir(log_dir):
    return events
  paths = sorted(
      list_log_files(log_dir), key=lambda p: p.endswith('-thread-diag.log')
  )
  seen: Dict[Any, str] = {}
  for log_path in paths:
    for ev in parse_log_file(log_path):
      node = re.sub(r' \(thread-diag\)$', '', str(ev.details.get('node', '')))
      key = (node, ev.timestamp, ev.title)
      if node and seen.get(key, log_path) != log_path:
        continue
      seen[key] = log_path
      events.append(ev)
  return events


# Cirque flow plugin contract (see cirque/pcap/flow_visualizer.py).
FLOW_DISSECTORS = {
    'matter_packet': matter_packet_dissector,
    'matter_log': matter_log_dissector,
    'matter_stage': matter_stage_dissector,
}
FLOW_LAYERS: Dict[str, Dict[str, Any]] = {
    MATTER_LAYER: {'label': 'Matter', 'color': 'amber', 'order': 10},
}
