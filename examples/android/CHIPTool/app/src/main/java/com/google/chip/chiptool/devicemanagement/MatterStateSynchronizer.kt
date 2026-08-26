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
import android.util.Log
import chip.devicecontroller.ChipDeviceController
import chip.devicecontroller.ReportCallback
import chip.devicecontroller.SubscriptionEstablishedCallback
import chip.devicecontroller.ResubscriptionAttemptCallback
import chip.devicecontroller.model.ChipAttributePath
import chip.devicecontroller.model.ChipEventPath
import chip.devicecontroller.model.ChipPathId
import chip.devicecontroller.model.NodeState
import com.google.chip.chiptool.ChipClient
import com.google.chip.chiptool.voice.CommissionedNodeRegistry
import com.google.chip.chiptool.voice.MatterClusterMetaRegistry
import com.google.chip.chiptool.voice.MatterDataType
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Event emitted when a Matter attribute is updated via IM subscription or local optimistic mutation.
 */
data class LiveAttributeUpdate(
  val nodeId: Long,
  val endpointId: Int,
  val clusterId: Long,
  val attributeId: Long,
  val value: Any?,
  val isOptimistic: Boolean = false,
  val timestampMs: Long = System.currentTimeMillis()
)

/**
 * Real-Time Matter State Synchronizer.
 * Maintains persistent IM subscriptions (`ChipDeviceController.subscribeToPath`) across all
 * active commissioned fabric nodes, managing optimistic local updates, rollbacks on error,
 * and high-speed StateFlow/SharedFlow reactive streams for Material 3 UI views.
 */
class MatterStateSynchronizer(
  private val nodeRegistry: CommissionedNodeRegistry,
  private val coroutineScope: CoroutineScope = CoroutineScope(Dispatchers.Default)
) {
  companion object {
    private const val TAG = "MatterStateSync"
    private const val MIN_INTERVAL_SECONDS = 1
    private const val MAX_INTERVAL_SECONDS = 60

    @Volatile
    private var instance: MatterStateSynchronizer? = null

    fun getInstance(nodeRegistry: CommissionedNodeRegistry): MatterStateSynchronizer {
      return instance ?: synchronized(this) {
        instance ?: MatterStateSynchronizer(nodeRegistry).also { instance = it }
      }
    }
  }

  // Key format: "$nodeId:$endpointId:$clusterId:$attributeId"
  private val liveStateCache = ConcurrentHashMap<String, Any?>()
  private val activeSubscriptions = ConcurrentHashMap<Long, Boolean>()

  private val _stateUpdates = MutableSharedFlow<LiveAttributeUpdate>(replay = 10, extraBufferCapacity = 50)
  val stateUpdates: SharedFlow<LiveAttributeUpdate> = _stateUpdates.asSharedFlow()

  private val _allStates = MutableStateFlow<Map<String, Any?>>(emptyMap())
  val allStates: StateFlow<Map<String, Any?>> = _allStates.asStateFlow()

  fun buildAttributeKey(nodeId: Long, endpointId: Int, clusterId: Long, attributeId: Long): String {
    return "$nodeId:$endpointId:$clusterId:$attributeId"
  }

  fun getCachedAttributeValue(nodeId: Long, endpointId: Int, clusterId: Long, attributeId: Long): Any? {
    return liveStateCache[buildAttributeKey(nodeId, endpointId, clusterId, attributeId)]
  }

  fun getAllCachedStates(): Map<String, Any?> = HashMap(liveStateCache)

  /**
   * Applies an optimistic local update immediately before the network transaction completes.
   * Returns a rollback function to revert the state if the IM write fails.
   */
  fun applyOptimisticUpdate(
    nodeId: Long,
    endpointId: Int,
    clusterId: Long,
    attributeId: Long,
    optimisticValue: Any?
  ): () -> Unit {
    val key = buildAttributeKey(nodeId, endpointId, clusterId, attributeId)
    val previousValue = liveStateCache[key]

    liveStateCache[key] = optimisticValue
    _allStates.value = HashMap(liveStateCache)

    coroutineScope.launch {
      _stateUpdates.emit(
        LiveAttributeUpdate(
          nodeId = nodeId,
          endpointId = endpointId,
          clusterId = clusterId,
          attributeId = attributeId,
          value = optimisticValue,
          isOptimistic = true
        )
      )
    }

    return {
      if (previousValue != null) {
        liveStateCache[key] = previousValue
      } else {
        liveStateCache.remove(key)
      }
      _allStates.value = HashMap(liveStateCache)
      coroutineScope.launch {
        _stateUpdates.emit(
          LiveAttributeUpdate(
            nodeId = nodeId,
            endpointId = endpointId,
            clusterId = clusterId,
            attributeId = attributeId,
            value = previousValue,
            isOptimistic = false
          )
        )
      }
    }
  }

  /**
   * Records a confirmed attribute value from an IM ReportCallback or Read response.
   */
  fun recordConfirmedAttribute(
    nodeId: Long,
    endpointId: Int,
    clusterId: Long,
    attributeId: Long,
    confirmedValue: Any?
  ) {
    val key = buildAttributeKey(nodeId, endpointId, clusterId, attributeId)
    liveStateCache[key] = confirmedValue
    _allStates.value = HashMap(liveStateCache)

    // Also update CommissionedNode liveStates & battery if relevant
    if (clusterId == MatterClusterMetaRegistry.CLUSTER_ON_OFF && attributeId == 0x0000L) {
      nodeRegistry.updateNodeLiveState(nodeId, "onOff", confirmedValue)
    } else if (clusterId == MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL && attributeId == 0x0000L) {
      nodeRegistry.updateNodeLiveState(nodeId, "currentLevel", confirmedValue)
    }

    coroutineScope.launch {
      _stateUpdates.emit(
        LiveAttributeUpdate(
          nodeId = nodeId,
          endpointId = endpointId,
          clusterId = clusterId,
          attributeId = attributeId,
          value = confirmedValue,
          isOptimistic = false
        )
      )
    }
  }

  /**
   * Initiates a wild-card or multi-endpoint subscription to the given node.
   */
  suspend fun subscribeToNode(context: Context, nodeId: Long) {
    if (activeSubscriptions[nodeId] == true) {
      Log.d(TAG, "Already subscribed to node $nodeId")
      return
    }

    val node = nodeRegistry.getNode(nodeId) ?: return
    val controller = try {
      ChipClient.getDeviceController(context)
    } catch (e: Exception) {
      Log.e(TAG, "Failed to get ChipDeviceController for node $nodeId subscription", e)
      return
    }

    val attributePaths = mutableListOf<ChipAttributePath>()
    for (ep in node.endpoints) {
      for (clusterId in ep.serverClusters) {
        attributePaths.add(
          ChipAttributePath.newInstance(
            ChipPathId.forId(ep.endpointId.toLong()),
            ChipPathId.forId(clusterId),
            ChipPathId.forWildcard()
          )
        )
      }
    }

    if (attributePaths.isEmpty()) return

    try {
      val devicePointer = ChipClient.getConnectedDevicePointer(context, nodeId)
      val subscriptionCallback = SubscriptionEstablishedCallback { subscriptionId ->
        Log.d(TAG, "Subscription established for node $nodeId with id: $subscriptionId")
        activeSubscriptions[nodeId] = true
      }
      val resubscriptionCallback = ResubscriptionAttemptCallback { terminationCause, nextIntervalMs ->
        Log.d(TAG, "Resubscription attempt for node $nodeId: cause=$terminationCause, nextIntervalMs=$nextIntervalMs")
      }
      controller.subscribeToPath(
        subscriptionCallback,
        resubscriptionCallback,
        object : ReportCallback {
          override fun onError(
            attributePath: ChipAttributePath?,
            eventPath: ChipEventPath?,
            ex: Exception
          ) {
            Log.e(TAG, "Subscription report error on node $nodeId", ex)
          }

          override fun onReport(nodeState: NodeState) {
            processNodeStateReport(nodeId, nodeState)
          }

          override fun onDone() {
            Log.d(TAG, "Subscription onDone for node $nodeId")
            activeSubscriptions[nodeId] = true
          }
        },
        devicePointer,
        attributePaths,
        emptyList<ChipEventPath>(),
        MIN_INTERVAL_SECONDS,
        MAX_INTERVAL_SECONDS,
        true,
        true,
        0
      )
    } catch (e: Exception) {
      Log.e(TAG, "Exception establishing subscription for node $nodeId", e)
    }
  }

  /**
   * Processes incoming NodeState reports from Matter Interaction Model.
   */
  fun processNodeStateReport(nodeId: Long, nodeState: NodeState) {
    for ((epId, endpointState) in nodeState.endpointStates) {
      for ((clusterId, clusterState) in endpointState.clusterStates) {
        for ((attrId, attributeState) in clusterState.attributeStates) {
          val value = attributeState.value
          recordConfirmedAttribute(
            nodeId = nodeId,
            endpointId = epId,
            clusterId = clusterId,
            attributeId = attrId,
            confirmedValue = value
          )
        }
      }
    }
  }

  /**
   * Helper to decode TLV byte representations when raw payloads are received.
   */
  fun decodeRawAttributeTlv(
    dataType: MatterDataType,
    rawBytes: ByteArray?
  ): Any? {
    if (rawBytes == null || rawBytes.isEmpty()) return null
    return try {
      when (dataType) {
        MatterDataType.BOOLEAN -> (rawBytes[0].toInt() != 0)
        MatterDataType.UINT8 -> (rawBytes[0].toInt() and 0xFF)
        MatterDataType.UINT16 -> {
          if (rawBytes.size >= 2) {
            (rawBytes[0].toInt() and 0xFF) or ((rawBytes[1].toInt() and 0xFF) shl 8)
          } else (rawBytes[0].toInt() and 0xFF)
        }
        MatterDataType.INT16 -> {
          if (rawBytes.size >= 2) {
            val unsigned = (rawBytes[0].toInt() and 0xFF) or ((rawBytes[1].toInt() and 0xFF) shl 8)
            unsigned.toShort().toInt()
          } else rawBytes[0].toInt()
        }
        MatterDataType.UINT32 -> {
          var res = 0L
          for (i in 0 until minOf(4, rawBytes.size)) {
            res = res or ((rawBytes[i].toLong() and 0xFFL) shl (i * 8))
          }
          res
        }
        MatterDataType.UTF8_STRING -> String(rawBytes, Charsets.UTF_8)
        else -> rawBytes
      }
    } catch (e: Exception) {
      Log.e(TAG, "Error decoding raw attribute TLV", e)
      null
    }
  }
}
