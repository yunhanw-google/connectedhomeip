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

Tests for the helper shims that re-export implementations living in cirque.
"""

import importlib
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from helper import _cirque_reexport  # pylint: disable=g-import-not-at-top

_SHIMS = {
    'helper.wifi_lease': ['cirque.virtual_wifi.lease'],
    'helper.pcap_oracles': ['cirque.pcap.oracles', 'cirque.protocols.matter.icd'],
    'helper.android_emulator_logs': ['cirque.nodes.android_diagnostics'],
}


class CirqueReexportShimTest(unittest.TestCase):

    def test_shim_names_match_cirque_all(self):
        for shim_name, impl_names in _SHIMS.items():
            with self.subTest(shim=shim_name):
                shim = importlib.import_module(shim_name)
                impl_all = []
                for impl_name in impl_names:
                    impl = importlib.import_module(impl_name)
                    impl_all += list(impl.__all__)
                    for name in impl.__all__:
                        self.assertIs(getattr(shim, name), getattr(impl, name))
                self.assertEqual(sorted(shim.__all__), sorted(impl_all))

    def test_lazy_fallback_without_module(self):
        ns = {}
        _cirque_reexport.reexport(
            ns, 'cirque_module_that_does_not_exist', ['some_helper', 'SOME_CONST'])
        self.assertNotIn('SOME_CONST', ns)
        self.assertEqual(ns['some_helper'].__name__, 'some_helper')
        with self.assertRaises(ImportError):
            ns['some_helper']()


if __name__ == '__main__':
    unittest.main()
