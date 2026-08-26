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

import com.google.chip.chiptool.voice.CommissionedEndpoint
import com.google.chip.chiptool.voice.CommissionedNode
import com.google.chip.chiptool.voice.CommissionedNodeRegistry
import com.google.chip.chiptool.voice.MatterClusterMetaRegistry
import com.google.chip.chiptool.voice.MatterDeviceTypes
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class RoomDeviceHierarchyTest {

  private lateinit var registry: CommissionedNodeRegistry

  @Before
  fun setUp() {
    registry = CommissionedNodeRegistry()
    registry.loadDefaultSmartHomeFabric()
  }

  @Test
  fun testHierarchyMappingForColorLight() {
    val lightNode = registry.getNode(0x1001L)
    assertNotNull(lightNode)

    val liveStates = mapOf(
      "0x1001:1:6:0" to true,
      "0x1001:1:8:0" to 190
    )
    val cardState = RoomDeviceHierarchyMapper.toDeviceCardState(lightNode!!, liveStates)

    assertEquals("Living Room Ceiling Light", cardState.nodeLabel)
    assertEquals("Living Room", cardState.roomName)
    assertTrue(cardState.isOnline)
    assertTrue(cardState.endpoints.isNotEmpty())

    // Verify endpoint 1 descriptor
    val ep1 = cardState.endpoints.first { it.endpointId == 1 }
    assertEquals("Extended Color Light", ep1.deviceTypeName)
    assertEquals(3, ep1.serverClusters.size)

    // Check cluster categories
    val onOffCluster = ep1.serverClusters.first { it.clusterId == MatterClusterMetaRegistry.CLUSTER_ON_OFF }
    assertEquals("Lighting & Power", onOffCluster.category)

    // Verify Quick Control
    assertTrue(cardState.quickControl is QuickControlAction.ToggleSwitch)
  }

  @Test
  fun testHierarchyMappingForDoorLock() {
    val lockNode = registry.getNode(0x2001L)
    assertNotNull(lockNode)

    val liveStates = mapOf("0x2001:1:257:0" to 1) // 1=Locked
    val cardState = RoomDeviceHierarchyMapper.toDeviceCardState(lockNode!!, liveStates)

    assertEquals("Front Door Deadbolt", cardState.nodeLabel)
    assertEquals("Entryway", cardState.roomName)

    val lockBadge = cardState.badges.firstOrNull { it.type == DeviceBadgeType.STATE_LOCKED }
    assertNotNull(lockBadge)
    assertEquals("Locked", lockBadge?.value)

    assertTrue(cardState.quickControl is QuickControlAction.LockToggle)
  }

  @Test
  fun testHierarchyMappingForThermostat() {
    val thermostatNode = registry.getNode(0x3001L)
    assertNotNull(thermostatNode)

    val liveStates = mapOf("0x3001:1:513:0" to 2250) // 22.50 °C
    val cardState = RoomDeviceHierarchyMapper.toDeviceCardState(thermostatNode!!, liveStates)

    val tempBadge = cardState.badges.firstOrNull { it.type == DeviceBadgeType.TEMPERATURE }
    assertNotNull(tempBadge)
    assertEquals("22.5°C", tempBadge?.value)

    assertTrue(cardState.quickControl is QuickControlAction.ThermostatDial)
    val dial = cardState.quickControl as QuickControlAction.ThermostatDial
    assertEquals(22.5, dial.tempCelsius, 0.01)
  }

  @Test
  fun testColorTemperatureFormatting() {
    val ep = CommissionedEndpoint(
      endpointId = 1,
      deviceTypeId = MatterDeviceTypes.EXTENDED_COLOR_LIGHT,
      deviceTypeName = "Extended Color Light",
      serverClusters = setOf(MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL)
    )
    val desc = RoomDeviceHierarchyMapper.toEndpointDescriptor(ep, mapOf("1:768:7" to 370L))
    val colorCluster = desc.serverClusters.first { it.clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL }
    val tempAttr = colorCluster.attributes.first { it.attributeId == 0x0007L }

    assertEquals(AttributeControlType.SLIDER_COLOR_TEMP, tempAttr.controlType)
    assertTrue(tempAttr.formattedValue.contains("2702 K") || tempAttr.formattedValue.contains("2700 K"))
  }

  @Test
  fun testRoomGroupingAndOnlineCounts() {
    val nodes = registry.getAllNodes()
    val grouped = nodes.groupBy { it.roomName }
    assertTrue(grouped.containsKey("Living Room"))
    assertTrue(grouped.containsKey("Kitchen"))
    assertTrue(grouped.containsKey("Entryway"))

    val livingRoomNodes = grouped["Living Room"]!!
    assertTrue(livingRoomNodes.isNotEmpty())
  }

  @Test
  fun testEmptyFabricAndRoomHierarchyHandling() {
    registry.clear()
    assertEquals(0, registry.count())
    assertTrue(registry.getAllNodes().isEmpty())
    assertTrue(registry.getAllRooms().isEmpty())

    val emptyTopology = HomeFabricTopology(fabricId = 1L, rooms = emptyList())
    assertEquals(0, emptyTopology.totalDevicesCount)
    assertEquals(0, emptyTopology.totalOnlineCount)
    assertTrue(emptyTopology.rooms.isEmpty())
  }
}
