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

Raw-capture oracles for Cirque E2E tests.

Device log lines can be printed by the code under test whether or not the
underlying radio exchange happened. These helpers read the pcap files that
the Cirque virtual Bluetooth / Wi-Fi / 802.15.4 servers write (and flush per
frame) into CIRQUE_PCAP_DIR, so a test can assert on frames that actually
crossed the virtual medium. MAC-secured 802.15.4 frames are decrypted with
tshark and the Thread network key.
"""

import os
import shutil
import socket
import struct
import subprocess
import tempfile
from typing import Any, Dict, Iterator, List, Optional, Tuple

DLT_EN10MB = 1
DLT_IEEE802_15_4 = 195
DLT_BLUETOOTH_HCI_H4_WITH_PHDR = 201
DLT_IEEE802_15_4_NOFCS = 230

_PCAP_MAGIC_LE = b'\xd4\xc3\xb2\xa1'
_PCAP_MAGIC_BE = b'\xa1\xb2\xc3\xd4'


def iter_pcap_records_with_ts(path: str) -> Iterator[Tuple[int, float, bytes]]:
  """Yields (linktype, ts_sec, frame_bytes) for each record of a pcap file."""
  with open(path, 'rb') as f:
    hdr = f.read(24)
    if len(hdr) < 24:
      return
    if hdr[:4] == _PCAP_MAGIC_LE:
      endian = '<'
    elif hdr[:4] == _PCAP_MAGIC_BE:
      endian = '>'
    else:
      raise ValueError(f'{path}: not a classic pcap file')
    linktype = struct.unpack(endian + 'I', hdr[20:24])[0]
    while True:
      rec = f.read(16)
      if len(rec) < 16:
        return
      ts_sec, ts_usec, incl_len, _ = struct.unpack(endian + 'IIII', rec)
      data = f.read(incl_len)
      if len(data) < incl_len:
        return
      yield linktype, float(ts_sec) + float(ts_usec) * 1e-6, data


def iter_pcap_records(path: str) -> Iterator[Tuple[int, bytes]]:
  """Yields (linktype, frame_bytes) for every record in a classic pcap file."""
  for linktype, _, data in iter_pcap_records_with_ts(path):
    yield linktype, data


def hci_le_connection_complete_handles(path: str) -> List[int]:
  """Returns handles of successful HCI LE (Enhanced) Connection Complete events.

  Expects DLT_BLUETOOTH_HCI_H4_WITH_PHDR records: 4-byte direction header,
  then the H4 packet (0x04 = HCI event).
  """
  handles = []
  for linktype, frame in iter_pcap_records(path):
    if linktype != DLT_BLUETOOTH_HCI_H4_WITH_PHDR or len(frame) < 9:
      continue
    h4 = frame[4:]
    if h4[0] != 0x04 or h4[1] != 0x3E:
      continue
    sub_event = h4[3]
    if sub_event not in (0x01, 0x0A):
      continue
    status = h4[4]
    if status != 0x00 or len(h4) < 7:
      continue
    handles.append((h4[5] | (h4[6] << 8)) & 0x0FFF)
  return handles


def _iter_udp_ipv4(frame: bytes) -> Iterator[Tuple[int, int, bytes]]:
  if len(frame) < 14 or frame[12:14] != b'\x08\x00':
    return
  ip = frame[14:]
  if len(ip) < 20 or (ip[0] >> 4) != 4 or ip[9] != 17:
    return
  ihl = (ip[0] & 0x0F) * 4
  udp = ip[ihl:]
  if len(udp) < 8:
    return
  sport, dport = struct.unpack('!HH', udp[:4])
  yield sport, dport, udp[8:]


def dhcp_ack_addresses(path: str) -> List[str]:
  """Returns yiaddr of every DHCPACK (option 53 == 5) sent server->client."""
  acks = []
  for linktype, frame in iter_pcap_records(path):
    if linktype != DLT_EN10MB:
      continue
    for sport, dport, payload in _iter_udp_ipv4(frame):
      if sport != 67 or dport != 68 or len(payload) < 240:
        continue
      if payload[236:240] != b'\x63\x82\x53\x63':
        continue
      opts = payload[240:]
      i = 0
      msg_type = None
      while i < len(opts) and opts[i] != 0xFF:
        if opts[i] == 0x00:
          i += 1
          continue
        if i + 1 >= len(opts):
          break
        code, length = opts[i], opts[i + 1]
        if code == 53 and length == 1 and i + 2 < len(opts):
          msg_type = opts[i + 2]
        i += 2 + length
      if msg_type == 5:
        acks.append(socket.inet_ntoa(payload[16:20]))
  return acks


def eapol_frame_count(path: str) -> int:
  """Counts Ethernet frames with EtherType 0x888E (EAPOL)."""
  return sum(
      1
      for linktype, frame in iter_pcap_records(path)
      if linktype == DLT_EN10MB
      and len(frame) >= 14
      and frame[12:14] == b'\x88\x8e'
  )


def parse_matter_icd_checkin(data: bytes) -> Optional[Dict[str, Any]]:
  """Parses one Matter UDP payload as SecureChannel::ICD_CheckIn (0x0000:0x50).

  Returns None unless the message is an unsecured-session message carrying
  the ICD_CheckIn opcode with a >= 35-byte Check-In payload.
  """
  if len(data) < 14:
    return None
  msg_flags = data[0]
  if ((msg_flags & 0xF0) >> 4) != 0:
    return None
  session_id = struct.unpack_from('<H', data, 1)[0]
  security_flags = data[3]
  # ICD Check-In uses an unauthenticated session
  # (session_id == 0, session_type == 0).
  if session_id != 0 or (security_flags & 0x03) != 0:
    return None
  msg_counter = struct.unpack_from('<I', data, 4)[0]
  idx = 8
  source_node_id = None
  if msg_flags & 0x04:
    if idx + 8 > len(data):
      return None
    source_node_id = struct.unpack_from('<Q', data, idx)[0]
    idx += 8
  dsiz = msg_flags & 0x03
  if dsiz == 1:
    idx += 8
  elif dsiz == 2:
    idx += 2
  elif dsiz == 3:
    return None
  if security_flags & 0x20:
    if idx + 2 > len(data):
      return None
    mx_len = struct.unpack_from('<H', data, idx)[0]
    idx += 2 + mx_len
  if idx + 6 > len(data):
    return None
  ex_flags = data[idx]
  opcode = data[idx + 1]
  exchange_id = struct.unpack_from('<H', data, idx + 2)[0]
  p_idx = idx + 4
  vendor_id = 0
  if ex_flags & 0x10:
    if p_idx + 2 > len(data):
      return None
    vendor_id = struct.unpack_from('<H', data, p_idx)[0]
    p_idx += 2
  if p_idx + 2 > len(data):
    return None
  protocol_id = struct.unpack_from('<H', data, p_idx)[0]
  p_idx += 2
  if ex_flags & 0x02:
    p_idx += 4
  if ex_flags & 0x20:
    if p_idx + 2 > len(data):
      return None
    sx_len = struct.unpack_from('<H', data, p_idx)[0]
    p_idx += 2 + sx_len
  if p_idx > len(data):
    return None
  app_payload = data[p_idx:]
  # SecureChannel protocol (0x0000:0x0000), ICD_CheckIn opcode (0x50),
  # >= 35-byte Check-In payload.
  if (
      vendor_id != 0x0000
      or protocol_id != 0x0000
      or opcode != 0x50
      or len(app_payload) < 35
  ):
    return None
  return {
      'msg_counter': msg_counter,
      'source_node_id': source_node_id,
      'exchange_id': exchange_id,
      'payload_len': len(app_payload),
      'nonce_hex': app_payload[:13].hex(),
  }


def matter_icd_checkin_packets(path: str) -> List[Dict[str, Any]]:
  """Returns ICD_CheckIn (0x0000:0x50) UDP/IPv6 packets on port 5540.

  Reads an Ethernet (DLT_EN10MB) capture such as wifi_medium.pcap.
  """
  checkins: List[Dict[str, Any]] = []
  for linktype, ts, frame in iter_pcap_records_with_ts(path):
    if linktype != DLT_EN10MB or len(frame) < 14 + 40 + 8:
      continue
    if frame[12:14] != b'\x86\xdd':
      continue
    ip6 = frame[14:]
    if (ip6[0] >> 4) != 6 or ip6[6] != 17:
      continue
    udp = ip6[40:]
    if len(udp) < 8:
      continue
    sport, dport, ulen = struct.unpack('!HHH', udp[:6])
    if 5540 not in (sport, dport):
      continue
    parsed = parse_matter_icd_checkin(udp[8:ulen] if ulen >= 8 else udp[8:])
    if parsed is None:
      continue
    parsed.update({
        'ts': ts,
        'src_ip': socket.inet_ntop(socket.AF_INET6, ip6[8:24]),
        'dst_ip': socket.inet_ntop(socket.AF_INET6, ip6[24:40]),
        'sport': sport,
        'dport': dport,
    })
    checkins.append(parsed)
  return checkins


_TSHARK_FIELDS = (
    'frame.time_epoch',
    'wpan.src16',
    'wpan.dst16',
    'wpan.aux_sec.sec_level',
    'wpan.aux_sec.key_id_mode',
    'udp.srcport',
    'udp.dstport',
    'udp.payload',
)

# Thread MAC frames secured with a key derived from the network key use Key
# ID Mode 1. Mode 2 frames (for example MLE Discovery) use a fixed well-known
# key and decrypt without the network key, so they prove nothing about it.
_THREAD_NETWORK_KEY_ID_MODE = 1


def _first_field_int(value: str) -> Optional[int]:
  value = value.split(',')[0].strip()
  if not value:
    return None
  return int(value, 0)


def thread_decrypted_udp_frames(
    path: str, network_key_hex: str
) -> List[Dict[str, Any]]:
  """Returns MAC-secured 802.15.4 frames that tshark decrypts to UDP.

  Decrypts thread_<n>.pcap with the Thread network key ("Thread hash" key
  derivation) and decompresses 6LoWPAN. tshark runs with an empty
  configuration directory so that keys saved in the user's Wireshark profile
  cannot decrypt frames: a frame decrypts only if network_key_hex is the key
  the network used. Only frames secured with a network-key-derived MAC key
  (Key ID Mode 1) are returned; unsecured frames and Key ID Mode 2 frames
  (for example MLE Discovery) are skipped.

  Raises RuntimeError if tshark is missing or produces no output and fails.
  """
  tshark = shutil.which('tshark')
  if not tshark:
    raise RuntimeError(
        'tshark not found in PATH; it is required to decrypt 802.15.4 frames'
    )
  cmd = [
      tshark,
      '-n',
      '-r',
      path,
      '-o',
      f'uat:ieee802154_keys:"{network_key_hex}","0","Thread hash"',
      '-Y',
      'udp && wpan.aux_sec.sec_level',
      '-T',
      'fields',
      '-E',
      'separator=\t',
  ]
  for field in _TSHARK_FIELDS:
    cmd += ['-e', field]
  with tempfile.TemporaryDirectory(prefix='cirque_tshark_cfg_') as cfg:
    env = dict(os.environ, HOME=cfg, XDG_CONFIG_HOME=cfg)
    proc = subprocess.run(
        cmd, capture_output=True, text=True, env=env, check=False
    )
  # A capture still being flushed can end mid-record; tshark then exits
  # non-zero but still prints every complete frame.
  if proc.returncode != 0 and not proc.stdout.strip():
    raise RuntimeError(
        f'tshark failed on {path} (exit {proc.returncode}): {proc.stderr}'
    )
  frames: List[Dict[str, Any]] = []
  for line in proc.stdout.splitlines():
    cols = line.split('\t')
    if len(cols) != len(_TSHARK_FIELDS):
      continue
    ts, src16, dst16, sec_level, key_id_mode, sport, dport, payload = cols
    sec = _first_field_int(sec_level)
    if not sec:
      continue
    if _first_field_int(key_id_mode) != _THREAD_NETWORK_KEY_ID_MODE:
      continue
    frames.append({
        'ts': float(ts),
        'src16': _first_field_int(src16),
        'dst16': _first_field_int(dst16),
        'sec_level': sec,
        'sport': _first_field_int(sport),
        'dport': _first_field_int(dport),
        'payload': bytes.fromhex(
            payload.split(',')[0].replace(':', '').strip()
        ),
    })
  return frames


def thread_icd_checkin_frames(
    path: str, network_key_hex: str
) -> List[Dict[str, Any]]:
  """Returns ICD_CheckIn (0x0000:0x50) messages carried over 802.15.4.

  Each entry has the radio fields (ts, src16, dst16, sec_level, sport,
  dport) plus the parsed Matter fields (msg_counter, source_node_id,
  exchange_id, payload_len, nonce_hex).
  """
  checkins: List[Dict[str, Any]] = []
  for frame in thread_decrypted_udp_frames(path, network_key_hex):
    if 5540 not in (frame['sport'], frame['dport']):
      continue
    parsed = parse_matter_icd_checkin(frame['payload'])
    if parsed is None:
      continue
    parsed.update({k: v for k, v in frame.items() if k != 'payload'})
    checkins.append(parsed)
  return checkins


def ieee802154_mac_command_frames(path: str) -> List[Dict[str, Any]]:
  """Returns IEEE 802.15.4 MAC Command frames (fcf & 0x07 == 0x03, e.g.

  SED Data Requests).
  """
  frames: List[Dict[str, Any]] = []
  for linktype, ts, data in iter_pcap_records_with_ts(path):
    if (
        linktype not in (DLT_IEEE802_15_4, DLT_IEEE802_15_4_NOFCS)
        or len(data) < 3
    ):
      continue
    fcf = struct.unpack_from('<H', data, 0)[0]
    if (fcf & 0x07) != 0x03:
      continue
    frames.append({
        'ts': ts,
        'fcf': fcf,
        'seq': data[2],
        'frame_len': len(data),
    })
  return frames
