package com.google.chip.chiptool.voice

import android.content.Context
import android.util.Log
import com.google.gson.Gson
import com.google.gson.reflect.TypeToken
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

enum class Source {
  SUBSCRIPTION_REPORT,
  UNICAST_READ,
  SNAPSHOT,
}

data class CachedAttribute(
  val nodeId: Long,
  val endpointId: Int,
  val clusterId: Long,
  val attributeId: Long,
  val value: Any?,
  val updatedAtMs: Long,
  val source: Source,
)

data class CachedSwitchEvent(
  val nodeId: Long,
  val endpointId: Int,
  val eventId: Long,
  val eventName: String,
  val position: Int = 1,
  val pressCount: Int = 1,
  val updatedAtMs: Long,
  val eventNumber: Long = 0L,
) {
  fun describePressType(): String =
    when (eventId) {
      0x02L,
      0x04L -> "long press"
      0x00L -> "latched"
      0x05L,
      0x06L ->
        when {
          pressCount <= 1 -> "single press"
          pressCount == 2 -> "double press"
          pressCount == 3 -> "triple press"
          else -> "$pressCount presses"
        }
      else -> "single press"
    }

  internal fun priorityRank(): Int =
    when (eventId) {
      0x06L -> if (pressCount > 0) 4 else 0
      0x02L,
      0x04L -> 3
      0x05L,
      0x00L -> 2
      else -> 1
    }
}

object HomeStateCache {
  private const val TAG = "HomeStateCache"
  private val cache = ConcurrentHashMap<String, CachedAttribute>()
  private val switchEvents = ConcurrentHashMap<String, CachedSwitchEvent>()
  private val _changes = MutableSharedFlow<CachedAttribute>(extraBufferCapacity = 64)
  val changes: SharedFlow<CachedAttribute> = _changes.asSharedFlow()

  private val scope = CoroutineScope(Dispatchers.Default)
  private var snapshotFile: File? = null
  @Volatile private var loadedSnapshotPath: String? = null
  private var saveJob: Job? = null
  private val gson = Gson()

  fun init(context: Context?) {
    val dir = context?.filesDir ?: File(System.getProperty("java.io.tmpdir") ?: "/tmp")
    val file = File(dir, "home_state_snapshot.json")
    if (loadedSnapshotPath == file.path) return
    snapshotFile = file
    scope.launch(Dispatchers.IO) { loadSnapshot() }
  }

  private fun buildKey(nodeId: Long, endpointId: Int, clusterId: Long, attributeId: Long) =
    "$nodeId:$endpointId:$clusterId:$attributeId"

  private fun buildSwitchEventKey(nodeId: Long, endpointId: Int) = "$nodeId:$endpointId"

  fun get(nodeId: Long, endpointId: Int, clusterId: Long, attributeId: Long): CachedAttribute? {
    return cache[buildKey(nodeId, endpointId, clusterId, attributeId)]
  }

  fun findAll(clusterId: Long, attributeId: Long): List<CachedAttribute> {
    return cache.values.filter { it.clusterId == clusterId && it.attributeId == attributeId }
  }

  fun putSwitchEvent(
    nodeId: Long,
    endpointId: Int,
    eventId: Long,
    position: Int = 1,
    pressCount: Int = 1,
    timestampMs: Long = System.currentTimeMillis(),
    eventNumber: Long = 0L,
  ) {
    val eventName =
      MatterClusterMetaRegistry.getCluster(MatterClusterMetaRegistry.CLUSTER_SWITCH)
        ?.events
        ?.get(eventId)
        ?.name ?: "SwitchEvent(0x${eventId.toString(16)})"
    val candidate =
      CachedSwitchEvent(
        nodeId = nodeId,
        endpointId = endpointId,
        eventId = eventId,
        eventName = eventName,
        position = position,
        pressCount = pressCount,
        updatedAtMs = timestampMs,
        eventNumber = eventNumber,
      )
    val key = buildSwitchEventKey(nodeId, endpointId)
    switchEvents.compute(key) { _, current ->
      if (current == null) {
        candidate
      } else if (
        eventId == 0x06L &&
          pressCount <= 0 &&
          (current.eventId == 0x02L || current.eventId == 0x04L)
      ) {
        // Matter Switch spec: MultiPressComplete with TotalNumberOfPressesCounted=0 after LongPress
        current.copy(eventNumber = maxOf(current.eventNumber, candidate.eventNumber))
      } else if (timestampMs > current.updatedAtMs) {
        candidate
      } else if (timestampMs == current.updatedAtMs) {
        if (
          candidate.priorityRank() > current.priorityRank() ||
            (candidate.priorityRank() == current.priorityRank() &&
              candidate.pressCount >= current.pressCount)
        ) {
          candidate.copy(eventNumber = maxOf(current.eventNumber, candidate.eventNumber))
        } else {
          current.copy(eventNumber = maxOf(current.eventNumber, candidate.eventNumber))
        }
      } else {
        current
      }
    }
  }

  fun getLastSwitchEvent(nodeId: Long? = null, endpointId: Int? = null): CachedSwitchEvent? {
    return switchEvents.values
      .filter {
        (nodeId == null || it.nodeId == nodeId) &&
          (endpointId == null || it.endpointId == endpointId)
      }
      .maxWithOrNull(
        compareBy<CachedSwitchEvent> { it.updatedAtMs }
          .thenBy { it.eventNumber }
          .thenBy { it.priorityRank() }
      )
  }

  /** Highest Switch event number seen from [nodeId], used as eventMin when resubscribing. */
  fun lastSwitchEventNumber(nodeId: Long): Long? =
    switchEvents.values
      .filter { it.nodeId == nodeId && it.eventNumber > 0L }
      .maxOfOrNull { it.eventNumber }

  fun put(
    nodeId: Long,
    endpointId: Int,
    clusterId: Long,
    attributeId: Long,
    value: Any?,
    timestampMs: Long,
    source: Source,
  ) {
    val key = buildKey(nodeId, endpointId, clusterId, attributeId)
    cache
      .compute(key) { _, current ->
        if (current == null || current.updatedAtMs <= timestampMs) {
          CachedAttribute(nodeId, endpointId, clusterId, attributeId, value, timestampMs, source)
        } else {
          current
        }
      }
      ?.let {
        _changes.tryEmit(it)
        scheduleSnapshot()
      }
  }

  private fun scheduleSnapshot() {
    saveJob?.cancel()
    saveJob =
      scope.launch(Dispatchers.IO) {
        delay(2000)
        saveSnapshot()
      }
  }

  private suspend fun saveSnapshot() =
    withContext(Dispatchers.IO) {
      val file = snapshotFile ?: return@withContext
      try {
        val data = ArrayList(cache.values)
        val json = gson.toJson(data)
        file.writeText(json)
      } catch (e: Exception) {
        Log.e(TAG, "Failed to save snapshot", e)
      }
    }

  private suspend fun loadSnapshot() =
    withContext(Dispatchers.IO) {
      val file = snapshotFile ?: return@withContext
      if (!file.exists()) return@withContext
      try {
        val json = file.readText()
        val type = object : TypeToken<List<CachedAttribute>>() {}.type
        val data: List<CachedAttribute> = gson.fromJson(json, type) ?: return@withContext
        data.forEach {
          val snapshotItem = it.copy(source = Source.SNAPSHOT)
          // Never let the snapshot overwrite a value that is as new or newer.
          cache.compute(buildKey(it.nodeId, it.endpointId, it.clusterId, it.attributeId)) {
            _,
            current ->
            if (current == null || current.updatedAtMs < snapshotItem.updatedAtMs) snapshotItem
            else current
          }
        }
        loadedSnapshotPath = file.path
      } catch (e: Exception) {
        Log.e(TAG, "Failed to load snapshot", e)
      }
    }

  /** Retrieves high-level cached state of a lighting device endpoint. */
  fun getLightState(nodeId: Long, endpointId: Int = 1): LightState {
    val onOffVal =
      get(nodeId, endpointId, MatterClusterMetaRegistry.CLUSTER_ON_OFF, 0x0000L)?.value
        ?: findAll(MatterClusterMetaRegistry.CLUSTER_ON_OFF, 0x0000L)
          .firstOrNull { it.nodeId == nodeId }
          ?.value
    val isOn =
      when (onOffVal) {
        is Boolean -> onOffVal
        is Number -> onOffVal.toInt() != 0
        else -> null
      }

    val levelVal =
      get(nodeId, endpointId, MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL, 0x0000L)?.value
        ?: findAll(MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL, 0x0000L)
          .firstOrNull { it.nodeId == nodeId }
          ?.value
    val level = (levelVal as? Number)?.toInt()
    val levelPct = level?.let { MatterClusterMetaRegistry.levelToPercentage(it.toUByte()).toInt() }

    val colorVal =
      get(nodeId, endpointId, MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL, 0x0007L)?.value
        ?: findAll(MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL, 0x0007L)
          .firstOrNull { it.nodeId == nodeId }
          ?.value
    val mireds = (colorVal as? Number)?.toInt()
    val kelvin = mireds?.let { MatterClusterMetaRegistry.miredsToKelvin(it) }

    return LightState(
      isOn = isOn,
      level = level,
      levelPercent = levelPct,
      colorTemperatureMireds = mireds,
      kelvin = kelvin,
    )
  }

  // For tests
  fun clear() {
    cache.clear()
    switchEvents.clear()
    loadedSnapshotPath = null
    snapshotFile?.delete()
  }
}

/** High-level cached state for a light node. */
data class LightState(
  val isOn: Boolean? = null,
  val level: Int? = null,
  val levelPercent: Int? = null,
  val colorTemperatureMireds: Int? = null,
  val kelvin: Int? = null,
)
