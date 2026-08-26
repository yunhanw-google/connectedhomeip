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

class UniversalMatterDispatcherTest {

  private lateinit var registry: CommissionedNodeRegistry
  private lateinit var dispatcher: UniversalMatterDispatcher

  @Before
  fun setUp() {
    registry = CommissionedNodeRegistry()
    registry.loadDefaultSmartHomeFabric()
    dispatcher = UniversalMatterDispatcher(registry)
  }

  @Test
  fun testDecodedTemperatureStatusVoiceFeedback() {
    val cluster = MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_TEMPERATURE_MEASUREMENT)!!
    val attr = cluster.attributes[0x0000L]!!
    val decoded = DecodedAttributeValue(
      rawValue = 2240L,
      formattedValue = "22.4°C",
      unit = "°C",
      attributeMeta = attr
    )

    val result = MatterDispatchResult(
      isSuccess = true,
      message = "Read MeasuredValue: 22.4°C",
      voiceResponse = "The temperature at Living Room Climate Sensor is 22.4°C.",
      decodedAttribute = decoded
    )

    assertTrue(result.isSuccess)
    assertTrue(result.voiceResponse.contains("22.4°C"))
  }

  @Test
  fun testDecodedHumidityStatusVoiceFeedback() {
    val cluster = MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_RELATIVE_HUMIDITY_MEASUREMENT)!!
    val attr = cluster.attributes[0x0000L]!!
    val decoded = DecodedAttributeValue(
      rawValue = 4800L,
      formattedValue = "48.0%",
      unit = "%",
      attributeMeta = attr
    )

    val result = MatterDispatchResult(
      isSuccess = true,
      message = "Read MeasuredValue: 48.0%",
      voiceResponse = "The relative humidity at Living Room Climate Sensor is 48.0%.",
      decodedAttribute = decoded
    )

    assertTrue(result.isSuccess)
    assertTrue(result.voiceResponse.contains("48.0%"))
  }

  @Test
  fun testDecodedLockStateVoiceFeedback() {
    val cluster = MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK)!!
    val attr = cluster.attributes[0x0000L]!!
    val decoded = DecodedAttributeValue(
      rawValue = 1L,
      formattedValue = "Locked",
      attributeMeta = attr
    )

    val result = MatterDispatchResult(
      isSuccess = true,
      voiceResponse = "Front Door Deadbolt is currently Locked.",
      decodedAttribute = decoded
    )

    assertTrue(result.voiceResponse.contains("Locked"))
  }

  @Test
  fun testDecodedSmokeAlarmStateVoiceFeedback() {
    val cluster = MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_SMOKE_CO_ALARM)!!
    val attr = cluster.attributes[0x0000L]!!
    val decoded = DecodedAttributeValue(
      rawValue = 0L,
      formattedValue = "OK",
      attributeMeta = attr
    )

    val result = MatterDispatchResult(
      isSuccess = true,
      voiceResponse = "Smoke sensor on Kitchen Smoke Detector is OK.",
      decodedAttribute = decoded
    )

    assertTrue(result.voiceResponse.contains("OK"))
  }
}
