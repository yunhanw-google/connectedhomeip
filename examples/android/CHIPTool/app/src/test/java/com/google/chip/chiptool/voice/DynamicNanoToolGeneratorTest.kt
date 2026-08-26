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
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class DynamicNanoToolGeneratorTest {

  private lateinit var registry: CommissionedNodeRegistry
  private lateinit var generator: DynamicNanoToolGenerator

  @Before
  fun setUp() {
    registry = CommissionedNodeRegistry()
    registry.loadDefaultSmartHomeFabric()
    generator = DynamicNanoToolGenerator(registry)
  }

  @Test
  fun testDynamicToolsConstrainedToFabricClusters() {
    val nanoTool = generator.generateGeminiTools()
    val toolNames = nanoTool.functionDeclarations.map { it.name }

    // Verify lighting tools present
    assertTrue(toolNames.contains("matter_onoff_on"))
    assertTrue(toolNames.contains("matter_onoff_off"))
    assertTrue(toolNames.contains("matter_levelcontrol_movetolevel"))
    assertTrue(toolNames.contains("matter_colorcontrol_movetocolortemperature"))

    // Verify closure tools present
    assertTrue(toolNames.contains("matter_doorlock_lockdoor"))
    assertTrue(toolNames.contains("matter_windowcovering_gotoliftpercentage"))
    assertTrue(toolNames.contains("matter_barriercontrol_barriercontrolgotopercent"))

    // Verify HVAC tools present
    assertTrue(toolNames.contains("matter_thermostat_setpointraiselower"))
    assertTrue(toolNames.contains("matter_fancontrol_step"))

    // Verify Media tools present
    assertTrue(toolNames.contains("matter_mediaplayback_play"))
    assertTrue(toolNames.contains("matter_mediaplayback_pause"))
    assertTrue(toolNames.contains("matter_audiooutput_volumeup"))

    // Verify Appliance & Robotics tools present
    assertTrue(toolNames.contains("matter_rvcrunmode_changetomode"))
    assertTrue(toolNames.contains("matter_operationalstate_start"))

    // Verify EVSE tools present
    assertTrue(toolNames.contains("matter_energyevse_startcharge"))
    assertTrue(toolNames.contains("matter_energyevse_setmaxchargerate"))

    // Verify Sensor Status Read tools present
    assertTrue(toolNames.contains("matter_temperaturemeasurement_get_status"))
    assertTrue(toolNames.contains("matter_relativehumiditymeasurement_get_status"))
    assertTrue(toolNames.contains("matter_doorlock_get_status"))
  }

  @Test
  fun testSystemPromptIncludesFabricInventory() {
    val prompt = generator.generateSystemPrompt()
    assertTrue(prompt.contains("Living Room Ceiling Light"))
    assertTrue(prompt.contains("Front Door Deadbolt"))
    assertTrue(prompt.contains("Main Thermostat"))
    assertTrue(prompt.contains("RoboVac"))
    assertTrue(prompt.contains("EV Home Charger"))
    assertTrue(prompt.contains("Living Room Climate Sensor"))
  }

  @Test
  fun testCompileLightingCommandWithPercentage() {
    val args = mapOf(
      "targetDeviceOrRoom" to "Living Room Light",
      "brightnessPercentage" to 50.0
    )
    val intent = generator.compileToolCall(
      functionName = "matter_levelcontrol_movetolevel",
      arguments = args,
      userPrompt = "Set living room light to 50%"
    )

    assertEquals(InteractionType.INVOKE_COMMAND, intent.interactionType)
    assertEquals(0x1001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL, intent.clusterId)
    assertEquals(0x00L, intent.commandId) // MoveToLevel
    assertEquals(127L, intent.resolvedParameters["level"]) // 50% of 254 = 127
  }

  @Test
  fun testCompileColorTemperatureWithKelvinConversion() {
    val args = mapOf(
      "targetDeviceOrRoom" to "Living Room Light",
      "kelvin" to 2700
    )
    val intent = generator.compileToolCall(
      functionName = "matter_colorcontrol_movetocolortemperature",
      arguments = args,
      userPrompt = "Set living room light to 2700K warm white"
    )

    assertEquals(InteractionType.INVOKE_COMMAND, intent.interactionType)
    assertEquals(0x1001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL, intent.clusterId)
    assertEquals(0x0AL, intent.commandId) // MoveToColorTemperature
    assertEquals(370L, intent.resolvedParameters["colorTemperatureMireds"]) // 1M / 2700 = 370
  }

  @Test
  fun testCompileWindowCoveringPercentage() {
    val args = mapOf(
      "targetDeviceOrRoom" to "Living Room Blinds",
      "percentage" to 75.0
    )
    val intent = generator.compileToolCall(
      functionName = "matter_windowcovering_gotoliftpercentage",
      arguments = args,
      userPrompt = "Open living room blinds 75%"
    )

    assertEquals(0x2002L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING, intent.clusterId)
    assertEquals(0x05L, intent.commandId)
    assertEquals(7500L, intent.resolvedParameters["liftPercent100thsValue"])
  }

  @Test
  fun testCompileThermostatRaiseTemperature() {
    val args = mapOf(
      "targetDeviceOrRoom" to "Hallway",
      "mode" to 0L, // Heat
      "amount" to 15L // +1.5°C
    )
    val intent = generator.compileToolCall(
      functionName = "matter_thermostat_setpointraiselower",
      arguments = args,
      userPrompt = "Turn up the heat in the hallway by 1.5 degrees"
    )

    assertEquals(0x3001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_THERMOSTAT, intent.clusterId)
    assertEquals(0x00L, intent.commandId)
    assertEquals(15L, intent.resolvedParameters["amount"])
  }

  @Test
  fun testCompileRobotVacuumDockCommand() {
    val args = mapOf(
      "targetDeviceOrRoom" to "RoboVac",
      "newMode" to 3L // ReturningToDock
    )
    val intent = generator.compileToolCall(
      functionName = "matter_rvcrunmode_changetomode",
      arguments = args,
      userPrompt = "Send robot vacuum to dock"
    )

    assertEquals(0x5001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_RVC_RUN_MODE, intent.clusterId)
    assertEquals(0x00L, intent.commandId)
    assertEquals(3L, intent.resolvedParameters["newMode"])
  }

  @Test
  fun testCompileStatusQueryToolCall() {
    val args = mapOf(
      "targetDeviceOrRoom" to "Living Room Climate Sensor",
      "attributeName" to "MeasuredValue"
    )
    val intent = generator.compileToolCall(
      functionName = "matter_temperaturemeasurement_get_status",
      arguments = args,
      userPrompt = "What is the living room temperature?"
    )

    assertEquals(InteractionType.READ_ATTRIBUTE, intent.interactionType)
    assertEquals(0x7001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_TEMPERATURE_MEASUREMENT, intent.clusterId)
    assertEquals(0x0000L, intent.attributeId) // MeasuredValue
  }

  @Test
  fun testParseToolCallJson() {
    val jsonString = """
      {
        "name": "matter_doorlock_lockdoor",
        "parameters": {
          "targetDeviceOrRoom": "Front Door"
        }
      }
    """.trimIndent()

    val intent = generator.parseToolCallJson(jsonString, "Lock front door")
    assertEquals(InteractionType.INVOKE_COMMAND, intent.interactionType)
    assertEquals(0x2001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK, intent.clusterId)
    assertEquals(0x00L, intent.commandId)
  }
}
