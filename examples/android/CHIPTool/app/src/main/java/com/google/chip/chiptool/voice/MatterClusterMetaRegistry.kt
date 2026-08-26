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

import java.util.Collections
import kotlin.math.roundToInt

/**
 * Supported Matter Data Types in Interaction Model and TLV.
 */
enum class MatterDataType {
  BOOLEAN,
  INT8,
  INT16,
  INT32,
  INT64,
  UINT8,
  UINT16,
  UINT32,
  UINT64,
  FLOAT32,
  FLOAT64,
  UTF8_STRING,
  OCTET_STRING,
  ENUM8,
  ENUM16,
  BITMAP8,
  BITMAP16,
  BITMAP32,
  STRUCT,
  ARRAY,
  NULL
}

/**
 * Cluster functional categories for grouping and dynamic discovery.
 */
enum class ClusterCategory {
  LIGHTING_AND_POWER,
  CLOSURES,
  HVAC,
  MEDIA,
  APPLIANCES_AND_ROBOTICS,
  ENERGY_MANAGEMENT,
  SENSORS_AND_ALARMS,
  GENERAL_AND_SYSTEM
}

/**
 * Direction of command communication.
 */
enum class CommandDirection {
  CLIENT_TO_SERVER,
  SERVER_TO_CLIENT
}

/**
 * Metadata for a field inside a command or struct.
 */
data class MatterFieldMeta(
  val tagId: Int,
  val name: String,
  val type: MatterDataType,
  val isOptional: Boolean = false,
  val isNullable: Boolean = false,
  val description: String = "",
  val defaultValue: Any? = null,
  val enumValues: Map<String, Long> = emptyMap(),
  val minVal: Double? = null,
  val maxVal: Double? = null,
  val unit: String? = null
)

/**
 * Metadata for a Matter cluster command.
 */
data class MatterCommandMeta(
  val commandId: Long,
  val name: String,
  val description: String,
  val direction: CommandDirection = CommandDirection.CLIENT_TO_SERVER,
  val requestFields: List<MatterFieldMeta> = emptyList(),
  val responseCommandId: Long? = null,
  val isTimed: Boolean = false,
  val naturalSynonyms: List<String> = emptyList()
)

/**
 * Metadata for a Matter cluster attribute.
 */
data class MatterAttributeMeta(
  val attributeId: Long,
  val name: String,
  val type: MatterDataType,
  val isWritable: Boolean = false,
  val isReportable: Boolean = true,
  val isNullable: Boolean = false,
  val description: String = "",
  val defaultValue: Any? = null,
  val enumValues: Map<String, Long> = emptyMap(),
  val unit: String? = null,
  val naturalSynonyms: List<String> = emptyList()
)


/**
 * Metadata for a complete Matter Cluster.
 */
data class MatterClusterMeta(
  val clusterId: Long,
  val name: String,
  val category: ClusterCategory,
  val description: String,
  val commands: Map<Long, MatterCommandMeta> = emptyMap(),
  val attributes: Map<Long, MatterAttributeMeta> = emptyMap(),
  val naturalAliases: List<String> = emptyList()
)

/**
 * Universal Matter Cluster Schema Metamodel Registry.
 * Holds comprehensive semantic metadata for all standard Matter controllable clusters.
 */
object MatterClusterMetaRegistry {

  // Cluster IDs
  const val CLUSTER_ON_OFF = 0x0006L
  const val CLUSTER_LEVEL_CONTROL = 0x0008L
  const val CLUSTER_COLOR_CONTROL = 0x0300L
  const val CLUSTER_DOOR_LOCK = 0x0101L
  const val CLUSTER_WINDOW_COVERING = 0x0102L
  const val CLUSTER_BARRIER_CONTROL = 0x0103L
  const val CLUSTER_THERMOSTAT = 0x0201L
  const val CLUSTER_FAN_CONTROL = 0x0202L
  const val CLUSTER_MEDIA_PLAYBACK = 0x0506L
  const val CLUSTER_KEYPAD_INPUT = 0x0509L
  const val CLUSTER_AUDIO_OUTPUT = 0x050BL
  const val CLUSTER_MODE_SELECT = 0x0050L
  const val CLUSTER_RVC_RUN_MODE = 0x0054L
  const val CLUSTER_RVC_CLEAN_MODE = 0x0055L
  const val CLUSTER_OPERATIONAL_STATE = 0x0060L
  const val CLUSTER_DEVICE_ENERGY_MGMT = 0x0098L
  const val CLUSTER_DEVICE_ENERGY_MANAGEMENT = 0x0098L
  const val CLUSTER_ENERGY_EVSE = 0x0099L
  const val CLUSTER_BOOLEAN_STATE = 0x0045L
  const val CLUSTER_SMOKE_CO_ALARM = 0x005CL
  const val CLUSTER_TEMPERATURE_MEASUREMENT = 0x0402L
  const val CLUSTER_RELATIVE_HUMIDITY_MEASUREMENT = 0x0405L
  const val CLUSTER_OCCUPANCY_SENSING = 0x0406L
  const val CLUSTER_ILLUMINANCE_MEASUREMENT = 0x0400L

  private val registry = HashMap<Long, MatterClusterMeta>()

  init {
    registerLightingClusters()
    registerClosureClusters()
    registerHvacClusters()
    registerMediaClusters()
    registerApplianceAndRoboticsClusters()
    registerEnergyClusters()
    registerSensorAndAlarmClusters()
  }

  fun getCluster(clusterId: Long): MatterClusterMeta? = registry[clusterId]

  fun getClusterByName(name: String): MatterClusterMeta? {
    val cleanName = name.replace(" ", "").replace("_", "").lowercase()
    return registry.values.firstOrNull {
      it.name.replace(" ", "").replace("_", "").lowercase() == cleanName ||
        it.naturalAliases.any { alias -> alias.replace(" ", "").lowercase() == cleanName }
    }
  }

  fun getAllClusters(): List<MatterClusterMeta> = registry.values.toList()

  fun getClustersByCategory(category: ClusterCategory): List<MatterClusterMeta> {
    return registry.values.filter { it.category == category }
  }

  fun registerCluster(cluster: MatterClusterMeta) {
    registry[cluster.clusterId] = cluster
  }

  // --- Conversions & Helpers ---

  /**
   * Converts Color Temperature in Kelvin (e.g. 2700K - 6500K) to Matter Mireds (1,000,000 / K).
   */
  fun kelvinToMireds(kelvin: Int): Int {
    require(kelvin > 0) { "Kelvin must be positive" }
    return (1_000_000.0 / kelvin).roundToInt()
  }

  /**
   * Converts Matter Mireds to Color Temperature in Kelvin.
   */
  fun miredsToKelvin(mireds: Int): Int {
    require(mireds > 0) { "Mireds must be positive" }
    return (1_000_000.0 / mireds).roundToInt()
  }

  /**
   * Converts Celsius temperature to Matter Centidegrees (x100).
   */
  fun celsiusToCentidegrees(celsius: Double): Short {
    return (celsius * 100.0).roundToInt().toShort()
  }

  /**
   * Converts Matter Centidegrees to Celsius temperature.
   */
  fun centidegreesToCelsius(centidegrees: Short): Double {
    return centidegrees.toDouble() / 100.0
  }

  /**
   * Converts percentage (0..100) to Matter Level (0..254).
   */
  fun percentageToLevel(percentage: Double): UByte {
    val clamped = percentage.coerceIn(0.0, 100.0)
    return ((clamped * 254.0) / 100.0).roundToInt().toUByte()
  }

  /**
   * Converts Matter Level (0..254) to percentage (0..100).
   */
  fun levelToPercentage(level: UByte): Double {
    return (level.toDouble() * 100.0 / 254.0).coerceIn(0.0, 100.0)
  }

  // =========================================================================
  // 1. Lighting Clusters
  // =========================================================================
  private fun registerLightingClusters() {
    // On/Off (0x0006)
    val onOffCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "Off", "Turn the device off", naturalSynonyms = listOf("turn off", "switch off", "power off", "shut down")),
      0x01L to MatterCommandMeta(0x01L, "On", "Turn the device on", naturalSynonyms = listOf("turn on", "switch on", "power on", "activate")),
      0x02L to MatterCommandMeta(0x02L, "Toggle", "Toggle the device state", naturalSynonyms = listOf("toggle", "switch state")),
      0x40L to MatterCommandMeta(0x40L, "OffWithEffect", "Turn off with specified effect", requestFields = listOf(
        MatterFieldMeta(0, "effectIdentifier", MatterDataType.ENUM8, description = "Effect ID: 0=DelayedAllOff, 1=DyingLight"),
        MatterFieldMeta(1, "effectVariant", MatterDataType.UINT8, description = "Effect variant")
      )),
      0x41L to MatterCommandMeta(0x41L, "OnWithRecallGlobalScene", "Turn on recalling global scene"),
      0x42L to MatterCommandMeta(0x42L, "OnWithTimedOff", "Turn on with timed automatic off", requestFields = listOf(
        MatterFieldMeta(0, "onOffControl", MatterDataType.BITMAP8, description = "Control bitmap"),
        MatterFieldMeta(1, "onTime", MatterDataType.UINT16, description = "1/10th of a second on time"),
        MatterFieldMeta(2, "offWaitTime", MatterDataType.UINT16, description = "1/10th of a second off wait time")
      ))
    )
    val onOffAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "OnOff", MatterDataType.BOOLEAN, isWritable = true, description = "Current on/off state", naturalSynonyms = listOf("state", "power status")),
      0x4000L to MatterAttributeMeta(0x4000L, "GlobalSceneControl", MatterDataType.BOOLEAN, description = "Global scene recall status"),
      0x4001L to MatterAttributeMeta(0x4001L, "OnTime", MatterDataType.UINT16, isWritable = true, description = "Remaining on time (1/10 s)"),
      0x4002L to MatterAttributeMeta(0x4002L, "OffWaitTime", MatterDataType.UINT16, isWritable = true, description = "Off wait time (1/10 s)"),
      0x4003L to MatterAttributeMeta(0x4003L, "StartUpOnOff", MatterDataType.ENUM8, isWritable = true, description = "0=Off, 1=On, 2=Toggle, 3=Previous")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_ON_OFF, "OnOff", ClusterCategory.LIGHTING_AND_POWER,
      "Controls on/off power state of lights, switches, and appliances.",
      onOffCommands, onOffAttributes, listOf("light", "switch", "power", "plug", "outlet")
    ))

    // Level Control (0x0008)
    val levelCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "MoveToLevel", "Move to level (0-254)", requestFields = listOf(
        MatterFieldMeta(0, "level", MatterDataType.UINT8, description = "Target level 0..254", minVal = 0.0, maxVal = 254.0),
        MatterFieldMeta(1, "transitionTime", MatterDataType.UINT16, isOptional = true, defaultValue = 0, description = "Transition time in 1/10ths second"),
        MatterFieldMeta(2, "optionsMask", MatterDataType.BITMAP8, isOptional = true, defaultValue = 0),
        MatterFieldMeta(3, "optionsOverride", MatterDataType.BITMAP8, isOptional = true, defaultValue = 0)
      ), naturalSynonyms = listOf("set brightness", "set level", "dim", "brighten")),
      0x01L to MatterCommandMeta(0x01L, "Move", "Continuously move level up or down", requestFields = listOf(
        MatterFieldMeta(0, "moveMode", MatterDataType.ENUM8, description = "0=Up, 1=Down"),
        MatterFieldMeta(1, "rate", MatterDataType.UINT8, description = "Steps per second")
      )),
      0x02L to MatterCommandMeta(0x02L, "Step", "Step level by step size", requestFields = listOf(
        MatterFieldMeta(0, "stepMode", MatterDataType.ENUM8, description = "0=Up, 1=Down"),
        MatterFieldMeta(1, "stepSize", MatterDataType.UINT8, description = "Step size"),
        MatterFieldMeta(2, "transitionTime", MatterDataType.UINT16, isOptional = true, defaultValue = 0)
      )),
      0x03L to MatterCommandMeta(0x03L, "Stop", "Stop level transition"),
      0x04L to MatterCommandMeta(0x04L, "MoveToLevelWithOnOff", "Move to level and turn on if off", requestFields = listOf(
        MatterFieldMeta(0, "level", MatterDataType.UINT8, description = "Target level 0..254"),
        MatterFieldMeta(1, "transitionTime", MatterDataType.UINT16, isOptional = true, defaultValue = 0),
        MatterFieldMeta(2, "optionsMask", MatterDataType.BITMAP8, isOptional = true, defaultValue = 0),
        MatterFieldMeta(3, "optionsOverride", MatterDataType.BITMAP8, isOptional = true, defaultValue = 0)
      ), naturalSynonyms = listOf("set brightness with on", "brighten light"))
    )
    val levelAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "CurrentLevel", MatterDataType.UINT8, description = "Current brightness level 0..254", naturalSynonyms = listOf("brightness", "level")),
      0x0001L to MatterAttributeMeta(0x0001L, "RemainingTime", MatterDataType.UINT16, description = "Remaining transition time"),
      0x0002L to MatterAttributeMeta(0x0002L, "MinLevel", MatterDataType.UINT8, description = "Minimum supported level"),
      0x0003L to MatterAttributeMeta(0x0003L, "MaxLevel", MatterDataType.UINT8, description = "Maximum supported level"),
      0x0011L to MatterAttributeMeta(0x0011L, "OnLevel", MatterDataType.UINT8, isWritable = true, description = "Level when turned on")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_LEVEL_CONTROL, "LevelControl", ClusterCategory.LIGHTING_AND_POWER,
      "Controls level, dimming, and brightness of lighting and actuators.",
      levelCommands, levelAttributes, listOf("brightness", "dimmer", "level")
    ))

    // Color Control (0x0300)
    val colorCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "MoveToHue", "Move to specific Hue", requestFields = listOf(
        MatterFieldMeta(0, "hue", MatterDataType.UINT8, description = "Hue 0..254"),
        MatterFieldMeta(1, "direction", MatterDataType.ENUM8, description = "0=Shortest, 1=Longest, 2=Up, 3=Down"),
        MatterFieldMeta(2, "transitionTime", MatterDataType.UINT16, isOptional = true, defaultValue = 0)
      )),
      0x03L to MatterCommandMeta(0x03L, "MoveToSaturation", "Move to Saturation", requestFields = listOf(
        MatterFieldMeta(0, "saturation", MatterDataType.UINT8, description = "Saturation 0..254"),
        MatterFieldMeta(1, "transitionTime", MatterDataType.UINT16, isOptional = true, defaultValue = 0)
      )),
      0x06L to MatterCommandMeta(0x06L, "MoveToHueAndSaturation", "Move to Hue and Saturation", requestFields = listOf(
        MatterFieldMeta(0, "hue", MatterDataType.UINT8, description = "Hue 0..254"),
        MatterFieldMeta(1, "saturation", MatterDataType.UINT8, description = "Saturation 0..254"),
        MatterFieldMeta(2, "transitionTime", MatterDataType.UINT16, isOptional = true, defaultValue = 0)
      ), naturalSynonyms = listOf("set color", "change color")),
      0x07L to MatterCommandMeta(0x07L, "MoveToColor", "Move to CIE 1931 XY coordinates", requestFields = listOf(
        MatterFieldMeta(0, "colorX", MatterDataType.UINT16, description = "CIE X 0..65535"),
        MatterFieldMeta(1, "colorY", MatterDataType.UINT16, description = "CIE Y 0..65535"),
        MatterFieldMeta(2, "transitionTime", MatterDataType.UINT16, isOptional = true, defaultValue = 0)
      ), naturalSynonyms = listOf("set xy color")),
      0x0AL to MatterCommandMeta(0x0AL, "MoveToColorTemperature", "Move to Color Temperature in Mireds (1,000,000 / Kelvin)", requestFields = listOf(
        MatterFieldMeta(0, "colorTemperatureMireds", MatterDataType.UINT16, description = "Color Temperature in Mireds"),
        MatterFieldMeta(1, "transitionTime", MatterDataType.UINT16, isOptional = true, defaultValue = 0)
      ), naturalSynonyms = listOf("set color temperature", "set warm white", "set cool white", "set daylight"))
    )
    val colorAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "CurrentHue", MatterDataType.UINT8, description = "Current Hue 0..254"),
      0x0001L to MatterAttributeMeta(0x0001L, "CurrentSaturation", MatterDataType.UINT8, description = "Current Saturation 0..254"),
      0x0003L to MatterAttributeMeta(0x0003L, "CurrentX", MatterDataType.UINT16, description = "Current CIE X"),
      0x0004L to MatterAttributeMeta(0x0004L, "CurrentY", MatterDataType.UINT16, description = "Current CIE Y"),
      0x0007L to MatterAttributeMeta(0x0007L, "ColorTemperatureMireds", MatterDataType.UINT16, description = "Current Color Temperature in Mireds", naturalSynonyms = listOf("color temperature", "warmth")),
      0x0008L to MatterAttributeMeta(0x0008L, "ColorMode", MatterDataType.ENUM8, description = "0=Hue/Sat, 1=XY, 2=ColorTemp"),
      0x400BL to MatterAttributeMeta(0x400BL, "ColorTempPhysicalMinMireds", MatterDataType.UINT16, description = "Coldest limit (min mireds, max K)"),
      0x400CL to MatterAttributeMeta(0x400CL, "ColorTempPhysicalMaxMireds", MatterDataType.UINT16, description = "Warmest limit (max mireds, min K)")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_COLOR_CONTROL, "ColorControl", ClusterCategory.LIGHTING_AND_POWER,
      "Controls color, hue, saturation, and color temperature of lights.",
      colorCommands, colorAttributes, listOf("color", "color light", "rgb light")
    ))
  }

  // =========================================================================
  // 2. Closures & Barrier Clusters
  // =========================================================================
  private fun registerClosureClusters() {
    // Door Lock (0x0101)
    val doorLockCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "LockDoor", "Lock the door", isTimed = true, naturalSynonyms = listOf("lock", "lock door", "secure")),
      0x01L to MatterCommandMeta(0x01L, "UnlockDoor", "Unlock the door", isTimed = true, naturalSynonyms = listOf("unlock", "unlock door", "open lock")),
      0x03L to MatterCommandMeta(0x03L, "UnlockWithTimeout", "Unlock door with relock timeout in seconds", requestFields = listOf(
        MatterFieldMeta(0, "timeoutSeconds", MatterDataType.UINT16, description = "Relock timeout in seconds")
      ), isTimed = true, naturalSynonyms = listOf("unlock temporarily", "unlock for a minute"))
    )
    val doorLockAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "LockState", MatterDataType.ENUM8, description = "0=NotFullyLocked, 1=Locked, 2=Unlocked", enumValues = mapOf("NotFullyLocked" to 0L, "Locked" to 1L, "Unlocked" to 2L), naturalSynonyms = listOf("lock state", "is locked")),
      0x0001L to MatterAttributeMeta(0x0001L, "LockType", MatterDataType.ENUM8, description = "Lock physical hardware type"),
      0x0002L to MatterAttributeMeta(0x0002L, "ActuatorEnabled", MatterDataType.BOOLEAN, description = "Actuator enabled status"),
      0x0003L to MatterAttributeMeta(0x0003L, "DoorState", MatterDataType.ENUM8, description = "0=Open, 1=Closed, 2=Jammed, 3=ForcedOpen", naturalSynonyms = listOf("door state", "door open or closed"))
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_DOOR_LOCK, "DoorLock", ClusterCategory.CLOSURES,
      "Controls smart locks, deadbolts, and door latches.",
      doorLockCommands, doorLockAttributes, listOf("lock", "door lock", "deadbolt")
    ))

    // Window Covering (0x0102)
    val windowCoveringCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "UpOrOpen", "Open or raise window covering completely", naturalSynonyms = listOf("open blinds", "raise blinds", "open shades", "open curtains")),
      0x01L to MatterCommandMeta(0x01L, "DownOrClose", "Close or lower window covering completely", naturalSynonyms = listOf("close blinds", "lower blinds", "close shades", "close curtains")),
      0x02L to MatterCommandMeta(0x02L, "StopMotion", "Stop any ongoing window covering motion", naturalSynonyms = listOf("stop blinds", "stop shades", "hold curtains")),
      0x04L to MatterCommandMeta(0x04L, "GoToLiftValue", "Go to lift value", requestFields = listOf(
        MatterFieldMeta(0, "liftValue", MatterDataType.UINT16, description = "Lift value")
      )),
      0x05L to MatterCommandMeta(0x05L, "GoToLiftPercentage", "Go to lift percentage (0% = fully open, 100% = fully closed)", requestFields = listOf(
        MatterFieldMeta(0, "liftPercent100thsValue", MatterDataType.UINT16, description = "Lift percentage in 100ths (0..10000) e.g. 5000=50%")
      ), naturalSynonyms = listOf("set blinds percentage", "set shades to percent", "open blinds halfway")),
      0x07L to MatterCommandMeta(0x07L, "GoToTiltValue", "Go to tilt value", requestFields = listOf(
        MatterFieldMeta(0, "tiltValue", MatterDataType.UINT16, description = "Tilt value")
      )),
      0x08L to MatterCommandMeta(0x08L, "GoToTiltPercentage", "Go to tilt percentage (0..10000 in 100ths)", requestFields = listOf(
        MatterFieldMeta(0, "tiltPercent100thsValue", MatterDataType.UINT16, description = "Tilt percentage in 100ths (0..10000)")
      ), naturalSynonyms = listOf("tilt blinds", "set tilt"))
    )
    val windowCoveringAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "Type", MatterDataType.ENUM8, description = "Covering type: Roller, Blind, Drapery, Awning, Shutter"),
      0x0008L to MatterAttributeMeta(0x0008L, "CurrentPositionLiftPercentage", MatterDataType.UINT8, description = "Current lift position percentage (0..100)"),
      0x0009L to MatterAttributeMeta(0x0009L, "CurrentPositionTiltPercentage", MatterDataType.UINT8, description = "Current tilt position percentage (0..100)"),
      0x000AL to MatterAttributeMeta(0x000AL, "OperationalStatus", MatterDataType.BITMAP8, description = "Status bitmap of current motion"),
      0x000BL to MatterAttributeMeta(0x000BL, "TargetPositionLiftPercent100ths", MatterDataType.UINT16, description = "Target lift position in 100ths of %"),
      0x000CL to MatterAttributeMeta(0x000CL, "TargetPositionTiltPercent100ths", MatterDataType.UINT16, description = "Target tilt position in 100ths of %")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_WINDOW_COVERING, "WindowCovering", ClusterCategory.CLOSURES,
      "Controls motorized shades, blinds, shutters, and draperies.",
      windowCoveringCommands, windowCoveringAttributes, listOf("blinds", "shades", "curtains", "drapes", "shutters")
    ))

    // Barrier Control (0x0103)
    val barrierCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "BarrierControlGoToPercent", "Move barrier to target percent (0..100)", requestFields = listOf(
        MatterFieldMeta(0, "percentOpen", MatterDataType.UINT8, description = "Percent open 0..100")
      ), naturalSynonyms = listOf("open garage", "close garage door", "set garage door")),
      0x01L to MatterCommandMeta(0x01L, "BarrierControlStop", "Stop barrier movement", naturalSynonyms = listOf("stop garage door"))
    )
    val barrierAttributes = mapOf(
      0x0001L to MatterAttributeMeta(0x0001L, "BarrierMovingState", MatterDataType.ENUM8, description = "0=Stopped, 1=Closing, 2=Opening"),
      0x0002L to MatterAttributeMeta(0x0002L, "BarrierSafetyStatus", MatterDataType.BITMAP16, description = "Safety sensor trip status"),
      0x0003L to MatterAttributeMeta(0x0003L, "BarrierCapabilities", MatterDataType.BITMAP8, description = "Barrier capabilities"),
      0x000AL to MatterAttributeMeta(0x000AL, "BarrierPosition", MatterDataType.UINT8, description = "Current open percentage (0..100)")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_BARRIER_CONTROL, "BarrierControl", ClusterCategory.CLOSURES,
      "Controls motorized barrier gates, garage doors, and barriers.",
      barrierCommands, barrierAttributes, listOf("garage", "garage door", "gate", "barrier")
    ))
  }

  // =========================================================================
  // 3. HVAC Clusters
  // =========================================================================
  private fun registerHvacClusters() {
    // Thermostat (0x0201)
    val thermostatCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "SetpointRaiseLower", "Raise or lower setpoint temperature", requestFields = listOf(
        MatterFieldMeta(0, "mode", MatterDataType.ENUM8, description = "0=Heat, 1=Cool, 2=Both"),
        MatterFieldMeta(1, "amount", MatterDataType.INT8, description = "Step in 1/10th of deg C (-128..127)")
      ), naturalSynonyms = listOf("turn up heat", "turn down heat", "raise temperature", "lower temperature", "make it warmer", "make it cooler")),
      0x01L to MatterCommandMeta(0x01L, "SetWeeklySchedule", "Program schedule"),
      0x02L to MatterCommandMeta(0x02L, "GetWeeklySchedule", "Retrieve schedule", requestFields = listOf(
        MatterFieldMeta(0, "daysToReturn", MatterDataType.BITMAP8, description = "Days bitmap"),
        MatterFieldMeta(1, "modeToReturn", MatterDataType.BITMAP8, description = "Mode bitmap")
      )),
      0x03L to MatterCommandMeta(0x03L, "ClearWeeklySchedule", "Clear programmed weekly schedule")
    )
    val thermostatAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "LocalTemperature", MatterDataType.INT16, description = "Current temperature in centidegrees C (e.g. 2150 = 21.5°C)", unit = "°C", naturalSynonyms = listOf("room temperature", "current temperature")),
      0x0011L to MatterAttributeMeta(0x0011L, "OccupiedCoolingSetpoint", MatterDataType.INT16, isWritable = true, description = "Target cooling temperature in centidegrees C", unit = "°C", naturalSynonyms = listOf("cooling setpoint", "ac temperature")),
      0x0012L to MatterAttributeMeta(0x0012L, "OccupiedHeatingSetpoint", MatterDataType.INT16, isWritable = true, description = "Target heating temperature in centidegrees C", unit = "°C", naturalSynonyms = listOf("heating setpoint", "heat temperature", "target temperature")),
      0x001BL to MatterAttributeMeta(0x001BL, "ControlSequenceOfOperation", MatterDataType.ENUM8, isWritable = true, description = "HVAC capabilities mode"),
      0x001CL to MatterAttributeMeta(0x001CL, "SystemMode", MatterDataType.ENUM8, isWritable = true, description = "0=Off, 1=Auto, 3=Cool, 4=Heat, 5=EmergencyHeat, 6=Precooling, 7=FanOnly", enumValues = mapOf("Off" to 0L, "Auto" to 1L, "Cool" to 3L, "Heat" to 4L, "EmergencyHeat" to 5L, "Precooling" to 6L, "FanOnly" to 7L), naturalSynonyms = listOf("hvac mode", "thermostat mode", "climate mode")),
      0x0029L to MatterAttributeMeta(0x0029L, "ThermostatRunningState", MatterDataType.BITMAP16, description = "Bit 0: Heat, Bit 1: Cool, Bit 2: Fan")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_THERMOSTAT, "Thermostat", ClusterCategory.HVAC,
      "Controls climate control, HVAC system mode, heating/cooling setpoints.",
      thermostatCommands, thermostatAttributes, listOf("thermostat", "hvac", "ac", "heater", "climate")
    ))

    // Fan Control (0x0202)
    val fanCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "Step", "Step fan speed", requestFields = listOf(
        MatterFieldMeta(0, "direction", MatterDataType.ENUM8, description = "0=Increase, 1=Decrease"),
        MatterFieldMeta(1, "wrap", MatterDataType.BOOLEAN, isOptional = true, defaultValue = false),
        MatterFieldMeta(2, "lowestOff", MatterDataType.BOOLEAN, isOptional = true, defaultValue = false)
      ), naturalSynonyms = listOf("increase fan speed", "decrease fan speed"))
    )
    val fanAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "FanMode", MatterDataType.ENUM8, isWritable = true, description = "0=Off, 1=Low, 2=Medium, 3=High, 4=On, 5=Auto, 6=Smart", enumValues = mapOf("Off" to 0L, "Low" to 1L, "Medium" to 2L, "High" to 3L, "On" to 4L, "Auto" to 5L, "Smart" to 6L), naturalSynonyms = listOf("fan mode", "fan speed preset")),
      0x0001L to MatterAttributeMeta(0x0001L, "FanModeSequence", MatterDataType.ENUM8, description = "Supported fan modes sequence"),
      0x0002L to MatterAttributeMeta(0x0002L, "PercentSetting", MatterDataType.UINT8, isWritable = true, description = "Target fan speed percent (0..100)", unit = "%", naturalSynonyms = listOf("fan percent", "fan speed")),
      0x0003L to MatterAttributeMeta(0x0003L, "PercentCurrent", MatterDataType.UINT8, description = "Current fan speed percent (0..100)", unit = "%"),
      0x0004L to MatterAttributeMeta(0x0004L, "SpeedMax", MatterDataType.UINT8, description = "Max fan speed steps"),
      0x0005L to MatterAttributeMeta(0x0005L, "SpeedSetting", MatterDataType.UINT8, isWritable = true, description = "Target speed step"),
      0x0006L to MatterAttributeMeta(0x0006L, "SpeedCurrent", MatterDataType.UINT8, description = "Current speed step"),
      0x0007L to MatterAttributeMeta(0x0007L, "WindSupport", MatterDataType.BITMAP8, description = "Supported wind effects: Sleep, Natural"),
      0x0008L to MatterAttributeMeta(0x0008L, "WindSetting", MatterDataType.BITMAP8, isWritable = true, description = "Current wind mode")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_FAN_CONTROL, "FanControl", ClusterCategory.HVAC,
      "Controls ceiling fans, ventilation fans, and air circulators.",
      fanCommands, fanAttributes, listOf("fan", "ceiling fan", "ventilator")
    ))
  }

  // =========================================================================
  // 4. Media Clusters
  // =========================================================================
  private fun registerMediaClusters() {
    // Media Playback (0x0506)
    val mediaCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "Play", "Resume media playback", naturalSynonyms = listOf("play", "resume", "continue")),
      0x01L to MatterCommandMeta(0x01L, "Pause", "Pause media playback", naturalSynonyms = listOf("pause", "hold playback")),
      0x02L to MatterCommandMeta(0x02L, "Stop", "Stop media playback", naturalSynonyms = listOf("stop playing", "stop video", "stop music")),
      0x03L to MatterCommandMeta(0x03L, "StartOver", "Restart current media from beginning", naturalSynonyms = listOf("restart", "start over")),
      0x04L to MatterCommandMeta(0x04L, "Previous", "Play previous track/item", naturalSynonyms = listOf("previous track", "previous song", "go back")),
      0x05L to MatterCommandMeta(0x05L, "Next", "Play next track/item", naturalSynonyms = listOf("next track", "next song", "skip")),
      0x06L to MatterCommandMeta(0x06L, "Rewind", "Rewind media playback", naturalSynonyms = listOf("rewind")),
      0x07L to MatterCommandMeta(0x07L, "FastForward", "Fast-forward media playback", naturalSynonyms = listOf("fast forward")),
      0x08L to MatterCommandMeta(0x08L, "SkipForward", "Skip forward by milliseconds", requestFields = listOf(
        MatterFieldMeta(0, "deltaPositionMilliseconds", MatterDataType.UINT64, description = "Milliseconds to skip forward")
      ), naturalSynonyms = listOf("skip forward 30 seconds", "jump forward")),
      0x09L to MatterCommandMeta(0x09L, "SkipBackward", "Skip backward by milliseconds", requestFields = listOf(
        MatterFieldMeta(0, "deltaPositionMilliseconds", MatterDataType.UINT64, description = "Milliseconds to skip backward")
      ), naturalSynonyms = listOf("skip backward 10 seconds", "jump back")),
      0x0BL to MatterCommandMeta(0x0BL, "Seek", "Seek to absolute position in milliseconds", requestFields = listOf(
        MatterFieldMeta(0, "position", MatterDataType.UINT64, description = "Position in milliseconds")
      ), naturalSynonyms = listOf("seek to", "jump to"))
    )
    val mediaAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "CurrentState", MatterDataType.ENUM8, description = "0=Playing, 1=Paused, 2=NotPlaying, 3=Buffering", enumValues = mapOf("Playing" to 0L, "Paused" to 1L, "NotPlaying" to 2L, "Buffering" to 3L), naturalSynonyms = listOf("playback state")),
      0x0001L to MatterAttributeMeta(0x0001L, "StartTime", MatterDataType.UINT64, isNullable = true, description = "Playback start time"),
      0x0002L to MatterAttributeMeta(0x0002L, "Duration", MatterDataType.UINT64, isNullable = true, description = "Total duration in ms"),
      0x0004L to MatterAttributeMeta(0x0004L, "PlaybackSpeed", MatterDataType.FLOAT32, description = "Current playback speed multiplier")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_MEDIA_PLAYBACK, "MediaPlayback", ClusterCategory.MEDIA,
      "Controls media playback, play, pause, stop, rewind, and seek operations on TVs and speakers.",
      mediaCommands, mediaAttributes, listOf("tv", "speaker", "media player", "player")
    ))

    // Keypad Input (0x0509)
    val keypadCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "SendKey", "Send CEC / Keypad key code", requestFields = listOf(
        MatterFieldMeta(0, "keyCode", MatterDataType.ENUM8, description = "Key code e.g. 0=Select, 1=Up, 2=Down, 3=Left, 4=Right, 13=Back, 24=Home")
      ), naturalSynonyms = listOf("press key", "send remote key", "go home", "press back"))
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_KEYPAD_INPUT, "KeypadInput", ClusterCategory.MEDIA,
      "Simulates remote keypad input on media receivers and displays.",
      keypadCommands, emptyMap(), listOf("remote", "keypad")
    ))

    // Audio Output & Volume Control (0x050B)
    val audioCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "VolumeUp", "Increase volume step", naturalSynonyms = listOf("volume up", "turn up volume", "louder")),
      0x01L to MatterCommandMeta(0x01L, "VolumeDown", "Decrease volume step", naturalSynonyms = listOf("volume down", "turn down volume", "quieter")),
      0x02L to MatterCommandMeta(0x02L, "Mute", "Mute audio output", naturalSynonyms = listOf("mute", "silence")),
      0x03L to MatterCommandMeta(0x03L, "Unmute", "Unmute audio output", naturalSynonyms = listOf("unmute"))
    )
    val audioAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "Volume", MatterDataType.UINT8, isWritable = true, description = "Audio volume level 0..100", unit = "%", naturalSynonyms = listOf("volume", "sound level")),
      0x0001L to MatterAttributeMeta(0x0001L, "Muted", MatterDataType.BOOLEAN, isWritable = true, description = "Mute state", naturalSynonyms = listOf("is muted"))
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_AUDIO_OUTPUT, "AudioOutput", ClusterCategory.MEDIA,
      "Controls volume, mute, and speaker output channels.",
      audioCommands, audioAttributes, listOf("volume", "audio", "soundbar", "speaker volume")
    ))
  }

  // =========================================================================
  // 5. Appliances & Robotics Clusters
  // =========================================================================
  private fun registerApplianceAndRoboticsClusters() {
    // Mode Select (0x0050)
    val modeSelectCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "ChangeToMode", "Change mode to target mode integer", requestFields = listOf(
        MatterFieldMeta(0, "newMode", MatterDataType.UINT8, description = "Target mode value 0..255")
      ), naturalSynonyms = listOf("change mode", "switch mode", "set mode"))
    )
    val modeSelectAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "Description", MatterDataType.UTF8_STRING, description = "Mode description"),
      0x0001L to MatterAttributeMeta(0x0001L, "StandardNamespace", MatterDataType.UINT16, description = "Standard namespace identifier"),
      0x0003L to MatterAttributeMeta(0x0003L, "CurrentMode", MatterDataType.UINT8, description = "Current selected mode", naturalSynonyms = listOf("current mode"))
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_MODE_SELECT, "ModeSelect", ClusterCategory.APPLIANCES_AND_ROBOTICS,
      "Generic multi-mode selection for appliances and smart devices.",
      modeSelectCommands, modeSelectAttributes, listOf("mode select", "appliance mode")
    ))

    // RVC Run Mode (0x0054)
    val rvcRunModeCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "ChangeToMode", "Change RVC run mode: 0=Idle, 1=Cleaning, 2=Mapping, 3=ReturningToDock", requestFields = listOf(
        MatterFieldMeta(0, "newMode", MatterDataType.UINT8, description = "0=Idle, 1=Cleaning, 2=Mapping, 3=ReturningToDock")
      ), naturalSynonyms = listOf("start vacuum", "start cleaning", "vacuum room", "dock vacuum", "send vacuum home", "charge vacuum"))
    )
    val rvcRunModeAttributes = mapOf(
      0x0001L to MatterAttributeMeta(0x0001L, "CurrentMode", MatterDataType.UINT8, description = "0=Idle, 1=Cleaning, 2=Mapping, 3=ReturningToDock", enumValues = mapOf("Idle" to 0L, "Cleaning" to 1L, "Mapping" to 2L, "ReturningToDock" to 3L), naturalSynonyms = listOf("vacuum status", "vacuum state"))
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_RVC_RUN_MODE, "RvcRunMode", ClusterCategory.APPLIANCES_AND_ROBOTICS,
      "Controls Robotic Vacuum Cleaner run mode, docking, and cleaning cycles.",
      rvcRunModeCommands, rvcRunModeAttributes, listOf("vacuum", "robot vacuum", "roomba", "rvc")
    ))

    // RVC Clean Mode (0x0055)
    val rvcCleanModeCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "ChangeToMode", "Change cleaning intensity mode: 0=Vacuum, 1=Mop, 2=VacuumAndMop, 3=DeepClean", requestFields = listOf(
        MatterFieldMeta(0, "newMode", MatterDataType.UINT8, description = "0=Vacuum, 1=Mop, 2=VacuumAndMop, 3=DeepClean")
      ), naturalSynonyms = listOf("set vacuum mode", "mop floor", "vacuum and mop"))
    )
    val rvcCleanModeAttributes = mapOf(
      0x0001L to MatterAttributeMeta(0x0001L, "CurrentMode", MatterDataType.UINT8, description = "Current clean mode")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_RVC_CLEAN_MODE, "RvcCleanMode", ClusterCategory.APPLIANCES_AND_ROBOTICS,
      "Controls Robotic Vacuum Cleaner cleaning mode (vacuum, mop, deep clean).",
      rvcCleanModeCommands, rvcCleanModeAttributes, listOf("vacuum clean mode", "mop mode")
    ))

    // Operational State (0x0060)
    val operationalStateCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "Pause", "Pause ongoing appliance cycle", naturalSynonyms = listOf("pause appliance", "pause washer", "pause dryer")),
      0x01L to MatterCommandMeta(0x01L, "Stop", "Stop ongoing appliance cycle", naturalSynonyms = listOf("stop appliance", "cancel cycle")),
      0x02L to MatterCommandMeta(0x02L, "Start", "Start configured appliance cycle", naturalSynonyms = listOf("start appliance", "start cycle", "start washer", "start dishwasher")),
      0x03L to MatterCommandMeta(0x03L, "Resume", "Resume paused cycle", naturalSynonyms = listOf("resume appliance", "resume cycle"))
    )
    val operationalStateAttributes = mapOf(
      0x0001L to MatterAttributeMeta(0x0001L, "CurrentPhase", MatterDataType.UINT8, isNullable = true, description = "Current operation phase number"),
      0x0004L to MatterAttributeMeta(0x0004L, "OperationalState", MatterDataType.ENUM8, description = "0=Stopped, 1=Running, 2=Paused, 3=Error", enumValues = mapOf("Stopped" to 0L, "Running" to 1L, "Paused" to 2L, "Error" to 3L), naturalSynonyms = listOf("appliance status", "washer status", "dryer status"))
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_OPERATIONAL_STATE, "OperationalState", ClusterCategory.APPLIANCES_AND_ROBOTICS,
      "Controls start/pause/stop and monitors operational phases of washers, dryers, and dishwashers.",
      operationalStateCommands, operationalStateAttributes, listOf("washer", "dryer", "dishwasher", "oven", "appliance")
    ))
  }

  // =========================================================================
  // 6. Energy Management Clusters
  // =========================================================================
  private fun registerEnergyClusters() {
    // Energy EVSE (0x0099)
    val evseCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "Disable", "Disable EVSE charger"),
      0x01L to MatterCommandMeta(0x01L, "EnableCharging", "Enable EV charging until expiry time or duration", requestFields = listOf(
        MatterFieldMeta(0, "chargingEnabledUntil", MatterDataType.UINT32, isNullable = true, description = "Epoch seconds or null"),
        MatterFieldMeta(1, "minimumChargeCurrent", MatterDataType.INT64, description = "Milliamperes"),
        MatterFieldMeta(2, "maximumChargeCurrent", MatterDataType.INT64, description = "Milliamperes")
      ), naturalSynonyms = listOf("enable ev charging", "enable charger")),
      0x02L to MatterCommandMeta(0x02L, "StartCharge", "Start EV charging immediately", naturalSynonyms = listOf("charge car", "start charging car", "start ev charging", "charge ev")),
      0x03L to MatterCommandMeta(0x03L, "StopCharge", "Stop EV charging immediately", naturalSynonyms = listOf("stop charging car", "stop ev charging", "pause ev charger")),
      0x06L to MatterCommandMeta(0x06L, "SetMaxChargeRate", "Set maximum EV charging current rate in mA", requestFields = listOf(
        MatterFieldMeta(0, "maxChargeCurrent", MatterDataType.INT64, description = "Milliamperes (e.g. 32000 for 32A)")
      ), naturalSynonyms = listOf("set ev charge rate", "set car charging speed", "set charging current"))
    )
    val evseAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "State", MatterDataType.ENUM8, description = "0=NotPluggedIn, 1=PluggedInNoDemand, 2=PluggedInDemand, 3=PluggedInCharging, 4=PluggedInDischarging, 5=SessionEnding, 6=Fault", enumValues = mapOf("NotPluggedIn" to 0L, "PluggedInNoDemand" to 1L, "PluggedInDemand" to 2L, "PluggedInCharging" to 3L, "PluggedInDischarging" to 4L, "SessionEnding" to 5L, "Fault" to 6L), naturalSynonyms = listOf("evse status", "ev charging state", "is car plugged in")),
      0x0001L to MatterAttributeMeta(0x0001L, "SupplyState", MatterDataType.ENUM8, description = "0=Disabled, 1=ChargingEnabled, 2=DischargingEnabled, 3=DisabledError"),
      0x0005L to MatterAttributeMeta(0x0005L, "CircuitCapacity", MatterDataType.INT64, description = "Circuit capacity in mA"),
      0x0007L to MatterAttributeMeta(0x0007L, "MaximumChargeCurrent", MatterDataType.INT64, description = "Max charge current in mA"),
      0x0009L to MatterAttributeMeta(0x0009L, "UserMaximumChargeCurrent", MatterDataType.INT64, isWritable = true, description = "User defined max current in mA")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_ENERGY_EVSE, "EnergyEVSE", ClusterCategory.ENERGY_MANAGEMENT,
      "Controls Electric Vehicle Supply Equipment (EVSE), EV chargers, and vehicle charging limits.",
      evseCommands, evseAttributes, listOf("evse", "ev charger", "car charger", "electric vehicle")
    ))

    // Device Energy Management (0x0098)
    val demCommands = mapOf(
      0x00L to MatterCommandMeta(0x00L, "PowerAdjustRequest", "Request power consumption adjustment", requestFields = listOf(
        MatterFieldMeta(0, "power", MatterDataType.INT64, description = "Target power in mW"),
        MatterFieldMeta(1, "duration", MatterDataType.UINT32, description = "Duration in seconds")
      ), naturalSynonyms = listOf("limit power usage", "adjust power", "eco power mode")),
      0x01L to MatterCommandMeta(0x01L, "CancelPowerAdjustRequest", "Cancel power adjustment request", naturalSynonyms = listOf("cancel power adjustment"))
    )
    val demAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "ESAType", MatterDataType.ENUM8, description = "Energy Smart Appliance Type"),
      0x0001L to MatterAttributeMeta(0x0001L, "ESACanGenerate", MatterDataType.BOOLEAN, description = "Can generate power (solar/battery)"),
      0x0002L to MatterAttributeMeta(0x0002L, "ESAState", MatterDataType.ENUM8, description = "0=Offline, 1=Online, 2=Fault, 3=PowerAdjustActive"),
      0x0003L to MatterAttributeMeta(0x0003L, "AbsMinPower", MatterDataType.INT64, description = "Absolute min power in mW"),
      0x0004L to MatterAttributeMeta(0x0004L, "AbsMaxPower", MatterDataType.INT64, description = "Absolute max power in mW")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_DEVICE_ENERGY_MGMT, "DeviceEnergyManagement", ClusterCategory.ENERGY_MANAGEMENT,
      "Manages energy load shedding, solar generation, and home power storage.",
      demCommands, demAttributes, listOf("energy management", "power meter", "solar", "battery storage")
    ))
  }

  // =========================================================================
  // 7. Sensors & Alarms Clusters
  // =========================================================================
  private fun registerSensorAndAlarmClusters() {
    // Temperature Measurement (0x0402)
    val tempAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "MeasuredValue", MatterDataType.INT16, description = "Temperature in 100ths of °C (e.g. 2350 = 23.5°C)", unit = "°C", naturalSynonyms = listOf("temperature", "temp", "current temperature")),
      0x0001L to MatterAttributeMeta(0x0001L, "MinMeasuredValue", MatterDataType.INT16, description = "Min measurable temperature"),
      0x0002L to MatterAttributeMeta(0x0002L, "MaxMeasuredValue", MatterDataType.INT16, description = "Max measurable temperature")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_TEMPERATURE_MEASUREMENT, "TemperatureMeasurement", ClusterCategory.SENSORS_AND_ALARMS,
      "Reports environmental temperature readings.",
      emptyMap(), tempAttributes, listOf("temperature sensor", "thermometer")
    ))

    // Relative Humidity Measurement (0x0405)
    val humidityAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "MeasuredValue", MatterDataType.UINT16, description = "Relative humidity in 100ths of % (e.g. 4500 = 45.0%)", unit = "%", naturalSynonyms = listOf("humidity", "relative humidity")),
      0x0001L to MatterAttributeMeta(0x0001L, "MinMeasuredValue", MatterDataType.UINT16, description = "Min measurable humidity"),
      0x0002L to MatterAttributeMeta(0x0002L, "MaxMeasuredValue", MatterDataType.UINT16, description = "Max measurable humidity")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_RELATIVE_HUMIDITY_MEASUREMENT, "RelativeHumidityMeasurement", ClusterCategory.SENSORS_AND_ALARMS,
      "Reports environmental relative humidity percentage.",
      emptyMap(), humidityAttributes, listOf("humidity sensor", "hygrometer")
    ))

    // Occupancy Sensing (0x0406)
    val occupancyAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "Occupancy", MatterDataType.BITMAP8, description = "Bit 0: 1=Occupied, 0=Unoccupied", naturalSynonyms = listOf("occupancy", "presence", "is anyone in room")),
      0x0001L to MatterAttributeMeta(0x0001L, "OccupancySensorType", MatterDataType.ENUM8, description = "0=PIR, 1=Ultrasonic, 2=PIRAndUltrasonic, 3=PhysicalContact")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_OCCUPANCY_SENSING, "OccupancySensing", ClusterCategory.SENSORS_AND_ALARMS,
      "Reports motion, room occupancy, and human presence.",
      emptyMap(), occupancyAttributes, listOf("motion sensor", "occupancy sensor", "presence sensor")
    ))

    // Boolean State / Contact Sensor (0x0045)
    val booleanStateAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "StateValue", MatterDataType.BOOLEAN, description = "Boolean state: true (contact open / sensor active) / false (contact closed)", naturalSynonyms = listOf("contact state", "is window open", "is door open"))
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_BOOLEAN_STATE, "BooleanState", ClusterCategory.SENSORS_AND_ALARMS,
      "Reports binary contact sensor states for doors, windows, and gates.",
      emptyMap(), booleanStateAttributes, listOf("contact sensor", "door sensor", "window sensor")
    ))

    // Smoke & CO Alarm (0x005C)
    val smokeCoAttributes = mapOf(
      0x0000L to MatterAttributeMeta(0x0000L, "SmokeState", MatterDataType.ENUM8, description = "0=OK, 1=Warning, 2=Critical", enumValues = mapOf("OK" to 0L, "Warning" to 1L, "Critical" to 2L), naturalSynonyms = listOf("smoke status", "smoke alarm")),
      0x0001L to MatterAttributeMeta(0x0001L, "COState", MatterDataType.ENUM8, description = "0=OK, 1=Warning, 2=Critical", enumValues = mapOf("OK" to 0L, "Warning" to 1L, "Critical" to 2L), naturalSynonyms = listOf("carbon monoxide status", "co alarm")),
      0x0002L to MatterAttributeMeta(0x0002L, "BatteryAlert", MatterDataType.ENUM8, description = "0=OK, 1=Warning, 2=Critical", naturalSynonyms = listOf("alarm battery")),
      0x0003L to MatterAttributeMeta(0x0003L, "DeviceMuted", MatterDataType.ENUM8, description = "0=NotMuted, 1=Muted")
    )
    registerCluster(MatterClusterMeta(
      CLUSTER_SMOKE_CO_ALARM, "SmokeCOAlarm", ClusterCategory.SENSORS_AND_ALARMS,
      "Reports smoke detection, carbon monoxide levels, and alarm test states.",
      emptyMap(), smokeCoAttributes, listOf("smoke detector", "co detector", "fire alarm")
    ))
  }
}
