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

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.google.chip.chiptool.voice.CommissionedNode
import com.google.chip.chiptool.voice.CommissionedNodeRegistry
import com.google.chip.chiptool.voice.MatterClusterMetaRegistry
import com.google.chip.chiptool.voice.UniversalMatterDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/**
 * Bulk action options for all devices within a single room.
 */
enum class BulkRoomAction {
  TURN_ALL_ON,
  TURN_ALL_OFF,
  LOCK_ALL_DOORS
}

/**
 * Root ViewModel coordinating the Material 3 Room & Device Dashboard.
 * Groups commissioned nodes by room, enables search and category filtering,
 * handles room reassignments, and manages fabric-wide subscriptions.
 */
class DeviceManagementViewModel(
  private val nodeRegistry: CommissionedNodeRegistry,
  private val stateSynchronizer: MatterStateSynchronizer,
  private val dispatcher: UniversalMatterDispatcher,
  private val coroutineScope: CoroutineScope = CoroutineScope(Dispatchers.Default)
) : ViewModel() {

  private val _allNodes = MutableStateFlow<List<CommissionedNode>>(nodeRegistry.getAllNodes())
  val allNodes: StateFlow<List<CommissionedNode>> = _allNodes.asStateFlow()

  private val _selectedRoomFilter = MutableStateFlow<String?>(null) // null = All Rooms
  val selectedRoomFilter: StateFlow<String?> = _selectedRoomFilter.asStateFlow()

  private val _searchQuery = MutableStateFlow("")
  val searchQuery: StateFlow<String> = _searchQuery.asStateFlow()

  // Combined reactive stream of filtered room hierarchies
  val filteredRoomHierarchy: StateFlow<List<RoomHierarchyItem>> = combine(
    _allNodes,
    _selectedRoomFilter,
    _searchQuery,
    stateSynchronizer.allStates
  ) { nodes, roomFilter, query, liveStates ->
    val cleanQuery = query.trim().lowercase()

    // 1. Filter by query
    val matchingNodes = if (cleanQuery.isEmpty()) {
      nodes
    } else {
      nodes.filter { node ->
        node.nodeLabel.lowercase().contains(cleanQuery) ||
          cleanQuery.contains(node.nodeLabel.lowercase()) ||
          node.roomName.lowercase().contains(cleanQuery) ||
          cleanQuery.contains(node.roomName.lowercase()) ||
          node.aliases.any { it.lowercase().contains(cleanQuery) || cleanQuery.contains(it.lowercase()) } ||
          node.endpoints.any { it.deviceTypeName.lowercase().contains(cleanQuery) }
      }
    }

    // 2. Group by room
    val groupedByRoom = matchingNodes.groupBy { it.roomName }

    // 3. Construct RoomHierarchyItems
    val roomItems = groupedByRoom.map { (roomName, roomNodes) ->
      val deviceCards = roomNodes.map { node ->
        RoomDeviceHierarchyMapper.toDeviceCardState(node, liveStates)
      }
      RoomHierarchyItem(
        roomName = roomName,
        deviceCards = deviceCards
      )
    }.sortedBy { it.roomName }

    // 4. Apply Room Filter if set
    if (roomFilter != null) {
      roomItems.filter { it.roomName.equals(roomFilter, ignoreCase = true) }
    } else {
      roomItems
    }
  }.stateIn(coroutineScope, SharingStarted.Eagerly, emptyList())

  val availableRooms: StateFlow<List<String>> = combine(_allNodes) {
    nodeRegistry.getAllRooms()
  }.stateIn(coroutineScope, SharingStarted.Eagerly, emptyList())

  init {
    // Registry update listener
    nodeRegistry.addNodeRegistryChangeListener {
      _allNodes.value = nodeRegistry.getAllNodes()
    }

    // Sync updates
    coroutineScope.launch {
      stateSynchronizer.stateUpdates.collectLatest {
        _allNodes.value = nodeRegistry.getAllNodes()
      }
    }
  }

  fun refreshFabric() {
    _allNodes.value = nodeRegistry.getAllNodes()
  }

  fun setRoomFilter(roomName: String?) {
    _selectedRoomFilter.value = roomName
  }

  fun setSearchQuery(query: String) {
    _searchQuery.value = query
  }

  fun reassignDeviceRoom(nodeId: Long, newRoomName: String) {
    nodeRegistry.updateNodeRoom(nodeId, newRoomName)
    _allNodes.value = nodeRegistry.getAllNodes()
  }

  fun updateDeviceLabel(nodeId: Long, newLabel: String) {
    nodeRegistry.updateNodeLabel(nodeId, newLabel)
    _allNodes.value = nodeRegistry.getAllNodes()
  }

  fun updateDeviceAliases(nodeId: Long, aliases: List<String>) {
    nodeRegistry.updateNodeAliases(nodeId, aliases)
    _allNodes.value = nodeRegistry.getAllNodes()
  }

  /**
   * Dispatches bulk actions across all devices in the designated room.
   */
  fun executeBulkRoomAction(context: Context, roomName: String, action: BulkRoomAction) {
    val roomNodes = nodeRegistry.getNodesInRoom(roomName)

    viewModelScope.launch {
      for (node in roomNodes) {
        val primaryEp = node.endpoints.firstOrNull { it.endpointId != 0 } ?: continue

        when (action) {
          BulkRoomAction.TURN_ALL_ON -> {
            if (primaryEp.supportsCluster(MatterClusterMetaRegistry.CLUSTER_ON_OFF)) {
              dispatcher.invokeCommand(
                context = context,
                nodeId = node.nodeId,
                endpointId = primaryEp.endpointId,
                clusterId = MatterClusterMetaRegistry.CLUSTER_ON_OFF,
                commandId = 0x01L, // On
                targetDescription = "${node.nodeLabel} ($roomName)"
              )
              stateSynchronizer.recordConfirmedAttribute(
                node.nodeId,
                primaryEp.endpointId,
                MatterClusterMetaRegistry.CLUSTER_ON_OFF,
                0x0000L,
                true
              )
            }
          }

          BulkRoomAction.TURN_ALL_OFF -> {
            if (primaryEp.supportsCluster(MatterClusterMetaRegistry.CLUSTER_ON_OFF)) {
              dispatcher.invokeCommand(
                context = context,
                nodeId = node.nodeId,
                endpointId = primaryEp.endpointId,
                clusterId = MatterClusterMetaRegistry.CLUSTER_ON_OFF,
                commandId = 0x00L, // Off
                targetDescription = "${node.nodeLabel} ($roomName)"
              )
              stateSynchronizer.recordConfirmedAttribute(
                node.nodeId,
                primaryEp.endpointId,
                MatterClusterMetaRegistry.CLUSTER_ON_OFF,
                0x0000L,
                false
              )
            }
          }

          BulkRoomAction.LOCK_ALL_DOORS -> {
            if (primaryEp.supportsCluster(MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK)) {
              dispatcher.invokeCommand(
                context = context,
                nodeId = node.nodeId,
                endpointId = primaryEp.endpointId,
                clusterId = MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK,
                commandId = 0x00L, // LockDoor
                targetDescription = "${node.nodeLabel} ($roomName)"
              )
              stateSynchronizer.recordConfirmedAttribute(
                node.nodeId,
                primaryEp.endpointId,
                MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK,
                0x0000L,
                1
              )
            }
          }
        }
      }
      _allNodes.value = nodeRegistry.getAllNodes()
    }
  }

  /**
   * Establishes real-time subscriptions across all fabric devices.
   */
  fun subscribeToAllFabricNodes(context: Context) {
    coroutineScope.launch {
      for (node in nodeRegistry.getAllNodes()) {
        stateSynchronizer.subscribeToNode(context, node.nodeId)
      }
    }
  }
}
