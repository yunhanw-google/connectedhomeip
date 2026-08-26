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
import android.util.Log
import chip.devicecontroller.ChipDeviceController
import chip.devicecontroller.InvokeCallback
import chip.devicecontroller.ReportCallback
import chip.devicecontroller.WriteAttributesCallback
import chip.devicecontroller.model.AttributeWriteRequest
import chip.devicecontroller.model.ChipAttributePath
import chip.devicecontroller.model.ChipEventPath
import chip.devicecontroller.model.ChipPathId
import chip.devicecontroller.model.InvokeElement
import chip.devicecontroller.model.NodeState
import com.google.chip.chiptool.ChipClient
import kotlin.coroutines.resume
import kotlinx.coroutines.suspendCancellableCoroutine

/**
 * Result of dispatching a Matter Interaction Model operation.
 */
data class MatterDispatchResult(
  val isSuccess: Boolean,
  val responseCode: Long = 0,
  val message: String = "",
  val voiceResponse: String = "",
  val decodedAttribute: DecodedAttributeValue? = null,
  val rawTlv: ByteArray? = null
)

/**
 * Universal Matter Interaction Model Dispatcher.
 * Unifies command invocations, attribute writes, and attribute reads into a generic polymorphic
 * execution engine on top of [ChipDeviceController], handling device connection retrieval,
 * TLV serialization/deserialization, and voice feedback generation.
 */
class UniversalMatterDispatcher(
  private val nodeRegistry: CommissionedNodeRegistry
) {
  companion object {
    private const val TAG = "MatterDispatcher"
    private const val DEFAULT_IM_TIMEOUT_MS = 10_000
    private const val DEFAULT_TIMED_REQUEST_TIMEOUT_MS = 5_000
  }

  /**
   * Dispatches a compiled [VoiceIntent] asynchronously using coroutines.
   */
  suspend fun dispatchIntent(
    context: Context,
    intent: VoiceIntent
  ): MatterDispatchResult {
    return when (intent.interactionType) {
      InteractionType.INVOKE_COMMAND -> {
        val cmdId = intent.commandId ?: 0x00L
        invokeCommand(
          context = context,
          nodeId = intent.targetNodeId,
          endpointId = intent.targetEndpointId,
          clusterId = intent.clusterId,
          commandId = cmdId,
          params = intent.resolvedParameters,
          targetDescription = intent.targetDescription
        )
      }

      InteractionType.WRITE_ATTRIBUTE -> {
        val attrId = intent.attributeId ?: 0x0000L
        val value = intent.resolvedParameters["value"]
        writeAttribute(
          context = context,
          nodeId = intent.targetNodeId,
          endpointId = intent.targetEndpointId,
          clusterId = intent.clusterId,
          attributeId = attrId,
          value = value,
          targetDescription = intent.targetDescription
        )
      }

      InteractionType.READ_ATTRIBUTE -> {
        val attrId = intent.attributeId ?: 0x0000L
        readAttribute(
          context = context,
          nodeId = intent.targetNodeId,
          endpointId = intent.targetEndpointId,
          clusterId = intent.clusterId,
          attributeId = attrId,
          targetDescription = intent.targetDescription
        )
      }

      InteractionType.SUBSCRIBE_ATTRIBUTE -> {
        MatterDispatchResult(
          isSuccess = true,
          message = "Subscription established for ${intent.targetDescription}",
          voiceResponse = "Subscribed to updates for ${intent.targetDescription}"
        )
      }
    }
  }

  /**
   * Invokes an arbitrary Matter cluster command with dynamic TLV argument encoding.
   */
  suspend fun invokeCommand(
    context: Context,
    nodeId: Long,
    endpointId: Int,
    clusterId: Long,
    commandId: Long,
    params: Map<String, Any?> = emptyMap(),
    targetDescription: String = "Node $nodeId"
  ): MatterDispatchResult {
    val clusterMeta = MatterClusterMetaRegistry.getCluster(clusterId)
    val commandMeta = clusterMeta?.commands?.get(commandId)
    val commandName = commandMeta?.name ?: "Command 0x${commandId.toString(16)}"
    val clusterName = clusterMeta?.name ?: "Cluster 0x${clusterId.toString(16)}"

    // 1. Dynamic TLV encoding
    val tlvBytes = try {
      if (commandMeta != null) {
        UniversalTlvEncoder.encodeCommandPayload(commandMeta, params)
      } else {
        UniversalTlvEncoder.encodeCommandPayload(
          MatterCommandMeta(commandId, commandName, ""),
          params
        )
      }
    } catch (e: Exception) {
      Log.e(TAG, "TLV Encoding error for $clusterName::$commandName", e)
      return MatterDispatchResult(
        isSuccess = false,
        message = "TLV Encoding failed: ${e.message}",
        voiceResponse = "Failed to encode parameters for $commandName on $targetDescription."
      )
    }

    // 2. Get device pointer
    val devicePointer = try {
      ChipClient.getConnectedDevicePointer(context, nodeId)
    } catch (e: Exception) {
      Log.e(TAG, "Failed to get connected device pointer for node $nodeId", e)
      return MatterDispatchResult(
        isSuccess = false,
        message = "Failed to connect to node $nodeId: ${e.message}",
        voiceResponse = "Unable to connect to $targetDescription."
      )
    }

    val timedTimeout = if (commandMeta?.isTimed == true) DEFAULT_TIMED_REQUEST_TIMEOUT_MS else 0
    val invokeElement = InvokeElement.newInstance(
      endpointId,
      clusterId,
      commandId,
      tlvBytes,
      null
    )

    val controller = ChipClient.getDeviceController(context)

    return suspendCancellableCoroutine { continuation ->
      controller.invoke(
        object : InvokeCallback {
          override fun onError(ex: Exception?) {
            val errorMsg = ex?.message ?: "Unknown invoke error"
            Log.e(TAG, "Invoke command error on $targetDescription: $errorMsg", ex)
            continuation.resume(
              MatterDispatchResult(
                isSuccess = false,
                message = errorMsg,
                voiceResponse = "Error executing $commandName on $targetDescription: $errorMsg"
              )
            )
          }

          override fun onResponse(responseElement: InvokeElement?, successCode: Long) {
            val voiceMsg = synthesizeVoiceCommandFeedback(clusterMeta, commandMeta, params, targetDescription)
            continuation.resume(
              MatterDispatchResult(
                isSuccess = true,
                responseCode = successCode,
                message = "Command $commandName succeeded on $targetDescription",
                voiceResponse = voiceMsg,
                rawTlv = responseElement?.tlvByteArray
              )
            )
          }
        },
        devicePointer,
        invokeElement,
        timedTimeout,
        DEFAULT_IM_TIMEOUT_MS
      )
    }
  }

  /**
   * Writes an attribute on a target node endpoint.
   */
  suspend fun writeAttribute(
    context: Context,
    nodeId: Long,
    endpointId: Int,
    clusterId: Long,
    attributeId: Long,
    value: Any?,
    targetDescription: String = "Node $nodeId"
  ): MatterDispatchResult {
    val clusterMeta = MatterClusterMetaRegistry.getCluster(clusterId)
    val attrMeta = clusterMeta?.attributes?.get(attributeId)
    val attrName = attrMeta?.name ?: "Attribute 0x${attributeId.toString(16)}"

    val tlvBytes = try {
      if (attrMeta != null) {
        UniversalTlvEncoder.encodeAttributeWritePayload(attrMeta, value)
      } else {
        UniversalTlvEncoder.encodeAttributeWritePayload(
          MatterAttributeMeta(attributeId, attrName, MatterDataType.INT64),
          value
        )
      }
    } catch (e: Exception) {
      return MatterDispatchResult(
        isSuccess = false,
        message = "TLV write payload encoding error: ${e.message}",
        voiceResponse = "Could not format value for $attrName."
      )
    }

    val devicePointer = try {
      ChipClient.getConnectedDevicePointer(context, nodeId)
    } catch (e: Exception) {
      return MatterDispatchResult(
        isSuccess = false,
        message = "Connection failed: ${e.message}",
        voiceResponse = "Unable to connect to $targetDescription."
      )
    }

    val writeRequest = AttributeWriteRequest.newInstance(
      ChipPathId.forId(endpointId.toLong()),
      ChipPathId.forId(clusterId),
      ChipPathId.forId(attributeId),
      tlvBytes
    )

    val controller = ChipClient.getDeviceController(context)

    return suspendCancellableCoroutine { continuation ->
      controller.write(
        object : WriteAttributesCallback {
          override fun onError(attributePath: ChipAttributePath?, ex: Exception) {
            val err = ex.message ?: "Write attribute failed"
            continuation.resume(
              MatterDispatchResult(
                isSuccess = false,
                message = err,
                voiceResponse = "Failed to set $attrName on $targetDescription: $err"
              )
            )
          }

          override fun onResponse(attributePath: ChipAttributePath, status: chip.devicecontroller.model.Status) {
            continuation.resume(
              MatterDispatchResult(
                isSuccess = true,
                message = "Wrote $value to $attrName on $targetDescription",
                voiceResponse = "Updated $attrName to $value on $targetDescription"
              )
            )
          }
        },
        devicePointer,
        listOf(writeRequest),
        0,
        DEFAULT_IM_TIMEOUT_MS
      )
    }
  }

  /**
   * Reads an attribute from a target node endpoint and parses the TLV response into human voice text.
   */
  suspend fun readAttribute(
    context: Context,
    nodeId: Long,
    endpointId: Int,
    clusterId: Long,
    attributeId: Long,
    targetDescription: String = "Node $nodeId"
  ): MatterDispatchResult {
    val clusterMeta = MatterClusterMetaRegistry.getCluster(clusterId)
    val attrMeta = clusterMeta?.attributes?.get(attributeId)
    val attrName = attrMeta?.name ?: "Attribute 0x${attributeId.toString(16)}"

    val devicePointer = try {
      ChipClient.getConnectedDevicePointer(context, nodeId)
    } catch (e: Exception) {
      return MatterDispatchResult(
        isSuccess = false,
        message = "Connection failed: ${e.message}",
        voiceResponse = "Unable to connect to $targetDescription."
      )
    }

    val attributePath = ChipAttributePath.newInstance(endpointId, clusterId, attributeId)
    val controller = ChipClient.getDeviceController(context)

    return suspendCancellableCoroutine { continuation ->
      controller.readPath(
        object : ReportCallback {
          override fun onError(
            attributePath: ChipAttributePath?,
            eventPath: ChipEventPath?,
            ex: Exception
          ) {
            continuation.resume(
              MatterDispatchResult(
                isSuccess = false,
                message = "Read error: ${ex.message}",
                voiceResponse = "Failed to read $attrName from $targetDescription."
              )
            )
          }

          override fun onReport(nodeState: NodeState) {
            val tlv = nodeState
              .getEndpointState(endpointId)
              ?.getClusterState(clusterId)
              ?.getAttributeState(attributeId)
              ?.tlv


            if (tlv == null) {
              continuation.resume(
                MatterDispatchResult(
                  isSuccess = false,
                  message = "No TLV data received in attribute report",
                  voiceResponse = "$attrName on $targetDescription is currently unavailable."
                )
              )
              return
            }

            val decoded = if (attrMeta != null) {
              UniversalTlvEncoder.decodeAttributeReport(attrMeta, tlv)
            } else {
              DecodedAttributeValue(
                rawValue = null,
                formattedValue = "Unknown",
                attributeMeta = null
              )
            }

            val voiceFeedback = synthesizeVoiceStatusReport(targetDescription, attrMeta, decoded)

            continuation.resume(
              MatterDispatchResult(
                isSuccess = true,
                message = "Read $attrName: ${decoded.formattedValue}",
                voiceResponse = voiceFeedback,
                decodedAttribute = decoded,
                rawTlv = tlv
              )
            )
          }
        },
        devicePointer,
        listOf(attributePath),
        null,
        false,
        DEFAULT_IM_TIMEOUT_MS
      )
    }
  }

  // --- Voice Feedback Natural Language Synthesis ---

  private fun synthesizeVoiceCommandFeedback(
    clusterMeta: MatterClusterMeta?,
    commandMeta: MatterCommandMeta?,
    params: Map<String, Any?>,
    targetDesc: String
  ): String {
    if (clusterMeta == null || commandMeta == null) return "Command sent to $targetDesc."

    return when (clusterMeta.clusterId) {
      MatterClusterMetaRegistry.CLUSTER_ON_OFF -> {
        when (commandMeta.name) {
          "On" -> "Turned on $targetDesc."
          "Off" -> "Turned off $targetDesc."
          "Toggle" -> "Toggled $targetDesc."
          else -> "Executed ${commandMeta.name} on $targetDesc."
        }
      }

      MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL -> {
        val pct = params["brightnessPercentage"]
          ?: params["level"]?.let { (it as? Number)?.let { l -> MatterClusterMetaRegistry.levelToPercentage(l.toLong().toUByte()).toInt() } }
        if (pct != null) "Set brightness of $targetDesc to $pct%." else "Adjusted level of $targetDesc."
      }

      MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL -> {
        if (params.containsKey("kelvin")) {
          "Set $targetDesc color temperature to ${params["kelvin"]}K."
        } else if (params.containsKey("colorTemperatureMireds")) {
          val mireds = (params["colorTemperatureMireds"] as? Number)?.toInt() ?: 250
          val kelvin = MatterClusterMetaRegistry.miredsToKelvin(mireds)
          "Set $targetDesc warmth to ${kelvin}K."
        } else {
          "Changed color of $targetDesc."
        }
      }

      MatterClusterMetaRegistry.CLUSTER_DOOR_LOCK -> {
        when (commandMeta.name) {
          "LockDoor" -> "Locked $targetDesc."
          "UnlockDoor" -> "Unlocked $targetDesc."
          "UnlockWithTimeout" -> "Unlocked $targetDesc for ${params["timeoutSeconds"]} seconds."
          else -> "Updated lock state on $targetDesc."
        }
      }

      MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING -> {
        when (commandMeta.name) {
          "UpOrOpen" -> "Opened $targetDesc."
          "DownOrClose" -> "Closed $targetDesc."
          "StopMotion" -> "Stopped $targetDesc."
          "GoToLiftPercentage" -> {
            val p = (params["liftPercent100thsValue"] as? Number)?.toDouble()?.div(100.0) ?: 50.0
            "Set $targetDesc position to ${p.toInt()}%."
          }
          else -> "Adjusted $targetDesc."
        }
      }

      MatterClusterMetaRegistry.CLUSTER_THERMOSTAT -> {
        if (commandMeta.name == "SetpointRaiseLower") {
          val amount = (params["amount"] as? Number)?.toDouble()?.div(10.0) ?: 1.0
          val direction = if (amount >= 0) "raised" else "lowered"
          "Temperature $direction by ${Math.abs(amount)}°C on $targetDesc."
        } else {
          "Updated thermostat settings on $targetDesc."
        }
      }

      MatterClusterMetaRegistry.CLUSTER_MEDIA_PLAYBACK -> {
        when (commandMeta.name) {
          "Play" -> "Resumed playback on $targetDesc."
          "Pause" -> "Paused playback on $targetDesc."
          "Stop" -> "Stopped playback on $targetDesc."
          "Next" -> "Skipped to next track on $targetDesc."
          "Previous" -> "Playing previous track on $targetDesc."
          else -> "Executed ${commandMeta.name} on $targetDesc."
        }
      }

      MatterClusterMetaRegistry.CLUSTER_AUDIO_OUTPUT -> {
        when (commandMeta.name) {
          "VolumeUp" -> "Turned up volume on $targetDesc."
          "VolumeDown" -> "Turned down volume on $targetDesc."
          "Mute" -> "Muted $targetDesc."
          "Unmute" -> "Unmuted $targetDesc."
          else -> "Updated audio on $targetDesc."
        }
      }

      MatterClusterMetaRegistry.CLUSTER_RVC_RUN_MODE -> {
        when ((params["newMode"] as? Number)?.toInt()) {
          1 -> "Started vacuuming with $targetDesc."
          3 -> "Sending $targetDesc back to the dock."
          else -> "Set $targetDesc run mode."
        }
      }

      MatterClusterMetaRegistry.CLUSTER_ENERGY_EVSE -> {
        when (commandMeta.name) {
          "StartCharge" -> "Started charging EV on $targetDesc."
          "StopCharge" -> "Stopped charging EV on $targetDesc."
          "SetMaxChargeRate" -> {
            val amps = (params["maxChargeCurrent"] as? Number)?.toDouble()?.div(1000.0) ?: 32.0
            "Set EV charging current to ${amps.toInt()}A on $targetDesc."
          }
          else -> "Updated EV charger on $targetDesc."
        }
      }

      else -> "Successfully executed ${commandMeta.name} on $targetDesc."
    }
  }

  private fun synthesizeVoiceStatusReport(
    targetDesc: String,
    attrMeta: MatterAttributeMeta?,
    decoded: DecodedAttributeValue
  ): String {
    if (attrMeta == null) return "$targetDesc status: ${decoded.formattedValue}"

    return when (attrMeta.name) {
      "LocalTemperature", "MeasuredValue" -> {
        if (attrMeta.unit == "°C") {
          "The temperature at $targetDesc is ${decoded.formattedValue}."
        } else if (attrMeta.unit == "%") {
          "The relative humidity at $targetDesc is ${decoded.formattedValue}."
        } else {
          "$targetDesc ${attrMeta.name} is ${decoded.formattedValue}."
        }
      }
      "LockState" -> "$targetDesc is currently ${decoded.formattedValue}."
      "OnOff" -> "$targetDesc is currently turned ${decoded.formattedValue}."
      "CurrentLevel" -> "$targetDesc brightness is ${decoded.formattedValue}."
      "StateValue" -> "$targetDesc is currently ${decoded.formattedValue}."
      "Occupancy" -> {
        if (decoded.formattedValue.contains("1") || decoded.formattedValue.equals("Occupied", ignoreCase = true)) {
          "Motion is detected at $targetDesc."
        } else {
          "No motion detected at $targetDesc."
        }
      }
      "SmokeState" -> "Smoke sensor on $targetDesc is ${decoded.formattedValue}."
      "COState" -> "Carbon monoxide status on $targetDesc is ${decoded.formattedValue}."
      else -> "$targetDesc ${attrMeta.name} is ${decoded.formattedValue}."
    }
  }
}
