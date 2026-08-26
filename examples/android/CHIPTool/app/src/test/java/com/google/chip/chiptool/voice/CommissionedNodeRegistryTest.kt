/*
 *   Copyright (c) 2024 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */
package com.google.chip.chiptool.voice

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class CommissionedNodeRegistryTest {

  @Test
  fun testRegistryStartsEmptyByDefault() {
    val registry = CommissionedNodeRegistry()
    assertEquals(0, registry.count())
    assertTrue(registry.getAllNodes().isEmpty())
  }

  @Test
  fun testExplicitLoadDefaultDemoFabric() {
    val registry = CommissionedNodeRegistry()
    assertEquals(0, registry.count())
    registry.loadDefaultDemoFabric()
    assertEquals(14, registry.count())
    assertEquals(14, registry.getAllNodes().size)
  }

  @Test
  fun testClearAllNodesAndResetFabric() {
    val registry = CommissionedNodeRegistry()
    registry.loadDefaultDemoFabric()
    assertEquals(14, registry.count())

    registry.clearAllNodes()
    assertEquals(0, registry.count())
    assertTrue(registry.getAllNodes().isEmpty())

    registry.loadDefaultSmartHomeFabric()
    assertEquals(14, registry.count())

    registry.resetFabric()
    assertEquals(0, registry.count())
    assertTrue(registry.getAllNodes().isEmpty())
  }
}
