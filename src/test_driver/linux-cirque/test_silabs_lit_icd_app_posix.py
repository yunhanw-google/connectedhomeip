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

import base64
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

from AndroidBleThreadMobileDeviceTest import (
    DEVICE_CONFIG,
    TEST_DISCRIMINATOR,
    TestAndroidBleThreadMobileDevice,
)
from helper.paths import CHIP_REPO_STR, PTY_BRIDGE_LINUX_ESC
from helper.pcap_oracles import (
    ieee802154_mac_command_frames,
    matter_icd_checkin_packets,
    thread_decrypted_udp_frames,
    thread_icd_checkin_frames,
)
from helper.wifi_lease import wait_for_device_wlan0_ipv4

if 'device2' in DEVICE_CONFIG:
  del DEVICE_CONFIG['device2']

CHIP_SILABS_LIT_ICD_APP_ESC = os.path.join(
    CHIP_REPO_STR, 'out/arm-silabs-thread/chip-silabs-lit-icd-app'
)

# One row of `ot-ctl child table`:
# | ID | RLOC16 | Timeout | Age | LQ In | C_VN |R|D|N| Ver |CSL|...
_CHILD_TABLE_ROW_RE = re.compile(
    r'^\|\s*\d+\s*\|\s*(0x[0-9a-fA-F]{4})\s*\|(?:[^|]*\|){4}([01])\|',
    re.MULTILINE,
)

# Max time from the Check-In leaving the SED on 802.15.4 to the border router
# forwarding it onto the Wi-Fi medium.
_MAX_BR_FORWARD_DELAY_S = 1.0


def _sleepy_child_rloc16s(child_table: str) -> set:
  """Returns RLOC16s of child-table rows with RxOnWhenIdle (R) == 0."""
  return {
      int(rloc16, 16)
      for rloc16, rx_on_when_idle in _CHILD_TABLE_ROW_RE.findall(child_table)
      if rx_on_when_idle == '0'
  }


def _invert_key_hex(key_hex: str) -> str:
  return ''.join(f'{0xFF - b:02x}' for b in bytes.fromhex(key_hex))


SRP_BRIDGER_SCRIPT = """import subprocess, time, os, re

os.makedirs('/etc/avahi/services', exist_ok=True)
known = {}

def sync():
    out = subprocess.check_output(['ot-ctl', 'srp', 'server', 'service'], text=True, stderr=subprocess.DEVNULL)
    blocks = []
    cur_block = []
    for raw_line in out.splitlines():
        line_r = raw_line.rstrip()
        if not line_r or line_r == 'Done':
            continue
        if not line_r.startswith(' ') and not line_r.startswith('\\t'):
            if cur_block:
                blocks.append('\\n'.join(cur_block))
            cur_block = [line_r]
        else:
            cur_block.append(line_r)
    if cur_block:
        blocks.append('\\n'.join(cur_block))
    current = {}
    for b in blocks:
        lines = [l.strip() for l in b.strip().split('\\n') if l.strip()]
        if not lines or 'deleted: true' in b:
            continue
        inst = lines[0].split('.')[0].strip()
        if not inst or '-' not in inst:
            continue
        # Do not bridge the Android commissioner's own SRP entry back onto Wi-Fi mDNS
        if inst.endswith('-000000000001B669'):
            continue
        port = '5540'
        host = ''
        ips = []
        for l in lines[1:]:
            if l.startswith('port:'):
                port = l.split(':', 1)[1].strip()
            elif l.startswith('host:'):
                host = l.split(':', 1)[1].strip().split('.')[0]
            elif l.startswith('addresses:'):
                m = re.search(r'\\[(.*?)\\]', l)
                if m and m.group(1).strip():
                    ips = [x.strip() for x in m.group(1).split(',') if x.strip()]
        if host == 'android-chiptool':
            continue
        if inst and host and ips:
            current[inst] = (host, port, tuple(ips))

    if current and current != known:
        for inst, (host, port, ips) in current.items():
            if known.get(inst) == (host, port, ips):
                continue
            chosen_ip = ips[0]
            for ip in ips:
                if not ip.startswith('fdde:ad00:beef'):
                    chosen_ip = ip
                    break
            with open('/etc/avahi/hosts', 'a+') as hf:
                hf.seek(0)
                if chosen_ip not in hf.read():
                    hf.write(f'\\n{chosen_ip} {host}.local\\n')
            sub_tag = ''
            if '-' in inst:
                fid = inst.split('-')[0]
                sub_tag = f'    <subtype>_I{fid}._sub._matter._tcp</subtype>\\n'
            xml = f'''<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="no">{inst}</name>
  <service protocol="ipv6">
    <type>_matter._tcp</type>
{sub_tag}    <host-name>{host}.local</host-name>
    <port>{port}</port>
    <txt-record>SII=500</txt-record>
    <txt-record>SAI=300</txt-record>
    <txt-record>SAT=4000</txt-record>
    <txt-record>T=1</txt-record>
    <txt-record>ICD=1</txt-record>
  </service>
</service-group>
'''
            short_inst = inst[-12:] if len(inst) > 12 else inst
            with open(f'/etc/avahi/services/srp_{short_inst}.service', 'w') as sf:
                sf.write(xml)
        subprocess.run(['killall', '-HUP', 'avahi-daemon'], stderr=subprocess.DEVNULL)
        known.clear()
        known.update(current)

while True:
    try:
        sync()
    except (subprocess.SubprocessError, OSError) as exc:
        print('srp_bridger sync warning:', exc, flush=True)
    time.sleep(1)
"""


class TestAndroidBleThreadSilabsLitIcdDevice(TestAndroidBleThreadMobileDevice):

  def __init__(self, device_config, *args, **kwargs):
    if 'device2' in device_config:
      del device_config['device2']
    super().__init__(device_config, *args, **kwargs)
    self.app_proc = None
    self.pty_proc = None
    self.app_log_file = None

  def _get_end_device_binary_esc(self) -> str:
    return shlex.quote(
        f'{CHIP_REPO_STR}/out/arm-silabs-thread/chip-silabs-lit-icd-app'
    )

  def _cleanup_host_processes(self):
    if self.app_proc:
      try:
        self.app_proc.terminate()
        self.app_proc.wait(timeout=3)
      except (subprocess.SubprocessError, OSError):
        try:
          self.app_proc.kill()
        except OSError as exc:
          self.logger.debug('app_proc kill ignored: %s', exc)
      self.app_proc = None
    if self.pty_proc:
      try:
        self.pty_proc.terminate()
        self.pty_proc.wait(timeout=3)
      except (subprocess.SubprocessError, OSError):
        try:
          self.pty_proc.kill()
        except OSError as exc:
          self.logger.debug('pty_proc kill ignored: %s', exc)
      self.pty_proc = None
    if self.app_log_file:
      try:
        self.app_log_file.close()
      except OSError as exc:
        self.logger.debug('app_log_file close ignored: %s', exc)
      self.app_log_file = None
    subprocess.run(
        "pkill -9 -f '[c]hip-silabs-lit-icd-app' 2>/dev/null || true",
        shell=True,
    )
    subprocess.run(
        'killall -9 pty_bridge_linux 2>/dev/null || true', shell=True
    )

  def _get_thread_radio_context(self, tbr_id: str):
    """Returns (network_key_hex, child_table, sleepy_child_rloc16s) from TBR."""
    key_res = self.execute_device_cmd(tbr_id, 'ot-ctl networkkey')
    key_out = (
        key_res.get('output', '') if isinstance(key_res, dict) else str(key_res)
    )
    key_match = re.search(r'\b([0-9a-fA-F]{32})\b', key_out)
    self.assertTrue(
        key_match is not None,
        f'Could not read Thread network key from TBR {tbr_id}: {key_out!r}',
    )
    child_res = self.execute_device_cmd(tbr_id, 'ot-ctl child table')
    child_table = (
        child_res.get('output', '')
        if isinstance(child_res, dict)
        else str(child_res)
    )
    return (
        key_match.group(1).lower(),
        child_table,
        _sleepy_child_rloc16s(child_table),
    )

  def _install_enhanced_srp_bridger(self, tbr_id: str):
    b64_script = base64.b64encode(SRP_BRIDGER_SCRIPT.encode('utf-8')).decode(
        'ascii'
    )
    self.execute_device_cmd(
        tbr_id,
        'sh -c "pkill -9 -f \'[s]rp_bridger\' 2>/dev/null || true"',
    )
    self.execute_device_cmd(
        tbr_id,
        f"sh -c 'echo {b64_script} | base64 -d > /tmp/srp_bridger.py && cp"
        " /tmp/srp_bridger.py /tmp/srp_bridger_lit.py'",
    )
    self.execute_device_cmd(
        tbr_id,
        "sh -c 'nohup python3 /tmp/srp_bridger_lit.py >/tmp/srp_bridger.log"
        " 2>&1 </dev/null &'",
    )

  def run_android_ble_thread_commissioning_test(self):
    all_devs = list(self.device_config.values())
    tbr_devices = [d for d in all_devs if d.get('type') == 'ThreadBorderRouter']
    req_devices = [
        d
        for d in all_devs
        if d.get('type') in ('android_emulator', 'Android_Emulator')
    ]

    if not tbr_devices:
      raise RuntimeError('No ThreadBorderRouter in device_config')
    if not req_devices:
      raise RuntimeError('No android_emulator in device_config')

    tbr_ids = [d['id'] for d in tbr_devices]
    req_device_id = req_devices[0]['id']

    self._setup_android_emulator_node(req_device_id)
    super()._isolate_eth0_and_start_avahi(tbr_ids)
    tbr_dataset = self._setup_thread_border_routers(tbr_ids)
    self._install_enhanced_srp_bridger(tbr_ids[0])

    self.logger.info(
        'Setting up host QEMU execution for chip-silabs-lit-icd-app'
    )
    self._cleanup_host_processes()
    subprocess.run(
        'rm -rf /tmp/chip_kvs* /tmp/chip_*.ini /tmp/chip_ot_node2.log'
        ' /tmp/openthread-simulation 0_2.flash tmp/0_2.flash 2>/dev/null ||'
        ' true',
        shell=True,
    )
    time.sleep(0.5)

    hci_port = 23458
    try:
      bt_info = self.query_api('virtual_bt_info')
      if isinstance(bt_info, dict):
        hci_port = int(bt_info.get('hci_port', 23458) or 23458)
    except Exception as exc:
      self.logger.warning(
          'virtual_bt_info lookup failed (%s); using hci_port=%d', exc, hci_port
      )

    port_offset = '0'
    try:
      pids = subprocess.check_output(['pgrep', 'ot-rcp']).decode().split()
      for pid in pids:
        with open(f'/proc/{pid}/environ', 'rb') as env_f:
          env = env_f.read().decode('utf-8', errors='ignore')
          if 'PORT_OFFSET=' in env:
            for item in env.split('\0'):
              if item.startswith('PORT_OFFSET='):
                port_offset = item.split('=')[1]
                break
    except Exception as exc:
      self.logger.warning('Failed to find PORT_OFFSET from ot-rcp: %s', exc)

    self.logger.info(
        'Starting pty_bridge_linux on host (/tmp/ttySilabsBT0 -> silabs_hci0)'
    )
    self.pty_proc = subprocess.Popen(
        f'{PTY_BRIDGE_LINUX_ESC} /tmp/ttySilabsBT0 127.0.0.1 {hci_port}'
        ' silabs_hci0',
        shell=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.0)
    subprocess.run(
        'chmod 666 /tmp/ttySilabsBT0 2>/dev/null || true', shell=True
    )

    host_libs_dir = f'{CHIP_REPO_STR}/out/host_libs'
    app_log_path = '/tmp/chip-all-clusters.log'
    open(app_log_path, 'w').close()
    self.app_log_file = open(app_log_path, 'a')

    disable_reg = os.environ.get('DISABLE_ICD_REGISTRATION', '0')
    cmd = (
        'ulimit -c 0; env SILABS_BT_PTY_PATH=/tmp/ttySilabsBT0'
        f' PORT_OFFSET={port_offset} LD_LIBRARY_PATH={host_libs_dir} qemu-arm'
        f' -L /usr/arm-linux-gnueabihf/ {self._get_end_device_binary_esc()}'
        ' --thread-node-id 2 --ble-controller 0 --discriminator'
        f' {TEST_DISCRIMINATOR}'
    )
    self.logger.info(
        'Starting host chip-silabs-lit-icd-app under qemu-arm: %s', cmd
    )
    self.app_proc = subprocess.Popen(
        cmd, shell=True, stdout=self.app_log_file, stderr=self.app_log_file
    )
    time.sleep(8.0)

    tbr_ip = wait_for_device_wlan0_ipv4(self, tbr_ids[0])
    self.assertTrue(
        bool(tbr_ip and tbr_ip.startswith('10.0.1.')),
        f'Invalid TBR IP: {tbr_ip}',
    )

    self.execute_device_cmd(
        req_device_id,
        'adb shell su root ip -6 addr add fd11:22::3/64 dev wlan0 nodad'
        ' 2>/dev/null || true',
    )
    for prefix in ('fdde:ad00:beef:0::/64', 'fd11:33::/64'):
      self.execute_device_cmd(
          req_device_id,
          f'adb shell su root ip -6 route add {prefix} via fd11:22::2 dev wlan0'
          ' 2>/dev/null || true',
      )
      self.execute_device_cmd(
          req_device_id,
          f'adb shell su root ip -6 route add {prefix} via fd11:22::2 dev wlan0'
          ' table wlan0 2>/dev/null || true',
      )
      self.execute_device_cmd(
          req_device_id,
          f'adb shell su root ip -6 route add {prefix} via fd11:22::2 dev wlan0'
          ' table eth0 2>/dev/null || true',
      )
    self.execute_device_cmd(
        req_device_id,
        'adb shell su root ip -6 route add ff02::fb/128 dev wlan0 table eth0'
        ' 2>/dev/null || true',
    )

    is_lit_str = 'false' if disable_reg == '1' else 'true'
    self.logger.info(
        'Driving CHIPTool commissioning over BLE + Thread via ADB Intent'
        ' (isLIT=%s)',
        is_lit_str,
    )
    self.execute_device_cmd(req_device_id, 'adb logcat -c')
    self.execute_device_cmd(
        req_device_id, 'adb shell am force-stop com.google.chip.chiptool'
    )
    time.sleep(0.5)

    intent_cmd = (
        'adb shell am start -n com.google.chip.chiptool/.CHIPToolActivity '
        '-a com.google.chip.chiptool.action.COMMISSION_BLE_THREAD '
        f'--ei discriminator {TEST_DISCRIMINATOR} --el setupPinCode 20202021 '
        f'--ez isLIT {is_lit_str} '
        '--es threadOperationalDataset '
        + tbr_dataset.get('dataset_tlvs_hex', '')
    )
    self.execute_device_cmd(req_device_id, intent_cmd)

    comm_success = False
    registered_chiptool_srp = None
    start_wait = time.time()
    while time.time() - start_wait < 120.0:
      res = self.execute_device_cmd(
          req_device_id, 'adb shell "logcat -d -v epoch"'
      )
      out = res.get('output', '') if isinstance(res, dict) else str(res)
      pub_matches = re.findall(
          r'publish'
          r' serviceName=([0-9A-F]{16}-000000000001B669)\s+type=_matter\._tcp\S*\s+port=(\d+)',
          out,
      )
      if pub_matches:
        svc_name, svc_port = pub_matches[-1]
        if (svc_name, svc_port) != registered_chiptool_srp:
          self.logger.info(
              'Registering Android CHIPTool operational node %s (port=%s,'
              ' ip=fd11:22::3) in TBR SRP server',
              svc_name,
              svc_port,
          )
          srp_cmd = (
              'sh -c "ot-ctl srp client stop >/dev/null 2>&1 || true; ot-ctl'
              ' srp client host clear >/dev/null 2>&1 || true; ot-ctl srp'
              ' client host name android-chiptool; ot-ctl srp client host'
              f' address fd11:22::3; ot-ctl srp client service add {svc_name}'
              f' _matter._tcp {svc_port}; ot-ctl srp client autostart enable"'
          )
          self.execute_device_cmd(tbr_ids[0], srp_cmd)
          registered_chiptool_srp = (svc_name, svc_port)
      if (
          'Device commissioning completed with code: 0' in out
          or 'Commissioning complete' in out
          or ('onCommissioningComplete' in out and 'errorCode = 0' in out)
      ):
        comm_success = True
        break
      if (
          'Device commissioning completed with code:' in out
          and 'code: 0' not in out
      ):
        break
      time.sleep(2.0)

    try:
      self.assertTrue(
          comm_success,
          'BLE + Thread commissioning did not complete with code 0 within 120s',
      )

      pcap_dir = (
          os.environ.get('CIRQUE_PCAP_DIR')
          or os.environ.get('DEVICE_LOG_DIR')
          or '/tmp'
      )
      wifi_pcap = os.path.join(pcap_dir, 'wifi_medium.pcap')
      thread_pcap = os.path.join(pcap_dir, 'thread_0.pcap')
      self.assertTrue(
          os.path.exists(thread_pcap), f'Missing Thread capture {thread_pcap}'
      )

      if disable_reg == '1':
        self.logger.info(
            'Negative control mode (DISABLE_ICD_REGISTRATION=1): waiting 20s to'
            ' verify 0 Check-Ins'
        )
        time.sleep(20.0)
        network_key, child_table, sed_rloc16s = self._get_thread_radio_context(
            tbr_ids[0]
        )
        self.assertTrue(
            sed_rloc16s,
            'Negative control failed: TBR child table has no R=0 child:'
            f'\n{child_table}',
        )
        with open(app_log_path, 'r', encoding='utf-8', errors='ignore') as f:
          device_log = f.read()
        res = self.execute_device_cmd(
            req_device_id, 'adb shell "logcat -d -v epoch"'
        )
        logcat_out = (
            res.get('output', '') if isinstance(res, dict) else str(res)
        )
        wire_checkins = (
            matter_icd_checkin_packets(wifi_pcap)
            if os.path.exists(wifi_pcap)
            else []
        )
        radio_checkins = [
            f
            for f in thread_icd_checkin_frames(thread_pcap, network_key)
            if f['src16'] in sed_rloc16s
        ]
        # Non-vacuity: the SED's MAC-secured traffic must decrypt, so that
        # 0 radio Check-Ins means none were sent, not that decryption failed.
        sed_radio_udp = [
            f
            for f in thread_decrypted_udp_frames(thread_pcap, network_key)
            if f['src16'] in sed_rloc16s
        ]

        self.assertNotIn(
            'Cluster=0x0000_0046 Command=0x0000_0000',
            device_log,
            'Negative control failed: RegisterClient (0x0046:0x00) was'
            ' unexpectedly sent when isLIT=false',
        )
        self.assertNotIn(
            'SecureChannel:ICD_CheckInMessage',
            device_log,
            'Negative control failed: device sent ICD_CheckInMessage without'
            ' client registration',
        )
        self.assertEqual(
            logcat_out.count('onCheckInComplete'),
            0,
            'Negative control failed: Android logcat reported onCheckInComplete'
            ' when registration was disabled',
        )
        self.assertEqual(
            len(wire_checkins),
            0,
            'Negative control failed: wifi_medium.pcap contained'
            f' {len(wire_checkins)} ICD_CheckIn packets',
        )
        self.assertTrue(
            sed_radio_udp,
            'Negative control is vacuous: no MAC-secured UDP frame from SED'
            f' {sorted(hex(r) for r in sed_rloc16s)} decrypted in'
            f' {thread_pcap}',
        )
        self.assertEqual(
            len(radio_checkins),
            0,
            'Negative control failed: thread_0.pcap contained'
            f' {len(radio_checkins)} ICD_CheckIn frames from the SED',
        )
        self.logger.info(
            'Oracle 5 (Negative control) passed: 0 RegisterClient, 0 Check-Ins'
            ' on device/802.15.4/Wi-Fi/Android (sed_decrypted_udp=%d)',
            len(sed_radio_udp),
        )
        return

      self.logger.info(
          'Commissioning succeeded; polling up to 90s for >= 2 ICD Check-In'
          ' messages...'
      )
      logcat_out = ''
      start_wait = time.time()
      while time.time() - start_wait < 90.0:
        res = self.execute_device_cmd(
            req_device_id, 'adb shell "logcat -d -v epoch"'
        )
        logcat_out = (
            res.get('output', '') if isinstance(res, dict) else str(res)
        )
        if logcat_out.count('onCheckInComplete :') >= 2:
          break
        time.sleep(2.0)

      # Give one extra idle cycle so thread_0.pcap captures several slow-poll intervals
      time.sleep(6.0)
      res = self.execute_device_cmd(
          req_device_id, 'adb shell "logcat -d -v epoch"'
      )
      logcat_out = res.get('output', '') if isinstance(res, dict) else str(res)
      with open(app_log_path, 'r', encoding='utf-8', errors='ignore') as f:
        device_log = f.read()

      # Oracle 1: Registration
      self.assertIn(
          'Cluster=0x0000_0046 Command=0x0000_0000',
          device_log,
          'Oracle 1 failed: device log missing IcdManagement::RegisterClient'
          ' (0x0046:0x00)',
      )
      self.assertIn(
          'OnReadCommissioningInfo ICD - IdleModeDuration=5',
          logcat_out,
          'Oracle 1 failed: Android logcat did not read IdleModeDuration=5 from'
          ' ICD Management cluster',
      )
      self.assertIn(
          'onICDRegistrationComplete - errorCode: 0',
          logcat_out,
          'Oracle 1 failed: Android logcat missing onICDRegistrationComplete -'
          ' errorCode: 0',
      )
      self.logger.info('Oracle 1 (ICD Registration) passed')

      # Oracle 2: Check-In delivery along the whole path:
      #   2a device TX log -> 2b 802.15.4 radio (decrypted thread_0.pcap)
      #   -> 2c border-router egress (wifi_medium.pcap) -> 2d Android callback.
      # The radio leg is the primary proof that the sleepy device itself sent
      # the Check-In; wifi_medium.pcap only shows it after TBR routing.
      network_key, child_table, sed_rloc16s = self._get_thread_radio_context(
          tbr_ids[0]
      )
      self.assertTrue(
          sed_rloc16s,
          'Oracle 2 failed: TBR child table has no R=0 child to attribute'
          f' radio frames to:\n{child_table}',
      )
      dev_tx_counters = {
          int(c)
          for c in re.findall(
              r'<<< \[E:\d+i S:0 M:(\d+)\] .* Type 0000:50'
              r' \(SecureChannel:ICD_CheckInMessage\)',
              device_log,
          )
      }
      self.assertTrue(
          len(dev_tx_counters) >= 2,
          'Oracle 2a failed: expected >= 2 ICD_CheckInMessage transmissions in'
          f' device log, got {len(dev_tx_counters)}',
      )

      radio_checkins = [
          f
          for f in thread_icd_checkin_frames(thread_pcap, network_key)
          if f['src16'] in sed_rloc16s
      ]
      self.assertTrue(
          len(radio_checkins) >= 2,
          'Oracle 2b failed: expected >= 2 ICD_CheckIn (0x0000:0x50) frames'
          f' from SED {sorted(hex(r) for r in sed_rloc16s)} in decrypted'
          f' {thread_pcap}, got {len(radio_checkins)}',
      )
      radio_counters = [f['msg_counter'] for f in radio_checkins]
      self.assertTrue(
          set(radio_counters) <= dev_tx_counters,
          'Oracle 2b failed: radio Check-In counters'
          f' {radio_counters} not all in device TX log counters'
          f' {sorted(dev_tx_counters)}',
      )
      # Mutation check: the radio oracle must depend on the network key.
      wrong_key_frames = thread_decrypted_udp_frames(
          thread_pcap, _invert_key_hex(network_key)
      )
      self.assertEqual(
          len(wrong_key_frames),
          0,
          'Oracle 2b failed: a wrong network key still decrypted'
          f' {len(wrong_key_frames)} frames; radio oracle is not key-bound',
      )

      self.assertTrue(
          os.path.exists(wifi_pcap), f'Oracle 2c failed: missing {wifi_pcap}'
      )
      wire_checkins = matter_icd_checkin_packets(wifi_pcap)
      wire_by_nonce = {w['nonce_hex']: w for w in wire_checkins}
      forward_delays = []
      for f in radio_checkins:
        w = wire_by_nonce.get(f['nonce_hex'])
        self.assertTrue(
            w is not None,
            'Oracle 2c failed: radio Check-In (counter'
            f' {f["msg_counter"]}, nonce {f["nonce_hex"]}) never reached'
            f' {wifi_pcap}',
        )
        self.assertEqual(
            w['msg_counter'],
            f['msg_counter'],
            'Oracle 2c failed: Wi-Fi copy of Check-In nonce'
            f' {f["nonce_hex"]} has a different message counter',
        )
        delay = w['ts'] - f['ts']
        self.assertTrue(
            0.0 <= delay <= _MAX_BR_FORWARD_DELAY_S,
            f'Oracle 2c failed: TBR forward delay {delay:.4f}s outside'
            f' [0, {_MAX_BR_FORWARD_DELAY_S}]s for counter {f["msg_counter"]}',
        )
        forward_delays.append(delay)

      self.assertNotIn(
          'onCheckInComplete - FindMethod is failed!',
          logcat_out,
          'Oracle 2d failed: JNI callback lookup for onCheckInComplete failed',
      )
      android_checkin_cb = re.findall(
          r'onCheckInComplete\s*:\s*ICDClientInfo', logcat_out
      )
      self.assertTrue(
          len(android_checkin_cb) >= 2,
          'Oracle 2d failed: expected >= 2 onCheckInComplete callbacks in'
          f' Android logcat, got {len(android_checkin_cb)}',
      )
      self.logger.info(
          'Oracle 2 (Check-In delivery) passed: device_tx=%d, radio=%d'
          ' (src16=%s, sec_level=%s), wifi=%d, tbr_forward_ms=%s,'
          ' android_cb=%d',
          len(dev_tx_counters),
          len(radio_checkins),
          sorted({hex(f['src16']) for f in radio_checkins}),
          sorted({f['sec_level'] for f in radio_checkins}),
          len(wire_checkins),
          [round(d * 1000, 2) for d in forward_delays],
          len(android_checkin_cb),
      )

      # Oracle 3: Timing & strictly increasing Check-In counter
      counter_matches = re.findall(
          r'Check In Message processing'
          r' complete:\s*start_counter=(\d+)\s+offset=(\d+)',
          logcat_out,
      )
      self.assertTrue(
          len(counter_matches) >= 2,
          'Oracle 3 failed: expected >= 2 decoded Check-In counter lines in'
          f' logcat, got {len(counter_matches)}',
      )
      counters = [int(s) + int(o) for s, o in counter_matches]
      for i in range(len(counters) - 1):
        self.assertTrue(
            counters[i + 1] > counters[i],
            'Oracle 3 failed: Check-In counters not strictly increasing:'
            f' {counters}',
        )
      for i in range(len(radio_counters) - 1):
        self.assertTrue(
            radio_counters[i + 1] > radio_counters[i],
            'Oracle 3 failed: radio Check-In message counters not strictly'
            f' increasing: {radio_counters}',
        )
      radio_deltas = [
          radio_checkins[i + 1]['ts'] - radio_checkins[i]['ts']
          for i in range(len(radio_checkins) - 1)
      ]
      min_radio_delta = min(radio_deltas)
      # IdleModeDuration=5s + ActiveModeThreshold=5s => nominal ~10s cycle.
      self.assertTrue(
          3.0 <= min_radio_delta <= 18.0,
          f'Oracle 3 failed: Check-In radio interval {min_radio_delta:.2f}s'
          f' outside [3.0s, 18.0s] tolerance (deltas={radio_deltas})',
      )
      self.logger.info(
          'Oracle 3 (Timing & counter monotonicity) passed: counters=%s,'
          ' radio_msg_counters=%s, radio_deltas=%s',
          counters,
          radio_counters,
          [round(d, 2) for d in radio_deltas],
      )

      # Oracle 4: Sleepy End Device (SED) polling & child table R=0
      self.assertIn(
          'Setting OpenThread device type to SLEEPY END DEVICE',
          device_log,
          'Oracle 4 failed: device log missing SLEEPY END DEVICE configuration',
      )
      self.assertIn(
          'Device Role: CHILD',
          device_log,
          'Oracle 4 failed: device log missing Thread CHILD role transition',
      )
      # sed_rloc16s (R=0 children) was read from the TBR child table for
      # Oracle 2 and asserted non-empty there.
      self.logger.info(
          'Oracle 4: TBR child table R=0 children: %s',
          sorted(hex(r) for r in sed_rloc16s),
      )
      mac_cmds = ieee802154_mac_command_frames(thread_pcap)
      self.assertTrue(
          len(mac_cmds) >= 10,
          'Oracle 4 failed: expected >= 10 IEEE 802.15.4 MAC Command (Data'
          f' Request) frames in {thread_pcap}, got {len(mac_cmds)}',
      )
      poll_deltas = [
          mac_cmds[i + 1]['ts'] - mac_cmds[i]['ts']
          for i in range(len(mac_cmds) - 1)
          if mac_cmds[i + 1]['ts'] > mac_cmds[i]['ts']
      ]
      fast_polls = [d for d in poll_deltas if 0.02 <= d <= 0.50]
      slow_polls = [d for d in poll_deltas if d >= 0.80]
      self.assertTrue(
          len(fast_polls) >= 3,
          'Oracle 4 failed: expected fast-poll MAC Data Requests during'
          f' ActiveMode in {thread_pcap}',
      )
      self.assertTrue(
          len(slow_polls) >= 2,
          'Oracle 4 failed: expected slow-poll MAC Data Requests during'
          f' IdleMode in {thread_pcap}',
      )
      self.logger.info(
          'Oracle 4 (Sleepy polling) passed: mac_cmds=%d, fast_polls=%d,'
          ' slow_polls=%d',
          len(mac_cmds),
          len(fast_polls),
          len(slow_polls),
      )
    finally:
      log_dir = os.environ.get('DEVICE_LOG_DIR')
      if log_dir and os.path.isdir(log_dir) and os.path.exists(app_log_path):
        try:
          shutil.copy(
              app_log_path, os.path.join(log_dir, 'lit-icd-app-host.log')
          )
        except OSError as exc:
          self.logger.warning('Failed to copy lit-icd-app-host.log: %s', exc)
      self._cleanup_host_processes()


if __name__ == '__main__':
  sys.exit(TestAndroidBleThreadSilabsLitIcdDevice(DEVICE_CONFIG).run_test())
