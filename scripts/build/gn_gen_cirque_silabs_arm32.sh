#!/usr/bin/env bash

#
# Copyright (c) 2026 Project CHIP Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Builds the ARM32 Silabs artifacts the Cirque Silabs E2E tests need,
# separately from the x86_64 Cirque binaries (like the Android CHIPTool APK):
#   out/arm-silabs/chip-silabs-lighting-app        AndroidBleWiFiSilabsLightingTest
#   out/arm-silabs-thread/chip-silabs-lit-icd-app  AndroidBleThreadSilabsLitIcdTest
#   out/host_libs/pty_bridge_linux                 x86_64 PTY <-> TCP HCI H4 bridge
#   out/host_libs/qemu-arm                         only if QEMU_ARM_STATIC is set
# The ARM binaries are 32-bit ARM (armv7 hard-float) ELFs.
#
# SILABS_ARM32_APPS: space-separated subset of "lighting lit-icd" to build
# (default: both). CI builds each app in its own parallel job.
#
# QEMU_ARM_STATIC: optional path to a statically linked qemu-arm user-mode
# emulator (e.g. /usr/bin/qemu-arm-static from qemu-user-static). It is copied
# into CHIPEndDevice containers, so it must not depend on host shared libs.

set -e

CHIP_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "${ROOT_PATH:-$CHIP_ROOT}"

SILABS_ARM32_APPS="${SILABS_ARM32_APPS:-lighting lit-icd}"
for app in $SILABS_ARM32_APPS; do
    case "$app" in
        lighting | lit-icd) ;;
        *)
            echo "ERROR: unknown app '$app' in SILABS_ARM32_APPS (expected: lighting lit-icd)" >&2
            exit 1
            ;;
    esac
done

source "./scripts/activate.sh"

ARM_SYSROOT="$CHIP_ROOT/.environment/cipd/packages/pigweed/clang_sysroot"
if [ ! -f "$ARM_SYSROOT/lib/ld-linux-armhf.so.3" ]; then
    echo "ERROR: armhf sysroot not found at $ARM_SYSROOT (run scripts/bootstrap.sh)" >&2
    exit 1
fi

CCACHE_ARGS=()
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$CHIP_ROOT/.ccache}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"
    export CCACHE_COMPILERCHECK="content"
    export CCACHE_SLOPPINESS="include_file_mtime,time_macros"
    mkdir -p "$CCACHE_DIR"
    CCACHE_ARGS=('pw_command_launcher="ccache"')
fi

# ELF header check without depending on file(1): EI_CLASS (byte 4) must be 1
# (ELFCLASS32) and e_machine (bytes 18-19, little endian) must be 0x0028 (EM_ARM).
check_arm32_elf() {
    local elf_hdr
    elf_hdr="$(od -An -tx1 -N20 "$1" | tr -d ' \n')"
    if [ "${elf_hdr:0:8}" != "7f454c46" ] || [ "${elf_hdr:8:2}" != "01" ] || [ "${elf_hdr:36:4}" != "2800" ]; then
        echo "ERROR: $1 is not a 32-bit ARM ELF (header $elf_hdr)" >&2
        exit 1
    fi
}

build_lighting() {
    local args=(
        'target_cpu="arm"'
        'target_os="linux"'
        'is_clang=true'
        'chip_crypto="mbedtls"'
        'chip_mdns="minimal"'
        'chip_logging_backend="stdio"'
        'chip_enable_wifi=true'
        'chip_enable_ble=true'
        'chip_config_network_layer_ble=true'
        'chip_enable_thread=false'
        'chip_linux_use_silabs_ble_wifi=true'
        'chip_inet_config_enable_ipv4=false'
        'chip_enable_session_keylog=true'
        'chip_im_force_fabric_quota_check=true'
        'chip_support_thread_meshcop=false'
        'chip_build_tests=false'
        "sysroot=\"$ARM_SYSROOT\""
        "${CCACHE_ARGS[@]}"
    )
    if [ "${CIRQUE_COVERAGE:-0}" == "1" ]; then
        args+=('use_coverage=true')
    fi

    echo "Build: Cross-compile ARM32 Silicon Labs Lighting-App (out/arm-silabs/chip-silabs-lighting-app)"
    mkdir -p out/arm-silabs
    gn --root="$CHIP_ROOT/examples/all-clusters-app/linux" gen out/arm-silabs --args="${args[*]}"
    time ninja -C out/arm-silabs chip-silabs-lighting-app
    check_arm32_elf out/arm-silabs/chip-silabs-lighting-app
}

build_lit_icd() {
    local args=(
        'target_cpu="arm"'
        'target_os="linux"'
        'is_clang=true'
        'chip_crypto="mbedtls"'
        'chip_mdns="platform"'
        'chip_logging_backend="stdio"'
        'chip_enable_wifi=false'
        'chip_enable_ble=true'
        'chip_config_network_layer_ble=true'
        'chip_enable_thread=true'
        'chip_system_config_use_openthread_inet_endpoints=true'
        'openthread_config_dns_client_enable=true'
        'openthread_external_mbedtls="//third_party/connectedhomeip/third_party/mbedtls:mbedtls"'
        # all-clusters-app/linux/args.gni turns the MeshCoP Joiner on, which
        # needs EC-JPAKE in mbedtls; CHIP's mbedtls config leaves it off. The
        # device is commissioned over BLE, so drop the Joiner and CHIP's
        # Thread MeshCoP rendezvous (which calls the Joiner's otSeeker* API).
        'openthread_config_joiner_enable=false'
        'chip_device_config_enable_thread_meshcop=false'
        'chip_enable_icd_server=true'
        'chip_enable_icd_lit=true'
        'chip_enable_icd_dsls=true'
        'chip_linux_use_silabs_ble_wifi=true'
        'chip_inet_config_enable_ipv4=false'
        'chip_enable_session_keylog=true'
        'chip_im_force_fabric_quota_check=true'
        'chip_build_tests=false'
        'target_cflags=["-DCONFIG_BUILD_FOR_HOST_UNIT_TEST=0","-DCHIP_CONFIG_ICD_OBSERVERS_POOL_SIZE=5","-DCHIP_CONFIG_ICD_IDLE_MODE_DURATION_SEC=5","-DCHIP_CONFIG_ICD_SHORT_IDLE_MODE_DURATION_SEC=5","-DCHIP_CONFIG_ICD_ACTIVE_MODE_DURATION_MS=1000","-DCHIP_CONFIG_ICD_ACTIVE_MODE_THRESHOLD_MS=5000"]'
        "sysroot=\"$ARM_SYSROOT\""
        "${CCACHE_ARGS[@]}"
    )
    if [ "${CIRQUE_COVERAGE:-0}" == "1" ]; then
        args+=('use_coverage=true')
    fi

    echo "Build: Cross-compile ARM32 Silicon Labs LIT ICD App (out/arm-silabs-thread/chip-silabs-lit-icd-app)"
    mkdir -p out/arm-silabs-thread
    gn --root="$CHIP_ROOT/examples/all-clusters-app/linux" gen out/arm-silabs-thread --args="${args[*]}"
    time ninja -C out/arm-silabs-thread chip-silabs-lit-icd-app
    check_arm32_elf out/arm-silabs-thread/chip-silabs-lit-icd-app
}

for app in $SILABS_ARM32_APPS; do
    case "$app" in
        lighting) build_lighting ;;
        lit-icd) build_lit_icd ;;
    esac
done

echo "Build: pty_bridge_linux (host PTY <-> Cirque VirtualBluetoothServer TCP)"
mkdir -p out/host_libs
gcc -O2 "$CHIP_ROOT/third_party/cirque/repo/cirque/virtual_bt/android/pty_bridge.c" \
    -o out/host_libs/pty_bridge_linux -lutil
chmod 755 out/host_libs/pty_bridge_linux

if [ -n "${QEMU_ARM_STATIC:-}" ]; then
    if ! file -L "$QEMU_ARM_STATIC" | grep -q "static"; then
        echo "ERROR: $QEMU_ARM_STATIC is not statically linked" >&2
        exit 1
    fi
    cp -fL "$QEMU_ARM_STATIC" out/host_libs/qemu-arm
    chmod 755 out/host_libs/qemu-arm
fi

if command -v ccache >/dev/null 2>&1; then
    ccache --show-stats || true
fi
