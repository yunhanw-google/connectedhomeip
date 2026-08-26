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

import android.content.Context
import com.google.gson.Gson
import com.google.gson.reflect.TypeToken
import java.util.concurrent.ConcurrentHashMap

/**
 * Standard Matter Device Type IDs.
 */
object MatterDeviceTypes {
  const val ON_OFF_LIGHT = 0x0100L
  const val DIMMABLE_LIGHT = 0x0101L
  const val COLOR_TEMPERATURE_LIGHT = 0x010CL
  const val EXTENDED_COLOR_LIGHT = 0x010DL
  const val ON_OFF_PLUG_IN_UNIT = 0x010AL
  const val DIMMABLE_PLUG_IN_UNIT = 0x010BL
  const val DOOR_LOCK = 0x000AL
  const val WINDOW_COVERING = 0x0202L
  const val THERMOSTAT = 0x0301L
  const val FAN = 0x002BL
  const val SPEAKER = 0x0022L
  const val BASIC_VIDEO_PLAYER = 0x0028L
  const val ROBOTIC_VACUUM_CLEANER = 0x0074L
  const val EVSE = 0x050CL
  const val TEMPERATURE_SENSOR = 0x0302L
  const val HUMIDITY_SENSOR = 0x0307L
  const val CONTACT_SENSOR = 0x0015L
  const val OCCUPANCY_SENSOR = 0x0107L
  const val SMOKE_CO_ALARM = 0x0076L
}

/**
 * Represents an endpoint on a commissioned Matter Node.
 */
data class CommissionedEndpoint(
  val endpointId: Int,
  val deviceTypeId: Long,
  val deviceTypeName: String,
  val serverClusters: Set<Long> = emptySet(),
  val clientClusters: Set<Long> = emptySet()
) {
  fun supportsCluster(clusterId: Long): Boolean = serverClusters.contains(clusterId)
}

/**
 * Represents a commissioned Matter Node in the local fabric.
 */
data class CommissionedNode(
  val nodeId: Long,
  val nodeLabel: String,
  val roomName: String = "Default Room",
  val vendorName: String = "Standard Matter Vendor",
  val productName: String = "Matter Device",
  val endpoints: List<CommissionedEndpoint> = emptyList(),
  val isOnline: Boolean = true,
  val aliases: List<String> = emptyList(),
  val batteryPercent: Int? = null,
  val liveStates: Map<String, Any?> = emptyMap()
) {
  fun getEndpointForCluster(clusterId: Long): CommissionedEndpoint? {
    return endpoints.firstOrNull { it.supportsCluster(clusterId) }
  }

  fun getAllSupportedClusterIds(): Set<Long> {
    return endpoints.flatMap { it.serverClusters }.toSet()
  }

  fun getSupportedClusterMetas(): List<MatterClusterMeta> {
    return getAllSupportedClusterIds().mapNotNull { MatterClusterMetaRegistry.getCluster(it) }
  }
}

/**
 * Fabric-aware registry that manages active commissioned Matter nodes.
 * Used by the Voice Control Engine, Room-Centric UI, and Gemini Nano prompt generator to constrain tools
 * to the exact devices present on the user's home fabric.
 */
class CommissionedNodeRegistry {
  private val nodes = ConcurrentHashMap<Long, CommissionedNode>()
  private val listeners = mutableListOf<(CommissionedNode) -> Unit>()
  private val gson = Gson()

  fun addNodeRegistryChangeListener(listener: (CommissionedNode) -> Unit) {
    synchronized(listeners) {
      listeners.add(listener)
    }
  }

  fun removeNodeRegistryChangeListener(listener: (CommissionedNode) -> Unit) {
    synchronized(listeners) {
      listeners.remove(listener)
    }
  }

  private fun notifyNodeUpdated(node: CommissionedNode) {
    val snapshot = synchronized(listeners) { listeners.toList() }
    snapshot.forEach { it.invoke(node) }
  }

  fun registerNode(node: CommissionedNode) {
    nodes[node.nodeId] = node
    notifyNodeUpdated(node)
  }

  fun unregisterNode(nodeId: Long): CommissionedNode? {
    val removed = nodes.remove(nodeId)
    if (removed != null) {
      notifyNodeUpdated(removed.copy(isOnline = false))
    }
    return removed
  }

  fun getNode(nodeId: Long): CommissionedNode? = nodes[nodeId]

  fun getAllNodes(): List<CommissionedNode> = nodes.values.toList()

  fun getAllRooms(): List<String> {
    return nodes.values.map { it.roomName }.distinct().sorted()
  }

  fun getNodesInRoom(roomName: String): List<CommissionedNode> {
    return nodes.values.filter { it.roomName.equals(roomName, ignoreCase = true) }
  }

  fun updateNodeRoom(nodeId: Long, newRoomName: String): CommissionedNode? {
    val existing = nodes[nodeId] ?: return null
    val updated = existing.copy(roomName = newRoomName.trim())
    nodes[nodeId] = updated
    notifyNodeUpdated(updated)
    return updated
  }

  fun updateNodeLabel(nodeId: Long, newLabel: String): CommissionedNode? {
    val existing = nodes[nodeId] ?: return null
    val updated = existing.copy(nodeLabel = newLabel.trim())
    nodes[nodeId] = updated
    notifyNodeUpdated(updated)
    return updated
  }

  fun updateNodeAliases(nodeId: Long, aliases: List<String>): CommissionedNode? {
    val existing = nodes[nodeId] ?: return null
    val updated = existing.copy(aliases = aliases.map { it.trim() }.filter { it.isNotEmpty() })
    nodes[nodeId] = updated
    notifyNodeUpdated(updated)
    return updated
  }

  fun updateNodeOnlineStatus(nodeId: Long, isOnline: Boolean): CommissionedNode? {
    val existing = nodes[nodeId] ?: return null
    val updated = existing.copy(isOnline = isOnline)
    nodes[nodeId] = updated
    notifyNodeUpdated(updated)
    return updated
  }

  fun updateNodeBattery(nodeId: Long, batteryPercent: Int?): CommissionedNode? {
    val existing = nodes[nodeId] ?: return null
    val updated = existing.copy(batteryPercent = batteryPercent)
    nodes[nodeId] = updated
    notifyNodeUpdated(updated)
    return updated
  }

  fun updateNodeLiveState(nodeId: Long, key: String, value: Any?): CommissionedNode? {
    val existing = nodes[nodeId] ?: return null
    val newLiveStates = existing.liveStates.toMutableMap()
    newLiveStates[key] = value
    val updated = existing.copy(liveStates = newLiveStates)
    nodes[nodeId] = updated
    notifyNodeUpdated(updated)
    return updated
  }

  fun clear() {
    nodes.clear()
  }

  fun count(): Int = nodes.size

  /**
   * Returns all unique server cluster IDs across all commissioned nodes in this fabric.
   */
  fun getFabricClusterIds(): Set<Long> {
    return nodes.values.flatMap { it.getAllSupportedClusterIds() }.toSet()
  }

  /**
   * Returns all clusters supported on this fabric.
   */
  fun getFabricClusters(): List<MatterClusterMeta> {
    return getFabricClusterIds().mapNotNull { MatterClusterMetaRegistry.getCluster(it) }
  }

  /**
   * Finds nodes supporting a specific cluster ID.
   */
  fun findNodesSupportingCluster(clusterId: Long): List<CommissionedNode> {
    return nodes.values.filter { it.getEndpointForCluster(clusterId) != null }
  }

  /**
   * Finds nodes matching a target text (matches nodeLabel, roomName, aliases, or deviceTypeName).
   */
  fun findNodesByQuery(query: String): List<CommissionedNode> {
    val clean = query.trim().lowercase()
    return nodes.values.filter { node ->
      node.nodeLabel.lowercase().contains(clean) ||
        clean.contains(node.nodeLabel.lowercase()) ||
        node.roomName.lowercase().contains(clean) ||
        clean.contains(node.roomName.lowercase()) ||
        "${node.roomName} ${node.nodeLabel}".lowercase().contains(clean) ||
        node.aliases.any { it.lowercase().contains(clean) || clean.contains(it.lowercase()) } ||
        node.endpoints.any { it.deviceTypeName.lowercase().contains(clean) }
    }
  }

  /**
   * Resolves target node and endpoint given a query and cluster ID.
   */
  fun resolveTarget(query: String?, clusterId: Long): Pair<CommissionedNode, CommissionedEndpoint>? {
    val candidates = findNodesSupportingCluster(clusterId)
    if (candidates.isEmpty()) return null

    if (query.isNullOrBlank()) {
      val first = candidates.first()
      return Pair(first, first.getEndpointForCluster(clusterId)!!)
    }

    val matched = findNodesByQuery(query).filter { it.getEndpointForCluster(clusterId) != null }
    val node = matched.firstOrNull() ?: candidates.first()
    val endpoint = node.getEndpointForCluster(clusterId) ?: return null
    return Pair(node, endpoint)
  }

  /**
   * Serializes current fabric nodes to JSON.
   */
  fun toJson(): String {
    return gson.toJson(nodes.values)
  }

  /**
   * Loads nodes from JSON.
   */
  fun loadFromJson(json: String) {
    val listType = object : TypeToken<List<CommissionedNode>>() {}.type
    val loaded: List<CommissionedNode> = gson.fromJson(json, listType) ?: emptyList()
    nodes.clear()
    loaded.forEach { registerNode(it) }
  }

  fun saveFabricToPreferences(context: Context) {
    val prefs = context.getSharedPreferences("matter_voice_fabric", Context.MODE_PRIVATE)
    val json = gson.toJson(nodes.values.toList())
    prefs.edit().putString("commissioned_nodes", json).apply()
  }

  fun loadFabricFromPreferences(context: Context) {
    val prefs = context.getSharedPreferences("matter_voice_fabric", Context.MODE_PRIVATE)
    val json = prefs.getString("commissioned_nodes", null) ?: return
    try {
      val type = object : TypeToken<List<CommissionedNode>>() {}.type
      val loadedNodes: List<CommissionedNode> = gson.fromJson(json, type) ?: return
      nodes.clear()
      for (node in loadedNodes) {
        nodes[node.nodeId] = node
      }
    } catch (e: Exception) {
      // Fallback
    }
  }

  /**
   * Initializes the registry with a comprehensive, realistic multi-device smart home fabric.
   */
  fun loadDefaultSmartHomeFabric() {
    clear()

    // 1. Living Room Color Light
    registerNode(
      CommissionedNode(
        nodeId = 0x1001L,
        nodeLabel = "Living Room Ceiling Light",
        roomName = "Living Room",
        productName = "Matter Smart Color Bulb",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.EXTENDED_COLOR_LIGHT,
            deviceTypeName = "Extended Color Light",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_ON_OFF,
              MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL,
              MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL
            )
          )
        )
      )
    )

    // 2. Kitchen Dimmer Light
    registerNode(
      CommissionedNode(
        nodeId = 0x1002L,
        nodeLabel = "Kitchen Counter Light",
        roomName = "Kitchen",
        productName = "Matter Dimmable Spotlight",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.DIMMABLE_LIGHT,
            deviceTypeName = "Dimmable Light",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_ON_OFF,
              MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL
            )
          )
        )
      )
    )

    // 3. Front Door Smart Lock
    registerNode(
      CommissionedNode(
        nodeId = 0x2001L,
        nodeLabel = "Front Door Deadbolt",
        roomName = "Entryway",
        productName = "Matter Smart Deadbolt Lock",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.DOOR_LOCK,
            deviceTypeName = "Door Lock",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK
            )
          )
        )
      )
    )

    // 4. Living Room Window Shades
    registerNode(
      CommissionedNode(
        nodeId = 0x2002L,
        nodeLabel = "Living Room Blinds",
        roomName = "Living Room",
        productName = "Motorized Roller Shades",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.WINDOW_COVERING,
            deviceTypeName = "Window Covering",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING
            )
          )
        )
      )
    )

    // 5. Garage Door Barrier
    registerNode(
      CommissionedNode(
        nodeId = 0x2003L,
        nodeLabel = "Garage Door",
        roomName = "Garage",
        productName = "Smart Garage Door Opener",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = 0x0103L,
            deviceTypeName = "Barrier Control",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_BARRIER_CONTROL
            )
          )
        )
      )
    )

    // 6. Hallway Smart Thermostat
    registerNode(
      CommissionedNode(
        nodeId = 0x3001L,
        nodeLabel = "Main Thermostat",
        roomName = "Hallway",
        productName = "Matter Smart Climate Controller",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.THERMOSTAT,
            deviceTypeName = "Thermostat",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_THERMOSTAT
            )
          )
        )
      )
    )

    // 7. Bedroom Ceiling Fan
    registerNode(
      CommissionedNode(
        nodeId = 0x3002L,
        nodeLabel = "Bedroom Ceiling Fan",
        roomName = "Bedroom",
        productName = "Smart Fan Controller",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.FAN,
            deviceTypeName = "Fan",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_FAN_CONTROL
            )
          )
        )
      )
    )

    // 8. Living Room Smart TV
    registerNode(
      CommissionedNode(
        nodeId = 0x4001L,
        nodeLabel = "Living Room TV",
        roomName = "Living Room",
        productName = "Matter Connected Smart TV",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.BASIC_VIDEO_PLAYER,
            deviceTypeName = "Basic Video Player",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_MEDIA_PLAYBACK,
              MatterClusterMetaRegistry.CLUSTER_KEYPAD_INPUT,
              MatterClusterMetaRegistry.CLUSTER_AUDIO_OUTPUT
            )
          )
        )
      )
    )

    // 9. Robot Vacuum Cleaner
    registerNode(
      CommissionedNode(
        nodeId = 0x5001L,
        nodeLabel = "RoboVac",
        roomName = "Downstairs",
        productName = "Matter Autonomous Robot Vacuum",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.ROBOTIC_VACUUM_CLEANER,
            deviceTypeName = "Robotic Vacuum Cleaner",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_RVC_RUN_MODE,
              MatterClusterMetaRegistry.CLUSTER_RVC_CLEAN_MODE,
              MatterClusterMetaRegistry.CLUSTER_OPERATIONAL_STATE
            )
          )
        )
      )
    )

    // 10. Laundry Washing Machine
    registerNode(
      CommissionedNode(
        nodeId = 0x5002L,
        nodeLabel = "Washing Machine",
        roomName = "Laundry Room",
        productName = "Smart Front-Load Washer",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = 0x0075L,
            deviceTypeName = "Smart Washer",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_OPERATIONAL_STATE,
              MatterClusterMetaRegistry.CLUSTER_MODE_SELECT
            )
          )
        )
      )
    )

    // 11. Garage EV Charger
    registerNode(
      CommissionedNode(
        nodeId = 0x6001L,
        nodeLabel = "EV Home Charger",
        roomName = "Garage",
        productName = "Level 2 Matter EVSE",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.EVSE,
            deviceTypeName = "Energy EVSE",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_ENERGY_EVSE,
              MatterClusterMetaRegistry.CLUSTER_DEVICE_ENERGY_MGMT
            )
          )
        )
      )
    )

    // 12. Living Room Environmental Sensor
    registerNode(
      CommissionedNode(
        nodeId = 0x7001L,
        nodeLabel = "Living Room Climate Sensor",
        roomName = "Living Room",
        productName = "Multi-Sensor (Temp, Humidity, Motion)",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.TEMPERATURE_SENSOR,
            deviceTypeName = "Temperature & Humidity Sensor",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_TEMPERATURE_MEASUREMENT,
              MatterClusterMetaRegistry.CLUSTER_RELATIVE_HUMIDITY_MEASUREMENT,
              MatterClusterMetaRegistry.CLUSTER_OCCUPANCY_SENSING
            )
          )
        )
      )
    )

    // 13. Front Window Contact Sensor
    registerNode(
      CommissionedNode(
        nodeId = 0x7002L,
        nodeLabel = "Front Window Sensor",
        roomName = "Living Room",
        productName = "Door/Window Contact Sensor",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.CONTACT_SENSOR,
            deviceTypeName = "Contact Sensor",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_BOOLEAN_STATE
            )
          )
        )
      )
    )

    // 14. Kitchen Smoke & CO Alarm
    registerNode(
      CommissionedNode(
        nodeId = 0x7003L,
        nodeLabel = "Kitchen Smoke Detector",
        roomName = "Kitchen",
        productName = "Smart Smoke & CO Alarm",
        endpoints = listOf(
          CommissionedEndpoint(
            endpointId = 1,
            deviceTypeId = MatterDeviceTypes.SMOKE_CO_ALARM,
            deviceTypeName = "Smoke CO Alarm",
            serverClusters = setOf(
              MatterClusterMetaRegistry.CLUSTER_SMOKE_CO_ALARM
            )
          )
        )
      )
    )
  }
}
