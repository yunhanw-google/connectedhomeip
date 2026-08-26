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

import com.google.gson.Gson
import com.google.gson.GsonBuilder
import com.google.gson.JsonObject
import com.google.gson.JsonParser

/**
 * Type of Matter Interaction Model action.
 */
enum class InteractionType {
  INVOKE_COMMAND,
  WRITE_ATTRIBUTE,
  READ_ATTRIBUTE,
  SUBSCRIBE_ATTRIBUTE
}

/**
 * Structured Matter Voice Intent produced by compiling Gemini Nano / AICore output.
 */
data class VoiceIntent(
  val rawUserPrompt: String,
  val interactionType: InteractionType,
  val targetNodeId: Long,
  val targetEndpointId: Int,
  val clusterId: Long,
  val commandId: Long? = null,
  val attributeId: Long? = null,
  val resolvedParameters: Map<String, Any?> = emptyMap(),
  val targetDescription: String = "",
  val naturalFeedbackPrompt: String = ""
)

/**
 * Function Declaration Schema for Gemini Nano / AICore Function Calling.
 */
data class NanoToolProperty(
  val type: String,
  val description: String,
  val enum: List<String>? = null
)

data class NanoToolParameters(
  val type: String = "OBJECT",
  val properties: Map<String, NanoToolProperty>,
  val required: List<String> = emptyList()
)

data class NanoFunctionDeclaration(
  val name: String,
  val description: String,
  val parameters: NanoToolParameters
)

data class NanoTool(
  val functionDeclarations: List<NanoFunctionDeclaration>
)

/**
 * Dynamic Fabric-Aware Gemini Nano Prompt & Tool Generator.
 * Generates constrained JSON Schema tool definitions tailored to the commissioned Matter nodes
 * on the local fabric, synthesizes context prompts, and parses AICore tool call responses into
 * executable Matter Voice Intents.
 */
class DynamicNanoToolGenerator(
  private val nodeRegistry: CommissionedNodeRegistry
) {
  private val gson: Gson = GsonBuilder().setPrettyPrinting().create()

  /**
   * Generates dynamic Function Calling tools for Gemini Nano, strictly scoped to the clusters
   * present on the user's commissioned fabric.
   */
  fun generateGeminiTools(): NanoTool {
    val fabricClusters = nodeRegistry.getFabricClusters()
    val functionDeclarations = mutableListOf<NanoFunctionDeclaration>()

    for (cluster in fabricClusters) {
      // 1. Generate tool for each cluster command
      for ((cmdId, cmdMeta) in cluster.commands) {
        val toolName = "matter_${cluster.name.lowercase()}_${cmdMeta.name.lowercase()}"
        val properties = mutableMapOf<String, NanoToolProperty>()
        val requiredFields = mutableListOf<String>()

        // Target device identifier
        properties["targetDeviceOrRoom"] = NanoToolProperty(
          type = "STRING",
          description = "Target device name, label, or room name (e.g. 'Living Room Light', 'Front Door', 'Thermostat')"
        )
        requiredFields.add("targetDeviceOrRoom")

        for (field in cmdMeta.requestFields) {
          val propType = when (field.type) {
            MatterDataType.BOOLEAN -> "BOOLEAN"
            MatterDataType.FLOAT32, MatterDataType.FLOAT64 -> "NUMBER"
            MatterDataType.UTF8_STRING -> "STRING"
            else -> "INTEGER"
          }
          val enumList = if (field.enumValues.isNotEmpty()) field.enumValues.keys.toList() else null
          properties[field.name] = NanoToolProperty(
            type = propType,
            description = "${field.description}${if (field.unit != null) " in ${field.unit}" else ""}",
            enum = enumList
          )
          if (!field.isOptional && !field.isNullable) {
            requiredFields.add(field.name)
          }
        }

        // Special convenience parameter for brightness/color temperature kelvin
        if (cluster.clusterId == MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL && cmdMeta.name.startsWith("MoveToLevel")) {
          properties["brightnessPercentage"] = NanoToolProperty(
            type = "NUMBER",
            description = "Brightness percentage from 0 to 100"
          )
        }
        if (cluster.clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL && cmdMeta.name == "MoveToColorTemperature") {
          properties["kelvin"] = NanoToolProperty(
            type = "INTEGER",
            description = "Color temperature in Kelvin (e.g. 2700 for warm white, 4000 for neutral, 6500 for daylight)"
          )
        }

        functionDeclarations.add(
          NanoFunctionDeclaration(
            name = toolName,
            description = "${cmdMeta.description} on ${cluster.name} cluster (${cluster.description})",
            parameters = NanoToolParameters(
              properties = properties,
              required = requiredFields
            )
          )
        )
      }

      // 2. Generate tool for Reading Cluster Attributes / Status
      val readableAttributes = cluster.attributes.values.filter { it.isReportable }
      if (readableAttributes.isNotEmpty()) {
        val readToolName = "matter_${cluster.name.lowercase()}_get_status"
        val properties = mutableMapOf(
          "targetDeviceOrRoom" to NanoToolProperty(
            type = "STRING",
            description = "Target device name, label, or room (e.g. 'Living Room', 'Front Door')"
          ),
          "attributeName" to NanoToolProperty(
            type = "STRING",
            description = "Specific attribute to query or 'all' for full status",
            enum = readableAttributes.map { it.name } + listOf("all")
          )
        )
        functionDeclarations.add(
          NanoFunctionDeclaration(
            name = readToolName,
            description = "Query current status, state, or sensor readings for ${cluster.name} cluster (${cluster.description})",
            parameters = NanoToolParameters(
              properties = properties,
              required = listOf("targetDeviceOrRoom")
            )
          )
        )
      }

      // 3. Generate tool for Writing Writable Cluster Attributes
      val writableAttributes = cluster.attributes.values.filter { it.isWritable }
      for (attr in writableAttributes) {
        val writeToolName = "matter_${cluster.name.lowercase()}_set_${attr.name.lowercase()}"
        val propType = when (attr.type) {
          MatterDataType.BOOLEAN -> "BOOLEAN"
          MatterDataType.FLOAT32, MatterDataType.FLOAT64 -> "NUMBER"
          MatterDataType.UTF8_STRING -> "STRING"
          else -> "INTEGER"
        }
        val properties = mutableMapOf(
          "targetDeviceOrRoom" to NanoToolProperty(
            type = "STRING",
            description = "Target device name, label, or room name"
          ),
          "value" to NanoToolProperty(
            type = propType,
            description = "New value for attribute ${attr.name}${if (attr.unit != null) " in ${attr.unit}" else ""}",
            enum = if (attr.enumValues.isNotEmpty()) attr.enumValues.keys.toList() else null
          )
        )
        functionDeclarations.add(
          NanoFunctionDeclaration(
            name = writeToolName,
            description = "Directly update ${attr.name} attribute on ${cluster.name} cluster",
            parameters = NanoToolParameters(
              properties = properties,
              required = listOf("targetDeviceOrRoom", "value")
            )
          )
        )
      }
    }

    return NanoTool(functionDeclarations)
  }

  /**
   * Generates Gemini Tools in JSON format.
   */
  fun generateGeminiToolsJson(): String {
    return gson.toJson(generateGeminiTools())
  }

  /**
   * Generates the dynamic system prompt for Gemini Nano containing the active fabric topology.
   */
  fun generateSystemPrompt(): String {
    val builder = StringBuilder()
    builder.appendLine("You are the on-device Matter Voice Controller engine running inside Android CHIPTool with AICore / Gemini Nano.")
    builder.appendLine("Your duty is to map user natural language home automation requests directly to the corresponding Matter cluster tool call.")
    builder.appendLine()
    builder.appendLine("### ACTIVE MATTER FABRIC INVENTORY:")
    val nodes = nodeRegistry.getAllNodes()
    if (nodes.isEmpty()) {
      builder.appendLine("No devices currently commissioned on this fabric.")
    } else {
      for (node in nodes) {
        val aliasText = if (node.aliases.isNotEmpty()) " [Aliases: ${node.aliases.joinToString(", ")}]" else ""
        builder.appendLine("- Node 0x${node.nodeId.toString(16).uppercase()}: '${node.nodeLabel}' in room '${node.roomName}'$aliasText (${node.productName})")
        for (ep in node.endpoints) {
          val clusterNames = ep.serverClusters.mapNotNull { MatterClusterMetaRegistry.getCluster(it)?.name }
          builder.appendLine("    Endpoint ${ep.endpointId} [${ep.deviceTypeName}]: Clusters [${clusterNames.joinToString(", ")}]")
        }
      }
    }
    builder.appendLine()
    builder.appendLine("### INSTRUCTIONS:")
    builder.appendLine("1. When the user asks to control a device, select the exact corresponding Matter cluster tool function.")
    builder.appendLine("2. For brightness, you can supply either level (0..254) or brightnessPercentage (0..100).")
    builder.appendLine("3. For color temperature, if the user asks for Kelvin (e.g. 2700K warm white, 6500K daylight), supply 'kelvin'.")
    builder.appendLine("4. For temperature adjustments, handle both relative raising/lowering (SetpointRaiseLower) and direct setpoint writes.")
    builder.appendLine("5. For queries (e.g. 'is door locked?', 'what is the humidity?'), call the corresponding get_status tool.")
    return builder.toString()
  }

  /**
   * Compiles an AICore / Gemini Nano tool call into a validated executable [VoiceIntent].
   *
   * @param functionName Function name called by Gemini Nano (e.g. "matter_onoff_on", "matter_levelcontrol_movetolevel")
   * @param arguments JsonObject or Map containing the arguments supplied by Gemini Nano
   * @param userPrompt Original user voice prompt
   */
  fun compileToolCall(
    functionName: String,
    arguments: Map<String, Any?>,
    userPrompt: String
  ): VoiceIntent {
    val cleanName = functionName.removePrefix("matter_")
    val parts = cleanName.split("_")
    require(parts.size >= 2) { "Invalid function call format: $functionName" }

    val clusterName = parts[0]
    val clusterMeta = MatterClusterMetaRegistry.getClusterByName(clusterName)
      ?: throw IllegalArgumentException("Unknown cluster in tool call: $clusterName")

    val targetQuery = (arguments["targetDeviceOrRoom"] ?: arguments["target"] ?: "").toString()
    val targetPair = nodeRegistry.resolveTarget(targetQuery, clusterMeta.clusterId)
      ?: throw IllegalStateException("No commissioned node found supporting ${clusterMeta.name} matching '$targetQuery'")

    val targetNode = targetPair.first
    val targetEndpoint = targetPair.second

    // Check if this is a Status Read
    if (parts.size >= 2 && parts[1] == "get" && parts.getOrNull(2) == "status") {
      val attrName = arguments["attributeName"]?.toString() ?: "all"
      val matchedAttr = if (attrName != "all") {
        clusterMeta.attributes.values.firstOrNull { it.name.equals(attrName, ignoreCase = true) }
      } else {
        clusterMeta.attributes.values.firstOrNull { it.isReportable }
      }

      return VoiceIntent(
        rawUserPrompt = userPrompt,
        interactionType = InteractionType.READ_ATTRIBUTE,
        targetNodeId = targetNode.nodeId,
        targetEndpointId = targetEndpoint.endpointId,
        clusterId = clusterMeta.clusterId,
        attributeId = matchedAttr?.attributeId ?: 0x0000L,
        resolvedParameters = arguments,
        targetDescription = "${targetNode.nodeLabel} (${targetNode.roomName})",
        naturalFeedbackPrompt = "Reading status of ${clusterMeta.name} on ${targetNode.nodeLabel}"
      )
    }

    // Check if this is an Attribute Write
    if (parts.size >= 3 && parts[1] == "set") {
      val attrName = parts.subList(2, parts.size).joinToString("")
      val matchedAttr = clusterMeta.attributes.values.firstOrNull {
        it.name.replace("_", "").equals(attrName, ignoreCase = true)
      } ?: throw IllegalArgumentException("Attribute '$attrName' not found on cluster ${clusterMeta.name}")

      val value = arguments["value"] ?: arguments[matchedAttr.name]
      return VoiceIntent(
        rawUserPrompt = userPrompt,
        interactionType = InteractionType.WRITE_ATTRIBUTE,
        targetNodeId = targetNode.nodeId,
        targetEndpointId = targetEndpoint.endpointId,
        clusterId = clusterMeta.clusterId,
        attributeId = matchedAttr.attributeId,
        resolvedParameters = mapOf("value" to value),
        targetDescription = "${targetNode.nodeLabel} (${targetNode.roomName})",
        naturalFeedbackPrompt = "Setting ${matchedAttr.name} on ${targetNode.nodeLabel}"
      )
    }

    // Otherwise, this is a Command Invocation
    val actionName = parts.subList(1, parts.size).joinToString("")
    val commandMeta = clusterMeta.commands.values.firstOrNull {
      it.name.replace("_", "").equals(actionName, ignoreCase = true)
    } ?: throw IllegalArgumentException("Command '$actionName' not found in cluster ${clusterMeta.name}")

    // Parameter transformations and unit normalization
    val resolvedParams = HashMap<String, Any?>(arguments)

    // 1. Percentage to Level conversion
    if (clusterMeta.clusterId == MatterClusterMetaRegistry.CLUSTER_LEVEL_CONTROL) {
      if (resolvedParams.containsKey("brightnessPercentage")) {
        val pct = (resolvedParams["brightnessPercentage"] as? Number)?.toDouble() ?: 100.0
        resolvedParams["level"] = MatterClusterMetaRegistry.percentageToLevel(pct).toLong()
      }
    }

    // 2. Kelvin to Mireds conversion
    if (clusterMeta.clusterId == MatterClusterMetaRegistry.CLUSTER_COLOR_CONTROL) {
      if (resolvedParams.containsKey("kelvin")) {
        val k = (resolvedParams["kelvin"] as? Number)?.toInt() ?: 4000
        resolvedParams["colorTemperatureMireds"] = MatterClusterMetaRegistry.kelvinToMireds(k).toLong()
      }
    }

    // 3. Window covering percentage (0..100 -> 0..10000 100ths)
    if (clusterMeta.clusterId == MatterClusterMetaRegistry.CLUSTER_WINDOW_COVERING) {
      if (resolvedParams.containsKey("percent") || resolvedParams.containsKey("percentage")) {
        val p = ((resolvedParams["percent"] ?: resolvedParams["percentage"]) as? Number)?.toDouble() ?: 0.0
        resolvedParams["liftPercent100thsValue"] = (p * 100.0).toLong().coerceIn(0L, 10000L)
      }
    }

    return VoiceIntent(
      rawUserPrompt = userPrompt,
      interactionType = InteractionType.INVOKE_COMMAND,
      targetNodeId = targetNode.nodeId,
      targetEndpointId = targetEndpoint.endpointId,
      clusterId = clusterMeta.clusterId,
      commandId = commandMeta.commandId,
      resolvedParameters = resolvedParams,
      targetDescription = "${targetNode.nodeLabel} (${targetNode.roomName})",
      naturalFeedbackPrompt = "Executing ${commandMeta.name} on ${targetNode.nodeLabel}"
    )
  }

  /**
   * Helper parser from Gemini JSON output string.
   */
  fun parseToolCallJson(jsonString: String, userPrompt: String): VoiceIntent {
    val json = JsonParser().parse(jsonString).asJsonObject
    val funcName = (if (json.has("name")) json.get("name").asString else null)
      ?: (if (json.has("function")) json.get("function").asString else null)
      ?: throw IllegalArgumentException("Missing function name in tool call JSON")


    val argsObj = json.getAsJsonObject("parameters")
      ?: json.getAsJsonObject("arguments")
      ?: JsonObject()

    val argsMap = mutableMapOf<String, Any?>()
    for (key in argsObj.keySet()) {
      val elem = argsObj.get(key)
      if (elem.isJsonPrimitive) {
        val prim = elem.asJsonPrimitive
        if (prim.isBoolean) argsMap[key] = prim.asBoolean
        else if (prim.isNumber) argsMap[key] = prim.asNumber
        else argsMap[key] = prim.asString
      } else if (elem.isJsonNull) {
        argsMap[key] = null
      } else {
        argsMap[key] = elem.toString()
      }
    }

    return compileToolCall(funcName, argsMap, userPrompt)
  }

  /**
   * Translates a direct natural language prompt to a VoiceIntent via fuzzy intent classification
   * and fabric entity resolution (used for offline fast testing / speech recognizer integration).
   */
  fun matchPromptToIntent(prompt: String): VoiceIntent {
    val clean = prompt.trim().lowercase()

    // 1. Lighting & Level Control
    if (clean.contains("turn on") || clean.contains("switch on") || clean == "lights on" || clean.endsWith(" on")) {
      val target = prompt.replace("(?i)turn on|switch on|lights on|on".toRegex(), "").trim()
      val targetQuery = if (target.isEmpty()) "Light" else target
      return compileToolCall("matter_onoff_on", mapOf("targetDeviceOrRoom" to targetQuery), prompt)
    }

    if (clean.contains("turn off") || clean.contains("switch off") || clean == "lights off" || clean.endsWith(" off")) {
      val target = prompt.replace("(?i)turn off|switch off|lights off|off".toRegex(), "").trim()
      val targetQuery = if (target.isEmpty()) "Light" else target
      return compileToolCall("matter_onoff_off", mapOf("targetDeviceOrRoom" to targetQuery), prompt)
    }

    if (clean.contains("dim") || clean.contains("brightness") || clean.contains("%") || clean.contains("percent")) {
      val percentMatch = "(\\d+)\\s*%".toRegex().find(prompt) ?: "(\\d+)\\s*percent".toRegex().find(prompt)
      val percent = percentMatch?.groupValues?.get(1)?.toDoubleOrNull() ?: 50.0
      val target = prompt.replace("(?i)set|brightness|to|dim|level|%|percent|\\d+".toRegex(), "").trim()
      val targetQuery = if (target.isEmpty()) "Light" else target
      return compileToolCall("matter_levelcontrol_movetolevel", mapOf("targetDeviceOrRoom" to targetQuery, "brightnessPercentage" to percent), prompt)
    }

    // 2. Color Temperature (Kelvin / Warm / Cold)
    if (clean.contains("kelvin") || clean.contains("k ") || clean.endsWith("k") || clean.contains("warm") || clean.contains("cold") || clean.contains("white")) {
      val kelvinMatch = "(\\d{4})\\s*k".toRegex().find(clean) ?: "(\\d{4})\\s*kelvin".toRegex().find(clean)
      val kelvin = kelvinMatch?.groupValues?.get(1)?.toIntOrNull() ?: if (clean.contains("warm")) 2700 else 6500
      val target = prompt.replace("(?i)set|to|warm|cold|white|kelvin|k|\\d+".toRegex(), "").trim()
      val targetQuery = if (target.isEmpty()) "Light" else target
      return compileToolCall("matter_colorcontrol_movetocolortemperature", mapOf("targetDeviceOrRoom" to targetQuery, "kelvin" to kelvin), prompt)
    }

    // 3. Closures (Door Lock / Window Covering)
    if (clean.contains("unlock")) {
      val target = prompt.replace("(?i)unlock|the|door".toRegex(), "").trim()
      val targetQuery = if (target.isEmpty()) "Door" else target
      return compileToolCall("matter_doorlock_unlockdoor", mapOf("targetDeviceOrRoom" to targetQuery), prompt)
    }

    if (clean.contains("lock")) {
      val target = prompt.replace("(?i)lock|the|door".toRegex(), "").trim()
      val targetQuery = if (target.isEmpty()) "Door" else target
      return compileToolCall("matter_doorlock_lockdoor", mapOf("targetDeviceOrRoom" to targetQuery), prompt)
    }

    if (clean.contains("blind") || clean.contains("shade") || clean.contains("curtain") || clean.contains("cover")) {
      val isClose = clean.contains("close") || clean.contains("down")
      val percent = if (isClose) 0.0 else if (clean.contains("half")) 50.0 else 100.0
      val target = prompt.replace("(?i)open|close|the|blinds|shades|curtains|half|halfway".toRegex(), "").trim()
      val targetQuery = if (target.isEmpty()) "Blinds" else target
      return compileToolCall("matter_windowcovering_gotoliftpercentage", mapOf("targetDeviceOrRoom" to targetQuery, "percentage" to percent), prompt)
    }

    // 4. HVAC / Thermostat
    if (clean.contains("temperature") || clean.contains("ac") || clean.contains("thermostat") || clean.contains("degree") || clean.contains("warmer") || clean.contains("cooler")) {
      if (clean.contains("warmer") || clean.contains("cooler") || clean.contains("raise") || clean.contains("lower")) {
        val amount = if (clean.contains("cooler") || clean.contains("lower")) -20 else 20
        return compileToolCall("matter_thermostat_setpointraiselower", mapOf("targetDeviceOrRoom" to "Thermostat", "amount" to amount, "mode" to 0), prompt)
      }
      val tempMatch = "(\\d+)\\s*(c|celsius|degrees|f)?".toRegex().find(clean)
      val temp = tempMatch?.groupValues?.get(1)?.toDoubleOrNull() ?: 22.0
      return compileToolCall("matter_thermostat_setoccupiedheating", mapOf("targetDeviceOrRoom" to "Thermostat", "celsius" to temp), prompt)
    }

    // 5. Media Playback
    if (clean.contains("pause") || clean.contains("stop")) {
      return compileToolCall("matter_mediaplayback_pause", mapOf("targetDeviceOrRoom" to "TV"), prompt)
    }
    if (clean.contains("play") || clean.contains("resume")) {
      return compileToolCall("matter_mediaplayback_play", mapOf("targetDeviceOrRoom" to "TV"), prompt)
    }

    // 6. Robotics & Appliances
    if (clean.contains("vacuum") || clean.contains("clean") || clean.contains("sweep")) {
      return compileToolCall("matter_rvcrunmode_changetomode", mapOf("targetDeviceOrRoom" to "RoboVac", "newMode" to 1), prompt)
    }

    // 7. Energy / EVSE
    if (clean.contains("charge") || clean.contains("ev") || clean.contains("car")) {
      val ampMatch = "(\\d+)\\s*(a|amp|amperes)".toRegex().find(clean)
      val currentMa = (ampMatch?.groupValues?.get(1)?.toIntOrNull() ?: 32) * 1000
      return compileToolCall("matter_energyevse_setmaxchargerate", mapOf("targetDeviceOrRoom" to "EV Charger", "maxChargeCurrent" to currentMa), prompt)
    }

    // 8. Queries / Status
    if (clean.contains("status") || clean.contains("check") || clean.contains("what is") || clean.contains("is ")) {
      return compileToolCall("matter_temperaturemeasurement_get_status", mapOf("targetDeviceOrRoom" to "Sensor", "attributeName" to "MeasuredValue"), prompt)
    }

    // Default fallback to on/off toggle on first node
    return compileToolCall("matter_onoff_toggle", mapOf("targetDeviceOrRoom" to "Light"), prompt)
  }
}

