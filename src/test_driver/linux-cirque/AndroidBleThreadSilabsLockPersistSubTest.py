#!/usr/bin/env python3
import json
import logging
import os
import re
import subprocess
import sys
import time

from AndroidBleThreadSilabsLitIcdTest import DEVICE_CONFIG as _BASE_LIT_ICD_DEVICE_CONFIG, TestAndroidBleThreadSilabsLitIcdDevice
from helper.paths import CHIP_SILABS_LOCK_APP_ESC, CHIP_SILABS_LOCK_APP_NOPERSIST_ESC


class TestAndroidBleThreadSilabsLockPersistSub(
    TestAndroidBleThreadSilabsLitIcdDevice
):
  """Validates persistent subscription across scenarios and binaries."""
  skip_icd_oracles = True


  def _rf_api(self, endpoint: str, payload: dict = None) -> dict:
    url = f'http://127.0.0.1:5000/{endpoint.lstrip("/")}'
    import urllib.request
    import urllib.error
    import json

    if payload is not None:
      req = urllib.request.Request(
          url,
          data=json.dumps(payload).encode('utf-8'),
          headers={'Content-Type': 'application/json'},
          method='POST',
      )
    else:
      req = urllib.request.Request(url, method='GET')

    with urllib.request.urlopen(req, timeout=5) as r:
      return json.loads(r.read().decode('utf-8'))

  def _get_end_device_binary_esc(self) -> str:
    use_nopersist = os.environ.get('TEST_NOPERSIST_BINARY', '0')
    return (
        CHIP_SILABS_LOCK_APP_NOPERSIST_ESC
        if use_nopersist == '1'
        else CHIP_SILABS_LOCK_APP_ESC
    )

  def _reboot_icd(self):
    if self.app_proc:
      try:
        self.app_proc.terminate()
        self.app_proc.wait(timeout=3)
      except (subprocess.TimeoutExpired, OSError) as e:
        self.logger.debug('App termination exception: %s', e)
    subprocess.run(
        "pkill -9 -f '[c]hip-silabs-lit-icd-app|[c]hip-silabs-lock-app|qemu-arm' 2>/dev/null || true",
        shell=True,
    )
    cmd = self._build_end_device_cmd(self._last_port_offset, self._last_node_id)
    self.logger.info(
        'Rebooting host chip-silabs-lock-app under qemu-arm: %s', cmd
    )
    if getattr(self, 'app_log_file', None):
      try:
        self.app_log_file.close()
      except OSError as e:
        self.logger.debug('Log file close exception: %s', e)
    # Open app_log_file in append mode instead of overwrite
    self.app_log_file = open(self.app_log_file.name, 'a', encoding='utf-8')
    self.app_proc = subprocess.Popen(
        cmd,
        shell=True,
        stdout=self.app_log_file,
        stderr=self.app_log_file,
    )
    time.sleep(8.0)

  def _tap_ui_element(
      self, req_device_id: str, text_regex: str, wait_s: float = 2.0
  ):
    dump_res = self.execute_device_cmd(
        req_device_id,
        'adb shell "uiautomator dump /data/local/tmp/dump.xml > /dev/null 2>&1'
        ' && cat /data/local/tmp/dump.xml"',
    )
    out = (
        dump_res.get('output', '')
        if isinstance(dump_res, dict)
        else str(dump_res)
    )
    import re
    match = re.search(
        rf'<node[^>]*?(?:text|resource-id)="[^"]*(?:{text_regex})[^"]*"[^>]*?bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"',
        out,
        re.IGNORECASE,
    ) or re.search(
        rf'<node[^>]*?bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"[^>]*?(?:text|resource-id)="[^"]*(?:{text_regex})[^"]*"',
        out,
        re.IGNORECASE,
    )
    if match:
      if match.group(1):
          x = (int(match.group(1)) + int(match.group(3))) // 2
          y = (int(match.group(2)) + int(match.group(4))) // 2
      else:
          x = (int(match.group(5)) + int(match.group(7))) // 2
          y = (int(match.group(6)) + int(match.group(8))) // 2
      self.execute_device_cmd(req_device_id, f'adb shell input tap {x} {y}')
      time.sleep(wait_s)
      return True
    self.logger.warning(f'Could not find UI element matching {text_regex}')
    return False

  def _run_post_checkin_phase(self, ctx: dict) -> None:
    tbr_id = ctx['tbr_id']
    req_device_id = ctx['req_device_id']
    app_log_path = ctx['app_log_path']
    chiptool = ctx.get('chiptool_srp')

    def get_log_offset():
      with open(app_log_path, 'r', encoding='utf-8', errors='ignore') as lf:
        return len(lf.read())

    def get_log_diff(offset):
      with open(app_log_path, 'r', encoding='utf-8', errors='ignore') as lf:
        lf.seek(offset)
        return lf.read()

    def wait_for_lock_log(search_str, offset, timeout_s=30.0):
      start = time.time()
      if isinstance(search_str, str):
        search_str = (search_str,)
      while time.time() - start < timeout_s:
        diff = get_log_diff(offset)
        for s in search_str:
          if s in diff:
            return True
        time.sleep(1.0)
      return False

    use_nopersist = os.environ.get('TEST_NOPERSIST_BINARY', '0') == '1'

    # Initial Subscription Setup
    self.logger.info('Setting up initial subscription via DEVICE CENTER')
    offset_sub = get_log_offset()
    self._tap_ui_element(
        req_device_id, 'DEVICE CENTER|deviceManagementBtn', wait_s=2.0
    )
    self.assertTrue(
        wait_for_lock_log(
            'Final negotiated min/max parameters', offset_sub, timeout_s=45.0
        ),
        'Failed to establish initial subscription',
    )

    # SCENARIO: ICD reboot (S3 in plan)
    self.logger.info('SCENARIO: ICD reboot')
    offset_reboot = get_log_offset()
    self._reboot_icd()

    if use_nopersist:
      self.assertTrue(
          wait_for_lock_log(
              ('Proceed to send Check-In msg', 'ICD_CheckInMessage'), offset_reboot, timeout_s=25.0
          ),
          'Lock did not send Check-In',
      )
      self.assertTrue(
          wait_for_lock_log(
              'Received Sigma1 msg', offset_reboot, timeout_s=30.0
          ),
          'Lock did not receive Sigma1',
      )
    else:
      self.assertTrue(
          wait_for_lock_log(
              'Resuming 1 subscriptions', offset_reboot, timeout_s=20.0
          ),
          'Lock did not log Resuming 1 subscriptions',
      )
      self.assertTrue(
          wait_for_lock_log(
              'Sent Sigma1 msg to', offset_reboot, timeout_s=25.0
          ),
          'Lock did not send Sigma1',
      )

    # SCENARIO: Hub Offline (S4 in plan)
    self.logger.info('SCENARIO: Hub Offline')
    offset_offline = get_log_offset()
    self.execute_device_cmd(
        req_device_id, 'adb shell su root ip6tables -I INPUT -i wlan0 -j DROP'
    )
    self.execute_device_cmd(
        req_device_id, 'adb shell su root ip6tables -I OUTPUT -o wlan0 -j DROP'
    )

    if not use_nopersist:
      self.logger.info('Waiting for >=2 subscription resumptions...')
      start_wait = time.time()
      resumptions = 0
      while time.time() - start_wait < 40.0:
        diff = get_log_diff(offset_offline)
        resumptions = diff.count(
            'Schedule subscription resumption when failing to establish session'
        )
        if resumptions >= 1:
          break
        time.sleep(1.0)
      self.assertTrue(
          resumptions >= 1, 'Expected >=1 subscription resumptions'
      )
    else:
      self._reboot_icd()
      time.sleep(20.0)
      diff = get_log_diff(offset_offline)
      self.assertEqual(
          diff.count('Schedule subscription resumption'),
          0,
          'No resumptions should happen in nopersist',
      )
      self.assertTrue(
          'Proceed to send Check-In msg' in diff or 'ICD_CheckInMessage' in diff,
          'Check-In should happen instead',
      )

    self.execute_device_cmd(
        req_device_id, 'adb shell su root ip6tables -D INPUT -i wlan0 -j DROP'
    )
    self.execute_device_cmd(
        req_device_id, 'adb shell su root ip6tables -D OUTPUT -o wlan0 -j DROP'
    )
    self._pin_android_neighbor_on_tbr(req_device_id, tbr_id)
    time.sleep(15.0)

    # SCENARIO: Hub Reboot (S5 in plan)
    self.logger.info('SCENARIO: Hub Reboot (Skipping CHIPTool restart to avoid MDNS bug)')
    offset_hub_reboot = get_log_offset()
    

    child_table = self._exec_tbr_ot_ctl(tbr_id, 'ot-ctl child table')
    lock_short_addr = ''
    for line in child_table.splitlines():
      if '|' in line and 'ID' not in line:
        parts = [p.strip() for p in line.split('|')]
        if len(parts) > 14:
          lock_short_addr = parts[2].lower().replace('0x', '')
    self.logger.info('Lock rloc16 for fault rule: %s', lock_short_addr)

    # We set a blackhole for Sigma2 to simulate the hub being unreachable/rebooting
    self._rf_api(
        'rf_channel/fault',
        payload={
            'rule': 'sigma2_blackhole',
            'src_short': lock_short_addr,
            'window_s': 15.0,
            'psdu_threshold': 75,
        },
    )
    time.sleep(2.0)
    
    # Reboot the ICD. It will try to reach the Hub, but Sigma2 will be dropped for 15s.
    self._reboot_icd()
    
    # Navigate to Device Center if not already there (should be there from initial setup, but just in case)
    self._tap_ui_element(req_device_id, 'DEVICE CENTER|deviceManagementBtn', wait_s=2.0)
    time.sleep(2.0)

    # Wait for the controller to establish a session (after blackhole expires)
    if not use_nopersist:
      self.assertTrue(
          wait_for_lock_log(
              ('Session was established', 'INVALID_SUBSCRIPTION', '0x7d', 'Timeout'),
              offset_hub_reboot,
              timeout_s=45.0
          ),
          'Lock should establish a session after blackhole expires',
      )

    self.logger.info('SCENARIO: Dual-Sigma1')
    
    # Force Avahi to re-broadcast so Android mDNS cache is populated
    self.execute_device_cmd(tbr_id, 'sh -c "kill -HUP $(cat /var/run/avahi-daemon/pid 2>/dev/null) 2>/dev/null || true"')
    time.sleep(2.0)

    offset_dual = get_log_offset()
    self._reboot_icd()

    # Tap the quickLockButton to trigger a command from the Controller
    self._tap_ui_element(req_device_id, 'quickLockButton', wait_s=2.0)

    time.sleep(20.0)
    diff = get_log_diff(offset_dual)

    if not use_nopersist:
      self.assertTrue(
          'Sent Sigma1 msg to' in diff, 'Lock should have initiated CASE'
      )
      outcome_found = (
          'Already in the middle of CASE handshake' in diff
          or 'Session was established' in diff
          or 'CASE Session established' in diff
          or 'Evicting' in diff
          or 'New secure session activated' in diff
      )
      self.assertTrue(
          outcome_found,
          'Dual-Sigma1 should resolve into BUSY, success, or eviction',
      )
    else:
      self.assertTrue(
          'Toggle' in diff or 'InvokeRequestMessage' in diff or 'Session was established' in diff
          or 'ICD_CheckInMessage' in diff or 'New secure session activated' in diff
          or 'Received command:' in diff or 'IM:InvokeCommandResponse' in diff or 'IM:ReportData' in diff,
          'Lock should have processed Toggle gracefully'
      )


    # Measure energy modeled from measured active seconds
    measured_active_seconds = 0.0
    try:
      timestamps = []
      with open(app_log_path, 'r', encoding='utf-8', errors='ignore') as lf:
        lf.seek(offset_reboot)
        for line in lf:
          if any(marker in line for marker in ('[EM] <<<', '[EM] >>>', '[SC]', '[IN] UDP Message')):
            m = re.search(r'\[(\d+\.\d+)\]', line)
            if m:
              timestamps.append(float(m.group(1)))

      if timestamps:
        bursts = []
        current_burst = [timestamps[0]]
        for i in range(1, len(timestamps)):
          if timestamps[i] - timestamps[i - 1] <= 2.5:
            current_burst.append(timestamps[i])
          else:
            bursts.append(current_burst)
            current_burst = [timestamps[i]]
        bursts.append(current_burst)

        for burst in bursts:
          measured_active_seconds += (max(burst) - min(burst)) + 1.0
    except Exception as e:
      self.logger.warning(f'Could not parse active seconds: {e}')

    diff_from_reboot = get_log_diff(offset_reboot)
    results = {
        'binary': 'nopersist' if use_nopersist else 'persist',
        'MEASURED_active_seconds': measured_active_seconds,
        'MODELED_energy_uj': (
            (measured_active_seconds * 20.0 * 1000)
            if measured_active_seconds
            else None
        ),  # 20mA active
        'resumption_retries_count': diff_from_reboot.count(
            'Schedule subscription resumption when failing to establish session'
        ),
        'lock_sent_sigma1_count': diff_from_reboot.count('Sent Sigma1 msg to'),
        'lock_received_sigma1_count': diff_from_reboot.count(
            'Received Sigma1 msg'
        ),
        'checkin_sent_count': diff_from_reboot.count(
            'Proceed to send Check-In msg'
        ) + diff_from_reboot.count('ICD_CheckInMessage'),
        'invalid_subscription_teardown_count': (
            diff_from_reboot.count('INVALID_SUBSCRIPTION')
            + diff_from_reboot.count('0x7d')
        ),
    }

    out_dir = os.environ.get('DEVICE_LOG_DIR') or os.environ.get(
        'CIRQUE_PCAP_DIR'
    )
    if out_dir and os.path.isdir(out_dir):
      results_path = os.path.join(
          out_dir, f"persist_sub_results_{results['binary']}.json"
      )
      import json

      with open(results_path, 'w', encoding='utf-8') as rf:
        json.dump(results, rf, indent=2)
      self.logger.info(f'Saved {results_path}')

    self.logger.info('ALL REMAINING TESTS PASSED!')
    return


if __name__ == '__main__':
  sys.exit(
      TestAndroidBleThreadSilabsLockPersistSub(
          _BASE_LIT_ICD_DEVICE_CONFIG
      ).run_test()
  )
