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

import com.google.chip.chiptool.R
import com.google.chip.chiptool.voice.CommissionedEndpoint
import com.google.chip.chiptool.voice.CommissionedNode
import com.google.chip.chiptool.voice.CommissionedNodeRegistry
import com.google.chip.chiptool.voice.HomeStateCache
import com.google.chip.chiptool.voice.LinkPhase
import com.google.chip.chiptool.voice.LinkState
import com.google.chip.chiptool.voice.MatterAttributeMeta
import com.google.chip.chiptool.voice.MatterClusterMeta
import com.google.chip.chiptool.voice.MatterClusterMetaRegistry
import com.google.chip.chiptool.voice.MatterCommandMeta
import com.google.chip.chiptool.voice.MatterDataType

/** UI control widget types supported by the in-place interactive attribute editor. */
enum class AttributeControlType {
  SWITCH,
  SLIDER_NUMERIC,
  SLIDER_COLOR_TEMP,
  COLOR_PICKER,
  DROPDOWN_ENUM,
  READONLY_TEXT,
  INPLACE_TEXT_INPUT,
}

/** Visual badge classification for device cards in the room dashboard. */
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
  MODE_TEXT,
}

/** Live status badge displayed on a device card. */
data class LiveBadge(
  val type: DeviceBadgeType,
  val label: String,
  val value: String = "",
  val colorHex: String = "#4CAF50",
  val isWarning: Boolean = false,
)

/** State representing a controllable or viewable attribute in the Endpoint Inspector. */
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
  val isOptimistic: Boolean = false,
)

/** Descriptor for a Matter cluster within an endpoint. */
data class ClusterDescriptor(
  val clusterId: Long,
  val clusterName: String,
  val description: String,
  val isServer: Boolean,
  val attributes: List<WritableAttributeState> = emptyList(),
  val commands: List<MatterCommandMeta> = emptyList(),
  val category: String = "General",
)

/** Descriptor for a Matter Endpoint (Endpoint 0..N). */
data class EndpointDescriptor(
  val endpointId: Int,
  val deviceTypeId: Long,
  val deviceTypeName: String,
  val isRootEndpoint: Boolean = (endpointId == 0),
  val serverClusters: List<ClusterDescriptor> = emptyList(),
  val clientClusters: List<ClusterDescriptor> = emptyList(),
)

/** Primary quick action type for 1-tap manipulation on device cards. */
sealed class QuickControlAction {
  data class ToggleSwitch(val isOn: Boolean, val endpointId: Int) : QuickControlAction()

  data class BrightnessSlider(val levelPercent: Int, val endpointId: Int) : QuickControlAction()

  data class LockToggle(val isLocked: Boolean, val endpointId: Int) : QuickControlAction()

  data class ThermostatDial(val tempCelsius: Double, val endpointId: Int) : QuickControlAction()

  data class WindowShadeControl(val liftPercent: Int, val endpointId: Int) : QuickControlAction()

  object None : QuickControlAction()
}

/** Presentation state for an individual device card in the room dashboard. */
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
  val lastUpdatedTimestampMs: Long = 0L,
  val linkPhase: LinkPhase = LinkPhase.LIVE,
  val linkStatusText: String = "Live",
  val linkStatusColorHex: String = "#137333",
  val linkStatusIconRes: Int = R.drawable.ic_hub_live,
  val showConnectButton: Boolean = false,
  val sensorReadingText: String? = null,
  val switchEventText: String? = null,
  val voiceAnswer: String? = null,
  val isVoiceTargeted: Boolean = false,
)

/** Grouped hierarchy section representing a specific room and its devices. */
data class RoomHierarchyItem(
  val roomName: String,
  val deviceCards: List<DeviceCardState>,
  val totalCount: Int = deviceCards.size,
  val activeCount: Int = deviceCards.count { it.isOnline },
)

/** Complete fabric hierarchy state representing all rooms and devices. */
data class HomeFabricTopology(
  val fabricId: Long = 1L,
  val rooms: List<RoomHierarchyItem> = emptyList(),
  val totalDevicesCount: Int = rooms.sumOf { it.totalCount },
  val totalOnlineCount: Int = rooms.sumOf { it.activeCount },
)

/** Factory utilities for constructing rich UI models from CommissionedNode records. */
object RoomDeviceHierarchyMapper {

  fun getLiveAttributeValue(
    liveValues: Map<String, Any?>,
    nodeId: Long?,
    epId: Int,
    clusterId: Long,
    attrId: Long,
  ): Any? {
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

  fun formatAge(ageMs: Long): String =
    when {
      ageMs < 60_000L -> "just now"
      ageMs < 3_600_000L -> "${ageMs / 60_000L}m ago"
      ageMs < 86_400_000L -> "${ageMs / 3_600_000L}h ago"
      else -> "${ageMs / 86_400_000L}d ago"
    }

  fun toDeviceCardState(
    node: CommissionedNode,
    liveAttributeValues: Map<String, Any?> = emptyMap(),
    linkState: LinkState? = null,
    isVoiceTargeted: Boolean = false,
    voiceAnswer: String? = null,
  ): DeviceCardState {
    val endpointsToProcess =
      if (node.endpoints.none { it.endpointId == 0 }) {
        listOf(
          CommissionedEndpoint(
            endpointId = 0,
            deviceTypeId = 0x0016L,
            deviceTypeName = "Root Node",
            serverClusters = CommissionedNodeRegistry.STANDARD_ROOT_NODE_CLUSTERS,
          )
        ) + node.endpoints
      } else {
        node.endpoints
      }

    val endpointDescriptors = endpointsToProcess.map { ep ->
      toEndpointDescriptor(ep, liveAttributeValues)
    }

    val now = System.currentTimeMillis()
    val phase = linkState?.phase ?: if (node.isOnline) LinkPhase.LIVE else LinkPhase.NOT_CONNECTED

    val statusText: String
    val statusColorHex: String
    val statusIconRes: Int
    val showConnect: Boolean

    when (phase) {
      LinkPhase.LIVE -> {
        statusText = "Live"
        statusColorHex = "#137333"
        statusIconRes = R.drawable.ic_hub_live
        showConnect = false
      }
      LinkPhase.CONNECTING,
      LinkPhase.PRIMING -> {
        statusText = "Connecting…"
        statusColorHex = "#5F6368"
        statusIconRes = R.drawable.ic_hub_connecting
        showConnect = false
      }
      LinkPhase.RECOVERING -> {
        statusText = "Still trying…"
        statusColorHex = "#5F6368"
        statusIconRes = R.drawable.ic_hub_connecting
        showConnect = false
      }
      LinkPhase.ASLEEP -> {
        val lastHeard = linkState?.lastHeardMs ?: 0L
        val ageStr = if (lastHeard > 0L) formatAge(now - lastHeard) else ""
        statusText = if (ageStr.isNotEmpty()) "Asleep · heard $ageStr" else "Asleep"
        statusColorHex = "#3C4F76"
        statusIconRes = R.drawable.ic_hub_asleep
        showConnect = false
      }
      LinkPhase.OFFLINE -> {
        if (!node.descriptorVerified) {
          statusText = "Not reachable · may be asleep"
          statusColorHex = "#3C4F76"
          statusIconRes = R.drawable.ic_hub_asleep
          showConnect = true
        } else {
          val reason = linkState?.reason
          statusText = if (!reason.isNullOrBlank()) "Offline · $reason" else "Offline"
          statusColorHex = "#B3261E"
          statusIconRes = R.drawable.ic_hub_offline
          showConnect = true
        }
      }
      LinkPhase.NOT_CONNECTED -> {
        statusText = "Not connected"
        statusColorHex = "#5F6368"
        statusIconRes = R.drawable.ic_hub_not_connected
        showConnect = true
      }
    }

    val badges = mutableListOf<LiveBadge>()
    if (phase == LinkPhase.LIVE) {
      badges.add(
        LiveBadge(DeviceBadgeType.ONLINE, statusText, statusText, colorHex = statusColorHex)
      )
    } else {
      badges.add(
        LiveBadge(
          DeviceBadgeType.OFFLINE,
          statusText,
          statusText,
          colorHex = statusColorHex,
          isWarning = (phase == LinkPhase.OFFLINE),
        )
      )
    }

    if (node.roomName.equals("Unassigned", ignoreCase = true) || node.roomName.isBlank()) {
      badges.add(
        LiveBadge(
          DeviceBadgeType.UNASSIGNED,
          "Unassigned",
          "Unassigned",
          colorHex = "#757575",
          isWarning = false,
        )
      )
    }

    val batAttr =
      HomeStateCache.get(node.nodeId, 0, MatterClusterMetaRegistry.CLUSTER_POWER_SOURCE, 0x000CL)
        ?: HomeStateCache.findAll(MatterClusterMetaRegistry.CLUSTER_POWER_SOURCE, 0x000CL)
          .firstOrNull { it.nodeId == node.nodeId }
    val resolvedBattery = (batAttr?.value as? Number)?.toInt()?.div(2) ?: node.batteryPercent

    resolvedBattery?.let {
      badges.add(
        LiveBadge(
          DeviceBadgeType.BATTERY,
          "Battery",
          "$it%",
          colorHex = if (it < 20) "#B3261E" else "#137333",
          isWarning = it < 20,
        )
      )
    }

    // Determine primary endpoint & quick controls (enabled in all link states)
    val primaryEp =
      node.endpoints.firstOrNull { it.endpointId != 0 } ?: node.endpoints.firstOrNull()
    var quickAction: QuickControlAction = QuickControlAction.None

    primaryEp?.let { ep ->
      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_ON_OFF)) {
        val rawOn =
          getLiveAttributeValue(
            liveAttributeValues,
            node.nodeId,
            ep.endpointId,
            MatterClusterMetaRegistry.CLUSTER_ON_OFF,
            0L,
          )
        val isOn = (rawOn as? Boolean) ?: (node.liveStates["onOff"] as? Boolean) ?: false
        val onOffText = if (isOn) "ON" else "OFF"
        badges.add(
          LiveBadge(
            if (isOn) DeviceBadgeType.STATE_ON else DeviceBadgeType.STATE_OFF,
            onOffText,
            onOffText,
            colorHex = if (isOn) "#FBBC04" else "#9E9E9E",
          )
        )
        quickAction = QuickControlAction.ToggleSwitch(isOn, ep.endpointId)
      }

      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL)) {
        val rawLvl =
          getLiveAttributeValue(
            liveAttributeValues,
            node.nodeId,
            ep.endpointId,
            MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL,
            0L,
          ) ?: node.liveStates["currentLevel"]
        val level = (rawLvl as? Number)?.toInt() ?: 128
        val percent = (level * 100) / 254
        if (rawLvl != null) {
          badges.add(
            LiveBadge(DeviceBadgeType.STATE_ON, "Brightness", "$percent%", colorHex = "#FBBC04")
          )
        }
        if (quickAction is QuickControlAction.None) {
          quickAction = QuickControlAction.BrightnessSlider(percent, ep.endpointId)
        }
      }

      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK)) {
        val rawLock =
          getLiveAttributeValue(
            liveAttributeValues,
            node.nodeId,
            ep.endpointId,
            MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK,
            0L,
          )
        val lockState = (rawLock as? Number)?.toInt() ?: 1
        val isLocked = (lockState == 1)
        val lockText = if (isLocked) "Locked" else "Unlocked"
        if (rawLock != null) {
          badges.add(
            LiveBadge(
              if (isLocked) DeviceBadgeType.STATE_LOCKED else DeviceBadgeType.STATE_UNLOCKED,
              lockText,
              lockText,
              colorHex = if (isLocked) "#1A73E8" else "#EA4335",
              isWarning = !isLocked,
            )
          )
        }
        quickAction = QuickControlAction.LockToggle(isLocked, ep.endpointId)
      }

      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_THERMOSTAT)) {
        val rawTempVal =
          getLiveAttributeValue(
            liveAttributeValues,
            node.nodeId,
            ep.endpointId,
            MatterClusterMetaRegistry.CLUSTER_THERMOSTAT,
            0L,
          )
        val rawTemp = (rawTempVal as? Number)?.toDouble() ?: 2100.0
        val celsius = rawTemp / 100.0
        val tempText = String.format(java.util.Locale.US, "%.1f°C", celsius)
        if (rawTempVal != null) {
          badges.add(LiveBadge(DeviceBadgeType.TEMPERATURE, "Temp", tempText, colorHex = "#FF7043"))
        }
        quickAction = QuickControlAction.ThermostatDial(celsius, ep.endpointId)
      }

      if (ep.supportsCluster(MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING)) {
        val rawLift =
          getLiveAttributeValue(
            liveAttributeValues,
            node.nodeId,
            ep.endpointId,
            MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING,
            8L,
          )
        val liftVal = (rawLift as? Number)?.toInt() ?: 0
        if (rawLift != null) {
          badges.add(
            LiveBadge(DeviceBadgeType.BLINDS_LIFT, "Lift", "$liftVal%", colorHex = "#7E57C2")
          )
        }
        quickAction = QuickControlAction.WindowShadeControl(liftVal, ep.endpointId)
      }
    }

    // Sensor readings from HomeStateCache
    val tempAttr =
      HomeStateCache.get(node.nodeId, 1, 0x0402L, 0x0000L)
        ?: HomeStateCache.findAll(0x0402L, 0x0000L).firstOrNull { it.nodeId == node.nodeId }
    val humAttr =
      HomeStateCache.get(node.nodeId, 1, 0x0405L, 0x0000L)
        ?: HomeStateCache.findAll(0x0405L, 0x0000L).firstOrNull { it.nodeId == node.nodeId }

    val readingParts = mutableListOf<String>()
    var readingAgeMs: Long? = null
    if (tempAttr?.value is Number) {
      val tempC = (tempAttr.value as Number).toDouble() / 100.0
      readingParts.add(String.format(java.util.Locale.US, "%.1f °C", tempC))
      readingAgeMs = now - tempAttr.updatedAtMs
    }
    if (humAttr?.value is Number) {
      val humPct = (humAttr.value as Number).toDouble() / 100.0
      readingParts.add(String.format(java.util.Locale.US, "%.0f%% humidity", humPct))
      if (readingAgeMs == null) readingAgeMs = now - humAttr.updatedAtMs
    }
    val sensorReadingText =
      if (readingParts.isNotEmpty()) {
        val ageStr =
          if (readingAgeMs != null && readingAgeMs > 0) " · ${formatAge(readingAgeMs)}" else ""
        readingParts.joinToString(" · ") + ageStr
      } else null

    // Switch event from HomeStateCache
    val lastSwitch = HomeStateCache.getLastSwitchEvent(node.nodeId)
    val switchEventText =
      if (lastSwitch != null) {
        val switchAgeStr = formatAge(now - lastSwitch.updatedAtMs)
        "Button ${lastSwitch.position} · ${lastSwitch.describePressType()} · $switchAgeStr"
      } else null

    return DeviceCardState(
      nodeId = node.nodeId,
      nodeLabel = node.nodeLabel,
      roomName = node.roomName,
      vendorName = node.vendorName,
      productName = node.productName,
      isOnline = (phase == LinkPhase.LIVE || phase == LinkPhase.ASLEEP || node.isOnline),
      batteryPercent = resolvedBattery,
      aliases = node.aliases,
      badges = badges,
      endpoints = endpointDescriptors,
      quickControl = quickAction,
      primaryEndpointId = primaryEp?.endpointId ?: 1,
      linkPhase = phase,
      linkStatusText = statusText,
      linkStatusColorHex = statusColorHex,
      linkStatusIconRes = statusIconRes,
      showConnectButton = showConnect,
      sensorReadingText = sensorReadingText,
      switchEventText = switchEventText,
      voiceAnswer = voiceAnswer,
      isVoiceTargeted = isVoiceTargeted,
    )
  }

  fun toEndpointDescriptor(
    ep: CommissionedEndpoint,
    liveAttributeValues: Map<String, Any?> = emptyMap(),
  ): EndpointDescriptor {
    val serverClusters =
      ep.serverClusters.mapNotNull { clusterId ->
        val meta = MatterClusterMetaRegistry.getClusterOrDefault(clusterId)
        val attrStates =
          meta.attributes.values.map { attr ->
            toWritableAttributeState(ep.endpointId, meta, attr, liveAttributeValues)
          }
        ClusterDescriptor(
          clusterId = clusterId,
          clusterName = meta.name,
          description = meta.description,
          isServer = true,
          attributes = attrStates,
          commands = meta.commands.values.toList(),
          category = categorizeCluster(clusterId),
        )
      }

    val clientClusters =
      ep.clientClusters.mapNotNull { clusterId ->
        val meta = MatterClusterMetaRegistry.getClusterOrDefault(clusterId)
        ClusterDescriptor(
          clusterId = clusterId,
          clusterName = meta.name,
          description = meta.description,
          isServer = false,
          attributes = emptyList(),
          commands = meta.commands.values.toList(),
          category = categorizeCluster(clusterId),
        )
      }

    return EndpointDescriptor(
      endpointId = ep.endpointId,
      deviceTypeId = ep.deviceTypeId,
      deviceTypeName = ep.deviceTypeName,
      isRootEndpoint = (ep.endpointId == 0),
      serverClusters = serverClusters,
      clientClusters = clientClusters,
    )
  }

  fun toWritableAttributeState(
    endpointId: Int,
    clusterMeta: MatterClusterMeta,
    attrMeta: MatterAttributeMeta,
    liveValues: Map<String, Any?>,
  ): WritableAttributeState {
    val value =
      getLiveAttributeValue(
        liveValues,
        null,
        endpointId,
        clusterMeta.clusterId,
        attrMeta.attributeId,
      ) ?: attrMeta.defaultValue
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
      unit = attrMeta.unit,
    )
  }

  private fun resolveControlType(clusterId: Long, attr: MatterAttributeMeta): AttributeControlType {
    if (
      clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL &&
        attr.name.contains("ColorTemperature", ignoreCase = true)
    ) {
      return AttributeControlType.SLIDER_COLOR_TEMP
    }
    if (
      clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL &&
        attr.name.contains("Color", ignoreCase = true)
    ) {
      return AttributeControlType.COLOR_PICKER
    }
    if (!attr.isWritable) return AttributeControlType.READONLY_TEXT

    return when {
      attr.type == MatterDataType.BOOLEAN -> AttributeControlType.SWITCH
      attr.enumValues.isNotEmpty() -> AttributeControlType.DROPDOWN_ENUM
      attr.type in
        listOf(
          MatterDataType.UINT8,
          MatterDataType.UINT16,
          MatterDataType.INT16,
          MatterDataType.FLOAT32,
        ) -> AttributeControlType.SLIDER_NUMERIC
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

    if (
      clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL &&
        attr.name.contains("ColorTemperature", ignoreCase = true)
    ) {
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

      else -> {
        val meta = MatterClusterMetaRegistry.getCluster(clusterId)
        if (meta?.category == com.google.chip.chiptool.voice.ClusterCategory.GENERAL_AND_SYSTEM)
          "General & System"
        else "System / Custom"
      }
    }
  }
}
