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
"""

import importlib
import os
import sys
import unittest
from unittest import mock

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_CIRQUE_REPO = os.path.abspath(
    os.path.join(_THIS_DIR, '..', '..', '..', 'third_party', 'cirque', 'repo')
)
for _path in (_THIS_DIR, _CIRQUE_REPO):
  if _path not in sys.path:
    sys.path.insert(0, _path)

import AndroidTvCastingTest as casting_mod


class TestAndroidTvCastingHelpers(unittest.TestCase):
  """Unit tests for AndroidTvCastingTest Wi-Fi, mDNS, and UI helpers."""

  def _make_harness(self):
    harness = casting_mod.AndroidTvCastingTest.__new__(
        casting_mod.AndroidTvCastingTest
    )
    harness.logger = mock.MagicMock()
    return harness

  def test_cirque_url_reads_env_override(self):
    with mock.patch.dict(
        os.environ, {'CIRQUE_URL': 'http://127.0.0.1:5087'}, clear=False
    ):
      importlib.reload(casting_mod)
      self.assertEqual(casting_mod.CIRQUE_URL, 'http://127.0.0.1:5087')
    with mock.patch.dict(os.environ, {}, clear=False):
      os.environ.pop('CIRQUE_URL', None)
      importlib.reload(casting_mod)
      self.assertEqual(casting_mod.CIRQUE_URL, 'http://localhost:5000')

  def test_casting_ui_shows_connected_and_discovered(self):
    connected_xml = (
        'UI hierchary dumped to: /data/local/tmp/dump.xml\n'
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<hierarchy rotation="0">'
        '<node resource-id="com.chip.casting:id/connectionFragmentNextButton"'
        ' enabled="true" text="Next" bounds="[10,20][110,80]" />'
        '</hierarchy>'
    )
    self.assertTrue(casting_mod.casting_ui_shows_connected(connected_xml))
    self.assertFalse(casting_mod.casting_ui_shows_discovered(connected_xml))

    discovered_xml = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<hierarchy rotation="0">'
        '<node resource-id='
        '"com.chip.casting:id/commissionable_player_description"'
        ' text="Test TV Player" bounds="[20,100][400,200]" />'
        '</hierarchy>'
    )
    self.assertTrue(casting_mod.casting_ui_shows_discovered(discovered_xml))

    empty_discovered_xml = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<hierarchy rotation="0">'
        '<node resource-id='
        '"com.chip.casting:id/commissionable_player_description"'
        ' text="   " bounds="[20,100][400,200]" />'
        '</hierarchy>'
    )
    self.assertFalse(
        casting_mod.casting_ui_shows_discovered(empty_discovered_xml)
    )

  def test_ensure_emulator_wifi_ready_recovers_disconnected_wifi(self):
    harness = self._make_harness()
    executed_cmds = []
    attempt = [0]

    def fake_exec(container_id, cmd):
      self.assertEqual(container_id, 'emu-1')
      executed_cmds.append(cmd)
      if 'cmd wifi status' in cmd:
        attempt[0] += 1
        if attempt[0] == 1:
          return {'return_code': '0', 'output': 'Wifi is disconnected\n'}
        return {
            'return_code': '0',
            'output': 'Wifi is connected to "AndroidWifi"\n',
        }
      if 'dumpsys connectivity' in cmd:
        if attempt[0] == 1:
          return {
              'return_code': '0',
              'output': (
                  'Current Networks:\n'
                  '  NetworkAgentInfo{network{101} ni{MOBILE CONNECTED}}\n'
                  'Network Connectivity Log:\n'
                  '  [102 WIFI] going from CONNECTING to CONNECTED\n'
              ),
          }
        return {
            'return_code': '0',
            'output': (
                'Current Networks:\n'
                '  NetworkAgentInfo{network{102} ni{WIFI CONNECTED}}\n'
            ),
        }
      if 'ip -4 addr show' in cmd:
        if attempt[0] == 1:
          return {'return_code': '0', 'output': '3: wlan0: mtu 1500\n'}
        return {
            'return_code': '0',
            'output': '    inet 10.0.1.20/24 brd 10.0.1.255 dev wlan0\n',
        }
      return {'return_code': '0', 'output': 'ok\n'}

    harness.execute_device_cmd = fake_exec
    with mock.patch.object(casting_mod.time, 'sleep'):
      ready = harness._ensure_emulator_wifi_ready(
          'emu-1', expected_ip='10.0.1.11', timeout_sec=10.0
      )
    self.assertTrue(ready)
    self.assertEqual(attempt[0], 2)
    joined = '\n'.join(executed_cmds)
    self.assertIn('captive_portal_mode 0', joined)
    self.assertIn('captive_portal_detection_enabled 0', joined)
    self.assertIn('cmd wifi connect-network AndroidWifi open', joined)
    self.assertIn('224.0.0.0/4', joined)

  def test_wait_for_tv_mdns_ready_waits_for_commissioner_and_settle(self):
    harness = self._make_harness()
    polls = [0]

    def fake_exec(container_id, cmd):
      self.assertEqual(container_id, 'tv-1')
      if 'logcat' in cmd:
        polls[0] += 1
        if polls[0] == 1:
          return {
              'return_code': '0',
              'output': (
                  'I serviceDiscovery: [MdnsAdvertiser] Adding service '
                  'name: B2EE, type: _matterd._udp\n'
              ),
          }
        if polls[0] == 2:
          return {
              'return_code': '0',
              'output': (
                  'I serviceDiscovery: [MdnsAdvertiser] Adding service '
                  'name: B2EE, type: _matterd._udp\n'
                  'I SPT     : InitCommissioner nodeId=0x1\n'
                  'I serviceDiscovery: [MdnsAdvertiser] Adding service '
                  'name: A2EE, type: _matterc._udp\n'
              ),
          }
        return {
            'return_code': '0',
            'output': (
                'I serviceDiscovery: [MdnsAdvertiser] Adding service '
                'name: B2EE, type: _matterd._udp\n'
                'I SPT     : InitCommissioner nodeId=0x1\n'
                'I serviceDiscovery: [MdnsAdvertiser] Adding service '
                'name: A2EE, type: _matterc._udp\n'
                'I serviceDiscovery: [MdnsAdvertiser.wlan0] Probing\n'
            ),
        }
      return {'return_code': '0', 'output': ''}

    harness.execute_device_cmd = fake_exec
    with mock.patch.object(casting_mod.time, 'sleep'):
      ready = harness._wait_for_tv_mdns_ready(
          'tv-1', timeout_sec=20.0, settle_sec=0.0
      )
    self.assertTrue(ready)
    self.assertGreaterEqual(polls[0], 3)

  def test_wait_for_casting_discovery_reclicks_start_discovery_and_matches_ui(
      self,
  ):
    harness = self._make_harness()
    click_calls = []

    def fake_click(container_id, resource_id=None, text=None):
      click_calls.append((container_id, resource_id, text))
      return True

    dump_xml_empty = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<hierarchy rotation="0">'
        '<node resource-id="com.chip.casting:id/startDiscoveryButton"'
        ' text="Start Discovery" bounds="[10,10][200,80]" />'
        '</hierarchy>'
    )
    dump_xml_found = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<hierarchy rotation="0">'
        '<node resource-id='
        '"com.chip.casting:id/commissionable_player_description"'
        ' text="Matter TV" bounds="[10,100][400,180]" />'
        '</hierarchy>'
    )
    ui_polls = [0]

    def fake_exec(container_id, cmd):
      self.assertEqual(container_id, 'cast-1')
      if 'logcat' in cmd:
        return {'return_code': '0', 'output': ''}
      if 'uiautomator dump' in cmd:
        ui_polls[0] += 1
        if ui_polls[0] >= 4:
          return {'return_code': '0', 'output': dump_xml_found}
        return {'return_code': '0', 'output': dump_xml_empty}
      return {'return_code': '0', 'output': ''}

    harness.execute_device_cmd = fake_exec
    harness._click_ui_element = fake_click

    clock_values = iter([0.0, 0.0, 10.0, 21.0, 23.0])
    with (
        mock.patch.object(
            casting_mod.time, 'time', side_effect=lambda: next(clock_values)
        ),
        mock.patch.object(casting_mod.time, 'sleep'),
    ):
      found = harness._wait_for_casting_discovery(
          'cast-1', timeout_sec=60.0, rediscover_interval_sec=20.0
      )
    self.assertTrue(found)
    self.assertIn(('cast-1', 'startDiscoveryButton', None), click_calls)

  def test_wait_for_casting_discovery_matches_logcat_directly(self):
    harness = self._make_harness()

    def fake_exec(container_id, cmd):
      del container_id
      if 'logcat' in cmd:
        return {
            'return_code': '0',
            'output': 'I Discovery: Discovered CastingPlayer deviceId: 1234\n',
        }
      return {'return_code': '0', 'output': ''}

    harness.execute_device_cmd = fake_exec
    with mock.patch.object(casting_mod.time, 'sleep'):
      self.assertTrue(
          harness._wait_for_casting_discovery('cast-1', timeout_sec=10.0)
      )


if __name__ == '__main__':
  unittest.main()
