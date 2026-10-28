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

Helpers for the Thread parent-restart (child supervision) Cirque tests.

The pure functions here parse the logs those tests collect; the *_SCRIPT
constants run inside the ThreadBorderRouter container.
"""

import re
import struct
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple, Union

# OpenThread Settings key of a persisted child entry
# (OT_SETTINGS_KEY_CHILD_INFO, include/openthread/platform/settings.h).
OT_SETTINGS_KEY_CHILD_INFO = 0x0005


def strip_settings_key(data: bytes, key: int) -> Tuple[bytes, int]:
  """Removes every record with `key` from an OpenThread POSIX settings file.

  The file (src/posix/platform/settings_file.cpp) is a sequence of records:
  little-endian uint16 key, uint16 length, then `length` value bytes.

  Args:
    data: The settings file contents.
    key: The settings key to remove.

  Returns:
    (new contents, number of records removed).

  Raises:
    ValueError: If the file is not a well-formed record sequence.
  """
  out = bytearray()
  removed = 0
  offset = 0
  while offset < len(data):
    if offset + 4 > len(data):
      raise ValueError(f'truncated record header at offset {offset}')
    rec_key, length = struct.unpack_from('<HH', data, offset)
    end = offset + 4 + length
    if end > len(data):
      raise ValueError(f'record at offset {offset} overruns the file')
    if rec_key == key:
      removed += 1
    else:
      out += data[offset:end]
    offset = end
  return bytes(out), removed


# Run inside the ThreadBorderRouter container while otbr-agent is stopped.
# Removes the persisted child table (ChildInfo records) from every OpenThread
# settings file and prints one line per file with the removed count and the
# record keys it held (key:count).
STRIP_CHILD_INFO_SCRIPT = r'''
import collections, glob, struct, sys

KEY = %d

def strip(data):
    out, removed, off = bytearray(), 0, 0
    keys = collections.Counter()
    while off < len(data):
        if off + 4 > len(data):
            raise ValueError('truncated record header at %%d' %% off)
        key, length = struct.unpack_from('<HH', data, off)
        end = off + 4 + length
        if end > len(data):
            raise ValueError('record at %%d overruns the file' %% off)
        keys[key] += 1
        if key == KEY:
            removed += 1
        else:
            out += data[off:end]
        off = end
    return bytes(out), removed, keys

paths = sorted(set(glob.glob('/var/lib/thread/*.data')))
if not paths:
    print('no settings file found')
    sys.exit(2)
for path in paths:
    with open(path, 'rb') as f:
        data = f.read()
    try:
        new, removed, keys = strip(data)
    except ValueError as exc:
        print('%%s unparsed: %%s' %% (path, exc))
        continue
    if removed:
        with open(path, 'wb') as f:
            f.write(new)
    print('%%s removed=%%d bytes=%%d->%%d keys=%%s' %% (
        path, removed, len(data), len(new),
        ','.join('0x%%04x:%%d' %% kv for kv in sorted(keys.items()))))
''' % OT_SETTINGS_KEY_CHILD_INFO

# Run inside the ThreadBorderRouter container: probes the sleepy end device
# from the border router once per second and appends "<epoch> ok|fail".
# A reply proves the parent can still deliver downlink traffic to the child.
PING_PROBE_SCRIPT = r'''
#!/bin/sh
addr="$1"
while true; do
  t=$(date +%s.%N)
  if ping -6 -c 1 -W 4 "$addr" >/dev/null 2>&1; then r=ok; else r=fail; fi
  echo "$t $r"
  sleep 1
done
'''

_ROLE_RE = re.compile(r'^\[(\d+\.\d+)\].*Device Role: (\w+)', re.MULTILINE)
_LOG_ADDR_RE = re.compile(
    r'^\[(\d+\.\d+)\].*\s(fdde:ad00:beef:[0-9a-f:]+)/64 valid preferred\s*$',
    re.MULTILINE,
)
_CHECKIN_TX_RE = re.compile(
    r'^\[(\d+\.\d+)\].*<<< \[E:\d+i S:0 M:(\d+)\] .* Type 0000:50'
    r' \(SecureChannel:ICD_CheckInMessage\)',
    re.MULTILINE,
)
_LOGCAT_CHECKIN_RE = re.compile(
    r'^\s*(\d+\.\d+).*onCheckInComplete\s*:\s*ICDClientInfo', re.MULTILINE
)
_SUPERVISION_LOG_RE = re.compile(
    r'OpenThread child supervision: check_timeout=(\d+)s interval=(\d+)s'
    r' \((override|default)\)'
)
# One row of `ot-ctl child table`: | ID | RLOC16 | Timeout | ...
_CHILD_ROW_RE = re.compile(r'^\|\s*\d+\s*\|\s*0x([0-9a-fA-F]{4})\s*\|', re.MULTILINE)


def parse_child_rloc16s(child_table: str) -> set:
  """Returns the RLOC16s (lowercase hex) listed by `ot-ctl child table`."""
  return {r.lower() for r in _CHILD_ROW_RE.findall(child_table)}


_CHILD_EXT_RE = re.compile(
    r'^\|\s*\d+\s*\|\s*0x[0-9a-fA-F]{4}\s*\|.*\|\s*([0-9a-fA-F]{16})\s*\|\s*$',
    re.MULTILINE,
)


def parse_child_ext_addrs(child_table: str) -> List[str]:
  """Returns the Extended MAC column of `ot-ctl child table` rows."""
  return [e.lower() for e in _CHILD_EXT_RE.findall(child_table)]


def normalize_rloc16s(src16s: Optional[Iterable[Union[int, str]]]) -> Set[int]:
  if not src16s:
    return set()
  normalized = set()
  for s in src16s:
    if isinstance(s, int):
      normalized.add(s)
    elif isinstance(s, str):
      normalized.add(int(s, 16))
  return normalized


def ieee802154_mac_data_frames(
    pcap_path: str, src16s: Optional[Iterable[Union[int, str]]] = None
) -> List[Dict[str, Any]]:
  from helper.pcap_oracles import iter_pcap_records_with_ts  # pylint: disable=g-import-not-at-top
  normalized_src16s = normalize_rloc16s(src16s)
  frames = []
  for linktype, ts, data in iter_pcap_records_with_ts(pcap_path):
    if linktype not in (195, 230) or len(data) < 3:
      continue
    fcf = struct.unpack_from('<H', data, 0)[0]
    if (fcf & 0x07) != 0x01:
      continue
    if ((fcf >> 12) & 0x03) not in (0, 1):
      continue

    seq = data[2]
    dst_mode = (fcf >> 10) & 0x03
    src_mode = (fcf >> 14) & 0x03
    if dst_mode == 1 or src_mode == 1:
      continue
    pan_id_comp = (fcf >> 6) & 0x01

    offset = 3
    dst16 = None
    if dst_mode == 2:
      if offset + 4 > len(data):
        continue
      offset += 2
      dst16 = struct.unpack_from('<H', data, offset)[0]
      offset += 2
    elif dst_mode == 3:
      if offset + 10 > len(data):
        continue
      offset += 10

    if src_mode != 0 and pan_id_comp == 0:
      if offset + 2 > len(data):
        continue
      offset += 2

    src16 = None
    if src_mode == 2:
      if offset + 2 > len(data):
        continue
      src16 = struct.unpack_from('<H', data, offset)[0]
      offset += 2
    elif src_mode == 3:
      if offset + 8 > len(data):
        continue
      offset += 8

    if normalized_src16s and src16 not in normalized_src16s:
      continue

    frames.append({
        'ts': ts,
        'fcf': fcf,
        'seq': seq,
        'src16': src16,
        'dst16': dst16,
        'ack_req': bool(fcf & 0x20),
        'frame_len': len(data),
    })
  return frames


_ANSI_RE = re.compile(r'\x1b\[[0-9;]*m')


def strip_ansi(text: str) -> str:
  """Removes ANSI color codes (the app colors its log lines)."""
  return _ANSI_RE.sub('', text)


def otbr_unknown_neighbor_drops(
    otbr_log: str,
    pid_exclude: Optional[str] = None,
    src16s: Optional[Iterable[Union[int, str]]] = None,
    frame_type: Optional[str] = None,
) -> int:
  drops = 0
  normalized_src16s = normalize_rloc16s(src16s)
  pid_prefix = f'otbr-agent[{pid_exclude}]:' if pid_exclude else None

  for line in strip_ansi(otbr_log).splitlines():
    if pid_prefix and line.startswith(pid_prefix):
      continue
    if 'error:UnknownNeighbor' not in line:
      continue
    if normalized_src16s:
      lower_line = line.lower()
      if not any(f'src:0x{s:04x}' in lower_line for s in normalized_src16s):
        continue
    if frame_type:
      if not re.search(fr'type:{re.escape(frame_type)}(,|$|\s|\]|\))', line):
        continue
    drops += 1
  return drops


_ICD_CHECKIN_DNS_RE = re.compile(
    r'^\[(\d+\.\d+)\].*Node Address resolution failed for ICD Check-In',
    re.MULTILINE,
)


def device_checkin_resolution_failures(device_log: str) -> List[float]:
  return [
      float(m.group(1))
      for m in _ICD_CHECKIN_DNS_RE.finditer(strip_ansi(device_log))
  ]


def device_roles(device_log: str) -> List[Tuple[float, str]]:
  """Returns (epoch, role) for every "Device Role:" line of the app log."""
  return [
      (float(t), role) for t, role in _ROLE_RE.findall(strip_ansi(device_log))
  ]


def device_ml_eid(device_log: str) -> Optional[str]:
  """Returns the end device's latest mesh-local EID from the app log.

  The app logs its addresses on every Thread state change; the mesh-local
  RLOC (…:0:ff:fe00:xxxx, tagged "rloc") is skipped.
  """
  addrs = [
      a
      for _, a in _LOG_ADDR_RE.findall(strip_ansi(device_log))
      if ':0:ff:fe00:' not in a
  ]
  return addrs[-1] if addrs else None


def device_checkin_tx(device_log: str) -> List[Tuple[float, int]]:
  """Returns (epoch, message counter) of every Check-In the app sent."""
  return [
      (float(t), int(c))
      for t, c in _CHECKIN_TX_RE.findall(strip_ansi(device_log))
  ]


def logcat_checkin_callbacks(logcat: str) -> List[float]:
  """Returns epoch times of Android onCheckInComplete callbacks.

  Needs logcat printed with `-v epoch`.
  """
  return [float(t) for t in _LOGCAT_CHECKIN_RE.findall(strip_ansi(logcat))]


def supervision_config(device_log: str) -> Optional[Dict[str, Any]]:
  """Returns the child supervision settings the app logged at start."""
  m = _SUPERVISION_LOG_RE.search(strip_ansi(device_log))
  if not m:
    return None
  return {
      'check_timeout_s': int(m.group(1)),
      'interval_s': int(m.group(2)),
      'source': m.group(3),
  }


def parse_ping_log(text: str) -> List[Tuple[float, bool]]:
  """Parses PING_PROBE_SCRIPT output into (epoch, ok) samples."""
  samples = []
  for line in text.splitlines():
    parts = line.split()
    if len(parts) != 2 or parts[1] not in ('ok', 'fail'):
      continue
    try:
      samples.append((float(parts[0]), parts[1] == 'ok'))
    except ValueError:
      continue
  return samples


def first_ok_after(
    samples: Sequence[Tuple[float, bool]], t: float
) -> Optional[float]:
  """Returns the start time of the first successful probe at or after t."""
  for ts, ok in samples:
    if ts >= t and ok:
      return ts
  return None


def last_ok_before(
    samples: Sequence[Tuple[float, bool]], t: float
) -> Optional[float]:
  """Returns the start time of the last successful probe before t."""
  best = None
  for ts, ok in samples:
    if ts < t and ok:
      best = ts
  return best


def ok_run_length(samples: Sequence[Tuple[float, bool]]) -> int:
  """Returns the length of the trailing run of successful probes."""
  run = 0
  for _, ok in reversed(list(samples)):
    if not ok:
      break
    run += 1
  return run


def in_window(times: Sequence[float], start: float, end: float) -> List[float]:
  return [t for t in times if start <= t < end]


def summarize_ping(
    samples: Sequence[Tuple[float, bool]], start: float, end: float
) -> Dict[str, Any]:
  """Counts probes in [start, end) and their success rate."""
  window = [(t, ok) for t, ok in samples if start <= t < end]
  ok = sum(1 for _, good in window if good)
  return {
      'probes': len(window),
      'ok': ok,
      'fail': len(window) - ok,
  }
