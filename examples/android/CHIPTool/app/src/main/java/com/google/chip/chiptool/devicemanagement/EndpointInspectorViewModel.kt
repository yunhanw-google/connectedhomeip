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
import com.google.chip.chiptool.voice.MatterDataType
import com.google.chip.chiptool.voice.MatterDispatchResult
import com.google.chip.chiptool.voice.UniversalMatterDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch

/**
 * ViewModel powering the deep Endpoint & Cluster Inspector modal ("Beautiful View").
 * Dynamically traverses Endpoint 0..N, discovers all Server & Client clusters,
 * displays categorized cluster cards, and executes in-place interactive attribute writes
 * and command invocations.
 */
class EndpointInspectorViewModel(
  val nodeId: Long,
  private val nodeRegistry: CommissionedNodeRegistry,
  private val stateSynchronizer: MatterStateSynchronizer,
  private val dispatcher: UniversalMatterDispatcher,
  private val coroutineScope: CoroutineScope = CoroutineScope(Dispatchers.Default)
) : ViewModel() {

  private val _node = MutableStateFlow<CommissionedNode?>(nodeRegistry.getNode(nodeId))
  val node: StateFlow<CommissionedNode?> = _node.asStateFlow()

  private val _selectedEndpointIndex = MutableStateFlow(0)
  val selectedEndpointIndex: StateFlow<Int> = _selectedEndpointIndex.asStateFlow()

  private val _endpoints = MutableStateFlow<List<EndpointDescriptor>>(emptyList())
  val endpoints: StateFlow<List<EndpointDescriptor>> = _endpoints.asStateFlow()

  private val _currentEndpoint = MutableStateFlow<EndpointDescriptor?>(null)
  val currentEndpoint: StateFlow<EndpointDescriptor?> = _currentEndpoint.asStateFlow()

  private val _operationStatus = MutableStateFlow<String?>(null)
  val operationStatus: StateFlow<String?> = _operationStatus.asStateFlow()

  init {
    reloadEndpoints()

    // Listen for live IM updates to refresh attribute values in inspector
    coroutineScope.launch {
      stateSynchronizer.stateUpdates.collectLatest { update ->
        if (update.nodeId == nodeId) {
          reloadEndpoints()
        }
      }
    }
  }

  fun reloadEndpoints() {
    val currentNode = nodeRegistry.getNode(nodeId)
    _node.value = currentNode
    if (currentNode == null) return

    val descriptors = currentNode.endpoints.map { ep ->
      RoomDeviceHierarchyMapper.toEndpointDescriptor(ep, stateSynchronizer.getAllCachedStates())
    }
    _endpoints.value = descriptors

    val index = _selectedEndpointIndex.value.coerceIn(0, maxOf(0, descriptors.size - 1))
    _currentEndpoint.value = descriptors.getOrNull(index)
  }

  fun selectEndpoint(index: Int) {
    if (index in 0 until _endpoints.value.size) {
      _selectedEndpointIndex.value = index
      _currentEndpoint.value = _endpoints.value[index]
    }
  }

  /**
   * Executes an in-place attribute write with dynamic type parsing, range checks,
   * optimistic UI updates, and rollback on error.
   */
  fun inPlaceWriteAttribute(
    context: Context,
    endpointId: Int,
    clusterId: Long,
    attributeId: Long,
    inputValue: Any
  ) {
    val clusterMeta = MatterClusterMetaRegistry.getCluster(clusterId) ?: return
    val attrMeta = clusterMeta.attributes[attributeId] ?: return
    val nodeLabel = _node.value?.nodeLabel ?: "Node $nodeId"

    // Coerce / validate input value
    val typedValue: Any = try {
      coerceInputValue(attrMeta.type, inputValue, clusterId, attrMeta.name)
    } catch (e: Exception) {
      _operationStatus.value = "Validation error: ${e.message}"
      return
    }

    // Apply optimistic update
    val rollback = stateSynchronizer.applyOptimisticUpdate(
      nodeId = nodeId,
      endpointId = endpointId,
      clusterId = clusterId,
      attributeId = attributeId,
      optimisticValue = typedValue
    )
    reloadEndpoints()

    coroutineScope.launch {
      _operationStatus.value = "Writing ${attrMeta.name}..."
      val result = dispatcher.writeAttribute(
        context = context,
        nodeId = nodeId,
        endpointId = endpointId,
        clusterId = clusterId,
        attributeId = attributeId,
        value = typedValue,
        targetDescription = "$nodeLabel [EP $endpointId]"
      )

      if (result.isSuccess) {
        stateSynchronizer.recordConfirmedAttribute(
          nodeId = nodeId,
          endpointId = endpointId,
          clusterId = clusterId,
          attributeId = attributeId,
          confirmedValue = typedValue
        )
        _operationStatus.value = "Updated ${attrMeta.name} successfully"
      } else {
        rollback()
        _operationStatus.value = "Failed to write ${attrMeta.name}: ${result.message}"
      }
      reloadEndpoints()
    }
  }

  /**
   * Invokes an arbitrary cluster command from the inspector.
   */
  fun invokeCommand(
    context: Context,
    endpointId: Int,
    clusterId: Long,
    commandId: Long,
    params: Map<String, Any?> = emptyMap()
  ) {
    val clusterMeta = MatterClusterMetaRegistry.getCluster(clusterId) ?: return
    val cmdMeta = clusterMeta.commands[commandId] ?: return
    val nodeLabel = _node.value?.nodeLabel ?: "Node $nodeId"

    coroutineScope.launch {
      _operationStatus.value = "Executing ${cmdMeta.name}..."
      val result: MatterDispatchResult = dispatcher.invokeCommand(
        context = context,
        nodeId = nodeId,
        endpointId = endpointId,
        clusterId = clusterId,
        commandId = commandId,
        params = params,
        targetDescription = "$nodeLabel [EP $endpointId]"
      )

      if (result.isSuccess) {
        _operationStatus.value = "${cmdMeta.name} succeeded"
      } else {
        _operationStatus.value = "${cmdMeta.name} failed: ${result.message}"
      }
      reloadEndpoints()
    }
  }

  /**
   * Reads an attribute directly using Matter IM Read request.
   */
  fun readAttribute(
    context: Context,
    endpointId: Int,
    clusterId: Long,
    attributeId: Long
  ) {
    val nodeLabel = _node.value?.nodeLabel ?: "Node $nodeId"
    coroutineScope.launch {
      _operationStatus.value = "Reading attribute..."
      val result = dispatcher.readAttribute(
        context = context,
        nodeId = nodeId,
        endpointId = endpointId,
        clusterId = clusterId,
        attributeId = attributeId,
        targetDescription = "$nodeLabel [EP $endpointId]"
      )

      if (result.isSuccess && result.decodedAttribute != null) {
        stateSynchronizer.recordConfirmedAttribute(
          nodeId = nodeId,
          endpointId = endpointId,
          clusterId = clusterId,
          attributeId = attributeId,
          confirmedValue = result.decodedAttribute.value
        )
        _operationStatus.value = "Read value: ${result.decodedAttribute.formattedValue}"
      } else {
        _operationStatus.value = "Read failed: ${result.message}"
      }
      reloadEndpoints()
    }
  }

  private fun coerceInputValue(
    dataType: MatterDataType,
    inputValue: Any,
    clusterId: Long,
    attrName: String
  ): Any {
    if (clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL && attrName.contains("ColorTemperature", ignoreCase = true)) {
      val num = (inputValue as? Number)?.toLong() ?: inputValue.toString().toDouble().toLong()
      if (num in 2000..10000) {
        // Input is in Kelvin, convert to Mireds
        return 1_000_000L / num
      }
      return num
    }

    return when (dataType) {
      MatterDataType.BOOLEAN -> {
        when (inputValue) {
          is Boolean -> inputValue
          is Number -> inputValue.toInt() != 0
          else -> inputValue.toString().equals("true", ignoreCase = true) || inputValue.toString() == "1"
        }
      }

      MatterDataType.UINT8 -> {
        val n = (inputValue as? Number)?.toInt() ?: inputValue.toString().toDouble().toInt()
        n.coerceIn(0, 254)
      }

      MatterDataType.UINT16 -> {
        val n = (inputValue as? Number)?.toInt() ?: inputValue.toString().toDouble().toInt()
        n.coerceIn(0, 65535)
      }

      MatterDataType.INT16 -> {
        val n = (inputValue as? Number)?.toInt() ?: inputValue.toString().toDouble().toInt()
        n.coerceIn(-32768, 32767)
      }

      MatterDataType.UINT32 -> {
        val n = (inputValue as? Number)?.toLong() ?: inputValue.toString().toDouble().toLong()
        n.coerceIn(0L, 4294967295L)
      }

      MatterDataType.FLOAT32, MatterDataType.FLOAT64 -> {
        (inputValue as? Number)?.toDouble() ?: inputValue.toString().toDouble()
      }

      MatterDataType.UTF8_STRING -> inputValue.toString()
      else -> inputValue
    }
  }
}
