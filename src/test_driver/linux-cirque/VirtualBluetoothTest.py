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

import os
from pathlib import Path
import subprocess
import sys
import time
import unittest

CHIP_REPO_ROOT = Path(__file__).resolve().parents[3]
CIRQUE_REPO_ROOT = CHIP_REPO_ROOT / "third_party/cirque/repo"
if str(CIRQUE_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(CIRQUE_REPO_ROOT))
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from cirque.capabilities.bluetoothcapability import (  # noqa: E402
    BlueToothCapability,
)
from CommissioningTest import (  # noqa: E402
    DEVICE_CONFIG,
    TEST_DISCRIMINATOR,
    TEST_DISCRIMINATOR2,
)


class TestCirqueVirtualBluetoothCommissioning(unittest.TestCase):
    """Verifies CommissioningTest Virtual BT HCI and Matter BLE BTP."""

    def cleanup_virtual_bt(self):
        BlueToothCapability.stop_virtual_server()
        BlueToothCapability.BLE_ADAPTS_LIST.clear()

    def test_commissioning_device_config_virtual_hci_and_ble_btp(self):
        """Allocates hci0..hci4 for DEVICE_CONFIG and runs BLE BTP."""
        capabilities = {}
        try:
            for dev_name, cfg in DEVICE_CONFIG.items():
                self.assertIn('Bluetooth', cfg['capability'])
                capabilities[dev_name] = BlueToothCapability(
                    use_virtual_bt_tcp=True
                )

            self.assertEqual(capabilities['device0'].ble_adapt, 'hci0')
            self.assertEqual(capabilities['device0'].ble_adapt_id, 0)
            self.assertEqual(capabilities['device1'].ble_adapt, 'hci1')
            self.assertEqual(capabilities['device1'].ble_adapt_id, 1)
            self.assertEqual(capabilities['device2'].ble_adapt, 'hci2')
            self.assertEqual(capabilities['device2'].ble_adapt_id, 2)

            # Verify inside-Docker hciconfig CLI reports hci0..hci4
            desc0 = capabilities['device0'].description
            hciconfig_bin = desc0['hciconfig_path']
            self.assertTrue(os.path.exists(hciconfig_bin))
            out_all = subprocess.check_output([hciconfig_bin], text=True)
            for idx in range(5):
                expected_line = f'hci{idx}:\tType: Primary  Bus: Virtual'
                self.assertIn(expected_line, out_all)

            # Verify CommissioningTest device1..device2 advertise 0xFFF6
            # and device0 (MobileDevice on hci0) scans and exchanges BTP
            mgr = BlueToothCapability._SHARED_DOCKER_MANAGER
            central_hci0 = mgr.adapter_bridges['hci0']
            end_dev1_hci1 = mgr.adapter_bridges['hci1']
            end_dev2_hci2 = mgr.adapter_bridges['hci2']

            end_dev1_hci1.start_matter_advertising(
                discriminator=TEST_DISCRIMINATOR
            )
            end_dev2_hci2.start_matter_advertising(
                discriminator=TEST_DISCRIMINATOR2
            )

            target_addr = central_hci0.scan_for_matter_discriminator(
                discriminator=TEST_DISCRIMINATOR2, timeout=3.0
            )
            self.assertEqual(target_addr, capabilities['device2'].bd_addr)

            conn_handle = central_hci0.connect_to_peripheral(target_addr)
            self.assertGreater(conn_handle, 0)

            deadline = time.time() + 2.0
            while (
                not end_dev2_hci2.connected_handles
                and time.time() < deadline
            ):
                time.sleep(0.05)
            self.assertTrue(end_dev2_hci2.connected_handles)
            periph_handle = next(
                iter(end_dev2_hci2.connected_handles.keys())
            )

            # Send Matter BTP Handshake over C1 Write and C2 Indication
            btp_req = bytes.fromhex('656c04000000f40005')
            central_hci0.write_c1_request(conn_handle, btp_req)
            deadline = time.time() + 2.0
            while not end_dev2_hci2.rx_writes and time.time() < deadline:
                time.sleep(0.05)
            self.assertEqual(end_dev2_hci2.rx_writes[0], btp_req)

            btp_rsp = bytes.fromhex('656c0400f40005')
            end_dev2_hci2.send_c2_indication(periph_handle, btp_rsp)
            deadline = time.time() + 2.0
            while (
                not central_hci0.rx_indications
                and time.time() < deadline
            ):
                time.sleep(0.05)
            self.assertEqual(central_hci0.rx_indications[0], btp_rsp)
        finally:
            for cap in capabilities.values():
                cap.disable_capability(None)
            self.cleanup_virtual_bt()


if __name__ == "__main__":
    unittest.main(verbosity=2)
