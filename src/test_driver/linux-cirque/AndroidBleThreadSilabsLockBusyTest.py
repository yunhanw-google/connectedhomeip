#!/usr/bin/env python3
import json
import logging
import os
import re
import sys
import time

from AndroidBleThreadSilabsLitIcdTest import DEVICE_CONFIG as _BASE_LIT_ICD_DEVICE_CONFIG, TestAndroidBleThreadSilabsLitIcdDevice
from helper.paths import CHIP_SILABS_LOCK_APP_ESC


class TestAndroidBleThreadSilabsLockBusy(
    TestAndroidBleThreadSilabsLitIcdDevice
):
  """Validates CASEServer BUSY response when handshakes overlap."""
  skip_icd_oracles = True

  def _get_end_device_binary_esc(self) -> str:
    return CHIP_SILABS_LOCK_APP_ESC

  def _rf_api(self, endpoint: str, payload: dict = None) -> dict:
    url = f'http://127.0.0.1:5000/{endpoint.lstrip("/")}'
    import urllib.request
    import urllib.error

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

  def _wait_for_android_log(
      self, req_device_id: str, search_str: str, timeout_s: float = 15.0
  ) -> bool:
    start_time = time.time()
    while time.time() - start_time < timeout_s:
      log_check = self.execute_device_cmd(
          req_device_id, f'adb shell "logcat -d | awk \\\'/{search_str}/{{print}}\\\' || true"'
      )
      out = (
          log_check.get('output', '')
          if isinstance(log_check, dict)
          else str(log_check)
      )
      if search_str in out:
        return True
      time.sleep(1.0)
    return False

  def _run_post_checkin_phase(self, ctx: dict) -> None:
    tbr_id = ctx['tbr_id']
    req_device_id = ctx['req_device_id']
    app_log_path = ctx['app_log_path']

    def get_log_offset():
      with open(app_log_path, 'r', encoding='utf-8', errors='ignore') as lf:
        return len(lf.read())

    def get_log_diff(offset):
      with open(app_log_path, 'r', encoding='utf-8', errors='ignore') as lf:
        lf.seek(offset)
        return lf.read()

    def wait_for_lock_log(search_str, offset, timeout_s=30.0):
      start = time.time()
      while time.time() - start < timeout_s:
        diff = get_log_diff(offset)
        if search_str in diff:
          return True
        time.sleep(1.0)
      return False

    child_table = self._exec_tbr_ot_ctl(tbr_id, 'ot-ctl child table')
    ext_addr = None
    rloc16 = None
    for line in child_table.splitlines():
      if '|' in line and 'ID' not in line:
        parts = [p.strip() for p in line.split('|')]
        if len(parts) > 14:
          rloc16 = parts[2].lower().replace('0x', '')
          ext_addr = parts[14].lower()

    self.assertTrue(ext_addr, 'Could not find lock extended MAC in child table')
    self.logger.info(
        'Lock ext_addr for fault rule: %s, rloc16: %s', ext_addr, rloc16
    )

    self._restart_chiptool_and_register_srp(req_device_id, tbr_id)
    self.logger.info('Opening LIGHT ON/OFF cluster fragment')
    self._tap_ui_element(req_device_id, 'onOffClusterBtn', wait_s=2.0)
    
    # Arm rule under ideal RF to ensure Sigma1 reaches the lock
    self._rf_api(
        'rf_channel', {'preset': 'ideal', 'seed': 12345, 'enabled': True}
    )
    self._rf_api(
        'rf_channel/fault',
        {
            'rule': 'sigma2_blackhole',
            'src_ext': ext_addr,
            'src_short': rloc16,
            'window_s': 15.0,
            'psdu_threshold': 75,
        },
    )

    offset_read1 = get_log_offset()
    self.logger.info('Triggering CHIPTool Read #1 (CASE timeout attempt)')
    self._tap_ui_element(req_device_id, 'Toggle', wait_s=1.0)

    self.logger.info('Waiting for lock to receive Sigma1 and send Sigma2...')
    self.assertTrue(
        wait_for_lock_log('Received Sigma1', offset_read1, timeout_s=15.0),
        'Lock did not receive Sigma1',
    )
    self.assertTrue(
        wait_for_lock_log('Sent Sigma2', offset_read1, timeout_s=15.0),
        'Lock did not send Sigma2',
    )

    # Verify fault rule dropped Sigma2
    start_poll = time.time()
    dropped = False
    while time.time() - start_poll < 5.0:
      st = self._rf_api('rf_channel')
      if st.get('fault_rule', {}).get('dropped_count', 0) >= 1:
        dropped = True
        break
      time.sleep(1.0)
    self.assertTrue(dropped, 'RF fault rule did not drop Sigma2')

    # Force restart CHIPTool to abort the pending exchange immediately
    self.logger.info('Restarting CHIPTool to force a new CASE session (Read #2)...')
    self.execute_device_cmd(req_device_id, 'adb shell am force-stop com.google.chip.chiptool')
    time.sleep(1.0)
    self.execute_device_cmd(
        req_device_id,
        'adb shell am start -n com.google.chip.chiptool/.CHIPToolActivity'
    )
    time.sleep(4.0)
    self.logger.info('Opening LIGHT ON/OFF cluster fragment again')
    self._tap_ui_element(req_device_id, 'onOffClusterBtn', wait_s=2.0)

    offset_read2 = get_log_offset()
    self.logger.info('Triggering CHIPTool Read #2 (should get BUSY)')
    
    found_busy = False
    for i in range(2):
      self._tap_ui_element(req_device_id, 'Toggle', wait_s=1.0)
      if wait_for_lock_log('sending busy status report', offset_read2, timeout_s=4.0):
        found_busy = True
        break
    
    self.assertTrue(found_busy, 'Lock did not send BUSY')

    # Read the fault counters to log the BUSY wait value/StatusReport block if any
    fault_status = self._rf_api('rf_channel')
    self.logger.info(f'RF Proxy Fault Status: {fault_status}')

    # Disarm rule immediately
    self._rf_api('rf_channel/fault', {'rule': ''})

    # Wait for the lock's first kSentSigma2 exchange to finish timing out
    self.assertTrue(
        wait_for_lock_log(
            'CASE Session establishment failed', offset_read1, timeout_s=35.0
        ),
        'Lock first CASE session did not timeout',
    )
    time.sleep(2.0)

    offset_read3 = get_log_offset()
    self.logger.info('Triggering CHIPTool Read #3 (should succeed)')
    self._tap_ui_element(req_device_id, 'Toggle', wait_s=5.0)

    self.assertTrue(
        wait_for_lock_log('CASE Session established', offset_read3, timeout_s=20.0) or wait_for_lock_log('Session was established', offset_read3, timeout_s=20.0),
        'Session was not successfully established on Read 3',
    )

    offset_read4 = get_log_offset()
    self.logger.info(
        'Negative control: Triggering CHIPTool Read #4 (should succeed'
        ' instantly)'
    )
    self._tap_ui_element(req_device_id, 'Toggle', wait_s=5.0)

    # Negative control oracle: ZERO busy reports in the window after disarming
    diff_read4 = get_log_diff(offset_read4)
    self.assertNotIn(
        'sending busy status report',
        diff_read4,
        'Negative control failed: sent BUSY when disarmed',
    )

    # Check CHIPTool logcat for CHIP_ERROR_BUSY
    log_check = self.execute_device_cmd(req_device_id, 'adb shell "logcat -d"')
    logcat_out = (
        log_check.get('output', '')
        if isinstance(log_check, dict)
        else str(log_check)
    )
    if (
        'CHIP_ERROR_BUSY' not in logcat_out
        and 'status report' not in logcat_out.lower()
    ):
      self.logger.info(
          'BUSY might not be explicitly printed in CHIPTool logs without higher'
          ' detail, relying on Lock logs.'
      )
    
    self.logger.info('ALL REMAINING TESTS PASSED!')
    return


if __name__ == '__main__':
  sys.exit(
      TestAndroidBleThreadSilabsLockBusy(_BASE_LIT_ICD_DEVICE_CONFIG).run_test()
  )
