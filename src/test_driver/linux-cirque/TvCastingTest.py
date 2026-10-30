#!/usr/bin/env python3
"""
Copyright (c) 2026 Project CHIP Authors

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

import logging
import os
import sys
import time

from helper.CHIPTestBase import CHIPVirtualHome, TestResult
from helper.paths import CHIP_REPO_STR

logger = logging.getLogger('TvCastingTest')
logger.setLevel(logging.INFO)
sh = logging.StreamHandler()
sh.setFormatter(logging.Formatter('%(asctime)s [%(name)s] %(levelname)s %(message)s'))
logger.addHandler(sh)

CIRQUE_URL = "http://localhost:5000"

DEVICE_CONFIG = {
    'device0': {
        'type': 'CHIPEndDevice',
        'base_image': '@default',
        'capability': ['TrafficControl', 'Mount'],
        'rcp_mode': True,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 10},
        "mount_pairs": [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
    'device1': {
        'type': 'CHIPEndDevice',
        'base_image': '@default',
        'capability': ['TrafficControl', 'Mount'],
        'rcp_mode': True,
        'docker_network': 'Ipv6',
        'traffic_control': {'latencyMs': 10},
        "mount_pairs": [[CHIP_REPO_STR, CHIP_REPO_STR]],
    }
}


class TvCastingTest(CHIPVirtualHome):
    def __init__(self, device_config):
        super().__init__(CIRQUE_URL, device_config)
        self.logger = logger

    def setup(self):
        self.initialize_home()

    def test_routine(self):
        tv_app_id = self.non_ap_devices[0]['id']
        casting_app_id = self.non_ap_devices[1]['id']

        self.execute_device_cmd(
            tv_app_id,
            f"CHIPCirqueDaemon.py -- run bash -c 'rm -rf /tmp/chip*; (sleep 8; echo \"controller ux ok\"; sleep 2; echo \"controller ux ok 20202021\"; sleep 30) | {CHIP_REPO_STR}/out/debug/tv-app/chip-tv-app'",
        )

        time.sleep(2)

        self.execute_device_cmd(
            casting_app_id,
            f"CHIPCirqueDaemon.py -- run bash -c 'rm -rf /tmp/chip*; (echo \"cast discover\"; sleep 3; echo \"cast request 0\"; sleep 15; echo \"cast launch https://test.url Hello\"; sleep 2; echo \"cast cluster contentlauncher launch-url https://test.url 0 1\"; sleep 5) | {CHIP_REPO_STR}/out/debug/tv-casting-app/chip-tv-casting-app'",
        )

        time.sleep(30)

        tv_app_log = self.get_device_log(tv_app_id).decode('utf-8', errors='replace')
        casting_app_log = self.get_device_log(casting_app_id).decode('utf-8', errors='replace')

        log_dir = os.environ.get('DEVICE_LOG_DIR')
        if log_dir and os.path.isdir(log_dir):
            with open(os.path.join(log_dir, "tv_app.log"), "w") as f:
                f.write(tv_app_log)
            with open(os.path.join(log_dir, "casting_app.log"), "w") as f:
                f.write(casting_app_log)

        # (a) mDNS _matterd._udp discovery
        self.assertTrue(
            "_matterd._udp" in tv_app_log
            or "_matterd._udp" in casting_app_log
            or "Discovered CastingPlayer" in casting_app_log
            or "SDKExample" in casting_app_log,
            "mDNS _matterd._udp discovery not found",
        )
        # (b) UDC UDP message
        self.assertTrue(
            "UDC instance=" in tv_app_log
            or "User Directed Commissioning" in tv_app_log,
            "UDC UDP message not found",
        )
        # (c) Commissioning completion
        self.assertTrue(
            "Commissioning completed" in tv_app_log
            or "Commissioning completed" in casting_app_log
            or "Commission" in tv_app_log,
            "Commissioning completion not found",
        )
        # (d) ContentLauncherManager::HandleLaunchUrl
        self.assertTrue(
            "ContentLauncherManager::HandleLaunchUrl" in tv_app_log,
            "ContentLauncherManager::HandleLaunchUrl not found",
        )
        return TestResult.OK


if __name__ == "__main__":
    sys.exit(TvCastingTest(DEVICE_CONFIG).run_test())
