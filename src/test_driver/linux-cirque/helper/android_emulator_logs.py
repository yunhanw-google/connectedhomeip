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

Android emulator diagnostics collection (re-exports).

The implementation lives in cirque.nodes.android_diagnostics;
this module keeps the old import path without requiring cirque at import
time (see helper/_cirque_reexport.py).
"""

from helper._cirque_reexport import reexport

__all__ = [
    'CMD_HOST_STATE',
    'CMD_LOGCAT_BOUNDED',
    'save_android_emulator_diagnostics',
]
reexport(
    globals(),
    'cirque.nodes.android_diagnostics',
    __all__ + ['_save_android_screen_recordings'],
)
