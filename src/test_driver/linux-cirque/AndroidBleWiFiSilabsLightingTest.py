#!/usr/bin/env python3
"""Copyright (c) 2026 Project CHIP Authors

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

import sys

from AndroidBleWiFiMobileDeviceTest import (
    DEVICE_CONFIG,
    TestAndroidBleWiFiMobileDevice,
)

# SIMULATION-DISCLOSURE: Executes the ARM32 Silicon Labs Lighting-App
# (examples/lighting-app/silabs/src/AppTask.cpp, CustomerAppTask.cpp,
# LEDWidget.cpp, RGBLEDWidget.cpp, src/platform/silabs/efr32/BLEManagerImpl.cpp,
# and lighting-wifi-app.zap) under qemu-arm inside a Cirque CHIPEndDevice Docker
# container over userspace Virtual Bluetooth (pty_bridge_linux +
# VirtualBluetoothServer) and userspace Virtual Wi-Fi (VirtualWiFiServer +
# WpaSupplicantDbusService), commissioned and controlled end-to-end by Android
# CHIPTool (cirque-android-runner).


class TestAndroidBleWiFiSilabsLightingDevice(TestAndroidBleWiFiMobileDevice):

  def __init__(self, device_config=None):
    super().__init__(
        device_config=device_config,
        end_device_app='silabs-lighting',
    )


if __name__ == '__main__':
  sys.exit(TestAndroidBleWiFiSilabsLightingDevice(DEVICE_CONFIG).run_test())
