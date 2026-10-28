#
#    Copyright (c) 2026 Project CHIP Authors
#    All rights reserved.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#
"""Re-exports helpers that now live in cirque, keeping drivers cirque-free.

Test drivers must import without cirque on sys.path (see
test_00_test_drivers_import_without_cirque_pythonpath). When cirque is
importable the names are bound directly. Otherwise each lower-case name
becomes a function that imports cirque on first call, so importing a driver
still works and only actually calling the helper needs cirque. Upper-case
names (constants) are only available when cirque is importable.
"""

import importlib
from typing import Any, Dict, Iterable


def reexport(namespace: Dict[str, Any], module_name: str,
             names: Iterable[str]) -> None:
    """Binds names from module_name into namespace, lazily if unavailable."""
    try:
        module = importlib.import_module(module_name)
    except ImportError:
        module = None
    for name in names:
        if module is not None:
            namespace[name] = getattr(module, name)
        elif not name.isupper():
            namespace[name] = _lazy_function(module_name, name)


def _lazy_function(module_name: str, name: str):
    def call(*args, **kwargs):
        return getattr(importlib.import_module(module_name), name)(
            *args, **kwargs)

    call.__name__ = call.__qualname__ = name
    call.__doc__ = f'Calls {module_name}.{name} (imported on first use).'
    return call
