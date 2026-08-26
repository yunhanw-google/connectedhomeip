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
import com.google.chip.chiptool.voice.MatterAttributeMeta
import com.google.chip.chiptool.voice.MatterClusterMeta
import com.google.chip.chiptool.voice.MatterClusterMetaRegistry
import com.google.chip.chiptool.voice.MatterCommandMeta
import com.google.chip.chiptool.voice.MatterDataType
import com.google.chip.chiptool.voice.MatterDeviceTypes

/**
 * UI control widget types supported by the in-place interactive attribute editor.
 */
enum class AttributeControlType {
  SWITCH,
  SLIDER_NUMERIC,
  SLIDER_COLOR_TEMP,
  COLOR_PICKER,
  DROPDOWN_ENUM,
  READONLY_TEXT,
  INPLACE_TEXT_INPUT
}

/**
 * Visual badge classification for device cards in the room dashboard.
 */
enum class DeviceBadgeType {
  ONLINE,
  OFFLINE,
  UNASSIGNED,
  BATTERY,
  STATE_ON,
  STATE_OFF,
  STATE_LOCKED,
  STATE_UNLOCKED,
  TEMPERATURE,
  HUMIDITY,
  FAN_SPEED,
  BLINDS_LIFT,
  POWER_WATTS,
  MODE_TEXT
}

/**
 * Live status badge displayed on a device card.
 */
data class LiveBadge(
  val type: DeviceBadgeType,
  val label: String,
  val value: String = "",
  val colorHex: String = "#4CAF50",
  val isWarning: Boolean = false
)

/**
 * State representing a controllable or viewable attribute in the Endpoint Inspector.
 */
data class WritableAttributeState(
  val endpointId: Int,
  val clusterId: Long,
  val clusterName: String,
  val attributeId: Long,
  val attributeName: String,
  val dataType: MatterDataType,
  val isWritable: Boolean,
  val isReportable: Boolean,
  val currentValue: Any?,
  val formattedValue: String,
  val controlType: AttributeControlType,
  val minValue: Double? = null,
  val maxValue: Double? = null,
  val step: Double? = null,
  val enumOptions: Map<String, Long> = emptyMap(),
  val unit: String? = null,
  val isOptimistic: Boolean = false
)

/**
 * Descriptor for a Matter cluster within an endpoint.
 */
data class ClusterDescriptor(
  val clusterId: Long,
  val clusterName: String,
  val description: String,
  val isServer: Boolean,
  val attributes: List<WritableAttributeState> = emptyList(),
  val commands: List<MatterCommandMeta> = emptyList(),
  val category: String = "General"
)

/**
 * Descriptor for a Matter Endpoint (Endpoint 0..N).
 */
data class EndpointDescriptor(
  val endpointId: Int,
  val deviceTypeId: Long,
  val deviceTypeName: String,
  val isRootEndpoint: Boolean = (endpointId == 0),
  val serverClusters: List<ClusterDescriptor> = emptyList(),
  val clientClusters: List<ClusterDescriptor> = emptyList()
)

/**
 * Primary quick action type for 1-tap manipulation on device cards.
 */
sealed class QuickControlAction {
  data class ToggleSwitch(val isOn: Boolean, val endpointId: Int) : QuickControlAction()
  data class BrightnessSlider(val levelPercent: Int, val endpointId: Int) : QuickControlAction()
  data class LockToggle(val isLocked: Boolean, val endpointId: Int) : QuickControlAction()
  data class ThermostatDial(val tempCelsius: Double, val endpointId: Int) : QuickControlAction()
  data class WindowShadeControl(val liftPercent: Int, val endpointId: Int) : QuickControlAction()
  object None : QuickControlAction()
}

/**
 * Presentation state for an individual device card in the room dashboard.
 */
data class DeviceCardState(
  val nodeId: Long,
  val nodeLabel: String,
  val roomName: String,
  val vendorName: String,
  val productName: String,
  val isOnline: Boolean,
  val batteryPercent: Int? = null,
  val aliases: List<String> = emptyList(),
  val badges: List<LiveBadge> = emptyList(),
  val endpoints: List<EndpointDescriptor> = emptyList(),
  val quickControl: QuickControlAction = QuickControlAction.None,
  val primaryEndpointId: Int = 1,
  val lastUpdatedTimestampMs: Long = System.currentTimeMillis()
)

/**
 * Grouped hierarchy section representing a specific room and its devices.
 */
data class RoomHierarchyItem(
  val roomName: String,
  val deviceCards: List<DeviceCardState>,
  val totalCount: Int = deviceCards.size,
  val activeCount: Int = deviceCards.count { it.isOnline }
)

/**
 * Complete fabric hierarchy state representing all rooms and devices.
 */
data class HomeFabricTopology(
  val fabricId: Long = 1L,
  val rooms: List<RoomHierarchyItem> = emptyList(),
  val totalDevicesCount: Int = rooms.sumOf { it.totalCount },
  val totalOnlineCount: Int = rooms.sumOf { it.activeCount }
)

/**
 * Factory utilities for constructing rich UI models from CommissionedNode records.
 */
object RoomDeviceHierarchyMapper {

  fun getLiveAttributeValue(liveValues: Map<String, Any?>, nodeId: Long?, epId: Int, clusterId: Long, attrId: Long): Any? {
    if (nodeId != null) {
      val k1 = "$nodeId:$epId:$clusterId:$attrId"
      if (liveValues.containsKey(k1)) return liveValues[k1]
      val hex = "0x" + nodeId.toString(16)
      val k2 = "$hex:$epId:$clusterId:$attrId"
      if (liveValues.containsKey(k2)) return liveValues[k2]
      val hexUpper = "0x" + nodeId.toString(16).uppercase()
      val k3 = "$hexUpper:$epId:$clusterId:$attrId"
      if (liveValues.containsKey(k3)) return liveValues[k3]
    }
    val k4 = "$epId:$clusterId:$attrId"
    if (liveValues.containsKey(k4)) return liveValues[k4]
    return null
  }

  fun toDeviceCardState(
    node: CommissionedNode,
    liveAttributeValues: Map<String, Any?> = emptyMap()
  ): DeviceCardState {
    val endpointDescriptors = node.endpoints.map { ep ->
      toEndpointDescriptor(ep, liveAttributeValues)
    }

    val badges = mutableListOf<LiveBadge>()
    if (node.isOnline) {
      badges.add(LiveBadge(DeviceBadgeType.ONLINE, "Online", "Online", colorHex = "#34A853"))
    } else {
      badges.add(LiveBadge(DeviceBadgeType.OFFLINE, "Offline", "Offline", colorHex = "#EA4335", isWarning = true))
    }

    if (node.roomName.equals("Unassigned", ignoreCase = true) || node.roomName.isBlank()) {
      badges.add(LiveBadge(DeviceBadgeType.UNASSIGNED, "Unassigned", "Unassigned", colorHex = "#757575", isWarning = false))
    }

    node.batteryPercent?.let {
      badges.add(
        LiveBadge(
          DeviceBadgeType.BATTERY,
          "Battery",
          "$it%",
          colorHex = if (it < 20) "#EA4335" else "#34A853",
          isWarning = it < 20
        )
      )
    }

    // Determine primary endpoint & quick controls
    val primaryEp = node.endpoints.firstOrNull { it.endpointId != 0 } ?: node.endpoints.firstOrNull()
    var quickAction: QuickControlAction = QuickControlAction.None

    primaryEp?.let { ep ->
      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_ON_OFF)) {
        val rawOn = getLiveAttributeValue(liveAttributeValues, node.nodeId, ep.endpointId, MatterClusterMetaRegistry.CLUSTER_ON_OFF, 0L)
        val isOn = (rawOn as? Boolean) ?: (node.liveStates["onOff"] as? Boolean) ?: false
        val onOffText = if (isOn) "ON" else "OFF"
        badges.add(
          LiveBadge(
            if (isOn) DeviceBadgeType.STATE_ON else DeviceBadgeType.STATE_OFF,
            onOffText,
            onOffText,
            colorHex = if (isOn) "#FBBC04" else "#9E9E9E"
          )
        )
        quickAction = QuickControlAction.ToggleSwitch(isOn, ep.endpointId)
      }

      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL)) {
        val rawLvl = getLiveAttributeValue(liveAttributeValues, node.nodeId, ep.endpointId, MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL, 0L)
        val level = (rawLvl as? Number)?.toInt() ?: 128
        val percent = (level * 100) / 254
        badges.add(LiveBadge(DeviceBadgeType.STATE_ON, "Brightness", "$percent%", colorHex = "#FBBC04"))
        if (quickAction is QuickControlAction.None) {
          quickAction = QuickControlAction.BrightnessSlider(percent, ep.endpointId)
        }
      }

      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK)) {
        val rawLock = getLiveAttributeValue(liveAttributeValues, node.nodeId, ep.endpointId, MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK, 0L)
        val lockState = (rawLock as? Number)?.toInt() ?: 1
        val isLocked = (lockState == 1)
        val lockText = if (isLocked) "Locked" else "Unlocked"
        badges.add(
          LiveBadge(
            if (isLocked) DeviceBadgeType.STATE_LOCKED else DeviceBadgeType.STATE_UNLOCKED,
            lockText,
            lockText,
            colorHex = if (isLocked) "#1A73E8" else "#EA4335",
            isWarning = !isLocked
          )
        )
        quickAction = QuickControlAction.LockToggle(isLocked, ep.endpointId)
      }

      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_THERMOSTAT)) {
        val rawTemp = (getLiveAttributeValue(liveAttributeValues, node.nodeId, ep.endpointId, MatterClusterMetaRegistry.CLUSTER_THERMOSTAT, 0L) as? Number)?.toDouble() ?: 2100.0
        val celsius = rawTemp / 100.0
        val tempText = String.format(java.util.Locale.US, "%.1f°C", celsius)
        badges.add(LiveBadge(DeviceBadgeType.TEMPERATURE, "Temp", tempText, colorHex = "#FF7043"))
        quickAction = QuickControlAction.ThermostatDial(celsius, ep.endpointId)
      }

      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING)) {
        val rawLift = getLiveAttributeValue(liveAttributeValues, node.nodeId, ep.endpointId, MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING, 8L)
        val liftVal = (rawLift as? Number)?.toInt() ?: 0
        badges.add(LiveBadge(DeviceBadgeType.BLINDS_LIFT, "Lift", "$liftVal%", colorHex = "#7E57C2"))
        quickAction = QuickControlAction.WindowShadeControl(liftVal, ep.endpointId)
      }
    }

    return DeviceCardState(
      nodeId = node.nodeId,
      nodeLabel = node.nodeLabel,
      roomName = node.roomName,
      vendorName = node.vendorName,
      productName = node.productName,
      isOnline = node.isOnline,
      batteryPercent = node.batteryPercent,
      aliases = node.aliases,
      badges = badges,
      endpoints = endpointDescriptors,
      quickControl = quickAction,
      primaryEndpointId = primaryEp?.endpointId ?: 1
    )
  }

  fun toEndpointDescriptor(
    ep: CommissionedEndpoint,
    liveAttributeValues: Map<String, Any?> = emptyMap()
  ): EndpointDescriptor {
    val serverClusters = ep.serverClusters.mapNotNull { clusterId ->
      val meta = MatterClusterMetaRegistry.getCluster(clusterId) ?: return@mapNotNull null
      val attrStates = meta.attributes.values.map { attr ->
        toWritableAttributeState(ep.endpointId, meta, attr, liveAttributeValues)
      }
      ClusterDescriptor(
        clusterId = clusterId,
        clusterName = meta.name,
        description = meta.description,
        isServer = true,
        attributes = attrStates,
        commands = meta.commands.values.toList(),
        category = categorizeCluster(clusterId)
      )
    }

    val clientClusters = ep.clientClusters.mapNotNull { clusterId ->
      val meta = MatterClusterMetaRegistry.getCluster(clusterId) ?: return@mapNotNull null
      ClusterDescriptor(
        clusterId = clusterId,
        clusterName = meta.name,
        description = meta.description,
        isServer = false,
        category = categorizeCluster(clusterId)
      )
    }

    return EndpointDescriptor(
      endpointId = ep.endpointId,
      deviceTypeId = ep.deviceTypeId,
      deviceTypeName = ep.deviceTypeName,
      isRootEndpoint = (ep.endpointId == 0),
      serverClusters = serverClusters,
      clientClusters = clientClusters
    )
  }

  fun toWritableAttributeState(
    endpointId: Int,
    clusterMeta: MatterClusterMeta,
    attrMeta: MatterAttributeMeta,
    liveValues: Map<String, Any?>
  ): WritableAttributeState {
    val value = getLiveAttributeValue(liveValues, null, endpointId, clusterMeta.clusterId, attrMeta.attributeId) ?: attrMeta.defaultValue
    val controlType = resolveControlType(clusterMeta.clusterId, attrMeta)

    return WritableAttributeState(
      endpointId = endpointId,
      clusterId = clusterMeta.clusterId,
      clusterName = clusterMeta.name,
      attributeId = attrMeta.attributeId,
      attributeName = attrMeta.name,
      dataType = attrMeta.type,
      isWritable = attrMeta.isWritable,
      isReportable = attrMeta.isReportable,
      currentValue = value,
      formattedValue = formatDisplayValue(clusterMeta.clusterId, attrMeta, value),
      controlType = controlType,
      minValue = resolveMinValue(attrMeta),
      maxValue = resolveMaxValue(attrMeta),
      step = if (attrMeta.type == MatterDataType.FLOAT32) 0.5 else 1.0,
      enumOptions = attrMeta.enumValues,
      unit = attrMeta.unit
    )
  }

  private fun resolveControlType(clusterId: Long, attr: MatterAttributeMeta): AttributeControlType {
    if (clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL && attr.name.contains("ColorTemperature", ignoreCase = true)) {
      return AttributeControlType.SLIDER_COLOR_TEMP
    }
    if (clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL && attr.name.contains("Color", ignoreCase = true)) {
      return AttributeControlType.COLOR_PICKER
    }
    if (!attr.isWritable) return AttributeControlType.READONLY_TEXT

    return when {
      attr.type == MatterDataType.BOOLEAN -> AttributeControlType.SWITCH
      attr.enumValues.isNotEmpty() -> AttributeControlType.DROPDOWN_ENUM
      attr.type in listOf(MatterDataType.UINT8, MatterDataType.UINT16, MatterDataType.INT16, MatterDataType.FLOAT32) ->
        AttributeControlType.SLIDER_NUMERIC
      else -> AttributeControlType.INPLACE_TEXT_INPUT
    }
  }

  private fun resolveMinValue(attr: MatterAttributeMeta): Double? {
    return when (attr.type) {
      MatterDataType.UINT8 -> 0.0
      MatterDataType.UINT16 -> 0.0
      MatterDataType.INT16 -> -32768.0
      MatterDataType.FLOAT32 -> 0.0
      else -> null
    }
  }

  private fun resolveMaxValue(attr: MatterAttributeMeta): Double? {
    return when (attr.type) {
      MatterDataType.UINT8 -> 254.0
      MatterDataType.UINT16 -> 65535.0
      MatterDataType.INT16 -> 32767.0
      MatterDataType.FLOAT32 -> 100.0
      else -> null
    }
  }

  private fun formatDisplayValue(clusterId: Long, attr: MatterAttributeMeta, value: Any?): String {
    if (value == null) return "N/A"

    if (clusterId == MatterClusterMetaRegistry.CLUSTER_THERMOSTAT && attr.attributeId == 0x0000L) {
      val centi = (value as? Number)?.toDouble() ?: return "$value"
      return String.format("%.1f °C", centi / 100.0)
    }

    if (clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL && attr.name.contains("ColorTemperature", ignoreCase = true)) {
      val mireds = (value as? Number)?.toLong() ?: return "$value Mireds"
      if (mireds > 0) {
        val kelvin = 1_000_000L / mireds
        return "$kelvin K ($mireds Mireds)"
      }
    }

    if (attr.enumValues.isNotEmpty()) {
      val numVal = (value as? Number)?.toLong()
      val matchedName = attr.enumValues.entries.firstOrNull { it.value == numVal }?.key
      if (matchedName != null) return matchedName
    }

    return if (attr.unit != null) "$value ${attr.unit}" else "$value"
  }

  private fun categorizeCluster(clusterId: Long): String {
    return when (clusterId) {
      MatterClusterMetaRegistry.CLUSTER_ON_OFF,
      MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL,
      MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL -> "Lighting & Power"

      MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK,
      MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING,
      MatterClusterMetaRegistry.CLUSTER_BARRIER_CONTROL -> "Closures"

      MatterClusterMetaRegistry.CLUSTER_THERMOSTAT,
      MatterClusterMetaRegistry.CLUSTER_FAN_CONTROL -> "HVAC & Climate"

      MatterClusterMetaRegistry.CLUSTER_MEDIA_PLAYBACK,
      MatterClusterMetaRegistry.CLUSTER_KEYPAD_INPUT,
      MatterClusterMetaRegistry.CLUSTER_AUDIO_OUTPUT -> "Media & Entertainment"

      MatterClusterMetaRegistry.CLUSTER_MODE_SELECT,
      MatterClusterMetaRegistry.CLUSTER_RVC_RUN_MODE,
      MatterClusterMetaRegistry.CLUSTER_RVC_CLEAN_MODE,
      MatterClusterMetaRegistry.CLUSTER_OPERATIONAL_STATE -> "Appliances & Robotics"

      MatterClusterMetaRegistry.CLUSTER_ENERGY_EVSE,
      MatterClusterMetaRegistry.CLUSTER_DEVICE_ENERGY_MANAGEMENT -> "Energy Management"

      MatterClusterMetaRegistry.CLUSTER_TEMPERATURE_MEASUREMENT,
      MatterClusterMetaRegistry.CLUSTER_RELATIVE_HUMIDITY_MEASUREMENT,
      MatterClusterMetaRegistry.CLUSTER_OCCUPANCY_SENSING,
      MatterClusterMetaRegistry.CLUSTER_BOOLEAN_STATE,
      MatterClusterMetaRegistry.CLUSTER_SMOKE_CO_ALARM -> "Sensors & Alarms"

      else -> "System / Custom"
    }
  }
}