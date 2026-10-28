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

Virtual Wi-Fi DHCP lease helpers (re-exports).

The implementation lives in cirque.virtual_wifi.lease;
this module keeps the old import path without requiring cirque at import
time (see helper/_cirque_reexport.py).
"""

from helper._cirque_reexport import reexport

__all__ = [
    'get_device_wlan0_ipv4',
    'get_device_wlan0_ipv6',
    'next_pool_address',
    'parse_device_wlan0_ipv4',
    'parse_device_wlan0_ipv6',
    'wait_for_device_wlan0_ipv4',
    'wait_for_device_wlan0_ipv6',
]
reexport(globals(), 'cirque.virtual_wifi.lease', __all__)
