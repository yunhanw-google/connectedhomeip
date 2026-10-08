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
#     tests (AndroidBleWiFiMobileDeviceTest, AndroidBleThreadMobileDeviceTest)
#     in run_all_tests, or 0 to skip. Requires KVM and cirque-android-runner.

SOURCE=${BASH_SOURCE[0]}
SOURCE_DIR=$(cd "$(dirname "$SOURCE")" >/dev/null 2>&1 && pwd)
REPO_DIR=$SOURCE_DIR/../../
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
    "AndroidBleThreadMobileDeviceTest"
)

BOLD_GREEN_TEXT="\033[1;32m"
BOLD_YELLOW_TEXT="\033[1;33m"
BOLD_RED_TEXT="\033[1;31m"
RESET_COLOR="\033[0m"

function __cirquetest_start_flask() {
    echo 'Start Flask'
    if ! grep docker.sock /proc/1/mountinfo 2>/dev/null; then
        docker ps -aq | xargs -r docker stop -t 0 >/dev/null 2>&1 || true
    fi
    cd "$REPO_DIR"/third_party/cirque/repo
    # When running the ManualTests, if Ctrl-C is send to the shell, it will stop flask as well.
    # This is not expected. Start a new session to prevent it from receiving signals
    local cirque_dir="$REPO_DIR/third_party/cirque/repo"
    setsid bash -c 'FLASK_APP=cirque/restservice/service.py \
        CHIP_CIRQUE_BASE_IMAGE="${CHIP_CIRQUE_BASE_IMAGE:-ghcr.io/project-chip/chip-cirque-device-base}" \
        PYTHONPATH="'"$cirque_dir"':'"${PYTHONPATH:-}"'" \
        PATH="'"$PATH"'":"'"$REPO_DIR"'"/third_party/openthread/repo/build/simulation/examples/apps/ncp/ \
        python3 -m flask run >"'"$LOG_DIR"'"/"'"$CURRENT_TEST"'"/flask.log 2>&1' &
    FLASK_PID=$!
    echo "Flask running in backgroud with pid $FLASK_PID"

    # Wait for Flask service to be ready (poll up to 10s instead of hard sleep)
    local flask_ready=0
    for _ in $(seq 1 50); do
        if curl -s -f http://127.0.0.1:5000/get_homes >/dev/null 2>&1; then
            flask_ready=1
            break
        fi
        sleep 0.2
    done
    if [[ "$flask_ready" -ne 1 ]]; then
        echo "Warning: Flask server did not respond to /get_homes within 10 seconds"
    fi
}

function __cirquetest_clean_flask() {
    echo "Cleanup Flask pid $FLASK_PID"
    kill -SIGTERM -"$FLASK_PID"
    if ! grep docker.sock /proc/1/mountinfo 2>/dev/null; then
        docker ps -aq | xargs -r docker stop -t 0 >/dev/null 2>&1 || true
    fi
    mv "$LOG_DIR/$CURRENT_TEST"/flask.log "$LOG_DIR/$CURRENT_TEST"/flask.log.old
    cat "$LOG_DIR/$CURRENT_TEST"/flask.log.old | sed 's/\\n/\n/g' | sed 's/\\t/ /g' >"$LOG_DIR/$CURRENT_TEST"/flask.log
    rm "$LOG_DIR/$CURRENT_TEST"/flask.log.old
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
    shift
    mkdir -p "$DEVICE_LOG_DIR"
    __cirquetest_start_flask
    PYTHONPATH="$REPO_DIR/third_party/cirque/repo:${PYTHONPATH:-}" \
        CHIP_CIRQUE_BASE_IMAGE="ghcr.io/project-chip/chip-cirque-device-base" \
        "$TEST_DIR/$CURRENT_TEST.py" "$@"
    exitcode=$?
    __cirquetest_clean_flask
    # TODO: Do docker system prune, we cannot filter which container
    # is created by cirque now. This will be implemented later. Currently, only do this on CI

    # After test finished, the container is perserved and networks will not be deleted
    # This is useful when running tests on local workstation, but not for CI.
    if [[ "$CLEANUP_DOCKER_FOR_CI" = "1" ]]; then
        echo "Do docker container and network prune"
        # TODO: Filter cirque containers ?
        if ! grep docker.sock /proc/1/mountinfo; then
            docker ps -aq | xargs -r docker stop -t 0 >/dev/null 2>&1
        fi
        docker container prune -f >/dev/null 2>&1
        docker network prune -f >/dev/null 2>&1
    fi
    echo "Test log can be found at $DEVICE_LOG_DIR"
    return "$exitcode"
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
    if [[ "${CIRQUE_ENABLE_ANDROID_TESTS:-1}" = "1" ]]; then
        tests_to_run+=("${CIRQUE_ANDROID_TESTS[@]}")
    fi
    for test_name in "${tests_to_run[@]}"; do
        echo "[ RUN] $test_name"
        if cirquetest_run_test "$test_name" >"$LOG_DIR/$test_name.log" 2>&1; then
            echo -e "[$BOLD_GREEN_TEXT""PASS""$RESET_COLOR] $test_name"
        else
            echo -e "[$BOLD_RED_TEXT""FAIL""$RESET_COLOR] $test_name (Exitcode: $exitcode)"
            test_pass=0
        fi
    done

    if [[ "$GITHUB_ACTION_RUN" = "1" ]]; then
        echo -e "[$BOLD_YELLOW_TEXT""INFO""$RESET_COLOR] Logs will be uploaded to artifacts."
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
