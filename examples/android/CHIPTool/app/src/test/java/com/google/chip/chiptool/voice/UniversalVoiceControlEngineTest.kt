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

class UniversalVoiceControlEngineTest {

  private lateinit var registry: CommissionedNodeRegistry
  private lateinit var engine: VoiceControlEngine

  @Before
  fun setUp() {
    registry = CommissionedNodeRegistry()
    registry.loadDefaultSmartHomeFabric()
    engine = VoiceControlEngine.getInstance(registry)
  }

  @Test
  fun testAICoreConfigurationGeneration() {
    val (prompt, tools) = engine.getAICoreConfiguration()
    assertNotNull(prompt)
    assertTrue(prompt.contains("ACTIVE MATTER FABRIC INVENTORY"))
    assertTrue(tools.functionDeclarations.size >= 15)
  }

  @Test
  fun testEndToEndLightingVoiceIntent() {
    val toolJson = """
      {
        "name": "matter_onoff_on",
        "parameters": {
          "targetDeviceOrRoom": "Living Room Light"
        }
      }
    """.trimIndent()

    val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Turn on living room light")
    assertEquals(InteractionType.INVOKE_COMMAND, intent.interactionType)
    assertEquals(0x1001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_ON_OFF, intent.clusterId)
    assertEquals(0x01L, intent.commandId)

    // Verify TLV serialization
    val cluster = MatterClusterMetaRegistry.getCluster(intent.clusterId)!!
    val cmd = cluster.commands[intent.commandId!!]!!
    val tlv = UniversalTlvEncoder.encodeCommandPayload(cmd, intent.resolvedParameters)
    assertTrue(tlv.isNotEmpty())
  }

  @Test
  fun testEndToEndColorTemperatureKelvinVoiceIntent() {
    val toolJson = """
      {
        "name": "matter_colorcontrol_movetocolortemperature",
        "parameters": {
          "targetDeviceOrRoom": "Living Room Ceiling Light",
          "kelvin": 3000
        }
      }
    """.trimIndent()

    val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Set living room light to 3000K warm white")
    assertEquals(0x1001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL, intent.clusterId)
    assertEquals(0x0AL, intent.commandId)
    assertEquals(333L, intent.resolvedParameters["colorTemperatureMireds"]) // 1M / 3000 = 333

    val cluster = MatterClusterMetaRegistry.getCluster(intent.clusterId)!!
    val cmd = cluster.commands[intent.commandId!!]!!
    val tlv = UniversalTlvEncoder.encodeCommandPayload(cmd, intent.resolvedParameters)
    assertTrue(tlv.isNotEmpty())
  }

  @Test
  fun testEndToEndDoorLockVoiceIntent() {
    val toolJson = """
      {
        "name": "matter_doorlock_unlockwithtimeout",
        "parameters": {
          "targetDeviceOrRoom": "Front Door",
          "timeoutSeconds": 120
        }
      }
    """.trimIndent()

    val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Unlock front door for 2 minutes")
    assertEquals(0x2001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK, intent.clusterId)
    assertEquals(0x03L, intent.commandId)
    assertEquals(120, (intent.resolvedParameters["timeoutSeconds"] as Number).toInt())
  }

  @Test
  fun testEndToEndWindowCoveringVoiceIntent() {
    val toolJson = """
      {
        "name": "matter_windowcovering_gotoliftpercentage",
        "parameters": {
          "targetDeviceOrRoom": "Living Room Blinds",
          "percentage": 50
        }
      }
    """.trimIndent()

    val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Open blinds halfway")
    assertEquals(0x2002L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING, intent.clusterId)
    assertEquals(0x05L, intent.commandId)
    assertEquals(5000L, intent.resolvedParameters["liftPercent100thsValue"])
  }

  @Test
  fun testEndToEndThermostatVoiceIntent() {
    val toolJson = """
      {
        "name": "matter_thermostat_setpointraiselower",
        "parameters": {
          "targetDeviceOrRoom": "Hallway",
          "mode": 0,
          "amount": 20
        }
      }
    """.trimIndent()

    val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Make hallway 2 degrees warmer")
    assertEquals(0x3001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_THERMOSTAT, intent.clusterId)
    assertEquals(0x00L, intent.commandId)
  }

  @Test
  fun testEndToEndRobotVacuumVoiceIntent() {
    val toolJson = """
      {
        "name": "matter_rvcrunmode_changetomode",
        "parameters": {
          "targetDeviceOrRoom": "RoboVac",
          "newMode": 1
        }
      }
    """.trimIndent()

    val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Start vacuuming")
    assertEquals(0x5001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_RVC_RUN_MODE, intent.clusterId)
    assertEquals(0x00L, intent.commandId)
    assertEquals(1, (intent.resolvedParameters["newMode"] as Number).toInt())
  }

  @Test
  fun testEndToEndEVSEVoiceIntent() {
    val toolJson = """
      {
        "name": "matter_energyevse_setmaxchargerate",
        "parameters": {
          "targetDeviceOrRoom": "EV Home Charger",
          "maxChargeCurrent": 48000
        }
      }
    """.trimIndent()

    val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Set car charging current to 48A")
    assertEquals(0x6001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_ENERGY_EVSE, intent.clusterId)
    assertEquals(0x06L, intent.commandId)
    assertEquals(48000, (intent.resolvedParameters["maxChargeCurrent"] as Number).toInt())
  }

  @Test
  fun testEndToEndStatusQueryVoiceIntent() {
    val toolJson = """
      {
        "name": "matter_temperaturemeasurement_get_status",
        "parameters": {
          "targetDeviceOrRoom": "Living Room Climate Sensor",
          "attributeName": "MeasuredValue"
        }
      }
    """.trimIndent()

    val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Check temperature in living room")
    assertEquals(InteractionType.READ_ATTRIBUTE, intent.interactionType)
    assertEquals(0x7001L, intent.targetNodeId)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_TEMPERATURE_MEASUREMENT, intent.clusterId)
    assertEquals(0x0000L, intent.attributeId)
  }

  @Test
  fun testPerformanceBenchmark1000IntentCompilationsAndTlvEncodings() {
    val toolJson = """
      {
        "name": "matter_levelcontrol_movetolevel",
        "parameters": {
          "targetDeviceOrRoom": "Living Room Light",
          "brightnessPercentage": 75.0
        }
      }
    """.trimIndent()

    val iterations = 1000
    val startTime = System.nanoTime()

    for (i in 0 until iterations) {
      val intent = engine.toolGenerator.parseToolCallJson(toolJson, "Set living room light to 75%")
      val cluster = MatterClusterMetaRegistry.getCluster(intent.clusterId)!!
      val cmd = cluster.commands[intent.commandId!!]!!
      val tlv = UniversalTlvEncoder.encodeCommandPayload(cmd, intent.resolvedParameters)
      assertTrue(tlv.isNotEmpty())
    }

    val totalDurationMs = (System.nanoTime() - startTime) / 1_000_000.0
    val avgLatencyPerOpMs = totalDurationMs / iterations

    println("Benchmark: 1000 Intent Compilations & TLV Encodings completed in ${totalDurationMs}ms (avg ${avgLatencyPerOpMs}ms/op)")
    assertTrue("Average compilation + encoding latency should be under 1ms per operation", avgLatencyPerOpMs < 1.0)
  }
}
