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
package com.google.chip.chiptool.devicemanagement

import com.google.chip.chiptool.voice.CommissionedNodeRegistry
import com.google.chip.chiptool.voice.MatterDataType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class MatterStateSynchronizerTest {

  private lateinit var registry: CommissionedNodeRegistry
  private lateinit var synchronizer: MatterStateSynchronizer

  @Before
  fun setUp() {
    registry = CommissionedNodeRegistry()
    registry.loadDefaultSmartHomeFabric()
    synchronizer = MatterStateSynchronizer(registry)
  }

  @Test
  fun testOptimisticUpdateAndRollback() {
    val rollback = synchronizer.applyOptimisticUpdate(
      nodeId = 0x1001L,
      endpointId = 1,
      clusterId = 6L,
      attributeId = 0L,
      optimisticValue = true
    )

    assertEquals(true, synchronizer.getCachedAttributeValue(0x1001L, 1, 6L, 0L))

    // Revert via rollback
    rollback.invoke()
    assertNull(synchronizer.getCachedAttributeValue(0x1001L, 1, 6L, 0L))
  }

  @Test
  fun testRecordConfirmedAttribute() {
    synchronizer.recordConfirmedAttribute(
      nodeId = 0x1001L,
      endpointId = 1,
      clusterId = 8L,
      attributeId = 0L,
      confirmedValue = 200
    )

    assertEquals(200, synchronizer.getCachedAttributeValue(0x1001L, 1, 8L, 0L))
    val node = registry.getNode(0x1001L)
    assertEquals(200, node?.liveStates?.get("currentLevel"))
  }

  @Test
  fun testDecodeRawAttributeTlv() {
    // Boolean
    assertEquals(true, synchronizer.decodeRawAttributeTlv(MatterDataType.BOOLEAN, byteArrayOf(0x01)))
    assertEquals(false, synchronizer.decodeRawAttributeTlv(MatterDataType.BOOLEAN, byteArrayOf(0x00)))

    // UINT8
    assertEquals(254, synchronizer.decodeRawAttributeTlv(MatterDataType.UINT8, byteArrayOf(0xFE.toByte())))

    // UINT16
    assertEquals(4000, synchronizer.decodeRawAttributeTlv(MatterDataType.UINT16, byteArrayOf(0xA0.toByte(), 0x0F.toByte())))

    // UTF8 String
    assertEquals("Smart Bulb", synchronizer.decodeRawAttributeTlv(MatterDataType.UTF8_STRING, "Smart Bulb".toByteArray()))
  }
}
