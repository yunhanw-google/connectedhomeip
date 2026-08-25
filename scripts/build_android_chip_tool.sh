#!/usr/bin/env bash
#
# Build script for Matter SDK (connectedhomeip) Android CHIPTool
#

set -e

REPO_DIR="/usr/local/google/home/yunhanw/connectedhomeip"

echo "=== 1. Checking Repository & Submodules ==="
if [ ! -d "$REPO_DIR" ]; then
  echo "Cloning connectedhomeip repository..."
  git clone --depth 1 https://github.com/project-chip/connectedhomeip.git "$REPO_DIR"
fi

cd "$REPO_DIR"

echo "Checking out Android submodules..."
python3 scripts/checkout_submodules.py --allow-changing-global-git-config --platform android --recursive

echo "=== 2. Applying Python 3.13 & Environment Patches ==="
# Patch constraints.txt for Python 3.13 pip/setuptools/wheel issues
sed -i 's/^pip==/# pip==/g' scripts/setup/constraints.txt 2>/dev/null || true
sed -i 's/^setuptools==/# setuptools==/g' scripts/setup/constraints.txt 2>/dev/null || true
sed -i 's/^wheel==/# wheel==/g' scripts/setup/constraints.txt 2>/dev/null || true

# Patch gn_run_binary.py to fallback to sys.executable/python3 if python is missing
if ! grep -q "shutil.which" build/gn_run_binary.py; then
  sed -i 's/import subprocess/import shutil\nimport subprocess/g' build/gn_run_binary.py
  sed -i 's/args = sys.argv\[1:\]/args = sys.argv[1:]\nif args and args[0] == "python":\n    args[0] = shutil.which("python") or sys.executable or "python3"/g' build/gn_run_binary.py
fi

echo "=== 3. Bootstrapping Environment ==="
PIP_INDEX_URL=https://pypi.org/simple bash -c "source scripts/bootstrap.sh"

# Install setuptools<70 into pigweed-venv for pkg_resources compatibility
"$REPO_DIR/.environment/pigweed-venv/bin/python" -m pip install --index-url https://pypi.org/simple "setuptools<70" toml click 2>/dev/null || true

echo "=== 4. Setting Up Build Variables ==="
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export PATH="/usr/lib/kotlinc/bin:$JAVA_HOME/bin:$REPO_DIR/.environment/cipd/packages/pigweed:$REPO_DIR/.environment/pigweed-venv/bin:$PATH"
export ANDROID_HOME=/usr/local/google/home/yunhanw/Android/Sdk
export ANDROID_NDK_HOME=/usr/local/google/home/yunhanw/Android/Sdk/ndk/29.0.13846066
export TARGET_CPU=arm64
export PW_PROJECT_ROOT="$REPO_DIR"

echo "=== 5. Running Android IDE Setup (GN/CMake) ==="
"$REPO_DIR/scripts/run_in_build_env.sh" "export JAVA_HOME=$JAVA_HOME; export PATH=$PATH; export ANDROID_HOME=$ANDROID_HOME; export ANDROID_NDK_HOME=$ANDROID_NDK_HOME; export TARGET_CPU=$TARGET_CPU; export PW_PROJECT_ROOT=$PW_PROJECT_ROOT; ./scripts/examples/android_app_ide.sh"

echo "=== 6. Compiling Native C++ JNI & Java/Kotlin Targets ==="
"$REPO_DIR/scripts/run_in_build_env.sh" "export JAVA_HOME=$JAVA_HOME; export PATH=$PATH; ninja -C $REPO_DIR/out/android-arm64-chip-tool src/controller/java:android src/controller/java:java src/platform/android:java src/app/server/java:java"

echo "=== 7. Packaging JNI Libraries & Matter SDK Jar ==="
mkdir -p "$REPO_DIR/examples/android/CHIPTool/app/libs/jniLibs/arm64-v8a"

jar cf "$REPO_DIR/examples/android/CHIPTool/app/libs/chip-sdk.jar" \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/app/server/java/java/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/controller/java/android_chip_im/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/controller/java/chipcluster/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/controller/java/chipclusterID/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/controller/java/java/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/controller/java/jsontlv/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/controller/java/kotlin_matter_controller/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/controller/java/onboarding_payload/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/controller/java/tlv/classes" . \
  -C "$REPO_DIR/out/android-arm64-chip-tool/obj/src/platform/android/java/classes" .

cp "$REPO_DIR/out/android-arm64-chip-tool/lib/jni/arm64-v8a/"*.so "$REPO_DIR/examples/android/CHIPTool/app/libs/jniLibs/arm64-v8a/"

# Clean up duplicate standalone jars
rm -f "$REPO_DIR/examples/android/CHIPTool/app/libs/CHIPClusterID.jar" "$REPO_DIR/examples/android/CHIPTool/app/libs/libMatterTlv.jar" "$REPO_DIR/examples/android/CHIPTool/app/libs/"*android.jar

echo "=== 8. Configuring Gradle & Building APK ==="
sed -i 's/matterSdkSourceBuild=true/matterSdkSourceBuild=false/g' "$REPO_DIR/examples/android/CHIPTool/gradle.properties" 2>/dev/null || true
sed -i 's/ndkVersion "28.2.13676358"/ndkVersion "29.0.13846066"/g' "$REPO_DIR/examples/android/CHIPTool/app/build.gradle" 2>/dev/null || true

cd "$REPO_DIR/examples/android/CHIPTool"
./gradlew assembleDebug

echo "=== BUILD COMPLETE! ==="
echo "APK location: $REPO_DIR/examples/android/CHIPTool/app/build/outputs/apk/debug/app-debug.apk"
