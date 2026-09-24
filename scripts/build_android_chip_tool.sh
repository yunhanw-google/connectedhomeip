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

set -e

REPO_DIR="${CHIP_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$REPO_DIR"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/29.0.13846066}"
export TARGET_CPU="${TARGET_CPU:-arm64}"
export PW_PROJECT_ROOT="$REPO_DIR"
export ZAP_INSTALL_PATH="${ZAP_INSTALL_PATH:-$REPO_DIR/.environment/cipd/packages/zap}"
export PATH="/usr/lib/kotlinc/bin:$JAVA_HOME/bin:$ZAP_INSTALL_PATH:$REPO_DIR/.environment/cipd/packages/pigweed:$REPO_DIR/.environment/pigweed-venv/bin:$PATH"

OUT_DIR="${OUT_DIR:-$REPO_DIR/out/android_$TARGET_CPU}"

if [ ! -f "$REPO_DIR/.environment/activate.sh" ]; then
    echo "=== 1. Checking Out Android Submodules & Bootstrapping ==="
    python3 scripts/checkout_submodules.py --allow-changing-global-git-config --platform android --recursive
    PIP_INDEX_URL=https://pypi.org/simple bash -c "source scripts/bootstrap.sh"
fi

if [ -x "$REPO_DIR/.environment/cipd/cipd" ] && [ -f "$REPO_DIR/scripts/setup/zap.json" ]; then
    ZAP_TAG=$(python3 -c 'import json; print(json.load(open("scripts/setup/zap.json"))["packages"][0]["tags"][0])' 2>/dev/null || true)
    if [ -n "$ZAP_TAG" ]; then
        "$REPO_DIR/.environment/cipd/cipd" ensure -ensure-file <(printf '$VerifiedPlatform linux-amd64\n@Subdir packages/zap\nexperimental/matter/zap/${platform} %s\n' "$ZAP_TAG") -root "$REPO_DIR/.environment/cipd" >/dev/null 2>&1 || true
    fi
fi

if [ -x "$REPO_DIR/.environment/pigweed-venv/bin/python" ]; then
    "$REPO_DIR/.environment/pigweed-venv/bin/python" -m pip install --index-url https://pypi.org/simple "setuptools<70" toml click 2>/dev/null || true
fi

echo "=== 2. Running Android IDE Setup (GN/CMake) ==="
"$REPO_DIR/scripts/run_in_build_env.sh" \
    "export JAVA_HOME=$JAVA_HOME; export ZAP_INSTALL_PATH=$ZAP_INSTALL_PATH; export PATH=/usr/lib/kotlinc/bin:$JAVA_HOME/bin:$ZAP_INSTALL_PATH:\$PATH; export ANDROID_HOME=$ANDROID_HOME; export ANDROID_NDK_HOME=$ANDROID_NDK_HOME; export TARGET_CPU=$TARGET_CPU; export PW_PROJECT_ROOT=$PW_PROJECT_ROOT; ./scripts/examples/android_app_ide.sh"

echo "=== 3. Compiling Native C++ JNI & Java/Kotlin Targets ==="
"$REPO_DIR/scripts/run_in_build_env.sh" \
    "export JAVA_HOME=$JAVA_HOME; export ZAP_INSTALL_PATH=$ZAP_INSTALL_PATH; export PATH=/usr/lib/kotlinc/bin:$JAVA_HOME/bin:$ZAP_INSTALL_PATH:\$PATH; ninja -C $OUT_DIR src/controller/java:android src/controller/java:java src/controller/java:jsontlv src/controller/java:kotlin_matter_controller src/controller/java:onboarding_payload src/platform/android:java src/app/server/java:java"

echo "=== 4. Packaging JNI Libraries & Matter SDK Jar ==="
mkdir -p "$REPO_DIR/examples/android/CHIPTool/app/libs/jniLibs/arm64-v8a"

jar cf "$REPO_DIR/examples/android/CHIPTool/app/libs/chip-sdk.jar" \
    -C "$OUT_DIR/obj/src/app/server/java/java/classes" . \
    -C "$OUT_DIR/obj/src/controller/java/android_chip_im/classes" . \
    -C "$OUT_DIR/obj/src/controller/java/chipcluster/classes" . \
    -C "$OUT_DIR/obj/src/controller/java/chipclusterID/classes" . \
    -C "$OUT_DIR/obj/src/controller/java/java/classes" . \
    -C "$OUT_DIR/obj/src/controller/java/jsontlv/classes" . \
    -C "$OUT_DIR/obj/src/controller/java/kotlin_matter_controller/classes" . \
    -C "$OUT_DIR/obj/src/controller/java/onboarding_payload/classes" . \
    -C "$OUT_DIR/obj/src/controller/java/tlv/classes" . \
    -C "$OUT_DIR/obj/src/platform/android/java/classes" .

cp "$OUT_DIR/lib/jni/arm64-v8a/"*.so "$REPO_DIR/examples/android/CHIPTool/app/libs/jniLibs/arm64-v8a/"

rm -f "$REPO_DIR/examples/android/CHIPTool/app/libs/CHIPClusterID.jar" \
    "$REPO_DIR/examples/android/CHIPTool/app/libs/libMatterTlv.jar" \
    "$REPO_DIR/examples/android/CHIPTool/app/libs/"*android.jar

echo "=== 5. Building Android CHIPTool APK via Gradle ==="
cd "$REPO_DIR/examples/android/CHIPTool"
./gradlew -PmatterSdkSourceBuild=false assembleDebug

echo "=== BUILD COMPLETE! ==="
echo "APK location: $REPO_DIR/examples/android/CHIPTool/app/build/outputs/apk/debug/app-debug.apk"
