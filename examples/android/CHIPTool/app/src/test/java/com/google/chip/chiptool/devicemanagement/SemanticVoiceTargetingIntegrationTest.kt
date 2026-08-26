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
import com.google.chip.chiptool.voice.DynamicNanoToolGenerator
import com.google.chip.chiptool.voice.InteractionType
import com.google.chip.chiptool.voice.MatterClusterMetaRegistry
import com.google.chip.chiptool.voice.UniversalMatterDispatcher
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class SemanticVoiceTargetingIntegrationTest {

  private lateinit var registry: CommissionedNodeRegistry
  private lateinit var synchronizer: MatterStateSynchronizer
  private lateinit var dispatcher: UniversalMatterDispatcher
  private lateinit var cardViewModel: DeviceCardViewModel
  private lateinit var toolGenerator: DynamicNanoToolGenerator

  @Before
  fun setUp() {
    registry = CommissionedNodeRegistry()
    registry.loadDefaultSmartHomeFabric()
    synchronizer = MatterStateSynchronizer(registry)
    dispatcher = UniversalMatterDispatcher(registry)

    val lightNode = registry.getNode(0x1001L)!!
    cardViewModel = DeviceCardViewModel(lightNode, registry, synchronizer, dispatcher)
    toolGenerator = DynamicNanoToolGenerator(registry)
  }

  @Test
  fun testUIChangeRoomInstantVoiceResolution() {
    // 1. User updates room in UI from "Living Room" to "Sunroom"
    cardViewModel.updateRoom("Sunroom")

    // 2. Simulate Gemini Nano receiving voice prompt: "turn off lights in sunroom"
    val prompt = toolGenerator.generateSystemPrompt()
    assertTrue(prompt.contains("Sunroom"))

    // 3. Compile tool call targeting newly named room
    val intent = toolGenerator.compileToolCall(
      functionName = "matter_onoff_off",
      arguments = mapOf("targetDeviceOrRoom" to "Sunroom"),
      userPrompt = "turn off lights in sunroom"
    )

    assertEquals(InteractionType.INVOKE_COMMAND, intent.interactionType)
    assertEquals(0x1001L, intent.targetNodeId)
    assertEquals(1, intent.targetEndpointId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_ON_OFF, intent.clusterId)
  }

  @Test
  fun testUIAddAliasInstantVoiceResolution() {
    // 1. User adds custom aliases in UI: "piano lamp", "reading chandelier"
    cardViewModel.updateAliases(listOf("piano lamp", "reading chandelier"))

    // 2. Verify prompt includes aliases
    val prompt = toolGenerator.generateSystemPrompt()
    assertTrue(prompt.contains("piano lamp"))

    // 3. Compile tool call using newly added alias
    val intent = toolGenerator.compileToolCall(
      functionName = "matter_levelcontrol_movetolevel",
      arguments = mapOf(
        "targetDeviceOrRoom" to "piano lamp",
        "brightnessPercentage" to 75.0
      ),
      userPrompt = "set piano lamp to 75%"
    )

    assertEquals(0x1001L, intent.targetNodeId)
    assertEquals(1, intent.targetEndpointId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL, intent.clusterId)
    assertEquals(191L, intent.resolvedParameters["level"])
  }

  @Test
  fun testUIRenameLabelInstantVoiceResolution() {
    // 1. User renames device label in UI to "Main Chandelier"
    cardViewModel.updateLabel("Main Chandelier")

    // 2. Compile tool call
    val intent = toolGenerator.compileToolCall(
      functionName = "matter_onoff_on",
      arguments = mapOf("targetDeviceOrRoom" to "Main Chandelier"),
      userPrompt = "turn on main chandelier"
    )

    assertEquals(0x1001L, intent.targetNodeId)
    assertEquals(1, intent.targetEndpointId)
  }
}
