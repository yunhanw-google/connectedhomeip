# Lint as: python3
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

Hermetic tests for the Thread data path helpers of
AndroidBleThreadMobileDeviceTest (no docker, no emulator).
"""

import base64
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
  sys.path.insert(0, SCRIPT_DIR)
CIRQUE_DIR = os.path.abspath(
    os.path.join(SCRIPT_DIR, '..', '..', '..', 'third_party', 'cirque', 'repo')
)
if CIRQUE_DIR not in sys.path:
  sys.path.insert(0, CIRQUE_DIR)

import AndroidBleThreadMobileDeviceTest as abt  # noqa: E402

B64_RE = re.compile(r'echo \\"([A-Za-z0-9+/=]+)\\" \| base64 -d')


def _new_test():
  return abt.TestAndroidBleThreadMobileDevice()


def _cmds_for(mock_exec, dev_id):
  return [c[0][1] for c in mock_exec.call_args_list if c[0][0] == dev_id]


class _HelperDone(Exception):
  """Stops the joiner helper at its final idle sleep."""


class JoinerHelperTest(unittest.TestCase):

  def test_payload_compiles(self):
    compile(abt.THREAD_END_DEVICE_JOINER_HELPER, 'joiner_helper', 'exec')

  def test_removed_shims_stay_removed(self):
    self.assertFalse(hasattr(abt, 'TBR_BORDER_PROXY_HELPER'))
    self.assertNotIn('mdns0', abt.THREAD_END_DEVICE_JOINER_HELPER)

  def test_no_dataset_reform_or_forced_role(self):
    helper = abt.THREAD_END_DEVICE_JOINER_HELPER
    for forbidden in (
        'dataset init new',
        'dataset commit',
        'dataset networkkey',
        'thread stop',
        'thread start',
        'state leader',
    ):
      self.assertNotIn(forbidden, helper)

  def test_helper_run_registers_srp_and_never_writes_dataset(self):
    issued = []
    states = iter([b'disabled', b'detached', b'detached'])
    instance = 'ABCDEF0123456789-0000000000000001'
    ip6 = 'fd11:33:0:0:1c2d:3e4f:5a6b:7c8d'

    def fake_check_output(cmd, **unused_kwargs):
      issued.append(cmd)
      if cmd == 'ot-ctl state':
        return next(states, b'child')
      if cmd == 'ot-ctl ipaddr':
        return (
            'fdde:ad00:beef:0:0:ff:fe00:4401\n'
            f'{ip6}\nfe80:0:0:0:a8bb:ccdd:eeff:1\nDone\n'
        ).encode('utf-8')
      return b'Done'

    def fake_sleep(seconds):
      if seconds >= 3600:
        raise _HelperDone()

    def fake_open(path, *args, **kwargs):
      self.assertEqual(path, '/tmp/chip-all-clusters.log')
      return io.StringIO(
          "[DIS] CHIP minimal mDNS configured as 'Operational device';"
          f' instance name: {instance.lower()}\n'
      )

    out = io.StringIO()
    with contextlib.ExitStack() as stack:
      stack.enter_context(
          mock.patch.object(
              subprocess, 'check_output', side_effect=fake_check_output
          )
      )
      stack.enter_context(
          mock.patch.object(time, 'sleep', side_effect=fake_sleep)
      )
      stack.enter_context(contextlib.redirect_stdout(out))
      with self.assertRaises(_HelperDone):
        exec(  # pylint: disable=exec-used
            compile(abt.THREAD_END_DEVICE_JOINER_HELPER, 'helper', 'exec'),
            {'__name__': '__joiner_helper__', 'open': fake_open},
        )

    self.assertFalse(
        [c for c in issued if 'dataset' in c or 'thread ' in c], issued
    )
    srp_cmds = [c for c in issued if 'srp client' in c]
    self.assertEqual(
        srp_cmds,
        [
            f'ot-ctl srp client host name {instance}',
            f'ot-ctl srp client host address {ip6}',
            f'ot-ctl srp client service add {instance} _matter._tcp 5540 0 0'
            ' 075349493D353030075341493D33303003543D31',
            'ot-ctl srp client autostart enable',
        ],
    )
    self.assertIn('[SRP-Shim] SRP registration requested', out.getvalue())


class HelperLaunchTest(unittest.TestCase):

  def _decode(self, cmd):
    m = B64_RE.search(cmd)
    self.assertIsNotNone(m, cmd)
    return base64.b64decode(m.group(1)).decode('utf-8')

  def test_srp_bridger_runs_unbuffered_and_decodes(self):
    test_inst = _new_test()
    test_inst.execute_device_cmd = mock.MagicMock(
        return_value={'output': 'leader\n'}
    )
    test_inst._get_tbr_active_dataset = mock.MagicMock(return_value={})
    test_inst._save_thread_network_key = mock.MagicMock()
    with mock.patch.object(abt.time, 'sleep'):
      test_inst._setup_thread_border_routers(['device1'])
    cmds = [
        c for c in _cmds_for(test_inst.execute_device_cmd, 'device1')
        if 'srp_bridger.py' in c
    ]
    self.assertEqual(len(cmds), 1)
    self.assertIn('nohup python3 -u /tmp/srp_bridger.py', cmds[0])
    src = self._decode(cmds[0])
    self.assertIn('ot-ctl srp server service', src)
    self.assertIn('/etc/avahi/services/srp_', src)
    compile(src, 'helper', 'exec')

  def test_joiner_runs_unbuffered_and_decodes(self):
    test_inst = _new_test()
    test_inst.execute_device_cmd = mock.MagicMock(return_value='')
    test_inst.get_device_pretty_id = mock.MagicMock(return_value='ED')
    test_inst._start_ble_thread_end_devices(['device2'])
    cmds = [
        c for c in _cmds_for(test_inst.execute_device_cmd, 'device2')
        if 'joiner_helper.py' in c
    ]
    self.assertEqual(len(cmds), 1)
    self.assertIn('nohup python3 -u /tmp/joiner_helper.py', cmds[0])
    src = self._decode(cmds[0])
    self.assertEqual(src, abt.THREAD_END_DEVICE_JOINER_HELPER)
    compile(src, 'helper', 'exec')


class ThreadDiagnosticsTest(unittest.TestCase):

  def test_required_commands_present(self):
    for cmd in (
        'cat /tmp/joiner_helper.log',
        'cat /tmp/chip-all-clusters.log',
        'ip -6 addr',
        'ip -6 route show table all',
        'ip -6 route get fd11:33::1',
        'ot-ctl state',
        'ot-ctl ipaddr',
    ):
      self.assertIn(cmd, abt.END_DEVICE_DIAG_CMDS)
    for cmd in (
        'cat /tmp/otbr-agent.log',
        'ip -6 route show table all',
        'ss -lunp',
        'ot-ctl child table',
        'ot-ctl counters ip',
        'ot-ctl srp server host',
        'ot-ctl srp server service',
    ):
      self.assertIn(cmd, abt.TBR_DIAG_CMDS)
    self.assertEqual(
        abt.ANDROID_FULL_LOGCAT_CMD, 'adb logcat -d -v threadtime'
    )

  def test_save_writes_one_file_per_node(self):
    test_inst = _new_test()
    test_inst.non_ap_devices = [
        {'id': 'aaaaaaaa1111', 'type': 'ThreadBorderRouter'},
        {'id': 'bbbbbbbb2222', 'type': 'CHIPEndDevice'},
        {'id': 'cccccccc3333', 'type': 'android_emulator'},
        {'id': 'dddddddd4444', 'type': 'wifi_ap'},
    ]

    def fake_exec(dev_id, cmd, stream=False):
      self.assertTrue(stream)
      if 'ss -lunp' in cmd:
        raise RuntimeError('ss missing')
      return mock.Mock(content=f'{dev_id}:{cmd}'.encode('utf-8'))

    test_inst.execute_device_cmd = mock.MagicMock(side_effect=fake_exec)
    with tempfile.TemporaryDirectory() as tmp:
      with mock.patch.dict(os.environ, {'DEVICE_LOG_DIR': tmp}):
        test_inst._save_thread_diagnostics(123)
      names = sorted(os.listdir(tmp))
      self.assertEqual(
          names,
          [
              'CHIPEndDevice-123-bbbbbbbb-thread-diag.log',
              'ThreadBorderRouter-123-aaaaaaaa-thread-diag.log',
              'android_emulator-123-cccccccc-logcat-full.log',
          ],
      )
      with open(os.path.join(tmp, names[0]), 'rb') as fp:
        ed = fp.read().decode('utf-8')
      with open(os.path.join(tmp, names[1]), 'rb') as fp:
        tbr = fp.read().decode('utf-8')
      with open(os.path.join(tmp, names[2]), 'rb') as fp:
        logcat = fp.read().decode('utf-8')
    for cmd in abt.END_DEVICE_DIAG_CMDS:
      self.assertIn(f'=== {cmd} ===', ed)
    self.assertIn('sh -c "ip -6 route get fd11:33::1 2>&1"', ed)
    self.assertIn('<command failed: ss missing>', tbr)
    self.assertIn('=== ot-ctl counters ip ===', tbr)
    self.assertIn('adb logcat -d -v threadtime', logcat)

  def test_save_device_logs_calls_thread_diagnostics(self):
    test_inst = _new_test()
    with mock.patch.object(
        abt.CHIPVirtualHome, 'save_device_logs'
    ), mock.patch.object(
        abt, 'save_android_emulator_diagnostics'
    ), mock.patch.object(
        test_inst, '_save_thread_diagnostics'
    ) as diag:
      test_inst.save_device_logs()
    diag.assert_called_once()


class SessionKeylogTest(unittest.TestCase):

  KEY_LINE = (
      'MATTER_SESSION_KEYS v=1 type=PASE role=responder local_sid=1'
      ' peer_sid=2 local_node=0000000000000000 peer_node=0000000000000000'
      f' i2r={"11" * 16} r2i={"22" * 16}'
  )

  def test_end_device_starts_with_keylog_env(self):
    test_inst = _new_test()
    test_inst.execute_device_cmd = mock.MagicMock(return_value='')
    test_inst.get_device_pretty_id = mock.MagicMock(return_value='ED')
    test_inst._start_ble_thread_end_devices(['device2'])
    cmds = _cmds_for(test_inst.execute_device_cmd, 'device2')
    reset = cmds.index(abt.RESET_SESSION_KEYLOG_CMD)
    (start,) = [i for i, c in enumerate(cmds) if 'CHIPCirqueDaemon.py' in c]
    self.assertLess(reset, start)
    self.assertIn(
        'CHIP_SESSION_KEYLOG_FILE=/tmp/matter_session_keys.log', cmds[start]
    )

  def test_save_copies_keylog_next_to_captures(self):
    test_inst = _new_test()
    test_inst.non_ap_devices = [
        {'id': 'bbbbbbbb2222', 'type': 'CHIPEndDevice'},
        {'id': 'aaaaaaaa1111', 'type': 'ThreadBorderRouter'},
    ]
    test_inst.execute_device_cmd = mock.MagicMock(
        return_value={'output': self.KEY_LINE + '\n'}
    )
    with tempfile.TemporaryDirectory() as tmp:
      with mock.patch.dict(
          os.environ, {'DEVICE_LOG_DIR': tmp, 'CIRQUE_PCAP_DIR': tmp}
      ), mock.patch.object(
          abt.CHIPVirtualHome, 'save_device_logs'
      ), mock.patch.object(
          abt, 'save_android_emulator_diagnostics'
      ), mock.patch.object(test_inst, '_save_thread_diagnostics'):
        test_inst.save_device_logs()
      with open(os.path.join(tmp, 'matter_session_keys.txt')) as fp:
        content = fp.read()
    test_inst.execute_device_cmd.assert_called_once_with(
        'bbbbbbbb2222', 'cat /tmp/matter_session_keys.log'
    )
    self.assertIn('TEST-ONLY', content)
    self.assertIn(self.KEY_LINE, content)


if __name__ == '__main__':
  unittest.main()
