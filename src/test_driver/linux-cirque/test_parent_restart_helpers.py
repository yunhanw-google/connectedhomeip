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

Unit tests for helper/parent_restart.py and the pcap Ack/MLE helpers.
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from helper import parent_restart as pr  # pylint: disable=g-import-not-at-top
from helper.pcap_oracles import acked_mac_commands  # pylint: disable=g-import-not-at-top


def _rec(key: int, value: bytes) -> bytes:
  return struct.pack('<HH', key, len(value)) + value


class StripSettingsTest(unittest.TestCase):

  def test_removes_only_child_info(self):
    data = (
        _rec(0x0003, b'netinfo')
        + _rec(0x0005, b'child-a')
        + _rec(0x0001, b'')
        + _rec(0x0005, b'child-b')
    )
    new, removed = pr.strip_settings_key(data, pr.OT_SETTINGS_KEY_CHILD_INFO)
    self.assertEqual(removed, 2)
    self.assertEqual(new, _rec(0x0003, b'netinfo') + _rec(0x0001, b''))

  def test_no_match_is_identity(self):
    data = _rec(0x0003, b'netinfo')
    self.assertEqual(pr.strip_settings_key(data, 5), (data, 0))

  def test_malformed_raises(self):
    with self.assertRaises(ValueError):
      pr.strip_settings_key(b'\x05\x00\x09\x00abc', 5)
    with self.assertRaises(ValueError):
      pr.strip_settings_key(b'\x05\x00', 5)

  def test_container_script_matches_helper(self):
    """The in-container script strips a settings file the same way."""
    data = _rec(0x0003, b'n') + _rec(0x0005, b'c') + _rec(0x0007, b'x')
    with tempfile.TemporaryDirectory() as tmp:
      path = os.path.join(tmp, 'node.data')
      with open(path, 'wb') as f:
        f.write(data)
      script = pr.STRIP_CHILD_INFO_SCRIPT.replace(
          "glob.glob('/var/lib/thread/*.data')", f'[{path!r}]'
      )
      out = subprocess.run(
          [sys.executable, '-c', script],
          capture_output=True,
          text=True,
          check=True,
          env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'),
      ).stdout
      with open(path, 'rb') as f:
        stripped = f.read()
    self.assertIn('removed=1', out)
    self.assertEqual(stripped, pr.strip_settings_key(data, 5)[0])


_DEVICE_LOG = """\
\x1b[0;32m[1792818800.100] [1:1] [SVR] [SILABS] OpenThread child supervision: check_timeout=30s interval=129s (override)\x1b[0m
\x1b[0;34m[1792818808.119] [1:1] [DL]    Device Role: DETACHED\x1b[0m
\x1b[0;34m[1792818808.122] [1:1] [DL]         fdde:ad00:beef:0:7ebc:8b37:67ca:db48/64 valid preferred\x1b[0m
[1792818809.198] [1:1] [DL]    Device Role: CHILD
[1792818809.204] [1:1] [DL]         fdde:ad00:beef:0:0:ff:fe00:c401/64 valid preferred rloc
\x1b[0;34m[1792818809.204] [1:1] [DL]         fdde:ad00:beef:0:7ebc:8b37:67ca:db49/64 valid preferred\x1b[0m
\x1b[0;32m[1792818820.000] [1:1] [EM] <<< [E:123i S:0 M:77] (U) Msg TX to 0:0000000000000000 [0000] [UDP:[fd11:22::3]:5540] --- Type 0000:50 (SecureChannel:ICD_CheckInMessage) (B:62)\x1b[0m
"""


class LogParsingTest(unittest.TestCase):

  def test_roles(self):
    self.assertEqual(
        pr.device_roles(_DEVICE_LOG),
        [(1792818808.119, 'DETACHED'), (1792818809.198, 'CHILD')],
    )

  def test_ml_eid_skips_rloc(self):
    self.assertEqual(
        pr.device_ml_eid(_DEVICE_LOG), 'fdde:ad00:beef:0:7ebc:8b37:67ca:db49'
    )
    self.assertIsNone(pr.device_ml_eid('no addresses'))

  def test_checkin_tx(self):
    self.assertEqual(pr.device_checkin_tx(_DEVICE_LOG), [(1792818820.0, 77)])

  def test_supervision_config(self):
    self.assertEqual(
        pr.supervision_config(_DEVICE_LOG),
        {'check_timeout_s': 30, 'interval_s': 129, 'source': 'override'},
    )
    self.assertIsNone(pr.supervision_config('nothing'))

  def test_logcat_checkins(self):
    logcat = (
        '  1792818830.250  1234  1250 I ICDListener: onCheckInComplete :'
        ' ICDClientInfo{...}\n'
        '  1792818831.000  1234  1250 I Other: unrelated\n'
    )
    self.assertEqual(pr.logcat_checkin_callbacks(logcat), [1792818830.25])

  def test_child_table(self):
    table = (
        '| ID  | RLOC16 | Timeout    | Age        | LQ In | C_VN |R|D|N|\n'
        '+-----+--------+------------+------------+-------+------+-+-+-+\n'
        '|   1 | 0xc401 |        240 |          3 |     3 |  151 |0|0|0|\n'
        'Done\n'
    )
    self.assertEqual(pr.parse_child_rloc16s(table), {'c401'})
    self.assertEqual(pr.parse_child_rloc16s('| ID | RLOC16 |\nDone\n'), set())

  def test_child_ext_addrs_real_format(self):
    table = (
        '| ID  | RLOC16 | Timeout    | Age        | LQ In | C_VN |R|D|N|Ver|CSL'
        '|QMsgCnt|Suprvsn| Extended MAC     |\r\r\n'
        '+-----+--------+------------+------------+-------+------+-+-+-+---+---'
        '+-------+-------+------------------+\r\r\n'
        '|   1 | 0x7801 |        240 |          0 |     3 |   42 |0|0|1|  4| 0 '
        '|     0 |   129 | be0deae71f880ea6 |\r\r\n'
        '\r\r\nDone\r\r\n'
    )
    self.assertEqual(pr.parse_child_ext_addrs(table), ['be0deae71f880ea6'])
    self.assertEqual(pr.parse_child_rloc16s(table), {'7801'})


class PingTest(unittest.TestCase):

  samples = [
      (10.0, True),
      (11.0, True),
      (20.0, False),
      (25.0, False),
      (40.0, True),
      (41.0, True),
  ]

  def test_parse(self):
    text = '10.5 ok\n11.5 fail\ngarbage\n12 maybe\n'
    self.assertEqual(pr.parse_ping_log(text), [(10.5, True), (11.5, False)])

  def test_first_last_ok(self):
    self.assertEqual(pr.first_ok_after(self.samples, 15.0), 40.0)
    self.assertIsNone(pr.first_ok_after(self.samples, 50.0))
    self.assertEqual(pr.last_ok_before(self.samples, 20.0), 11.0)

  def test_run_and_summary(self):
    self.assertEqual(pr.ok_run_length(self.samples), 2)
    self.assertEqual(pr.ok_run_length(self.samples[:4]), 0)
    self.assertEqual(
        pr.summarize_ping(self.samples, 15.0, 41.0),
        {'probes': 3, 'ok': 1, 'fail': 2},
    )


class AckPairingTest(unittest.TestCase):

  def test_pairs_by_seq_and_delay(self):
    polls = [
        {'ts': 1.0, 'seq': 5},
        {'ts': 2.0, 'seq': 6},
        {'ts': 3.0, 'seq': 7},
    ]
    acks = [
        {'ts': 1.001, 'seq': 5, 'frame_pending': True},
        {'ts': 2.2, 'seq': 6, 'frame_pending': False},  # too late
        {'ts': 3.002, 'seq': 9, 'frame_pending': False},  # wrong seq
    ]
    matched = acked_mac_commands(polls, acks)
    self.assertEqual([m['seq'] for m in matched], [5])
    self.assertTrue(matched[0]['frame_pending'])


class BlackHoleHelpersTest(unittest.TestCase):

  def test_otbr_unknown_neighbor_drops(self):
    log = (
        'otbr-agent[742]: 00:00:26.819 [I] Mac-----------: Frame rx failed,'
        ' error:UnknownNeighbor, len:122, seqnum:153, type:Data, src:0xb001,'
        ' dst:0xb000, sec:yes, ackreq:yes, radio:15.4\notbr-agent[742]:'
        ' 00:00:26.821 [I] Mac-----------: Frame rx failed,'
        ' error:UnknownNeighbor, len:22, seqnum:154, type:Cmd(DataReq),'
        ' src:0xb001, dst:0xb000, sec:yes, ackreq:yes,'
        ' radio:15.4\notbr-agent[53]: 00:00:26.900 [I] Mac-----------: Frame rx'
        ' failed, error:UnknownNeighbor, len:122, seqnum:155, type:Data,'
        ' src:0xb001, dst:0xb000\notbr-agent[742]: 00:00:26.900 [I]'
        ' Mac-----------: Frame rx failed, error:UnknownNeighbor, len:122,'
        ' seqnum:155, type:Data, src:0x8c00, dst:0xb000\nunrelated line\n'
    )

    # Total drops
    self.assertEqual(pr.otbr_unknown_neighbor_drops(log), 4)
    # PID exclude
    self.assertEqual(pr.otbr_unknown_neighbor_drops(log, pid_exclude='53'), 3)
    # Src16s int/str matching
    self.assertEqual(pr.otbr_unknown_neighbor_drops(log, src16s={0xB001}), 3)
    self.assertEqual(pr.otbr_unknown_neighbor_drops(log, src16s={'b001'}), 3)
    self.assertEqual(pr.otbr_unknown_neighbor_drops(log, src16s={'0xb001'}), 3)
    # Both filters
    self.assertEqual(
        pr.otbr_unknown_neighbor_drops(log, pid_exclude='53', src16s={0xB001}),
        2,
    )
    # Frame type data
    self.assertEqual(
        pr.otbr_unknown_neighbor_drops(
            log, pid_exclude='53', src16s={0xB001}, frame_type='Data'
        ),
        1,
    )
    # Frame type with regex metacharacters (e.g. parentheses in Cmd(DataReq))
    self.assertEqual(
        pr.otbr_unknown_neighbor_drops(
            log, pid_exclude='53', src16s={0xB001}, frame_type='Cmd(DataReq)'
        ),
        1,
    )
    self.assertEqual(
        pr.otbr_unknown_neighbor_drops(
            'error:UnknownNeighbor, type:CmdDataReq, src:0xb001',
            frame_type='Cmd(DataReq)',
        ),
        0,
    )

  def test_uplink_mac_data_frames(self):
    # FCF = 0x8861 (Data frame, Short Dest, Short Src, Ack Req, PAN ID Comp)
    data_frame_b001 = (
        struct.pack('<H', 0x8861)
        + bytes([12])
        + struct.pack('<HHH', 0xBEEF, 0xB000, 0xB001)
    )
    data_frame_8c00 = (
        struct.pack('<H', 0x8861)
        + bytes([13])
        + struct.pack('<HHH', 0xBEEF, 0xB000, 0x8C00)
    )
    # MAC Command frame (FCF = 0x8863)
    cmd_frame = (
        struct.pack('<H', 0x8863)
        + bytes([14])
        + struct.pack('<HHH', 0xBEEF, 0xB000, 0xB001)
    )
    # Truncated data frame
    trunc_frame = struct.pack('<H', 0x8861) + bytes([15, 0xEF, 0xBE])

    records = [
        (1.0, data_frame_b001),
        (2.0, data_frame_8c00),
        (3.0, cmd_frame),
        (4.0, trunc_frame),
    ]
    with tempfile.TemporaryDirectory() as tmp:
      path = os.path.join(tmp, 'test.pcap')
      with open(path, 'wb') as f:
        f.write(struct.pack('<IHHiIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 230))
        for ts, data in records:
          ts_sec = int(ts)
          ts_usec = int((ts - ts_sec) * 1e6)
          f.write(struct.pack('<IIII', ts_sec, ts_usec, len(data), len(data)))
          f.write(data)

      # With int
      frames = pr.ieee802154_mac_data_frames(path, {0xB001})
      self.assertEqual(len(frames), 1)
      self.assertEqual(frames[0]['seq'], 12)
      self.assertEqual(frames[0]['src16'], 0xB001)
      self.assertTrue(frames[0]['ack_req'])

      # With str 'b001'
      frames = pr.ieee802154_mac_data_frames(path, {'b001'})
      self.assertEqual(len(frames), 1)
      self.assertEqual(frames[0]['seq'], 12)

      # With str '0xb001'
      frames = pr.ieee802154_mac_data_frames(path, {'0xb001'})
      self.assertEqual(len(frames), 1)
      self.assertEqual(frames[0]['seq'], 12)

  def test_device_checkin_resolution_failures(self):
    log = (
        '[1793035671.123] Node Address resolution failed for ICD'
        ' Check-In\n[1793035672.456] unrelated\n\x1b[0;32m[1793035673.789]'
        ' \x1b[0mNode Address resolution failed for ICD Check-In\n'
    )
    self.assertEqual(
        pr.device_checkin_resolution_failures(log),
        [1793035671.123, 1793035673.789],
    )


if __name__ == '__main__':
  unittest.main()
