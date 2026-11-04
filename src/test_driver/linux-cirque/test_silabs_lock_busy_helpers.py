#!/usr/bin/env python3
import unittest
from helper.paths import CHIP_SILABS_LOCK_APP_ESC
import AndroidBleThreadSilabsLockBusyTest as busy_test

class SilabsLockBusyHarnessContractTest(unittest.TestCase):
  def test_harness_binds_silabs_lock_app(self):
    harness = busy_test.TestAndroidBleThreadSilabsLockBusy()
    self.assertEqual(harness._get_end_device_binary_esc(), CHIP_SILABS_LOCK_APP_ESC)

if __name__ == '__main__':
  unittest.main()
