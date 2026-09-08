#!/usr/bin/env python3
# Copyright 2026 Google LLC
#
# Automated End-to-End Test Suite for Matter Virtual BLE Commissioning on Linux.
# Runs User-Space Virtual BLE Commissioner and End-Device without kernel btvirt/vhci/bluez.

import os
import signal
import socket
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VIRTUAL_BLE_DIR = os.path.dirname(SCRIPT_DIR)
MATTER_ROOT = os.path.abspath(os.path.join(VIRTUAL_BLE_DIR, "..", "..", "..", ".."))


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def compile_virtual_ble_binaries():
    print("[VirtualBleTestRunner] Compiling Virtual BLE End-Device & Commissioner prototypes...")

    accessory_src = os.path.join(SCRIPT_DIR, "VirtualBleAccessory.cpp")
    commissioner_src = os.path.join(SCRIPT_DIR, "VirtualBleCommissioner.cpp")
    platform_src = os.path.join(VIRTUAL_BLE_DIR, "VirtualBlePlatform.cpp")

    accessory_bin = os.path.join(SCRIPT_DIR, "virtual_ble_accessory")
    commissioner_bin = os.path.join(SCRIPT_DIR, "virtual_ble_commissioner")

    includes = [
        "-DCHIP_SYSTEM_CONFIG_USE_SOCKETS=1",
        "-DCHIP_SYSTEM_CONFIG_USE_POSIX_TIME_FUNCTS=1",
        "-DHAVE_CLOCK_GETTIME=1",
        f"-I{MATTER_ROOT}/src",
        f"-I{MATTER_ROOT}/src/include",
        f"-I{MATTER_ROOT}/src/platform/Linux",
        f"-I{MATTER_ROOT}/third_party/nlio/repo/include",
        f"-I{MATTER_ROOT}/third_party/pigweed/repo/pw_unit_test/public",
    ]

    matter_sources = [
        os.path.join(MATTER_ROOT, "src", "system", "SystemPacketBuffer.cpp"),
        os.path.join(MATTER_ROOT, "src", "system", "SystemLayerImplSelect.cpp"),
        os.path.join(MATTER_ROOT, "src", "system", "SystemClock.cpp"),
        os.path.join(MATTER_ROOT, "src", "system", "SystemTimer.cpp"),
        os.path.join(MATTER_ROOT, "src", "system", "WakeEvent.cpp"),
        os.path.join(MATTER_ROOT, "src", "system", "SystemError.cpp"),
        os.path.join(MATTER_ROOT, "src", "system", "SystemLayer.cpp"),
        os.path.join(MATTER_ROOT, "src", "lib", "core", "CHIPError.cpp"),
        os.path.join(MATTER_ROOT, "src", "lib", "core", "ErrorStr.cpp"),
        os.path.join(MATTER_ROOT, "src", "lib", "support", "CHIPMem.cpp"),
        os.path.join(MATTER_ROOT, "src", "lib", "support", "CHIPMem-Malloc.cpp"),
        os.path.join(MATTER_ROOT, "src", "lib", "support", "Pool.cpp"),
        os.path.join(MATTER_ROOT, "src", "lib", "support", "TimeUtils.cpp"),
        os.path.join(MATTER_ROOT, "src", "lib", "support", "logging", "TextOnlyLogging.cpp"),
        os.path.join(MATTER_ROOT, "src", "platform", "logging", "impl", "Stdio.cpp"),
        os.path.join(MATTER_ROOT, "src", "lib", "support", "BufferReader.cpp"),
        os.path.join(SCRIPT_DIR, "platform_event_stub.cpp"),
        os.path.join(MATTER_ROOT, "src", "system", "SystemMutex.cpp"),
        os.path.join(MATTER_ROOT, "src", "ble", "BleLayer.cpp"),
        os.path.join(MATTER_ROOT, "src", "ble", "BleError.cpp"),
        os.path.join(MATTER_ROOT, "src", "ble", "BleUUID.cpp"),
        os.path.join(MATTER_ROOT, "src", "ble", "BLEEndPoint.cpp"),
        os.path.join(MATTER_ROOT, "src", "ble", "BtpEngine.cpp"),
    ]

    cmd_acc = [
        "g++", "-std=c++17", "-pthread",
        accessory_src, platform_src, *matter_sources,
        *includes,
        "-o", accessory_bin
    ]

    cmd_comm = [
        "g++", "-std=c++17", "-pthread",
        commissioner_src, platform_src, *matter_sources,
        *includes,
        "-o", commissioner_bin
    ]

    print(f"Executing: {' '.join(cmd_acc)}")
    res1 = subprocess.run(cmd_acc, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res1.returncode != 0:
        print(f"Compilation Failed for Accessory:\n{res1.stderr}")
        sys.exit(1)

    print(f"Executing: {' '.join(cmd_comm)}")
    res2 = subprocess.run(cmd_comm, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res2.returncode != 0:
        print(f"Compilation Failed for Commissioner:\n{res2.stderr}")
        sys.exit(1)

    print("[VirtualBleTestRunner] Compilation Succeeded!")
    return accessory_bin, commissioner_bin


def run_ble_commissioning_test():
    accessory_bin, commissioner_bin = compile_virtual_ble_binaries()

    port = find_free_port()
    discriminator = 3840

    print(f"\n=======================================================")
    print(f"   STARTING MATTER VIRTUAL BLE COMMISSIONING TEST")
    print(f"   Port: {port} | Target Discriminator: {discriminator}")
    print(f"   Environment: User-Space TCP Socket (No btvirt/vhci)")
    print(f"=======================================================\n")

    # 1. Start Virtual BLE End-Device (Accessory)
    acc_proc = subprocess.Popen(
        [accessory_bin, str(port), str(discriminator)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    # Wait for accessory to signal LISTENING
    while True:
        line = acc_proc.stdout.readline()
        if "LISTENING" in line or not line:
            break

    # 2. Start Virtual BLE Commissioner
    comm_proc = subprocess.Popen(
        [commissioner_bin, str(port), str(discriminator)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    comm_out, comm_err = comm_proc.communicate(timeout=10.0)

    try:
        acc_proc.terminate()
        acc_proc.wait(timeout=2.0)
    except Exception:
        acc_proc.kill()

    print("[VirtualBleTestRunner] Commissioner Process Output:")
    print(comm_out)

    if comm_proc.returncode == 0 and "SUCCESS: BLE Commissioning Flow Completed" in comm_out:
        print("\n✅ SUCCESS: Matter BLE Commissioning Test PASSED cleanly over User-Space Virtual BLE!")
        return 0
    else:
        print(f"\n❌ FAILED: Commissioner exited with code {comm_proc.returncode}")
        print(comm_err)
        return 1


if __name__ == "__main__":
    sys.exit(run_ble_commissioning_test())
