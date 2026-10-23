#!/usr/bin/env bash

#
# Copyright (c) 2020 Project CHIP Authors
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
# Don't warn about unreachable commands in this file:
# shellcheck disable=SC2317
#
# Optional environment variables:
#   CIRQUE_ENABLE_ANDROID_TESTS: Set to 1 (default) to include Android emulator
#     tests (AndroidBleWiFiMobileDeviceTest, AndroidBleWiFiSilabsLightingTest,
#     AndroidBleThreadMobileDeviceTest, AndroidBleWiFiSilabsLitIcdTest) in
#     run_all_tests, or 0 to skip.
#     Requires KVM and cirque-android-runner.

SOURCE=${BASH_SOURCE[0]}
SOURCE_DIR=$(cd "$(dirname "$SOURCE")" >/dev/null 2>&1 && pwd)
REPO_DIR=$(cd "$SOURCE_DIR/../.." && pwd)
TEST_DIR=$REPO_DIR/src/test_driver/linux-cirque
export PYTHONPATH="$REPO_DIR/third_party/cirque/repo${PYTHONPATH:+:$PYTHONPATH}"

LOG_DIR=${LOG_DIR:-$(mktemp -d)}
GITHUB_ACTION_RUN=${GITHUB_ACTION_RUN:-"0"}

# The image build will clone its own ot-br-posix checkout due to limitations of git submodule.
# Using the same ot-br-posix version as chip
OPENTHREAD=$REPO_DIR/third_party/openthread/repo
OPENTHREAD_CHECKOUT=$(cd "$REPO_DIR" && git rev-parse :third_party/openthread/repo)
OT_BR_POSIX_CHECKOUT=$(cd "$REPO_DIR" && git rev-parse :third_party/ot-br-posix/repo)

CIRQUE_CACHE_PATH=${GITHUB_CACHE_PATH:-"/tmp/cirque-cache/"}
OT_SIMULATION_CACHE="$CIRQUE_CACHE_PATH/ot-simulation-cmake.tgz"
OT_SIMULATION_CACHE_STAMP_FILE="$CIRQUE_CACHE_PATH/ot-simulation.commit"

# Append test name here to add more tests for run_all_tests
#
# NOTE:
#   "InteractionModelTest" is currently disabled due to it overriding
#   internal data model methods (for example it says "CommandExists" for
#   paths where endpoint/cluster do not)
CIRQUE_TESTS=(
    "EchoTest"
    "EchoOverTcpTest"
    "FailsafeTest"
    "MobileDeviceTest"
    "CommissioningTest"
    "IcdDeviceTest"
    "SplitCommissioningTest"
    "CommissioningFailureTest"
    "CommissioningFailureOnReportTest"
    "PythonCommissioningTest"
    "CommissioningWindowTest"
    "SubscriptionResumptionTest"
    "SubscriptionResumptionCapacityTest"
    "SubscriptionResumptionTimeoutTest"
    "BleMobileDeviceTest"
    "BleWiFiMobileDeviceTest"
)

CIRQUE_ANDROID_TESTS=(
    "AndroidBleWiFiMobileDeviceTest"
    "AndroidBleWiFiSilabsLightingTest"
    "AndroidBleThreadMobileDeviceTest"
    "AndroidBleWiFiSilabsLitIcdTest"
)

BOLD_GREEN_TEXT="\033[1;32m"
BOLD_YELLOW_TEXT="\033[1;33m"
BOLD_RED_TEXT="\033[1;31m"
RESET_COLOR="\033[0m"

function __cirquetest_cleanup_stale_cirque_resources() {
    local self_id
    self_id=$(hostname 2>/dev/null || true)
    local self_name
    self_name=$(docker inspect "$self_id" --format '{{.Name}}' 2>/dev/null |
        tr -d '/' || true)
    local protected_re='^('
    local protected_custom="${CIRQUE_PROTECTED_CONTAINERS:-}"
    if [[ -n "$protected_custom" ]]; then
        # Convert comma/space/pipe-separated container names into regex alternation
        local sanitized_custom
        sanitized_custom=$(echo "$protected_custom" | tr ', ' '||' | sed 's/||*/|/g' | sed 's/^|//;s/|$//')
        protected_re+="$sanitized_custom"
    fi
    if [[ -n "$self_name" ]]; then
        if [[ "$protected_re" != "^(" ]]; then
            protected_re+="|"
        fi
        protected_re+="$self_name"
    fi
    if [[ -n "$self_id" ]]; then
        if [[ "$protected_re" != "^(" ]]; then
            protected_re+="|"
        fi
        protected_re+="$self_id"
    fi
    if [[ "$protected_re" == "^(" ]]; then
        protected_re+='__none__'
    fi
    protected_re+=')($|[^a-zA-Z0-9_-])'

    local stale_containers
    stale_containers=$(docker ps -a --format '{{.Names}}\t{{.Image}}' 2>/dev/null |
        grep -E $'\t.*(cirque|wifi_ap|mac80211_ap|otbr|android_emulator|generic_node_image)' |
        cut -f1 |
        grep -Ev "$protected_re" || true)
    if [[ -n "$stale_containers" ]]; then
        echo "$stale_containers" |
            xargs -r docker rm -f >/dev/null 2>&1 || true
    fi
    docker network prune -f >/dev/null 2>&1 || true
    ip link delete cirque_tap0 2>/dev/null || true
}

function __cirquetest_start_flask() {
    echo 'Start Flask'
    pkill -f 'cirque/restservice/service.py|python3 -m flask run' 2>/dev/null || true
    __cirquetest_cleanup_stale_cirque_resources
    cd "$REPO_DIR"/third_party/cirque/repo
    # When running the ManualTests, if Ctrl-C is send to the shell, it will stop flask as well.
    # This is not expected. Start a new session to prevent it from receiving signals
    local cirque_dir="$REPO_DIR/third_party/cirque/repo"
    local matter_ext="$TEST_DIR/mcp/matter_cirque_extension.py"
    setsid bash -c 'FLASK_APP=cirque/restservice/service.py \
        CHIP_CIRQUE_BASE_IMAGE="${CHIP_CIRQUE_BASE_IMAGE:-ghcr.io/project-chip/chip-cirque-device-base}" \
        CIRQUE_PCAP_DIR="'"$DEVICE_LOG_DIR"'" \
        DEVICE_LOG_DIR="'"$DEVICE_LOG_DIR"'" \
        CIRQUE_MCP_EXTENSIONS="'"$matter_ext"'" \
        PYTHONPATH="'"$cirque_dir"':'"$TEST_DIR"'/mcp:'"${PYTHONPATH:-}"'" \
        PATH="'"$PATH"'":"'"$REPO_DIR"'"/third_party/openthread/repo/build/simulation/examples/apps/ncp/ \
        python3 -m flask run >"'"$LOG_DIR"'"/"'"$CURRENT_TEST"'"/flask.log 2>&1' &
    FLASK_PID=$!
    echo "Flask running in backgroud with pid $FLASK_PID"

    # Wait for Flask service to be ready (poll up to 10s for valid JSON response)
    local flask_ready=0
    for _ in $(seq 1 50); do
        local resp
        resp=$(curl -s http://127.0.0.1:5000/get_homes 2>/dev/null || true)
        if [[ "$resp" == "["* ]]; then
            flask_ready=1
            break
        fi
        sleep 0.2
    done
    if [[ "$flask_ready" -ne 1 ]]; then
        echo "Warning: Flask server did not respond with valid JSON to /get_homes within 10 seconds"
    fi
    cd "$REPO_DIR"
}

function __cirquetest_clean_flask() {
    echo "Cleanup Flask pid $FLASK_PID"
    if command -v curl >/dev/null 2>&1; then
        local homes
        homes=$(curl -s http://127.0.0.1:5000/get_homes 2>/dev/null || true)
        if [[ "$homes" == "["* && "$homes" != "[]" ]]; then
            python3 -c "
import json, sys, urllib.request
try:
    for h in json.loads('$homes'):
        urllib.request.urlopen(f'http://127.0.0.1:5000/destroy_home/{h}', timeout=10)
except Exception as e:
    sys.stderr.write(f'destroy_home failed: {e}\n')
" 2>/dev/null || true
        fi
    fi
    kill -SIGTERM -"$FLASK_PID" 2>/dev/null || true
    __cirquetest_cleanup_stale_cirque_resources
    if [[ -f "$LOG_DIR/$CURRENT_TEST/flask.log" ]]; then
        mv "$LOG_DIR/$CURRENT_TEST"/flask.log "$LOG_DIR/$CURRENT_TEST"/flask.log.old
        cat "$LOG_DIR/$CURRENT_TEST"/flask.log.old | sed 's/\\n/\n/g' | sed 's/\\t/ /g' >"$LOG_DIR/$CURRENT_TEST"/flask.log
        rm "$LOG_DIR/$CURRENT_TEST"/flask.log.old
    fi
    cd "$REPO_DIR"
}

function __cirquetest_generate_coverage_report() {
    local out_dir="$1"
    local cov_dir="$2"
    local extra_args=()
    if [[ "${CIRQUE_SKIP_GENHTML:-0}" = "1" ]]; then
        extra_args+=("--skip-genhtml")
    fi
    mkdir -p "$cov_dir"
    cd "$REPO_DIR"
    if ! grep -q 'docker\|containerd' /proc/1/cgroup 2>/dev/null && ! test -f /.dockerenv && [[ -x "$REPO_DIR/integrations/docker/images/stage-2/chip-build-cirque/run.sh" ]] && command -v docker >/dev/null 2>&1; then
        DOCKER_RUN_DIR="$REPO_DIR" \
        "$REPO_DIR/integrations/docker/images/stage-2/chip-build-cirque/run.sh" \
            --volume /tmp:/tmp \
            --volume "$REPO_DIR:$REPO_DIR" \
            -- python3 "$TEST_DIR/helper/coverage_report.py" \
                --out-dir "$out_dir" \
                --output-dir "$cov_dir" \
                --chip-root "$REPO_DIR" \
                "${extra_args[@]}" || echo "Warning: Coverage report generation failed"
    elif command -v lcov >/dev/null 2>&1; then
        python3 "$TEST_DIR/helper/coverage_report.py" \
            --out-dir "$out_dir" \
            --output-dir "$cov_dir" \
            --chip-root "$REPO_DIR" \
            "${extra_args[@]}" || echo "Warning: Coverage report generation failed"
    else
        python3 "$TEST_DIR/helper/coverage_report.py" \
            --out-dir "$out_dir" \
            --output-dir "$cov_dir" \
            --chip-root "$REPO_DIR" \
            "${extra_args[@]}" || echo "Warning: Coverage report generation failed"
    fi
}

function __cirquetest_sanitize_logs_for_ci_upload() {
    local target_dir="${1:-$LOG_DIR}"
    [[ -d "$target_dir" ]] || return 0
    # Keep the uploaded zip (~20 MB, with full aggregated_coverage/html report
    # intact) well under Chrome's 50 MB (52,428,800 bytes) ZipAnalyzer and
    # Enterprise Connector limit so SafeBrowsing never marks the archive as
    # ArchiveAnalysisResult::kTooLarge (INVALID_ZIP).
    find "$target_dir" -type f -name 'lcov_raw.info' -delete 2>/dev/null || true
    rm -rf "$target_dir/coverage_report/html" "$target_dir/coverage_report/lcov.info" 2>/dev/null || true
    while IFS= read -r -d '' f; do
        mv -f "$f" "${f}.txt" 2>/dev/null || true
    done < <(find "$target_dir" -type f \( -iname '*.py' -o -iname '*.sh' \) -print0 2>/dev/null)
}

function __cirquetest_build_ot() {
    echo -e "[$BOLD_YELLOW_TEXT""INFO""$RESET_COLOR] Cache miss, build openthread simulation."
    script/cmake-build simulation -DOT_THREAD_VERSION=1.2 -DOT_MTD=OFF -DOT_FTD=OFF -DWEB_GUI=0 -DNETWORK_MANAGER=0 -DREST_API=0 -DNAT64=0 -DOT_LOG_OUTPUT=PLATFORM_DEFINED -DOT_LOG_LEVEL=DEBG
    mkdir -p "$(dirname "$OT_SIMULATION_CACHE")"
    tar czf "$OT_SIMULATION_CACHE" build
    echo "$OPENTHREAD_CHECKOUT" >"$OT_SIMULATION_CACHE_STAMP_FILE"
}

function __cirquetest_build_ot_lazy() {
    pushd .
    cd "$REPO_DIR"/third_party/openthread/repo
    ([[ -f "$OT_SIMULATION_CACHE_STAMP_FILE" ]] &&
        [[ "$(cat "$OT_SIMULATION_CACHE_STAMP_FILE")" = "$OPENTHREAD_CHECKOUT" ]] &&
        [[ -f "$OT_SIMULATION_CACHE" ]] &&
        tar zxf "$OT_SIMULATION_CACHE") ||
        __cirquetest_build_ot
    popd
}

function __cirquetest_self_hash() {
    shasum "$SOURCE" | awk '{ print $1 }'
}

function cirquetest_cachekey() {
    echo "$("$REPO_DIR"/integrations/docker/images/stage-2/chip-cirque-device-base/cachekey.sh).openthread.$OPENTHREAD_CHECKOUT.cirque_test.$(__cirquetest_self_hash)"
}

function cirquetest_cachekeyhash() {
    cirquetest_cachekey | shasum | awk '{ print $1 }'
}

function cirquetest_bootstrap() {
    set -ex

    cd "$REPO_DIR"/third_party/cirque/repo
    pip3 install --break-system-packages pycodestyle==2.5.0 wheel

    make NO_GRPC=1 install -j

    git config --global --add safe.directory /home/runner/work/connectedhomeip/connectedhomeip

    "$REPO_DIR"/integrations/docker/images/stage-2/chip-cirque-device-base/build.sh --build-arg OT_BR_POSIX_CHECKOUT="$OT_BR_POSIX_CHECKOUT"
    docker tag "$IMAGE:$IMAGE_VERSION" cirque-device-base:latest 2>/dev/null || true
    docker tag "$IMAGE:$IMAGE_VERSION" cirque-virtual-rf-node:latest 2>/dev/null || true
    if docker image inspect project-chip/chip-cirque-device-base:latest >/dev/null 2>&1; then
        docker tag project-chip/chip-cirque-device-base:latest cirque-device-base:latest 2>/dev/null || true
        docker tag project-chip/chip-cirque-device-base:latest cirque-virtual-rf-node:latest 2>/dev/null || true
    fi
    if docker image inspect ghcr.io/project-chip/chip-cirque-device-base:latest >/dev/null 2>&1; then
        docker tag ghcr.io/project-chip/chip-cirque-device-base:latest cirque-device-base:latest 2>/dev/null || true
        docker tag ghcr.io/project-chip/chip-cirque-device-base:latest cirque-virtual-rf-node:latest 2>/dev/null || true
    fi

    local nodes_dir="$REPO_DIR/third_party/cirque/repo/cirque/nodes"
    if [[ -f "$nodes_dir/Dockerfile.android_runner" ]]; then
        docker build -t cirque-android-runner:latest \
            -f "$nodes_dir/Dockerfile.android_runner" "$nodes_dir"
    fi

    __cirquetest_build_ot_lazy
    pip3 install --break-system-packages -r requirements_nogrpc.txt

    echo "OpenThread Version: $OPENTHREAD_CHECKOUT"
    echo "ot-br-posix Version: $OT_BR_POSIX_CHECKOUT"
}

function cirquetest_run_test() {
    # Ensure cirque-device-base:latest and cirque-virtual-rf-node:latest are tagged
    if docker image inspect project-chip/chip-cirque-device-base:latest >/dev/null 2>&1; then
        docker tag project-chip/chip-cirque-device-base:latest cirque-device-base:latest 2>/dev/null || true
        docker tag project-chip/chip-cirque-device-base:latest cirque-virtual-rf-node:latest 2>/dev/null || true
    fi
    if docker image inspect ghcr.io/project-chip/chip-cirque-device-base:latest >/dev/null 2>&1; then
        docker tag ghcr.io/project-chip/chip-cirque-device-base:latest cirque-device-base:latest 2>/dev/null || true
        docker tag ghcr.io/project-chip/chip-cirque-device-base:latest cirque-virtual-rf-node:latest 2>/dev/null || true
    fi

    # Start Cirque flash server
    export CURRENT_TEST="$1"
    export DEVICE_LOG_DIR="$LOG_DIR/$CURRENT_TEST"/device_logs
    export CIRQUE_PCAP_DIR="$DEVICE_LOG_DIR"
    shift
    mkdir -p "$DEVICE_LOG_DIR"
    if [[ "${CIRQUE_COVERAGE:-0}" = "1" && "${CIRQUE_IN_RUN_ALL_TESTS:-0}" != "1" ]]; then
        echo "Zeroing coverage counters for single test $CURRENT_TEST..."
        find "$REPO_DIR/out/debug" -name "*.gcda" -delete 2>/dev/null || true
    fi
    __cirquetest_start_flask
    local test_timeout="${CIRQUE_TEST_TIMEOUT:-900}"
    PYTHONPATH="$REPO_DIR/third_party/cirque/repo:${PYTHONPATH:-}" \
        CHIP_CIRQUE_BASE_IMAGE="ghcr.io/project-chip/chip-cirque-device-base" \
        timeout --kill-after=30s "${test_timeout}s" \
        "$TEST_DIR/$CURRENT_TEST.py" "$@"
    exitcode=$?
    if [[ $exitcode -eq 124 || $exitcode -eq 137 ]]; then
        echo -e "[$BOLD_RED_TEXT""TIMEOUT""$RESET_COLOR]: $CURRENT_TEST timed out after ${test_timeout}s" >&2
    fi
    if [[ $exitcode -ne 0 ]]; then
        echo "=== Flask server log for failed/timed out test $CURRENT_TEST ==="
        tail -n 60 "$LOG_DIR/$CURRENT_TEST/flask.log" 2>/dev/null || true
        echo "=== End Flask log ==="
    fi
    __cirquetest_clean_flask
    # Generate interactive cross-layer flow visualizer HTML & JSON trace
    if PYTHONPATH="$REPO_DIR/third_party/cirque/repo:${PYTHONPATH:-}" \
        python3 -m cirque.pcap.flow_visualizer --pcap-dir "$DEVICE_LOG_DIR" \
        --output "$DEVICE_LOG_DIR/cirque_flow_visualization.html" \
        --json-output "$DEVICE_LOG_DIR/cirque_flow_trace.json" \
        --plugin "$TEST_DIR/mcp/matter_flow_plugin.py" \
        --require-non-empty >/dev/null; then
        echo "Flow visualizer generated at $DEVICE_LOG_DIR/cirque_flow_visualization.html"
    else
        echo "Flow visualizer FAILED (plugin: $TEST_DIR/mcp/matter_flow_plugin.py); see stderr above" >&2
    fi
    # Files are written by root inside the privileged CI container; keep them
    # readable for actions/upload-artifact and record what is on disk (with
    # checksums) so a file missing from the artifact can be told apart from a
    # file that was never written.
    chmod -R a+rX "$LOG_DIR/$CURRENT_TEST" 2>/dev/null || true
    (cd "$DEVICE_LOG_DIR" && ls -la && sha256sum -- *.pcap thread_network_key.txt ieee802154_keys matter_session_keys.txt 2>/dev/null) \
        >"$DEVICE_LOG_DIR/artifact_manifest.txt" || true
    # TODO: Do docker system prune, we cannot filter which container
    # is created by cirque now. This will be implemented later. Currently, only do this on CI

    # After test finished, cleanup was already run by __cirquetest_clean_flask.
    if [[ "$CLEANUP_DOCKER_FOR_CI" = "1" ]]; then
        echo "Do docker container and network prune"
    fi

    # When CIRQUE_IN_RUN_ALL_TESTS is set, coverage capture is deferred to the end of the suite
    if [[ "${CIRQUE_COVERAGE:-0}" = "1" && "${CIRQUE_IN_RUN_ALL_TESTS:-0}" != "1" ]]; then
        local out_dir="$REPO_DIR/out/debug"
        local cov_dir="$LOG_DIR/$CURRENT_TEST/coverage"
        echo "Generating coverage report for $CURRENT_TEST at $cov_dir..."
        # Fix permissions on root-owned .gcda files written by docker containers
        find "$out_dir" \( -name "*.gcda" -o -name "*.profraw" \) -exec chown "$(id -u):$(id -g)" {} + 2>/dev/null || true
        chmod -R a+rX "$out_dir" 2>/dev/null || true
        __cirquetest_generate_coverage_report "$out_dir" "$cov_dir"
        if [[ -f "$cov_dir/coverage_summary.md" ]]; then
            cat "$cov_dir/coverage_summary.md"
        fi
    fi
    echo "Test log can be found at $DEVICE_LOG_DIR"
    return "$exitcode"
}

function cirquetest_coverage_report() {
    local out_dir="${1:-"$REPO_DIR/out/debug"}"
    local cov_dir="${2:-"$REPO_DIR/out/coverage"}"
    echo "Generating coverage report from $out_dir to $cov_dir..."
    find "$out_dir" \( -name "*.gcda" -o -name "*.profraw" \) -exec chown "$(id -u):$(id -g)" {} + 2>/dev/null || true
    chmod -R a+rX "$out_dir" 2>/dev/null || true
    __cirquetest_generate_coverage_report "$out_dir" "$cov_dir"
    if [[ -f "$cov_dir/coverage_summary.md" ]]; then
        cat "$cov_dir/coverage_summary.md"
    fi
}

function prewarm_cirque_device_base_wheels() {
    local base_image="${CHIP_CIRQUE_BASE_IMAGE:-"ghcr.io/project-chip/chip-cirque-device-base"}"
    local wheel_dir="$REPO_DIR/out/debug/linux_x64_gcc/obj/src/controller/python/matter-controller-wheels"
    if [[ ! -d "$wheel_dir" ]]; then
        wheel_dir="$REPO_DIR/out/debug/linux_x64_gcc/controller/python"
    fi
    if [[ -d "$wheel_dir" ]] && command -v docker >/dev/null 2>&1; then
        if timeout 30s docker run --rm --entrypoint /bin/bash "$base_image:latest" -c \
            "python3 -c 'import matter.ChipDeviceCtrl, matter.clusters'" >/dev/null 2>&1; then
            docker tag "$base_image:latest" cirque-device-base:latest 2>/dev/null || true
            docker tag "$base_image:latest" cirque-virtual-rf-node:latest 2>/dev/null || true
            echo "Base image $base_image:latest already has Matter Python controller wheels installed."
            return 0
        fi
        echo "Pre-warming $base_image:latest with matter_clusters and matter_core wheels..."
        local prewarm_container="cirque_wheel_prewarm_$$"
        docker rm -f "$prewarm_container" >/dev/null 2>&1 || true
        if timeout 180s docker run --name "$prewarm_container" --entrypoint /bin/bash \
            -v "$REPO_DIR:$REPO_DIR" "$base_image:latest" -c \
            "pip3 install --break-system-packages --no-cache-dir --find-links '$wheel_dir' matter_clusters matter_core && python3 -c 'import matter.ChipDeviceCtrl, matter.clusters'" \
            && docker commit --change 'ENTRYPOINT ["/opt/entrypoint.sh"]' --change 'CMD []' "$prewarm_container" "$base_image:latest" >/dev/null; then
            docker tag "$base_image:latest" cirque-device-base:latest 2>/dev/null || true
            docker tag "$base_image:latest" cirque-virtual-rf-node:latest 2>/dev/null || true
            echo "Pre-warming completed."
        else
            echo "Warning: Pre-warming failed or timed out; tests will install wheels at runtime."
        fi
        docker rm -f "$prewarm_container" >/dev/null 2>&1 || true
    fi
}

function cirquetest_run_all_tests() {
    # Ensure cirque-device-base:latest and cirque-virtual-rf-node:latest are tagged
    if docker image inspect project-chip/chip-cirque-device-base:latest >/dev/null 2>&1; then
        docker tag project-chip/chip-cirque-device-base:latest cirque-device-base:latest 2>/dev/null || true
        docker tag project-chip/chip-cirque-device-base:latest cirque-virtual-rf-node:latest 2>/dev/null || true
    fi
    if docker image inspect ghcr.io/project-chip/chip-cirque-device-base:latest >/dev/null 2>&1; then
        docker tag ghcr.io/project-chip/chip-cirque-device-base:latest cirque-device-base:latest 2>/dev/null || true
        docker tag ghcr.io/project-chip/chip-cirque-device-base:latest cirque-virtual-rf-node:latest 2>/dev/null || true
    fi

    prewarm_cirque_device_base_wheels

    # shellharden requires quotes around variables, which will break for-each loops
    # This is the workaround
    echo "Logs will be stored at $LOG_DIR"
    test_pass=1
    mkdir -p "$LOG_DIR"
    local tests_to_run=("${CIRQUE_TESTS[@]}")
    if [[ -n "${CIRQUE_TESTS_OVERRIDE:-}" ]]; then
        read -r -a tests_to_run <<< "$CIRQUE_TESTS_OVERRIDE"
    elif [[ "${CIRQUE_ENABLE_ANDROID_TESTS:-1}" = "1" ]]; then
        tests_to_run+=("${CIRQUE_ANDROID_TESTS[@]}")
    fi
    if [[ "${CIRQUE_COVERAGE:-0}" = "1" ]]; then
        echo "Zeroing coverage counters for aggregated test suite..."
        find "$REPO_DIR/out/debug" \( -name "*.gcda" -o -name "*.profraw" \) -delete 2>/dev/null || true
    fi
    export CIRQUE_IN_RUN_ALL_TESTS=1
    local test_idx=0
    for test_name in "${tests_to_run[@]}"; do
        ((test_idx++)) || true
        echo "[ RUN] $test_name"
        local test_log="$LOG_DIR/${test_idx}_${test_name}.log"
        if cirquetest_run_test "$test_name" >"$test_log" 2>&1; then
            echo -e "[$BOLD_GREEN_TEXT""PASS""$RESET_COLOR] $test_name"
        else
            echo -e "[$BOLD_RED_TEXT""FAIL""$RESET_COLOR] $test_name (Exitcode: $exitcode)"
            test_pass=0
        fi
    done
    unset CIRQUE_IN_RUN_ALL_TESTS

    if [[ "${CIRQUE_COVERAGE:-0}" = "1" ]]; then
        echo -e "[$BOLD_YELLOW_TEXT""INFO""$RESET_COLOR] Generating aggregated Cirque coverage report..."
        cirquetest_coverage_report "$REPO_DIR/out/debug" "$LOG_DIR/aggregated_coverage" || true
        if [[ -d "$LOG_DIR/aggregated_coverage" ]]; then
            mkdir -p "$LOG_DIR/coverage_report"
            cp -f "$LOG_DIR/aggregated_coverage/coverage_summary."* "$LOG_DIR/coverage_report/" 2>/dev/null || true
            if [[ "${GITHUB_ACTION_RUN:-0}" != "1" ]]; then
                cp -rf "$LOG_DIR/aggregated_coverage/"* "$LOG_DIR/coverage_report/" 2>/dev/null || true
            fi
        fi
    fi

    if [[ "$GITHUB_ACTION_RUN" = "1" ]]; then
        echo -e "[$BOLD_YELLOW_TEXT""INFO""$RESET_COLOR] Sanitizing logs for artifact upload..."
        __cirquetest_sanitize_logs_for_ci_upload "$LOG_DIR"
    fi

    if ((test_pass)); then
        echo -e "[$BOLD_GREEN_TEXT""PASS""$RESET_COLOR] Test finished, test log can be found at $LOG_DIR"
        return 0
    else
        echo -e "[$BOLD_RED_TEXT""FAIL""$RESET_COLOR] Test failed, test log can be found at $LOG_DIR"
        return 1
    fi
}

subcommand=$1
shift

case $subcommand in
    *)
        cirquetest_"$subcommand" "$@"
        exitcode=$?
        if ((exitcode == 127)); then
            echo "Unknown command: $subcommand" >&2
        fi
        exit "$exitcode"
        ;;
esac
