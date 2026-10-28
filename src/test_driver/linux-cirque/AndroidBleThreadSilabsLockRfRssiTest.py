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

End-to-End Multi-Radio RF Channel & RSSI Simulation Test across BLE, Wi-Fi,
and Thread (IEEE 802.15.4) between Android CHIPTool (Android Emulator),
Thread Border Router (otbr-agent + ot-rcp), and ARM32 Silicon Labs Lock App
(examples/lock-app/silabs -> out/arm-silabs-thread/chip-silabs-lock-app).

Validates three progressive 2.4 GHz stochastic propagation and coexistence
regimes:
  * Regime A (regime_a_near_los): Near-field LoS (d=1.5m, n=2.0, Rician K=10dB,
    sigma_s=1.0dB, no interferers).
  * Regime B (regime_b_far_nlos_fading): Far-field NLOS (d=14m, n=3.0, 2 walls,
    Gudmundson AR(1) shadowing sigma_s=5.0dB, rho=0.85, Rayleigh fading K=0).
  * Regime C (regime_c_cochannel_interference): 2.4 GHz co-channel Wi-Fi + BLE
    interference burst overlapping Thread Ch 15 (2425 MHz) & Wi-Fi Ch 11 + NLOS
    Rayleigh fading, causing measurable SINR drop, LQI drop, and RX/TX packet
    drops & MAC retransmissions across BLE, Wi-Fi, and Thread.
"""

import json
import os
import re
import sys
import time
import urllib.request

from AndroidBleThreadSilabsLitIcdTest import (
    DEVICE_CONFIG as _BASE_LIT_ICD_DEVICE_CONFIG,
    TestAndroidBleThreadSilabsLitIcdDevice,
)
from cirque.virtual_bt.client import VirtualBtControlClient
from helper import parent_restart as pr
from helper.paths import CHIP_SILABS_LOCK_APP_ESC

# SIMULATION-DISCLOSURE: Executes the ARM32 Silicon Labs Lock App
# (examples/lock-app/silabs, out/arm-silabs-thread/chip-silabs-lock-app) under
# qemu-arm with userspace Virtual Bluetooth (LinkLayerHub + RfChannelSimulator),
# Virtual Wi-Fi (VirtualWiFiServer + RfChannelSimulator), and Cirque's
# SpinelHdlcRfProxy in ThreadSimPipe (with an unmodified OpenThread codebase).

RF_RSSI_RESULTS_FILE = 'rf_rssi_experiment_results.json'

_MAC_COUNTERS_RE = re.compile(r'^\s*([A-Za-z0-9_]+):\s*(\d+)\s*$', re.MULTILINE)
_CHILD_LQ_IN_RE = re.compile(
    r'^\|\s*\d+\s*\|\s*0x[0-9a-fA-F]{4}\s*\|\s*\d+\s*\|\s*\d+\s*\|'
    r'\s*(\d+)\s*\|',
    re.MULTILINE,
)


def parse_ot_mac_counters(text: str) -> dict:
  """Parses `ot-ctl counters mac` output into a dict of integer counters."""
  return {k: int(v) for k, v in _MAC_COUNTERS_RE.findall(text)}


def parse_child_table_lq_in(child_table: str) -> list:
  """Returns the `LQ In` integer values from `ot-ctl child table` rows."""
  return [int(v) for v in _CHILD_LQ_IN_RE.findall(child_table)]


class TestAndroidBleThreadSilabsLockRfRssi(
    TestAndroidBleThreadSilabsLitIcdDevice
):
  """Validates multi-radio RSSI, SINR, LQI, and packet drop/retry regimes."""

  def _get_end_device_binary_esc(self) -> str:
    return CHIP_SILABS_LOCK_APP_ESC

  def _rf_api(self, path: str = 'rf_channel', payload: dict = None) -> dict:
    url = f'http://127.0.0.1:5000/{path.lstrip("/")}'
    if payload is not None:
      data = json.dumps(payload).encode('utf-8')
      req = urllib.request.Request(
          url,
          data=data,
          headers={'Content-Type': 'application/json'},
          method='POST',
      )
    else:
      req = urllib.request.Request(url, method='GET')
    with urllib.request.urlopen(req, timeout=10.0) as resp:
      return json.loads(resp.read().decode('utf-8'))

  def _generate_ble_traffic(self, bt_ctrl_port: int, count: int = 45) -> None:
    """Transmits BLE Link-Layer frames across VirtualBluetoothServer PHY."""
    client = VirtualBtControlClient('127.0.0.1', bt_ctrl_port)
    adv_payload = '0201061107f6fff6ff00001000800000805f9b34fb'
    for i in range(count):
      client.request({
          'cmd': 'inject_advertisement',
          'src_bd_addr': f'AA:BB:CC:00:11:{(i % 16):02X}',
          'adv_data_hex': adv_payload,
      })
      time.sleep(0.01)

  def _generate_wifi_traffic(self, req_device_id: str, count: int = 25) -> str:
    """Sends ICMPv6 Echo Requests from Android Emulator over Virtual Wi-Fi."""
    res = self.execute_device_cmd(
        req_device_id,
        f'adb shell "ping6 -c {count} -i 0.08 -s 128 fd11:22::2 || true"',
    )
    return res.get('output', '') if isinstance(res, dict) else str(res)

  def _generate_thread_traffic(
      self, tbr_id: str, ml_eid: str, count: int = 8
  ) -> str:
    """Sends ICMPv6 probes from Thread Border Router to the Silabs Lock SED."""
    res = self.execute_device_cmd(
        tbr_id,
        f'sh -c "ping -6 -c {count} -i 0.5 -W 2 {ml_eid} || true"',
    )
    return res.get('output', '') if isinstance(res, dict) else str(res)

  def _exercise_and_measure_regime(
      self,
      preset_name: str,
      tbr_id: str,
      req_device_id: str,
      ml_eid: str,
      bt_ctrl_port: int,
  ) -> dict:
    """Applies an RF preset, drives BLE/Wi-Fi/Thread traffic, and samples."""
    self.logger.info('Applying RF Channel preset: %s', preset_name)
    self._rf_api('rf_channel', {'preset': preset_name})

    mac_before_raw = self._exec_tbr_ot_ctl(tbr_id, 'ot-ctl counters mac')
    mac_before = parse_ot_mac_counters(mac_before_raw)
    wifi_before = self.query_api('virtual_wifi_info')
    wifi_drops_before = int(
        (wifi_before.get('frame_counters') or {}).get('dropped_rf_frames', 0)
    )

    # 1. BLE traffic across VirtualBluetoothServer LinkLayerHub
    self._generate_ble_traffic(bt_ctrl_port, count=50)

    # 2. Wi-Fi traffic across VirtualWiFiServer between Android & TBR
    wifi_ping_out = self._generate_wifi_traffic(req_device_id, count=30)

    # 3. Thread 802.15.4 traffic across ot-rcp <-> chip-silabs-lock-app
    thread_ping_out = self._generate_thread_traffic(tbr_id, ml_eid, count=8)
    # Allow one ICD IdleMode poll/Check-In cycle on 802.15.4
    time.sleep(4.0)

    mac_after_raw = self._exec_tbr_ot_ctl(tbr_id, 'ot-ctl counters mac')
    mac_after = parse_ot_mac_counters(mac_after_raw)
    child_table = self._exec_tbr_ot_ctl(tbr_id, 'ot-ctl child table')
    cca_rssi_raw = self._exec_tbr_ot_ctl(tbr_id, 'ot-ctl rssi')
    wifi_after = self.query_api('virtual_wifi_info')
    rf_status = self._rf_api('rf_channel')

    # Ensure Thread telemetry in Python RfChannelSimulator also evaluates sample
    # packets matching the Thread link budget for unified status comparison
    thread_tel = rf_status.get('telemetry', {}).get('thread', {})
    wifi_drops_after = int(
        (wifi_after.get('frame_counters') or {}).get('dropped_rf_frames', 0)
    )

    mac_delta = {
        k: mac_after.get(k, 0) - mac_before.get(k, 0)
        for k in set(mac_before) | set(mac_after)
    }

    summary = {
        'preset': preset_name,
        'rf_status': rf_status,
        'ble_telemetry': rf_status.get('telemetry', {}).get('ble', {}),
        'wifi_telemetry': rf_status.get('telemetry', {}).get('wifi', {}),
        'thread_telemetry': thread_tel,
        'thread_effective_params': rf_status.get('thread_effective_params', {}),
        'wifi_dropped_rf_frames_delta': wifi_drops_after - wifi_drops_before,
        'wifi_stations': wifi_after.get('stations', []),
        'ot_mac_counters_before': mac_before,
        'ot_mac_counters_after': mac_after,
        'ot_mac_counters_delta': mac_delta,
        'ot_child_table': child_table,
        'ot_child_lq_in': parse_child_table_lq_in(child_table),
        'ot_cca_rssi': cca_rssi_raw.strip(),
        'wifi_ping_tail': '\n'.join(wifi_ping_out.splitlines()[-4:]),
        'thread_ping_tail': '\n'.join(thread_ping_out.splitlines()[-4:]),
    }
    self.logger.info(
        'Regime %s summary: BLE(mean_rssi=%s, mean_sinr=%s, drops=%s)'
        ' WiFi(mean_rssi=%s, mean_sinr=%s, drops=%s)'
        ' Thread(tx=%s, rx=%s, drops=%s, mean_rssi=%s, mean_sinr=%s,'
        ' mean_lqi=%s, mac_delta=%s)',
        preset_name,
        summary['ble_telemetry'].get('mean_rssi_dbm'),
        summary['ble_telemetry'].get('mean_sinr_db'),
        summary['ble_telemetry'].get('dropped_packets'),
        summary['wifi_telemetry'].get('mean_rssi_dbm'),
        summary['wifi_telemetry'].get('mean_sinr_db'),
        summary['wifi_telemetry'].get('dropped_packets'),
        thread_tel.get('tx_packets'),
        thread_tel.get('rx_packets'),
        thread_tel.get('dropped_packets'),
        thread_tel.get('mean_rssi_dbm'),
        thread_tel.get('mean_sinr_db'),
        thread_tel.get('mean_lqi'),
        {
            k: mac_delta.get(k, 0)
            for k in (
                'TxTotal',
                'TxAckRequested',
                'TxAcked',
                'TxIndirectMaxRetryExpiry',
                'RxTotal',
            )
            if k in mac_delta
        },
    )
    return summary

  def _run_post_checkin_phase(self, ctx: dict) -> None:
    tbr_id = ctx['tbr_id']
    req_device_id = ctx['req_device_id']
    app_log_path = ctx['app_log_path']

    with open(app_log_path, 'r', encoding='utf-8', errors='ignore') as f:
      device_log = f.read()

    self.assertIn(
        'Door Lock server initialized',
        device_log,
        'Silabs Lock App did not log Door Lock server initialization',
    )
    ml_eid = pr.device_ml_eid(device_log)
    self.assertTrue(
        bool(ml_eid),
        'Could not find Silabs Lock App mesh-local EID in device log',
    )

    bt_info = self.query_api('virtual_bt_info')
    bt_ctrl_port = int(bt_info.get('control_port', 23457) or 23457)

    try:
      ideal_baseline = self._exercise_and_measure_regime(
          'ideal', tbr_id, req_device_id, ml_eid, bt_ctrl_port
      )
      regime_a = self._exercise_and_measure_regime(
          'regime_a_near_los', tbr_id, req_device_id, ml_eid, bt_ctrl_port
      )
      regime_b = self._exercise_and_measure_regime(
          'regime_b_far_nlos_fading',
          tbr_id,
          req_device_id,
          ml_eid,
          bt_ctrl_port,
      )
      regime_c = self._exercise_and_measure_regime(
          'regime_c_cochannel_interference',
          tbr_id,
          req_device_id,
          ml_eid,
          bt_ctrl_port,
      )
    finally:
      self._rf_api('rf_channel/reset', {'preset': 'ideal'})

    results = {
        'end_device_binary': 'chip-silabs-lock-app',
        'ml_eid': ml_eid,
        'regimes': {
            'ideal': ideal_baseline,
            'regime_a_near_los': regime_a,
            'regime_b_far_nlos_fading': regime_b,
            'regime_c_cochannel_interference': regime_c,
        },
    }

    out_dir = os.environ.get('DEVICE_LOG_DIR') or os.environ.get(
        'CIRQUE_PCAP_DIR'
    )
    if out_dir and os.path.isdir(out_dir):
      results_path = os.path.join(out_dir, RF_RSSI_RESULTS_FILE)
      with open(results_path, 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=2, sort_keys=True)
      self.logger.info('Saved RF RSSI experiment results to %s', results_path)

    # Oracle RF-1: Strict RSSI attenuation from Near-Field LoS (Regime A) to
    # Far-Field NLOS + Shadowing + Rayleigh Fading (Regime B) across all radios
    ble_rssi_a = float(regime_a['ble_telemetry']['mean_rssi_dbm'])
    ble_rssi_b = float(regime_b['ble_telemetry']['mean_rssi_dbm'])
    wifi_rssi_a = float(regime_a['wifi_telemetry']['mean_rssi_dbm'])
    wifi_rssi_b = float(regime_b['wifi_telemetry']['mean_rssi_dbm'])
    thr_rssi_a = float(regime_a['thread_telemetry']['mean_rssi_dbm'])
    thr_rssi_b = float(regime_b['thread_telemetry']['mean_rssi_dbm'])

    self.assertTrue(
        ble_rssi_a > ble_rssi_b + 8.0,
        f'Oracle RF-1 (BLE RSSI) failed: Regime A ({ble_rssi_a} dBm) not >'
        f' Regime B ({ble_rssi_b} dBm) + 8 dB',
    )
    self.assertTrue(
        wifi_rssi_a > wifi_rssi_b + 8.0,
        f'Oracle RF-1 (Wi-Fi RSSI) failed: Regime A ({wifi_rssi_a} dBm) not >'
        f' Regime B ({wifi_rssi_b} dBm) + 8 dB',
    )
    self.assertTrue(
        thr_rssi_a > thr_rssi_b + 8.0,
        f'Oracle RF-1 (Thread RSSI) failed: Regime A ({thr_rssi_a} dBm) not >'
        f' Regime B ({thr_rssi_b} dBm) + 8 dB',
    )

    # Oracle RF-2: Strict SINR degradation across Regimes A > B > C
    ble_sinr_a = float(regime_a['ble_telemetry']['mean_sinr_db'])
    ble_sinr_b = float(regime_b['ble_telemetry']['mean_sinr_db'])
    ble_sinr_c = float(regime_c['ble_telemetry']['mean_sinr_db'])

    wifi_sinr_a = float(regime_a['wifi_telemetry']['mean_sinr_db'])
    wifi_sinr_b = float(regime_b['wifi_telemetry']['mean_sinr_db'])
    wifi_sinr_c = float(regime_c['wifi_telemetry']['mean_sinr_db'])

    thr_sinr_a = float(regime_a['thread_telemetry']['mean_sinr_db'])
    thr_sinr_b = float(regime_b['thread_telemetry']['mean_sinr_db'])
    thr_sinr_c = float(regime_c['thread_telemetry']['mean_sinr_db'])

    self.assertTrue(
        ble_sinr_a > ble_sinr_b > ble_sinr_c,
        f'Oracle RF-2 (BLE SINR) failed: expected A ({ble_sinr_a}) > B'
        f' ({ble_sinr_b}) > C ({ble_sinr_c})',
    )
    self.assertTrue(
        wifi_sinr_a > wifi_sinr_b > wifi_sinr_c,
        f'Oracle RF-2 (Wi-Fi SINR) failed: expected A ({wifi_sinr_a}) > B'
        f' ({wifi_sinr_b}) > C ({wifi_sinr_c})',
    )
    self.assertTrue(
        thr_sinr_a > thr_sinr_b > thr_sinr_c,
        f'Oracle RF-2 (Thread SINR) failed: expected A ({thr_sinr_a}) > B'
        f' ({thr_sinr_b}) > C ({thr_sinr_c})',
    )

    # Oracle RF-3: Packet drops & PER increase under Regime C interference
    ble_drops_a = int(regime_a['ble_telemetry']['dropped_packets'])
    ble_drops_c = int(regime_c['ble_telemetry']['dropped_packets'])
    wifi_drops_a = int(regime_a['wifi_telemetry']['dropped_packets'])
    wifi_drops_c = int(regime_c['wifi_telemetry']['dropped_packets'])
    thr_drops_a = int(regime_a['thread_telemetry']['dropped_packets'])
    thr_drops_c = int(regime_c['thread_telemetry']['dropped_packets'])

    self.assertTrue(
        ble_drops_c > ble_drops_a,
        f'Oracle RF-3 (BLE drops) failed: Regime C ({ble_drops_c}) not >'
        f' Regime A ({ble_drops_a})',
    )
    self.assertTrue(
        wifi_drops_c > wifi_drops_a,
        f'Oracle RF-3 (Wi-Fi drops) failed: Regime C ({wifi_drops_c}) not >'
        f' Regime A ({wifi_drops_a})',
    )
    self.assertTrue(
        thr_drops_c > thr_drops_a,
        'Oracle RF-3 (Thread 802.15.4 Spinel proxy drops) failed: Regime C'
        f' ({thr_drops_c}) not > Regime A ({thr_drops_a})',
    )

    # Oracle RF-4: Thread 802.15.4 LQI degradation from Regime A to Regime C
    thr_lqi_a = float(regime_a['thread_telemetry']['mean_lqi'])
    thr_lqi_c = float(regime_c['thread_telemetry']['mean_lqi'])
    self.assertTrue(
        thr_lqi_a > thr_lqi_c + 25.0,
        f'Oracle RF-4 (Thread LQI) failed: Regime A LQI ({thr_lqi_a}) not >'
        f' Regime C LQI ({thr_lqi_c}) + 25',
    )

    # Oracle RF-5: Default 'ideal' preset vs 'regime_b_far_nlos_fading'
    # Verifies that 'ideal' has 0% stochastic drops and 0.0 dB RSSI jitter
    # (so loss-driven hazards such as those in
    # docs/testing/cirque_rf_lossy_link.md cannot occur), whereas
    # 'regime_b_far_nlos_fading'
    # introduces realistic NLOS path-loss attenuation (> 15 dB below ideal) and
    # non-zero Gudmundson AR(1) + Rayleigh RSSI variance (std_rssi_db > 2.0 dB).
    ble_drops_ideal = int(ideal_baseline['ble_telemetry']['dropped_packets'])
    wifi_drops_ideal = int(ideal_baseline['wifi_telemetry']['dropped_packets'])
    thr_drops_ideal = int(ideal_baseline['thread_telemetry']['dropped_packets'])
    self.assertEqual(
        ble_drops_ideal + wifi_drops_ideal + thr_drops_ideal,
        0,
        'Oracle RF-5 (ideal baseline) failed: expected 0 drops under ideal'
        f' preset, got BLE={ble_drops_ideal}, Wi-Fi={wifi_drops_ideal},'
        f' Thread={thr_drops_ideal}',
    )
    thr_rssi_ideal = float(ideal_baseline['thread_telemetry']['mean_rssi_dbm'])
    thr_std_b = float(regime_b['thread_telemetry']['std_rssi_db'])
    self.assertTrue(
        thr_rssi_ideal > thr_rssi_b + 15.0,
        f'Oracle RF-5 (ideal vs Regime B RSSI) failed: ideal ({thr_rssi_ideal}'
        f' dBm) not > Regime B ({thr_rssi_b} dBm) + 15 dB',
    )
    self.assertTrue(
        thr_std_b > 2.0,
        'Oracle RF-5 (Regime B stochastic fading variance) failed: expected'
        f' std_rssi_db > 2.0 dB, got {thr_std_b} dB',
    )
    self.logger.info(
        'All Multi-Radio RF RSSI & Coexistence Oracles (RF-1..RF-5) PASSED!'
    )


if __name__ == '__main__':
  sys.exit(
      TestAndroidBleThreadSilabsLockRfRssi(
          _BASE_LIT_ICD_DEVICE_CONFIG
      ).run_test()
  )
