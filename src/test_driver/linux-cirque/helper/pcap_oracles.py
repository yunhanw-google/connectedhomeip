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
the Cirque virtual Bluetooth / Wi-Fi servers write (and flush per frame) into
CIRQUE_PCAP_DIR, so a test can assert on frames that actually crossed the
virtual medium.
"""

import socket
import struct
from typing import Iterator, List, Tuple

DLT_EN10MB = 1
DLT_BLUETOOTH_HCI_H4_WITH_PHDR = 201

_PCAP_MAGIC_LE = b'\xd4\xc3\xb2\xa1'
_PCAP_MAGIC_BE = b'\xa1\xb2\xc3\xd4'


def iter_pcap_records(path: str) -> Iterator[Tuple[int, bytes]]:
  """Yields (linktype, frame_bytes) for every record in a classic pcap file."""
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
      incl_len = struct.unpack(endian + 'I', rec[8:12])[0]
      data = f.read(incl_len)
      if len(data) < incl_len:
        return
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
