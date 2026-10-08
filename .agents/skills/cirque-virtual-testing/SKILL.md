---
name: cirque-virtual-testing
description:
    Use when orchestrating end-to-end Matter virtual home tests with Cirque,
    running Android BLE+Thread or BLE+Wi-Fi commissioning, testing Sleepy LIT ICD
    radio-gating, generating decrypted Matter wire flow visualizations, or
    collecting LCOV code coverage across Cirque virtual tests. Don't use for
    non-Matter protocol-agnostic Cirque RF development; use cirque-virtual-home
    instead.
---

# Cirque Virtual Testing: Matter E2E Virtual Home Orchestration & MCP Extensions

## Overview

`cirque-virtual-testing` defines the Matter protocol layer and test orchestration framework built on top of Cirque virtual home simulation. While Cirque provides the protocol-agnostic virtual RF media (Virtual Bluetooth LE, Virtual Wi-Fi WPA2, OpenThread simulation, Android Docker container nodes), this skill provides:
- **Layer-1 Matter MCP Extension Architecture**: Pluggable extension (`src/test_driver/linux-cirque/mcp/matter_cirque_extension.py`) registering Matter-specific tools, resources, prompts, and health oracles into Cirque MCP server.
- **Matter Cross-Layer Flow Dissector**: Decrypted Wire visualization plugin (`src/test_driver/linux-cirque/mcp/matter_flow_plugin.py`) classifying BTP handshakes, PASE/CASE key exchanges, operational UDP :5540 traffic, and network provisioning.
- **Comprehensive E2E Test Driver Workflows**: Executing Android BLE+Wi-Fi and BLE+Thread commissioning suites with screen recording and deferred single-pass LCOV code coverage.
- **Sleepy LIT ICD Radio-Gating**: Dynamic kernel iptables radio-off simulation and synchronous named-pipe wake-up triggers for Intermittently Connected Devices (LIT).

### Architecture & Layering

The Matter testing infrastructure anchors dynamically to Cirque via `${CIRQUE_DIR:-third_party/cirque/repo}`:
```
+-------------------------------------------------------------------------+
| Layer 1: Matter Virtual Testing (connectedhomeip)                      |
| - src/test_driver/linux-cirque/mcp/matter_cirque_extension.py           |
| - src/test_driver/linux-cirque/mcp/matter_flow_plugin.py                |
| - scripts/tests/cirque_tests.sh                                         |
| - AndroidBleWiFiMobileDeviceTest.py & AndroidBleThreadMobileDeviceTest  |
+-------------------------------------------------------------------------+
                                    |
                                    v (MCP Extension Handshake)
+-------------------------------------------------------------------------+
| Layer 0: Cirque Virtual Home (${CIRQUE_DIR:-third_party/cirque/repo})   |
| - cirque/mcp/server.py (CirqueMcpServer JSON-RPC 2.0)                   |
| - cirque/virtual_bt/ (Userspace H4 TCP + BlueZ D-Bus Daemon)            |
| - cirque/virtual_wifi/ (Userspace WPA2 4-Way Handshake + L2 TAP Relay)  |
| - cirque/nodes/androiddockernode.py (Headless KVM Android Emulator)     |
| - cirque/pcap/flow_visualizer.py (Interactive HTML5 Studio)             |
| - See third_party/cirque/repo/.agents/skills/cirque-virtual-home/SKILL.md|
+-------------------------------------------------------------------------+
```

---

## Mandatory Pre-Flight Handshake

Before invoking Matter-specific operations, ensure the Cirque MCP server has registered `matter_cirque_extension`:

1. **Environment Variable Configuration**:
   ```bash
   export CIRQUE_MCP_EXTENSIONS="src/test_driver/linux-cirque/mcp/matter_cirque_extension.py"
   ```
2. **Dynamic Runtime Registration**:
   Invoke `cirque_register_mcp_extension` with `module_or_path="src/test_driver/linux-cirque/mcp/matter_cirque_extension.py"`.
3. **Verification**:
   Call `cirque_list_mcp_extensions` and verify that `matter_cirque_extension` is listed with all 7 Matter tools registered.

---

## Matter MCP Extension Tools Reference (7 Tools)

The Matter extension (`src/test_driver/linux-cirque/mcp/matter_cirque_extension.py`) dynamically registers 7 high-level tools:

| Tool Name | Parameters | Description |
| :--- | :--- | :--- |
| `matter_commission_over_ble` | `home_id`, `device_id`, `network_type` (`wifi`/`thread`), `trigger` (`auto`/`intent`/`ui`), `execution_mode`, `ssid`, `psk`, `channel`, `pan_id`, `xpan_id`, `master_key`, `dataset_tlvs_hex`, `setup_pin_code`, `discriminator`, `manual_code`, `timeout_sec`, `record_video` | Executes end-to-end Matter BLE+Wi-Fi or BLE+Thread commissioning using Android CHIPTool. |
| `matter_cluster_action` | `home_id`, `device_id`, `cluster` (`OnOff`/`Descriptor`/`BasicInformation`/`OperationalCredentials`), `action` (`toggle`/`read`/`toggle_and_read`), `node_id`, `endpoint`, `timeout_sec`, `record_video` | Executes OnOff cluster toggle/read or reads Descriptor, BasicInformation, and OperationalCredentials cluster attributes. |
| `matter_diagnose_commissioning_health` | `home_id`, `android_device_id` | Evaluates Matter-specific diagnostic health oracles across PASE, CASE, and cluster communication. |
| `matter_verify_ble_commissioning_waterfall` | `home_id`, `device_id`, `network_type`, `execution_mode`, `timeout_sec` | Validates the complete 7-stage commissioning waterfall (ADV_DISCOVERY -> GATT_CONNECT -> PASE_HANDSHAKE -> FAILSAFE_ARMED -> NETWORK_CONFIG -> OPERATIONAL_CERT_ISSUED -> CASE_CONNECT). |
| `matter_exercise_ble_brownout_recovery` | `home_id`, `device_id`, `execution_mode`, `brownout_duration_sec`, `restore_after_brownout` | Injects RF brownout chaos during BLE commissioning and verifies autonomous self-healing recovery. |
| `matter_audit_cross_layer_contract` | `home_id`, `execution_mode` | Audits cross-layer contracts across HCI, Wi-Fi TAP, Thread mesh, and Matter data-model attributes. |
| `matter_generate_flow_visualization` | `pcap_dir`, `output_html`, `output_json`, `run_harness_if_empty` | Generates cross-layer HTML5 flow visualizer using `src/test_driver/linux-cirque/mcp/matter_flow_plugin.py` to classify decrypted Matter wire traffic. |

---

## Running Cirque E2E Tests & Fast Iterative Debugging

All Cirque tests in ConnectedHomeIP are managed via `scripts/tests/cirque_tests.sh`:

### 1. Single Test Execution
Run a specific test target directly:
```bash
scripts/tests/cirque_tests.sh run_test AndroidBleWiFiMobileDeviceTest
scripts/tests/cirque_tests.sh run_test AndroidBleThreadMobileDeviceTest
scripts/tests/cirque_tests.sh run_test IcdDeviceTest
scripts/tests/cirque_tests.sh run_test MobileDeviceTest
```

### 2. Per-Test Timeout Overrides
For slow hosts or complex emulators, configure `CIRQUE_TEST_TIMEOUT` (seconds):
```bash
CIRQUE_TEST_TIMEOUT=1200 scripts/tests/cirque_tests.sh run_test AndroidBleThreadMobileDeviceTest
```

### 3. Full Suite & Deferred Single-Pass LCOV Coverage
To avoid expensive per-test gcov flushing and capture overhead, Cirque supports deferred single-pass coverage collection:
```bash
CIRQUE_COVERAGE=1 CIRQUE_TESTS_OVERRIDE="AndroidBleWiFiMobileDeviceTest AndroidBleThreadMobileDeviceTest" \
    scripts/tests/cirque_tests.sh run_all_tests
```
**Why Deferred Single-Pass Coverage Works:**
When `CIRQUE_IN_RUN_ALL_TESTS=1` is active, `scripts/tests/cirque_tests.sh` runs all test targets without stopping to run `lcov --capture` after each one. When all tests complete:
1. Docker nodes flush `.gcda` coverage data upon graceful SIGTERM teardown.
2. A single comprehensive `lcov --capture` pass executes via `src/test_driver/linux-cirque/helper/coverage_report.py`.
3. Output artifacts are generated in `$LOG_DIR/aggregated_coverage` and `$LOG_DIR/coverage_report` (HTML, JSON, and summary Markdown).

---

## Specialized Breakthrough Capabilities

### 1. Sleepy LIT ICD Radio-Off & Wake-Up Check-In

Intermittently Connected Devices (LIT ICD) enter deep sleep where physical radios are powered down. In virtual simulation:
- **Radio-Gating Implementation**: `CHIP_CIRQUE_SLEEPY_RADIO_GATE=1` in `examples/lit-icd-app/linux/main.cpp` and `src/test_driver/linux-cirque/IcdDeviceTest.py`.
- **Kernel Packet Dropping**: Linux `iptables`/`ip6tables` chain `CIRQUE_ICD_RADIO_GATE` drops all UDP port 5540 and ICMPv6 traffic when the device transitions to `OnEnterIdleMode`.
- **Synchronous Wake-Up Trigger**: A named FIFO pipe `/tmp/chip_icd_fifo_<pid>` accepts command JSON `{"Name": "UserActiveModeTrigger"}`. The application flushes iptables drop rules and executes `NotifyNetworkActivityNotification()` to resume operational responsiveness.
- **Standalone Netns Testing**: Test radio-off and wake-up mechanics in isolated user network namespaces without Docker using `unshare -Ur -n python3 src/test_driver/linux-cirque/test_sleepy_lit_icd_prototype.py`.

### 2. Decrypted Wire Flow Visualization (`src/test_driver/linux-cirque/mcp/matter_flow_plugin.py`)

The Matter flow visualizer plugin bridges link-layer PCAP events with decrypted application session packets:
- **Session Key Extraction**: `CHIP_SESSION_KEYLOG_FILE` logs PASE and CASE session keys to `matter_session_keys.txt`.
- **Network Provisioning Mapping**:
  - `AddOrUpdateThreadNetwork` / `ConnectNetwork` -> mapped to `THREAD_PROVISIONED`.
  - `AddOrUpdateWiFiNetwork` -> mapped to `WIFI_PROVISIONED`.
- **Interactive UI**: Generates `cirque_flow_visualization.html` featuring:
  - `#filter-btn-DECRYPTED_WIRE` filter button to focus exclusively on decrypted application traffic.
  - Interleaved chronological swimlanes: Controller -> BlueZ/TAP -> Thread/Wi-Fi Medium -> CHIP Accessory Node.
  - Adaptive 150-frame sequence windowing for responsive playback.
