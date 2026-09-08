# User-Space Virtual Bluetooth Foundation for Matter SDK on Linux

## Overview

This module provides a pure **user-space virtual Bluetooth Low Energy (BLE) transport and platform delegate framework** for Project CHIP (Matter SDK) on Linux.

Unlike the default Linux BLE implementation (`BLEManagerImpl.cpp`), this virtual BLE foundation **does NOT rely on BlueZ DBus, kernel `vhci`, `btvirt`, or `mac80211_hwsim` kernel modules**. This enables virtual end-to-end testing of Matter BLE commissioning flows in unprivileged cloud machines, Docker containers, remote VMs, and cloudtops.

## Architecture

```
+---------------------------------------------------------------------------------+
|                                 Matter Stack                                    |
|                      (BleLayer / BTP Transport Protocol)                        |
+---------------------------------------------------------------------------------+
                                       |
    +----------------------------------+----------------------------------+
    |                                                                     |
    v                                                                     v
+------------------------------------+               +------------------------------------+
|  VirtualBlePlatformDelegate        |               |  VirtualBlePlatformDelegate        |
|  (Virtual Commissioner / Central)  |               |  (Virtual Accessory / Peripheral)  |
+------------------------------------+               +------------------------------------+
                |                                                       |
                +===================== TCP/IPC BUS =====================+
                                  (Port 16402 / User-Space)
```

## Features

1. **Zero Kernel Driver Dependency**: Operates entirely in user-space using standard POSIX TCP/sockets or IPC.
2. **Platform Delegate Abstraction**: Implements `chip::Ble::BlePlatformDelegate`, `chip::Ble::BleConnectionDelegate`, and `chip::Ble::BleApplicationDelegate`.
3. **Dual Role Support**:
   - **Virtual Peripheral (Accessory)**: Advertises setup discriminator and accepts incoming virtual BLE GATT connections.
   - **Virtual Central (Commissioner)**: Connects to target setup discriminator over virtual BLE socket bus.
4. **Packet Framing Format**:
   - `0x01` `ADV_REPORT`: Advertisement containing Discriminator and Product/Vendor IDs.
   - `0x02` `CONNECT_REQ`: Virtual connection establishment.
   - `0x04` `GATT_WRITE`: RX characteristic write request (Commissioner -> Accessory).
   - `0x05` `GATT_INDICATION`: TX characteristic indication (Accessory -> Commissioner).
   - `0x06` `SUBSCRIBE`: CCCD subscription request.

## Building and Usage

Include the module in your `BUILD.gn`:

```gn
deps += [ "${chip_root}/src/platform/Linux/virtual_ble" ]
```

### Initializing in Application Code

```cpp
#include <platform/Linux/virtual_ble/VirtualBlePlatform.h>

using chip::DeviceLayer::Internal::VirtualBlePlatformDelegate;

VirtualBlePlatformDelegate gVirtualBle;

// Initialize Server (Accessory) or Client (Commissioner)
gVirtualBle.Init(&chip::Ble::BleLayer, 16402);
```
