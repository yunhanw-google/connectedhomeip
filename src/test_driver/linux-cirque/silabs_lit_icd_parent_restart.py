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

Shared body of the Thread parent-restart Cirque tests.

After commissioning the ARM32 Silicon Labs Lock App
(examples/lock-app/silabs -> out/arm-silabs-thread/chip-silabs-lock-app, with
BLE + Thread + ICD enabled) over BLE + Thread and verifying its initial
Check-In oracles, these tests restart the ICD's Thread parent (otbr-agent in
the ThreadBorderRouter container) and measure how long the parent cannot reach
its sleepy child.

By default the restart drops the parent's persisted child table (the
ChildInfo settings records) while keeping its network info, so the parent
comes back as leader with the same RLOC16 but no entry for the child: the
"stale parent / black hole" case of the Matter ICD Thread Child Supervision
analysis. CIRQUE_PARENT_RESTART_KEEP_CHILD_INFO=1 restarts otbr-agent with its
settings untouched instead, as an OTBR software restart would; the tests then
record the same measurements without asserting on the black hole.

Measurements, all from raw artifacts:
  * downlink: the border router pings the child's mesh-local EID every
    second (PING_PROBE_SCRIPT);
  * child Thread role transitions from the app log;
  * MLE messages decrypted from thread_0.pcap (Child Update Request, Parent
    Request, Child ID Request from the child);
  * child data polls after the restart and the Imm-Acks the parent radio
    returned for them (thread_0.pcap);
  * Check-Ins sent by the child (app log), seen on 802.15.4, forwarded to
    Wi-Fi, and delivered to Android CHIPTool (onCheckInComplete).
Results are written to parent_restart_results.json in DEVICE_LOG_DIR.
"""

import base64
import copy
import json
import os
import re
import time

from AndroidBleThreadSilabsLitIcdTest import (
    DEVICE_CONFIG as _LIT_ICD_DEVICE_CONFIG,
    TestAndroidBleThreadSilabsLitIcdDevice,
)
from helper import parent_restart as pr
from helper.paths import CHIP_SILABS_LOCK_APP_ESC
from helper.pcap_oracles import (
    MLE_CMD_ADVERTISEMENT,
    MLE_CMD_CHILD_ID_REQUEST,
    MLE_CMD_CHILD_UPDATE_REQUEST,
    MLE_CMD_PARENT_REQUEST,
    acked_mac_commands,
    ieee802154_ack_frames,
    ieee802154_mac_command_frames,
    matter_icd_checkin_packets,
    thread_icd_checkin_frames,
    thread_mle_frames,
)

# SIMULATION-DISCLOSURE: the parent is otbr-agent driving a host-side
# OpenThread simulation RCP (ot-rcp), and the child is the ARM32 Silicon Labs
# Lock App (examples/lock-app/silabs, out/arm-silabs-thread/chip-silabs-lock-app)
# running under qemu-arm on the OpenThread simulation radio and userspace
# Virtual Bluetooth PTY. Only otbr-agent is restarted; the RCP keeps running,
# as when an OTBR host process restarts.

_OTBR_START_CMD = (
    'otbr-agent -I wpan0 -B eth0 spinel+hdlc+uart:///dev/ttyUSB0'
    ' >>/tmp/otbr-agent-restart.log 2>&1'
)
# The image has no killall; wait until no otbr-agent is left.
_OTBR_KILL_SCRIPT = (
    'pkill -15 -x otbr-agent 2>/dev/null || true\n'
    'for i in $(seq 1 50); do pidof otbr-agent >/dev/null || break;'
    ' sleep 0.1; done\n'
    'pkill -9 -x otbr-agent 2>/dev/null || true\n'
    'for i in $(seq 1 10); do pidof otbr-agent >/dev/null || break;'
    ' sleep 0.1; done\n'
    'pidof otbr-agent >/dev/null && echo still-running || echo stopped\n'
)
_PING_LOG = '/tmp/sed_ping.log'
_LEADER_TIMEOUT_S = 90.0
_ROUTER_JOIN_TIMEOUT_S = 90.0
_BASELINE_TIMEOUT_S = 60.0
_BASELINE_OK_RUN = 3
_RESULTS_FILE = 'parent_restart_results.json'

# The LIT ICD topology plus a second Thread router. With a neighbor router in
# the partition, the restarted parent re-joins through MLE Link Request/Accept
# within about a second and keeps its router ID (and so its RLOC16), as a
# router that reboots in a real mesh does. A lone leader instead searches for
# its partition for ~15s with a different short address, so a fast-polling
# child loses MAC Acks and detaches on its own before the parent is back.
PARENT_RESTART_DEVICE_CONFIG = copy.deepcopy(_LIT_ICD_DEVICE_CONFIG)
PARENT_RESTART_DEVICE_CONFIG['device4'] = {
    'type': 'ThreadRouter',
    'base_image': '@default',
    'capability': ['Thread', 'TrafficControl'],
    'rcp_mode': True,
    'docker_network': 'Ipv6',
    'traffic_control': {'latencyMs': 25, 'loss': 0},
}


def _out(res) -> str:
  return res.get('output', '') if isinstance(res, dict) else str(res)


class TestAndroidBleThreadSilabsLitIcdParentRestart(
    TestAndroidBleThreadSilabsLitIcdDevice
):
  """Restarts the child's Thread parent after commissioning (see module doc)."""

  # Child supervision check timeout (s) the end device runs with; 0 disables
  # the check. Set by each concrete test.
  SUPERVISION_CHECK_TIMEOUT_S = None
  # Seconds to watch after the parent is leader again.
  OBSERVE_S = 150.0

  def __init__(self, device_config=None, *args, **kwargs):
    if device_config is None:
      device_config = PARENT_RESTART_DEVICE_CONFIG
    super().__init__(device_config, *args, **kwargs)

  def _get_end_device_binary_esc(self) -> str:
    return CHIP_SILABS_LOCK_APP_ESC

  def _join_second_router(self, tbr_id: str, child_ext: str) -> dict:
    """Joins the ThreadRouter node to the border router's network."""
    routers = [
        d
        for d in self.device_config.values()
        if isinstance(d, dict) and d.get('type') == 'ThreadRouter'
    ]
    self.assertTrue(routers, 'No ThreadRouter node in device_config')
    router_id = routers[0]['id']
    dataset_hex = ''
    for line in self._tbr_ot(tbr_id, 'dataset active -x').splitlines():
      if re.match(r'^[0-9a-fA-F]{20,}$', line.strip()):
        dataset_hex = line.strip()
        break
    self.assertTrue(dataset_hex, 'Could not read the active dataset from TBR')
    # The second router only keeps the partition alive while the parent
    # restarts: it ignores the child's frames (MAC denylist), so the child
    # re-attaches to the restarted parent, the only parent in the doc's
    # single-hub home, and it runs no SRP server, so the child keeps using the
    # border router's, where the CHIPTool node is registered.
    for cmd in (
        'ot-ctl srp server disable',
        f'ot-ctl macfilter addr add {child_ext}',
        'ot-ctl macfilter addr denylist',
        'ot-ctl routerselectionjitter 1',
        f'ot-ctl dataset set active {dataset_hex}',
        'ot-ctl ifconfig up',
        'ot-ctl thread start',
    ):
      self.execute_device_cmd(router_id, cmd)
    state = ''
    deadline = time.time() + _ROUTER_JOIN_TIMEOUT_S
    while time.time() < deadline:
      state = _out(self.execute_device_cmd(router_id, 'ot-ctl state'))
      if 'router' in state or 'leader' in state:
        break
      time.sleep(1.0)
    self.assertTrue(
        'router' in state,
        f'Second Thread router did not become router: {state!r}',
    )
    rloc = re.search(
        r'\b([0-9a-fA-F]{4})\b',
        _out(self.execute_device_cmd(router_id, 'ot-ctl rloc16')),
    )
    macfilter = _out(
        self.execute_device_cmd(router_id, 'ot-ctl macfilter addr')
    )
    srp_state = _out(
        self.execute_device_cmd(router_id, 'ot-ctl srp server state')
    )
    self.assertTrue(
        'denylist' in macfilter.lower() and child_ext in macfilter.lower(),
        f'Second router does not deny the child {child_ext}: {macfilter!r}',
    )
    self.assertTrue(
        'disabled' in srp_state,
        f'Second router SRP server is not disabled: {srp_state!r}',
    )
    info = {'router_id': router_id, 'rloc16': rloc.group(1) if rloc else ''}
    self.logger.info('Second Thread router joined: %s', info)
    return info

  def _extra_app_env(self) -> str:
    return (
        'CHIP_OT_CHILD_SUPERVISION_CHECK_TIMEOUT_S='
        f'{int(self.SUPERVISION_CHECK_TIMEOUT_S)} '
    )

  def _keep_child_info(self) -> bool:
    return os.environ.get('CIRQUE_PARENT_RESTART_KEEP_CHILD_INFO') == '1'

  # -------------------------------------------------------------------------
  # Border router helpers

  def _tbr_sh(self, tbr_id: str, body: str) -> str:
    """Runs a shell script on the border router via base64 (no quoting)."""
    b64 = base64.b64encode(body.encode('utf-8')).decode('ascii')
    return _out(
        self.execute_device_cmd(
            tbr_id,
            f"sh -c 'echo {b64} | base64 -d > /tmp/pr_cmd.sh && sh"
            " /tmp/pr_cmd.sh'",
        )
    )

  def _tbr_ot(self, tbr_id: str, args: str) -> str:
    return self._exec_tbr_ot_ctl(tbr_id, f'ot-ctl {args}')

  def _tbr_rloc16(self, tbr_id: str) -> str:
    out = self._tbr_ot(tbr_id, 'rloc16')
    m = re.search(r'\b([0-9a-fA-F]{4})\b', out)
    return m.group(1).lower() if m else ''

  def _tbr_state(self, tbr_id: str) -> str:
    out = self._tbr_ot(tbr_id, 'state')
    for state in ('leader', 'router', 'child', 'detached', 'disabled'):
      if state in out:
        return state
    return ''

  def _start_ping_probe(self, tbr_id: str, addr: str) -> None:
    b64 = base64.b64encode(pr.PING_PROBE_SCRIPT.encode('utf-8')).decode('ascii')
    self._tbr_sh(
        tbr_id,
        "pkill -f '[s]ed_ping.sh' 2>/dev/null || true\n"
        f'echo {b64} | base64 -d > /tmp/sed_ping.sh\n'
        f': > {_PING_LOG}\n'
        f'nohup sh /tmp/sed_ping.sh {addr} >> {_PING_LOG} 2>&1 </dev/null &\n',
    )

  def _read_ping_log(self, tbr_id: str):
    return pr.parse_ping_log(self._tbr_sh(tbr_id, f'cat {_PING_LOG}'))

  def _reapply_br_network_config(self, tbr_id: str) -> None:
    """Restores the border-router config the test added at setup.

    The on-mesh prefix, the route to the Wi-Fi subnet and the wpan0
    addresses are local configuration, not part of the restored network
    state; without them no Check-In could leave the mesh whatever the child
    does.
    """
    for cmd in (
        'ot-ctl prefix add fd11:33::/64 pasor',
        'ot-ctl route add fd11:22::/64 s med',
        'ot-ctl netdata register',
        'ot-ctl srp server enable',
    ):
      self._exec_tbr_ot_ctl(tbr_id, cmd)
    self._tbr_sh(
        tbr_id,
        'ip6tables -P FORWARD ACCEPT 2>/dev/null || true\n'
        'sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null 2>&1 || true\n'
        'ip -6 addr replace fd11:33::1/64 dev wpan0 nodad 2>/dev/null || true\n'
        'ip -6 route replace fd11:33::/64 dev wpan0 2>/dev/null || true\n',
    )

  def _restart_parent(self, tbr_id: str) -> dict:
    """Restarts otbr-agent; drops the persisted child table unless kept."""
    info = {'keep_child_info': self._keep_child_info()}
    info['pid_before'] = self._tbr_sh(tbr_id, 'pidof otbr-agent\n').strip()
    info['settings_before'] = self._tbr_sh(
        tbr_id, 'ls -la /var/lib/thread 2>&1\n'
    )
    info['t_kill'] = time.time()
    info['kill_output'] = self._tbr_sh(tbr_id, _OTBR_KILL_SCRIPT).strip()
    if not info['keep_child_info']:
      b64 = base64.b64encode(pr.STRIP_CHILD_INFO_SCRIPT.encode('utf-8')).decode(
          'ascii'
      )
      info['strip_output'] = self._tbr_sh(
          tbr_id,
          f'echo {b64} | base64 -d > /tmp/strip.py\npython3 /tmp/strip.py\n',
      )
      info['child_info_removed'] = sum(
          int(n) for n in re.findall(r'removed=(\d+)', info['strip_output'])
      )
    info['t_start'] = time.time()
    self._tbr_sh(tbr_id, ': > /tmp/otbr-agent-restart.log\n')
    self._tbr_sh(tbr_id, f'nohup {_OTBR_START_CMD} </dev/null &\n')
    t_up = None
    deadline = time.time() + _LEADER_TIMEOUT_S
    while time.time() < deadline:
      if self._tbr_state(tbr_id) in ('leader', 'router'):
        t_up = time.time()
        break
      time.sleep(0.2)
    info['t_up'] = t_up
    info['state_after'] = self._tbr_state(tbr_id)
    info['pid_after'] = self._tbr_sh(tbr_id, 'pidof otbr-agent\n').strip()
    return info

  # -------------------------------------------------------------------------
  # Analysis

  def _collect(
      self, ctx: dict, restart: dict, observe_end: float, baseline_start: float
  ) -> dict:
    tbr_id = ctx['tbr_id']
    t_up = restart['t_up']
    with open(ctx['app_log_path'], 'r', encoding='utf-8', errors='ignore') as f:
      device_log = f.read()
    ping = self._read_ping_log(tbr_id)
    roles_after = [
        (t, r) for t, r in pr.device_roles(device_log) if t >= restart['t_kill']
    ]
    detach_t = next((t for t, r in roles_after if r == 'DETACHED'), None)
    reattach_t = next(
        (
            t
            for t, r in roles_after
            if r == 'CHILD' and detach_t and t > detach_t
        ),
        None,
    )
    downlink_back = pr.first_ok_after(ping, t_up)
    recovered = downlink_back is not None and downlink_back < observe_end

    mle = thread_mle_frames(ctx['thread_pcap'], ctx['network_key'])
    parent_ext = {f['src64'] for f in mle if f['cmd'] == MLE_CMD_ADVERTISEMENT}
    child_mle = [
        f
        for f in mle
        if f['src64']
        and f['src64'] not in parent_ext
        and t_up <= f['ts'] < observe_end
    ]

    def first_cmd(cmd):
      return next((f['ts'] for f in child_mle if f['cmd'] == cmd), None)

    polls = [
        p
        for p in ieee802154_mac_command_frames(ctx['thread_pcap'])
        if t_up <= p['ts'] < observe_end
    ]
    all_acks = ieee802154_ack_frames(ctx['thread_pcap'])
    acked = acked_mac_commands(polls, all_acks)
    blind_end = downlink_back if recovered else observe_end
    acked_blind = [p for p in acked if p['ts'] < blind_end]

    raw_dev_log = self.get_device_log(tbr_id)
    container_log = (
        raw_dev_log.decode('utf-8', errors='ignore')
        if isinstance(raw_dev_log, (bytes, bytearray))
        else str(raw_dev_log)
    )
    restart_file_log = self._tbr_sh(
        tbr_id,
        'cat /tmp/otbr-agent-restart.log /var/log/syslog 2>/dev/null || true',
    )
    combined_tbr_log = container_log + '\n' + restart_file_log

    otbr_drops = pr.otbr_unknown_neighbor_drops(
        combined_tbr_log,
        pid_exclude=restart.get('pid_before'),
        src16s=ctx['sed_rloc16s'],
    )
    otbr_data_drops = pr.otbr_unknown_neighbor_drops(
        combined_tbr_log,
        pid_exclude=restart.get('pid_before'),
        src16s=ctx['sed_rloc16s'],
        frame_type='Data',
    )

    uplink_data = [
        p
        for p in pr.ieee802154_mac_data_frames(
            ctx['thread_pcap'], ctx['sed_rloc16s']
        )
        if t_up <= p['ts'] < observe_end
    ]
    uplink_data_acked = acked_mac_commands(uplink_data, all_acks)
    uplink_data_acked_while_blind = len(
        [p for p in uplink_data_acked if p['ts'] < blind_end]
    )

    tx = pr.device_checkin_tx(device_log)
    logcat = self._poll_android_logcat_epoch(ctx['req_device_id'])
    android_cb = pr.logcat_checkin_callbacks(logcat)
    radio_ci = [
        f['ts']
        for f in thread_icd_checkin_frames(
            ctx['thread_pcap'], ctx['network_key']
        )
    ]
    wifi_ci = (
        [w['ts'] for w in matter_icd_checkin_packets(ctx['wifi_pcap'])]
        if os.path.exists(ctx['wifi_pcap'])
        else []
    )

    def window_counts(start, end):
      return {
          'device_tx': len([t for t, _ in tx if start <= t < end]),
          'radio': len(pr.in_window(radio_ci, start, end)),
          'wifi': len(pr.in_window(wifi_ci, start, end)),
          'android_cb': len(pr.in_window(android_cb, start, end)),
          'downlink_ping': pr.summarize_ping(ping, start, end),
      }

    return {
        'supervision': pr.supervision_config(device_log),
        'restart': restart,
        'observe_end': observe_end,
        'downlink': {
            'last_ok_before_kill': pr.last_ok_before(ping, restart['t_kill']),
            'first_ok_after_up': downlink_back,
            'blackhole_s': (downlink_back - t_up) if recovered else None,
            'blackhole_at_least_s': None if recovered else observe_end - t_up,
            'recovered': recovered,
        },
        'child_roles_after_kill': roles_after,
        'child_detach_t': detach_t,
        'child_reattach_t': reattach_t,
        'child_mle_after_up': [(f['ts'], f['cmd']) for f in child_mle],
        'first_child_update_request_t': first_cmd(MLE_CMD_CHILD_UPDATE_REQUEST),
        'first_parent_request_t': first_cmd(MLE_CMD_PARENT_REQUEST),
        'first_child_id_request_t': first_cmd(MLE_CMD_CHILD_ID_REQUEST),
        'polls_after_up': len(polls),
        'polls_acked_while_blind': len(acked_blind),
        'polls_acked_while_blind_fp': len(
            [p for p in acked_blind if p['frame_pending']]
        ),
        'uplink_data_after_up': len(uplink_data),
        'uplink_data_acked_while_blind': uplink_data_acked_while_blind,
        'otbr_unknown_neighbor_drops': otbr_drops,
        'otbr_unknown_neighbor_data_drops': otbr_data_drops,
        'checkin_dns_failures_while_blind': len(
            pr.in_window(
                pr.device_checkin_resolution_failures(device_log),
                t_up,
                blind_end,
            )
        ),
        'checkins': {
            'baseline': window_counts(baseline_start, restart['t_kill']),
            'outage': window_counts(restart['t_kill'], t_up),
            'blind': window_counts(t_up, blind_end),
            'after_recovery': (
                window_counts(blind_end, observe_end) if recovered else None
            ),
        },
    }

  def _poll_android_logcat_epoch(self, req_device_id: str) -> str:
    res = self.execute_device_cmd(
        req_device_id,
        'adb shell "logcat -d -v epoch | grep onCheckInComplete || true"',
    )
    return _out(res)

  def _write_results(self, results: dict) -> None:
    out_dir = os.environ.get('DEVICE_LOG_DIR') or os.environ.get(
        'CIRQUE_PCAP_DIR'
    )
    if not out_dir:
      return
    path = os.path.join(out_dir, _RESULTS_FILE)
    with open(path, 'w', encoding='utf-8') as f:
      json.dump(results, f, indent=2, sort_keys=True)
    self.logger.info('Parent-restart results written to %s', path)

  # -------------------------------------------------------------------------
  # Test phase

  def _run_post_checkin_phase(self, ctx: dict) -> None:
    tbr_id = ctx['tbr_id']
    with open(ctx['app_log_path'], 'r', encoding='utf-8', errors='ignore') as f:
      device_log = f.read()
    self.assertIn(
        'Door Lock server initialized',
        device_log,
        'End device did not initialize the Silabs Door Lock server',
    )
    sup = pr.supervision_config(device_log)
    self.assertTrue(
        sup is not None
        and sup['check_timeout_s'] == int(self.SUPERVISION_CHECK_TIMEOUT_S),
        'End device did not log child supervision check_timeout='
        f'{self.SUPERVISION_CHECK_TIMEOUT_S}s: {sup}',
    )
    ml_eid = pr.device_ml_eid(device_log)
    self.assertTrue(ml_eid, 'No mesh-local EID found in the end-device log')
    rloc_before = self._tbr_rloc16(tbr_id)
    child_table_before = self._tbr_ot(tbr_id, 'child table')
    self.assertTrue(
        ctx['sed_rloc16s'],
        f'No sleepy child in the parent child table:\n{child_table_before}',
    )
    second_router = self._join_second_router(
        tbr_id, pr.parse_child_ext_addrs(child_table_before)[0]
    )
    child_table_before = self._tbr_ot(tbr_id, 'child table')
    self.assertTrue(
        pr.parse_child_rloc16s(child_table_before),
        'The child left the border router when the second router joined:\n'
        f'{child_table_before}',
    )

    # Positive control: the parent reaches the child before the restart.
    self._start_ping_probe(tbr_id, ml_eid)
    baseline_start = time.time()
    deadline = baseline_start + _BASELINE_TIMEOUT_S
    baseline = []
    while time.time() < deadline:
      baseline = self._read_ping_log(tbr_id)
      if pr.ok_run_length(baseline) >= _BASELINE_OK_RUN:
        break
      time.sleep(2.0)
    self.assertTrue(
        pr.ok_run_length(baseline) >= _BASELINE_OK_RUN,
        f'Downlink baseline failed: border router could not ping {ml_eid}'
        f' {_BASELINE_OK_RUN} times in a row: {baseline[-10:]}',
    )
    self.logger.info(
        'Downlink baseline OK: %s reachable (%d probes); parent rloc16=%s,'
        ' supervision=%s',
        ml_eid,
        len(baseline),
        rloc_before,
        sup,
    )

    restart = self._restart_parent(tbr_id)
    self.logger.info(
        'otbr-agent restart: pid %s -> %s, kill=%s, settings before:\n%s\n'
        'strip: %s',
        restart['pid_before'],
        restart['pid_after'],
        restart['kill_output'],
        restart['settings_before'],
        restart.get('strip_output'),
    )
    self.assertTrue(
        restart['kill_output'].endswith('stopped'),
        f'otbr-agent did not stop: {restart["kill_output"]!r}',
    )
    self.assertTrue(
        restart['pid_after'] and restart['pid_after'] != restart['pid_before'],
        'otbr-agent was not restarted: pid'
        f' {restart["pid_before"]!r} -> {restart["pid_after"]!r}',
    )
    self.assertTrue(
        restart['t_up'] is not None,
        f'otbr-agent did not become leader within {_LEADER_TIMEOUT_S}s after'
        ' restart',
    )
    self._reapply_br_network_config(tbr_id)
    restart['second_router'] = second_router
    # The CHIPTool node is registered by the TBR's own SRP client on behalf
    # of Android; that state does not survive the restart. Register it again
    # so the child can resolve its Check-In peer (test harness, not product).
    if ctx.get('chiptool_srp'):
      restart['chiptool_srp_reregistered'] = (
          self._register_chiptool_srp_service(tbr_id, *ctx['chiptool_srp'])
      )
    restart['rloc16_before'] = rloc_before
    restart['rloc16_after'] = self._tbr_rloc16(tbr_id)
    restart['child_table_before'] = child_table_before
    restart['child_table_after_up'] = self._tbr_ot(tbr_id, 'child table')
    self.logger.info(
        'Parent restarted: keep_child_info=%s outage=%.1fs rloc16 %s -> %s,'
        ' child table after restart:\n%s',
        restart['keep_child_info'],
        restart['t_up'] - restart['t_kill'],
        rloc_before,
        restart['rloc16_after'],
        restart['child_table_after_up'],
    )
    if not restart['keep_child_info']:
      self.assertTrue(
          restart.get('child_info_removed', 0) >= 1,
          'Fault injection is vacuous: no ChildInfo record was removed from'
          f' the parent settings: {restart.get("strip_output")!r}',
      )
      self.assertEqual(
          pr.parse_child_rloc16s(restart['child_table_after_up']),
          set(),
          'Parent still lists a child right after the restart:\n'
          f'{restart["child_table_after_up"]}',
      )
      self.assertEqual(
          restart['rloc16_after'],
          rloc_before,
          'Parent did not reuse its RLOC16, so the stale-parent case is not'
          ' reproduced',
      )

    observe_end = restart['t_up'] + self.OBSERVE_S
    while time.time() < observe_end:
      time.sleep(5.0)
      if not self._tbr_sh(tbr_id, 'pidof otbr-agent\n').strip():
        self.logger.warning('otbr-agent crashed/exited! Relaunching...')
        self._tbr_sh(tbr_id, f'nohup {_OTBR_START_CMD} </dev/null &\n')
        deadline = time.time() + 15.0
        while time.time() < deadline:
          if self._tbr_state(tbr_id) in ('leader', 'router'):
            break
          time.sleep(0.5)
        self._reapply_br_network_config(tbr_id)
        if ctx.get('chiptool_srp'):
          self._register_chiptool_srp_service(tbr_id, *ctx['chiptool_srp'])
    restart['child_table_end'] = self._tbr_ot(tbr_id, 'child table')
    self.assertNotIn(
        'Connection refused',
        restart['child_table_end'],
        'otbr-agent crashed or refused connection at the end of the observe '
        'window',
    )

    results = self._collect(ctx, restart, observe_end, baseline_start)
    results['test'] = type(self).__name__
    results['end_device_app'] = (
        'examples/lock-app/silabs (out/arm-silabs-thread/chip-silabs-lock-app)'
    )
    self._write_results(results)
    self.logger.info(
        'Parent-restart summary: %s',
        json.dumps({
            'keep_child_info': restart['keep_child_info'],
            'supervision': results['supervision'],
            'downlink': results['downlink'],
            'child_detach_t': results['child_detach_t'],
            'child_reattach_t': results['child_reattach_t'],
            'first_child_update_request_t': results[
                'first_child_update_request_t'
            ],
            'polls_acked_while_blind': results['polls_acked_while_blind'],
            'checkins': results['checkins'],
        }),
    )
    if restart['keep_child_info']:
      self.logger.info(
          'CIRQUE_PARENT_RESTART_KEEP_CHILD_INFO=1: measurements recorded,'
          ' black-hole assertions skipped'
      )
      return
    self._assert_outcome(results)

  def _assert_outcome(self, results: dict) -> None:
    raise NotImplementedError
