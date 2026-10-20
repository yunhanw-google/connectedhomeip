#!/usr/bin/env bash

#
# Copyright (c) 2022 Project CHIP Authors
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

set -e

CHIP_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ -z "${CHIP_ROOT_PATH}" ]]; then
    CHIP_ROOT_PATH=""
fi

set -x

env

cd "${ROOT_PATH:-$CHIP_ROOT}"

echo "Ensure submodules for Linux builds are checked out"
if [ "${GITHUB_ACTION_RUN:-0}" != "1" ]; then
    ./scripts/checkout_submodules.py --allow-changing-global-git-config --shallow --platform linux
fi

echo "Setup build environment"
source "./scripts/activate.sh"

gn_args='target_os="all" chip_logging_backend="stdio" chip_build_tests=false chip_enable_wifi=false chip_enable_ble=true chip_config_network_layer_ble=true chip_im_force_fabric_quota_check=true enable_default_builds=false enable_host_gcc_build=true enable_standalone_chip_tool_build=false enable_linux_lit_icd_app_build=true chip_device_config_enable_joint_fabric=true'

ALL_CLUSTERS_ARGS=(
    chip_inet_config_enable_ipv4=false
    'chip_logging_backend="stdio"'
    'chip_enable_wifi=true'
    'chip_enable_ble=true'
    'chip_config_network_layer_ble=true'
    'chip_build_tests=false'
    'chip_im_force_fabric_quota_check=true'
    'chip_support_thread_meshcop=false'
    'chip_enable_session_keylog=true'
)

if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$CHIP_ROOT/.ccache}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    export CCACHE_COMPILERCHECK="content"
    export CCACHE_SLOPPINESS="include_file_mtime,time_macros"
    mkdir -p "$CCACHE_DIR"
    gn_args+=" pw_command_launcher=\"ccache\""
    ALL_CLUSTERS_ARGS+=('pw_command_launcher="ccache"')
fi

if [ "${CIRQUE_COVERAGE:-0}" = "1" ]; then
    echo "Enabling code coverage instrumentation (use_coverage=true)"
    gn_args+=" use_coverage=true"
    ALL_CLUSTERS_ARGS+=('use_coverage=true')
fi

echo "Build: GN configure (out/debug and out/debug/standalone in parallel)"
mkdir -p out/debug out/debug/standalone
gn --root="$CHIP_ROOT" gen --check --fail-on-unused-args out/debug --args="$gn_args" &
gn_debug_pid=$!
# chip_enable_session_keylog is TEST-ONLY: the Android Cirque tests set
# CHIP_SESSION_KEYLOG_FILE so the flow visualizer can decrypt the captured
# PASE/CASE traffic. Nothing is logged unless that variable is set.
gn --root="$CHIP_ROOT/examples/all-clusters-app/linux" gen --check --fail-on-unused-args out/debug/standalone --args="${ALL_CLUSTERS_ARGS[*]}" &
gn_standalone_pid=$!
wait "$gn_debug_pid"
wait "$gn_standalone_pid"

echo "Build: Ninja build (only the targets needed for Cirque tests)"
time ninja -C out/debug \
    linux_x64_gcc/chip-echo-requester \
    linux_x64_gcc/chip-echo-responder \
    linux_x64_gcc/chip-im-initiator \
    linux_x64_gcc/chip-im-responder \
    linux_x64_gcc/gen/src/controller/python/matter-controller-wheels.pw_pystamp \
    linux_lit_icd_app

echo "Build: Build all-clusters-app which has different configs than some other samples above."
time ninja -C out/debug/standalone

ARM_SYSROOT="$CHIP_ROOT/.environment/cipd/packages/pigweed/clang_sysroot"
if [ -d "$ARM_SYSROOT" ]; then
    echo "Build: Cross-compile ARM32 Silicon Labs Lighting-App (out/arm-silabs/chip-silabs-lighting-app)"
    ARM_SILABS_ARGS=(
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
    )
    if command -v ccache >/dev/null 2>&1; then
        ARM_SILABS_ARGS+=('pw_command_launcher="ccache"')
    fi
    mkdir -p out/arm-silabs out/host_libs
    gn --root="$CHIP_ROOT/examples/all-clusters-app/linux" gen out/arm-silabs --args="${ARM_SILABS_ARGS[*]}"
    time ninja -C out/arm-silabs chip-silabs-lighting-app
    cp -f out/arm-silabs/chip-silabs-lighting-app out/debug/standalone/chip-silabs-lighting-app
    if [ -f "$CHIP_ROOT/third_party/cirque/repo/cirque/virtual_bt/android/pty_bridge.c" ]; then
        gcc -O2 "$CHIP_ROOT/third_party/cirque/repo/cirque/virtual_bt/android/pty_bridge.c" -o out/host_libs/pty_bridge_linux -lutil
        chmod 755 out/host_libs/pty_bridge_linux
    fi
fi

if command -v ccache >/dev/null 2>&1; then
    ccache --show-stats || true
fi

