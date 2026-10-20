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

Unit and live binary tests for POSIX-hosted Silicon Labs Lighting-App
(chip-silabs-lighting-app) and AndroidBleWiFiSilabsLightingTest.
"""

import errno
import os
import pty
import select
import signal
import struct
import subprocess
import tempfile
import time
import tty
import unittest

from AndroidBleWiFiMobileDeviceTest import TestAndroidBleWiFiMobileDevice
from AndroidBleWiFiSilabsLightingTest import TestAndroidBleWiFiSilabsLightingDevice
from helper.paths import (
    ARM_SYSROOT_ESC,
    ARM_SYSROOT_PATH,
    CHIP_ALL_CLUSTERS_APP_ESC,
    CHIP_SILABS_LIGHTING_APP_ESC,
    CHIP_SILABS_LIGHTING_APP_PATH,
    PTY_BRIDGE_LINUX_ESC,
    QEMU_ARM_ESC,
    QEMU_ARM_PATH,
)
from helper.pcap_oracles import (
    DLT_BLUETOOTH_HCI_H4_WITH_PHDR,
    DLT_EN10MB,
    dhcp_ack_addresses,
    eapol_frame_count,
    hci_le_connection_complete_handles,
)

# PARSER-ONLY FIXTURE. These lines are hand-written input for
# _assert_ble_wifi_device_logs() so its sequence matching (and its negative
# cases) can be unit tested without a Cirque topology. They are not output of
# any device run and are not evidence that the E2E flow works; the E2E test
# asserts the radio milestones against raw pcaps (_assert_silabs_rf_captures).
SILABS_DEVICE_LOG_LINES = [
    '[1000.010] [185:185] [DL] Got WiFi interface: wlan0',
    '[1000.011] [185:185] [SVR] [SILABS] =====SL-Light starting=====',
    '[1000.012] [185:193] [SVR] [SILABS-LIGHT] WSTK LEDs initialized',
    '[1000.012] [185:193] [SVR] [SILABS-LIGHT] LED 1 state=OFF',
    '[1000.012] [185:193] [SVR] [SILABS] App Task started',
    '[1000.015] [185:191] [DL] GATT application registered successfully',
    '[1003.100] [185:191] [DL] New BLE connection: conn=0x40',
    '[1003.200] [185:185] [BLE] selected BTP version 4',
    '[1003.300] [185:185] [DL] Receive kCHIPoBLEConnectionEstablished',
    (
        '[1003.400] [185:185] [SVR] Commissioning completed session'
        ' establishment step'
    ),
    (
        '[1004.100] [185:185] [DMG] Received command for Endpoint=0'
        ' Cluster=0x0000_0031 Command=0x0000_0002'
    ),
    (
        '[1004.200] [185:185] [NP] LinuxWiFiDriver: ConnectNetwork'
        " 'CHIP-VirtualWiFi-AP'"
    ),
    (
        '[1005.000] [185:191] [DL] [SILABS-WIFI] wpa_supplicant'
        " State='completed' (read over D-Bus)"
    ),
    '[1006.000] [185:185] [SVR] [SILABS] Commissioning complete event received',
    '[1008.000] [185:185] [ZCL] Toggle ep1 on/off from state 0 to 1',
    '[1008.001] [185:185] [ZCL] Cluster callback: 0x0000_0006',
    '[1008.001] [185:185] [SVR] [SILABS-LIGHT] LED 1 state=ON',
    '[1009.000] [185:185] [EM] >>> Type 0001:02 (IM:ReadRequest)',
    '[1009.001] [185:185] [EM] <<< Type 0001:05 (IM:ReportData)',
]

HCI_RESET_CMD = b'\x01\x03\x0c\x00'
# HCI Command Complete: num_hci_cmd_pkts=1, opcode=0x0C03, status=0x00.
HCI_RESET_COMPLETE_EVT = b'\x04\x0e\x04\x01\x03\x0c\x00'


def _pcap_bytes(linktype, frames):
  out = struct.pack('<IHHiIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, linktype)
  for i, frame in enumerate(frames):
    out += struct.pack('<IIII', i, 0, len(frame), len(frame)) + frame
  return out


def _le_conn_complete_frame(status, handle):
  # 4-byte direction header + H4 event 0x3E, LE Connection Complete (0x01).
  params = bytes([0x01, status, handle & 0xFF, handle >> 8, 0x01, 0x00])
  params += b'\x11\x22\x33\x44\x55\x66' + b'\x18\x00\x00\x00\x48\x00\x00'
  return b'\x00\x00\x00\x01' + bytes([0x04, 0x3E, len(params)]) + params


def _dhcp_frame(msg_type, yiaddr):
  bootp = bytearray(240)
  bootp[0] = 2
  bootp[16:20] = bytes(int(x) for x in yiaddr.split('.'))
  bootp[236:240] = b'\x63\x82\x53\x63'
  payload = bytes(bootp) + bytes([53, 1, msg_type, 0xFF])
  udp = struct.pack('!HHHH', 67, 68, 8 + len(payload), 0) + payload
  ip = struct.pack(
      '!BBHHHBBH4s4s',
      0x45,
      0,
      20 + len(udp),
      0,
      0,
      64,
      17,
      0,
      b'\x0a\x00\x01\x01',
      b'\xff\xff\xff\xff',
  )
  return b'\xff' * 6 + b'\x02\x00\x00\x00\x01\x01' + b'\x08\x00' + ip + udp


class TestPcapOracles(unittest.TestCase):
  """Parser tests on constructed frames, with negative controls."""

  def _write(self, tmpdir, name, linktype, frames):
    path = os.path.join(tmpdir, name)
    with open(path, 'wb') as f:
      f.write(_pcap_bytes(linktype, frames))
    return path

  def test_le_connection_complete_requires_success_status(self):
    with tempfile.TemporaryDirectory() as tmpdir:
      ok = self._write(
          tmpdir,
          'ok.pcap',
          DLT_BLUETOOTH_HCI_H4_WITH_PHDR,
          [_le_conn_complete_frame(0x00, 0x40)],
      )
      failed = self._write(
          tmpdir,
          'failed.pcap',
          DLT_BLUETOOTH_HCI_H4_WITH_PHDR,
          [_le_conn_complete_frame(0x3E, 0x40)],
      )
      wrong_dlt = self._write(
          tmpdir, 'eth.pcap', DLT_EN10MB, [_le_conn_complete_frame(0x00, 0x40)]
      )
      self.assertEqual(hci_le_connection_complete_handles(ok), [0x40])
      self.assertEqual(hci_le_connection_complete_handles(failed), [])
      self.assertEqual(hci_le_connection_complete_handles(wrong_dlt), [])

  def test_dhcp_ack_only_counts_ack_messages(self):
    with tempfile.TemporaryDirectory() as tmpdir:
      path = self._write(
          tmpdir,
          'dhcp.pcap',
          DLT_EN10MB,
          [
              _dhcp_frame(2, '10.0.1.30'),  # OFFER must not count
              _dhcp_frame(5, '10.0.1.21'),  # ACK
          ],
      )
      self.assertEqual(dhcp_ack_addresses(path), ['10.0.1.21'])

  def test_eapol_count_by_ethertype(self):
    with tempfile.TemporaryDirectory() as tmpdir:
      eapol = b'\x00' * 12 + b'\x88\x8e' + b'\x02\x03\x00\x5f'
      ipv4 = _dhcp_frame(5, '10.0.1.21')
      path = self._write(tmpdir, 'ap.pcap', DLT_EN10MB, [eapol, ipv4, eapol])
      self.assertEqual(eapol_frame_count(path), 2)

  @unittest.skipUnless(
      os.environ.get('CIRQUE_SILABS_E2E_PCAP_DIR'),
      'CIRQUE_SILABS_E2E_PCAP_DIR not set (point it at a real E2E device_logs)',
  )
  def test_real_e2e_captures(self):
    pcap_dir = os.environ['CIRQUE_SILABS_E2E_PCAP_DIR']
    self.assertTrue(
        hci_le_connection_complete_handles(
            os.path.join(pcap_dir, 'bt_hci_silabs_hci0.pcap')
        )
    )
    self.assertGreaterEqual(
        eapol_frame_count(os.path.join(pcap_dir, 'wifi_ap.pcap')), 4
    )
    self.assertTrue(
        dhcp_ack_addresses(os.path.join(pcap_dir, 'wifi_dhcp_server.pcap'))
    )


class TestSilabsLightingAppPosix(unittest.TestCase):

  def _make_silabs_harness(self):
    harness = TestAndroidBleWiFiSilabsLightingDevice(
        {'device1': {'type': 'CHIPEndDevice', 'id': 'device1'}}
    )
    harness.get_device_pretty_id = lambda x: 'CHIPEndDevice'
    return harness

  def test_silabs_lighting_device_class_selects_silabs_binary(self):
    silabs_test = self._make_silabs_harness()
    self.assertTrue(silabs_test._is_silabs_lighting_app())
    self.assertEqual(
        silabs_test._get_end_device_binary_esc(),
        CHIP_SILABS_LIGHTING_APP_ESC,
    )
    default_test = TestAndroidBleWiFiMobileDevice(
        {'device1': {'type': 'CHIPEndDevice', 'id': 'device1'}},
        end_device_app='all-clusters',
    )
    self.assertFalse(default_test._is_silabs_lighting_app())
    self.assertEqual(
        default_test._get_end_device_binary_esc(),
        CHIP_ALL_CLUSTERS_APP_ESC,
    )

  def test_start_wifi_end_devices_invokes_silabs_lighting_binary(self):
    silabs_test = self._make_silabs_harness()
    executed_cmds = []
    silabs_test.execute_device_cmd = lambda dev_id, cmd: executed_cmds.append(
        (dev_id, cmd)
    )
    silabs_test._start_wifi_end_devices(
        [{'id': 'device1', 'description': {'ble_adapt_id': 2}}],
        '/out/host_libs',
    )
    self.assertEqual(len(executed_cmds), 2)
    prep_cmd = executed_cmds[0][1]
    self.assertIn(PTY_BRIDGE_LINUX_ESC, prep_cmd)
    self.assertIn('/dev/ttySilabsBT0', prep_cmd)
    launch_cmd = executed_cmds[1][1]
    self.assertIn(QEMU_ARM_ESC, launch_cmd)
    self.assertIn(ARM_SYSROOT_ESC, launch_cmd)
    self.assertIn(CHIP_SILABS_LIGHTING_APP_ESC, launch_cmd)
    self.assertIn('--wifi --ble-controller 2 --discriminator 3840', launch_cmd)

  @unittest.skipUnless(
      CHIP_SILABS_LIGHTING_APP_PATH.is_file(),
      f'{CHIP_SILABS_LIGHTING_APP_PATH} not built',
  )
  def test_silabs_lighting_binary_is_32bit_arm_elf(self):
    with open(CHIP_SILABS_LIGHTING_APP_PATH, 'rb') as f:
      hdr = f.read(20)
    self.assertEqual(hdr[:4], b'\x7fELF')
    self.assertEqual(hdr[4], 1, 'Expected ELFCLASS32 (32-bit ELF)')
    e_machine = struct.unpack('<H', hdr[18:20])[0]
    self.assertEqual(e_machine, 40, f'Expected EM_ARM (40), got {e_machine}')

  def test_silabs_lighting_device_log_sequence_assertions(self):
    """Exercises the log-sequence matcher on the parser-only fixture."""
    silabs_test = self._make_silabs_harness()

    def run_with(lines):
      silabs_test.get_device_log = lambda x: '\n'.join(lines).encode('utf-8')
      silabs_test._assert_ble_wifi_device_logs(['device1'])

    run_with(SILABS_DEVICE_LOG_LINES)

    removals = {
        'startup banner': '=====SL-Light starting=====',
        'LED actuation': 'LED 1 state=ON',
        'D-Bus State read-back': "State='completed' (read over D-Bus)",
    }
    for label, needle in removals.items():
      with self.subTest(missing=label):
        with self.assertRaises(AssertionError):
          run_with([l for l in SILABS_DEVICE_LOG_LINES if needle not in l])

    # The old unconditional shim marker must not satisfy the Silabs check.
    legacy = [
        l.replace(
            "[SILABS-WIFI] wpa_supplicant State='completed' (read over D-Bus)",
            'wpa_supplicant: Interface properties changed, state is'
            " 'completed'",
        )
        for l in SILABS_DEVICE_LOG_LINES
    ]
    with self.assertRaises(AssertionError):
      run_with(legacy)

  @unittest.skipUnless(
      CHIP_SILABS_LIGHTING_APP_PATH.is_file() and QEMU_ARM_PATH.is_file(),
      f'{CHIP_SILABS_LIGHTING_APP_PATH} or {QEMU_ARM_PATH} not built',
  )
  def test_live_silabs_lighting_binary_boot_and_button_toggle(self):
    """Runs the ARM32 ELF under qemu-arm with this test acting as the HCI

    controller on the PTY: BLEManagerImpl must not boot until HCI_Reset gets
    a Command Complete, and must then issue the advertising commands.
    """
    with tempfile.TemporaryDirectory(prefix='silabs_posix_test_') as tmpdir:
      fifo_path = os.path.join(tmpdir, 'app_pipe')
      kvs_path = os.path.join(tmpdir, 'chip_kvs')
      log_path = os.path.join(tmpdir, 'silabs.log')
      for stale_ini in (
          '/tmp/chip_factory.ini',
          '/tmp/chip_config.ini',
          '/tmp/chip_counters.ini',
          '/tmp/chip_kvs',
      ):
        if os.path.exists(stale_ini):
          os.remove(stale_ini)

      master_fd, slave_fd = pty.openpty()
      tty.setraw(slave_fd)
      slave_pty_path = os.ttyname(slave_fd)

      env = os.environ.copy()
      env['DBUS_SYSTEM_BUS_ADDRESS'] = (
          f'unix:path={os.path.join(tmpdir, "nonexistent_dbus")}'
      )
      env['SILABS_BT_PTY_PATH'] = slave_pty_path
      hci_rx = bytearray()
      reset_answered = False
      gatt_seen_before_reset = False

      def pump_hci(timeout):
        """Reads controller-bound bytes; returns False once the PTY hangs up."""
        rlist, _, _ = select.select([master_fd], [], [], timeout)
        if not rlist:
          return True
        try:
          hci_rx.extend(os.read(master_fd, 4096))
        except OSError as e:
          # EIO on a PTY master means every slave fd was closed.
          if e.errno == errno.EIO:
            return False
          raise
        return True

      def read_log():
        if not os.path.exists(log_path):
          return ''
        with open(log_path, 'r', encoding='utf-8', errors='replace') as r:
          return r.read()

      with open(log_path, 'wb') as log_file:
        proc = subprocess.Popen(
            [
                str(QEMU_ARM_PATH),
                '-L',
                str(ARM_SYSROOT_PATH),
                str(CHIP_SILABS_LIGHTING_APP_PATH),
                '--KVS',
                kvs_path,
                '--app-pipe',
                fifo_path,
                '--discriminator',
                '3840',
                '--passcode',
                '20202021',
            ],
            stdout=log_file,
            stderr=subprocess.STDOUT,
            env=env,
        )
        try:
          ready = False
          for _ in range(120):
            if not pump_hci(0.05):
              break
            if not reset_answered and HCI_RESET_CMD in hci_rx:
              # Negative control: nothing may claim GATT readiness before
              # the controller has completed HCI_Reset.
              gatt_seen_before_reset = (
                  'GATT application registered successfully' in read_log()
              )
              os.write(master_fd, HCI_RESET_COMPLETE_EVT)
              reset_answered = True
            txt = read_log()
            if (
                os.path.exists(fifo_path)
                and 'Starting event loop' in txt
                and 'GATT application registered successfully' in txt
            ):
              ready = True
              break
            time.sleep(0.05)
          self.assertTrue(reset_answered, 'HCI_Reset was never sent')
          self.assertFalse(
              gatt_seen_before_reset,
              'GATT registration happened before HCI_Reset completed',
          )
          self.assertTrue(
              ready, 'chip-silabs-lighting-app did not enter event loop'
          )

          for _ in range(10):
            if not pump_hci(0.05):
              break

          for _ in range(2):
            with open(fifo_path, 'w', encoding='utf-8') as pipe:
              pipe.write('{"Name":"Button","Button":1,"Action":1}\n')
              pipe.flush()
            time.sleep(0.3)
        finally:
          proc.send_signal(signal.SIGINT)
          try:
            proc.wait(timeout=5)
          except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
          os.close(master_fd)
          os.close(slave_fd)

      log_output = read_log()
      self.assertIn('[SILABS] =====SL-Light starting=====', log_output)
      self.assertIn('[SILABS-LIGHT] WSTK LEDs initialized', log_output)
      self.assertIn('[SILABS-LIGHT] LED 1 state=OFF', log_output)
      self.assertIn('[SILABS] App Task started', log_output)
      self.assertIn('[SILABS-BGAPI] Opened HCI H4 UART PTY', log_output)
      self.assertIn('[SILABS-BGAPI] HCI_Reset Command Complete', log_output)
      self.assertIn('GATT application registered successfully', log_output)
      self.assertIn('Toggle ep1 on/off from state 0 to 1', log_output)
      self.assertIn('Cluster callback: 0x0000_0006', log_output)
      self.assertIn('[SILABS-LIGHT] LED 1 state=ON', log_output)
      self.assertIn('Toggle ep1 on/off from state 1 to 0', log_output)
      # Real HCI H4 commands issued by sl_bt_* over the PTY:
      # 0x0C03 (HCI_Reset), 0x2005 (LE_Set_Random_Address),
      # 0x2008 (LE_Set_Adv_Data), 0x2009 (LE_Set_Scan_Rsp_Data),
      # 0x2006 (LE_Set_Adv_Params), 0x200A (LE_Set_Adv_Enable)
      rx = bytes(hci_rx)
      self.assertIn(HCI_RESET_CMD, rx)
      self.assertIn(b'\x01\x05\x20\x06', rx)
      self.assertIn(b'\x01\x08\x20\x20', rx)
      self.assertIn(b'\x01\x09\x20\x20', rx)
      self.assertIn(b'\x01\x06\x20\x0f', rx)
      self.assertIn(b'\x01\x0a\x20\x01\x01', rx)


if __name__ == '__main__':
  unittest.main()
