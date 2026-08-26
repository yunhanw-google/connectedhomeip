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
import org.junit.Test

class MatterClusterMetaRegistryTest {

  @Test
  fun testRegistryContainsAllSevenClusterFamilies() {
    val lighting = MatterClusterMetaRegistry.getClustersByCategory(ClusterCategory.LIGHTING_AND_POWER)
    assertTrue("Lighting clusters should not be empty", lighting.isNotEmpty())
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_ON_OFF))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL))

    val closures = MatterClusterMetaRegistry.getClustersByCategory(ClusterCategory.CLOSURES)
    assertTrue("Closure clusters should not be empty", closures.isNotEmpty())
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_BARRIER_CONTROL))

    val hvac = MatterClusterMetaRegistry.getClustersByCategory(ClusterCategory.HVAC)
    assertTrue("HVAC clusters should not be empty", hvac.isNotEmpty())
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_THERMOSTAT))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_FAN_CONTROL))

    val media = MatterClusterMetaRegistry.getClustersByCategory(ClusterCategory.MEDIA)
    assertTrue("Media clusters should not be empty", media.isNotEmpty())
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_MEDIA_PLAYBACK))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_KEYPAD_INPUT))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_AUDIO_OUTPUT))

    val appliances = MatterClusterMetaRegistry.getClustersByCategory(ClusterCategory.APPLIANCES_AND_ROBOTICS)
    assertTrue("Appliance clusters should not be empty", appliances.isNotEmpty())
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_RVC_RUN_MODE))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_RVC_CLEAN_MODE))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_OPERATIONAL_STATE))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_MODE_SELECT))

    val energy = MatterClusterMetaRegistry.getClustersByCategory(ClusterCategory.ENERGY_MANAGEMENT)
    assertTrue("Energy clusters should not be empty", energy.isNotEmpty())
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_ENERGY_EVSE))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_DEVICE_ENERGY_MGMT))

    val sensors = MatterClusterMetaRegistry.getClustersByCategory(ClusterCategory.SENSORS_AND_ALARMS)
    assertTrue("Sensor clusters should not be empty", sensors.isNotEmpty())
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_TEMPERATURE_MEASUREMENT))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_RELATIVE_HUMIDITY_MEASUREMENT))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_OCCUPANCY_SENSING))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_BOOLEAN_STATE))
    assertNotNull(MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_SMOKE_CO_ALARM))
  }

  @Test
  fun testClusterLookupByNameAndSynonym() {
    val onOff = MatterClusterMetaRegistry.getClusterByName("OnOff")
    assertNotNull(onOff)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_ON_OFF, onOff?.clusterId)

    val light = MatterClusterMetaRegistry.getClusterByName("light")
    assertNotNull(light)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_ON_OFF, light?.clusterId)

    val blinds = MatterClusterMetaRegistry.getClusterByName("blinds")
    assertNotNull(blinds)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING, blinds?.clusterId)

    val vacuum = MatterClusterMetaRegistry.getClusterByName("robot vacuum")
    assertNotNull(vacuum)
    assertEquals(MatterClusterMetaRegistry.CLUSTER_RVC_RUN_MODE, vacuum?.clusterId)
  }

  @Test
  fun testKelvinToMiredsConversions() {
    // 2700K Warm White -> ~370 Mireds
    assertEquals(370, MatterClusterMetaRegistry.kelvinToMireds(2700))
    // 4000K Neutral White -> 250 Mireds
    assertEquals(250, MatterClusterMetaRegistry.kelvinToMireds(4000))
    // 6500K Daylight -> ~154 Mireds
    assertEquals(154, MatterClusterMetaRegistry.kelvinToMireds(6500))

    // Inverse
    assertEquals(2703, MatterClusterMetaRegistry.miredsToKelvin(370))
    assertEquals(4000, MatterClusterMetaRegistry.miredsToKelvin(250))
    assertEquals(6494, MatterClusterMetaRegistry.miredsToKelvin(154))
  }

  @Test
  fun testCelsiusToCentidegreeConversions() {
    assertEquals(2150.toShort(), MatterClusterMetaRegistry.celsiusToCentidegrees(21.5))
    assertEquals((-500).toShort(), MatterClusterMetaRegistry.celsiusToCentidegrees(-5.0))
    assertEquals(21.5, MatterClusterMetaRegistry.centidegreesToCelsius(2150.toShort()), 0.001)
  }

  @Test
  fun testPercentageToLevelConversions() {
    assertEquals(0.toUByte(), MatterClusterMetaRegistry.percentageToLevel(0.0))
    assertEquals(127.toUByte(), MatterClusterMetaRegistry.percentageToLevel(50.0))
    assertEquals(254.toUByte(), MatterClusterMetaRegistry.percentageToLevel(100.0))

    assertEquals(0.0, MatterClusterMetaRegistry.levelToPercentage(0.toUByte()), 0.5)
    assertEquals(50.0, MatterClusterMetaRegistry.levelToPercentage(127.toUByte()), 0.5)
    assertEquals(100.0, MatterClusterMetaRegistry.levelToPercentage(254.toUByte()), 0.5)
  }
}
