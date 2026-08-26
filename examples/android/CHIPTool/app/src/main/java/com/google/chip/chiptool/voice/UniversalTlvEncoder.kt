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

import matter.tlv.AnonymousTag
import matter.tlv.ContextSpecificTag
import matter.tlv.Tag
import matter.tlv.TlvReader
import matter.tlv.TlvWriter
import java.util.Locale

/**
 * Result of decoding a Matter TLV attribute report.
 */
data class DecodedAttributeValue(
  val rawValue: Any?,
  val formattedValue: String,
  val unit: String? = null,
  val attributeMeta: MatterAttributeMeta? = null
) {
  val value: Any?
    get() = rawValue
}

/**
 * Universal Dynamic TLV Parameter Encoder and Decoder.
 * Serializes arbitrary Matter command parameters and attribute writes into standard Matter TLV bytes,
 * and decodes Matter IM TLV response streams into structured typed values and human-readable text.
 */
object UniversalTlvEncoder {

  /**
   * Encodes command arguments for a specific Matter command into a top-level TLV Structure.
   *
   * @param commandMeta Metadata defining the command schema and parameter fields.
   * @param params Key-value map of parameter names/aliases to values.
   * @return Matter TLV encoded byte array.
   */
  fun encodeCommandPayload(
    commandMeta: MatterCommandMeta,
    params: Map<String, Any?> = emptyMap()
  ): ByteArray {
    val writer = TlvWriter()
    writer.startStructure(AnonymousTag)

    for (field in commandMeta.requestFields) {
      val rawValue = findParamValue(params, field.name, field.tagId) ?: field.defaultValue
      if (rawValue == null) {
        if (!field.isOptional && !field.isNullable) {
          throw IllegalArgumentException(
            "Missing mandatory parameter '${field.name}' (tag ${field.tagId}) for command ${commandMeta.name}"
          )
        }
        if (field.isNullable) {
          writer.putNull(ContextSpecificTag(field.tagId))
        }
        continue
      }

      val tag = ContextSpecificTag(field.tagId)
      encodeFieldValue(writer, tag, field.type, rawValue, field)
    }


    writer.endStructure()
    return writer.validateTlv().getEncoded()
  }

  /**
   * Encodes an attribute value into TLV bytes for an Interaction Model Write Request.
   */
  fun encodeAttributeWritePayload(
    attributeMeta: MatterAttributeMeta,
    value: Any?
  ): ByteArray {
    val writer = TlvWriter()
    if (value == null) {
      writer.putNull(AnonymousTag)
    } else {
      encodeFieldValue(writer, AnonymousTag, attributeMeta.type, value, null)
    }
    return writer.validateTlv().getEncoded()
  }

  /**
   * Decodes an attribute report TLV byte buffer into structured and human-readable representations.
   */
  fun decodeAttributeReport(
    attributeMeta: MatterAttributeMeta,
    tlvBytes: ByteArray
  ): DecodedAttributeValue {
    val reader = TlvReader(tlvBytes)
    val rawValue = decodeFieldValue(reader, attributeMeta.type)
    val formatted = formatDecodedValue(rawValue, attributeMeta)
    return DecodedAttributeValue(
      rawValue = rawValue,
      formattedValue = formatted,
      unit = attributeMeta.unit,
      attributeMeta = attributeMeta
    )
  }

  // --- Internal Field Encoding ---

  private fun encodeFieldValue(
    writer: TlvWriter,
    tag: Tag,
    type: MatterDataType,
    value: Any,
    fieldMeta: MatterFieldMeta?
  ) {
    when (type) {
      MatterDataType.BOOLEAN -> {
        val b = when (value) {
          is Boolean -> value
          is Number -> value.toInt() != 0
          is String -> value.equals("true", ignoreCase = true) || value == "1" || value.equals("on", ignoreCase = true)
          else -> false
        }
        writer.put(tag, b)
      }

      MatterDataType.INT8, MatterDataType.INT16, MatterDataType.INT32, MatterDataType.INT64 -> {
        val num = coerceToLong(value, fieldMeta)
        when (type) {
          MatterDataType.INT8 -> writer.put(tag, num.toByte())
          MatterDataType.INT16 -> writer.put(tag, num.toShort())
          MatterDataType.INT32 -> writer.put(tag, num.toInt())
          MatterDataType.INT64 -> writer.put(tag, num)
          else -> writer.put(tag, num)
        }
      }

      MatterDataType.UINT8, MatterDataType.UINT16, MatterDataType.UINT32, MatterDataType.UINT64,
      MatterDataType.ENUM8, MatterDataType.ENUM16,
      MatterDataType.BITMAP8, MatterDataType.BITMAP16, MatterDataType.BITMAP32 -> {
        val num = coerceToULong(value, fieldMeta)
        when (type) {
          MatterDataType.UINT8, MatterDataType.ENUM8, MatterDataType.BITMAP8 ->
            writer.put(tag, num.toUByte())
          MatterDataType.UINT16, MatterDataType.ENUM16, MatterDataType.BITMAP16 ->
            writer.put(tag, num.toUShort())
          MatterDataType.UINT32, MatterDataType.BITMAP32 ->
            writer.put(tag, num.toUInt())
          MatterDataType.UINT64 ->
            writer.put(tag, num)
          else -> writer.put(tag, num)
        }
      }

      MatterDataType.FLOAT32 -> {
        val f = when (value) {
          is Number -> value.toFloat()
          is String -> value.toFloatOrNull() ?: 0f
          else -> 0f
        }
        writer.put(tag, f)
      }

      MatterDataType.FLOAT64 -> {
        val d = when (value) {
          is Number -> value.toDouble()
          is String -> value.toDoubleOrNull() ?: 0.0
          else -> 0.0
        }
        writer.put(tag, d)
      }

      MatterDataType.UTF8_STRING -> {
        writer.put(tag, value.toString())
      }

      MatterDataType.OCTET_STRING -> {
        val bytes = when (value) {
          is ByteArray -> value
          is String -> hexStringToByteArray(value)
          else -> ByteArray(0)
        }
        writer.put(tag, bytes)
      }

      MatterDataType.ARRAY -> {
        writer.startArray(tag)
        val list = when (value) {
          is Collection<*> -> value.toList()
          is Array<*> -> value.toList()
          else -> listOf(value)
        }
        for (item in list) {
          if (item != null) {
            encodeFieldValue(writer, AnonymousTag, MatterDataType.UINT32, item, null)
          }
        }
        writer.endArray()
      }

      MatterDataType.STRUCT -> {
        writer.startStructure(tag)
        if (value is Map<*, *>) {
          for ((k, v) in value) {
            val childTagNum = k?.toString()?.toIntOrNull() ?: 0
            if (v != null) {
              encodeFieldValue(writer, ContextSpecificTag(childTagNum), MatterDataType.UINT32, v, null)
            }
          }
        }

        writer.endStructure()
      }

      MatterDataType.NULL -> {
        writer.putNull(tag)
      }
    }
  }

  // --- Internal Field Decoding ---

  private fun decodeFieldValue(reader: TlvReader, type: MatterDataType): Any? {
    return try {
      when (type) {
        MatterDataType.BOOLEAN -> reader.getBool(AnonymousTag)
        MatterDataType.INT8 -> reader.getByte(AnonymousTag).toLong()
        MatterDataType.INT16 -> reader.getShort(AnonymousTag).toLong()
        MatterDataType.INT32 -> reader.getInt(AnonymousTag).toLong()
        MatterDataType.INT64 -> reader.getLong(AnonymousTag)
        MatterDataType.UINT8, MatterDataType.ENUM8, MatterDataType.BITMAP8 -> reader.getUByte(AnonymousTag).toLong()
        MatterDataType.UINT16, MatterDataType.ENUM16, MatterDataType.BITMAP16 -> reader.getUShort(AnonymousTag).toLong()
        MatterDataType.UINT32, MatterDataType.BITMAP32 -> reader.getUInt(AnonymousTag).toLong()
        MatterDataType.UINT64 -> reader.getULong(AnonymousTag).toString()
        MatterDataType.FLOAT32 -> reader.getFloat(AnonymousTag)
        MatterDataType.FLOAT64 -> reader.getDouble(AnonymousTag)
        MatterDataType.UTF8_STRING -> reader.getString(AnonymousTag)
        MatterDataType.OCTET_STRING -> reader.getByteString(AnonymousTag)
        MatterDataType.NULL -> null
        else -> reader.getLong(AnonymousTag)
      }
    } catch (e: Exception) {
      null
    }
  }

  private fun formatDecodedValue(rawValue: Any?, attributeMeta: MatterAttributeMeta): String {
    if (rawValue == null) return "Unknown"

    // Enum mapping check
    if (attributeMeta.enumValues.isNotEmpty()) {
      val longVal = (rawValue as? Number)?.toLong()
      val matchedName = attributeMeta.enumValues.entries.firstOrNull { it.value == longVal }?.key
      if (matchedName != null) return matchedName
    }

    // Special cluster attribute units
    return when (attributeMeta.name) {
      "LocalTemperature", "OccupiedHeatingSetpoint", "OccupiedCoolingSetpoint" -> {
        val centidegrees = (rawValue as? Number)?.toShort() ?: 0
        String.format(Locale.US, "%.1f°C", centidegrees / 100.0)
      }
      "MeasuredValue" -> {
        if (attributeMeta.unit == "°C") {
          val centidegrees = (rawValue as? Number)?.toShort() ?: 0
          String.format(Locale.US, "%.1f°C", centidegrees / 100.0)
        } else if (attributeMeta.unit == "%") {
          val centiPercent = (rawValue as? Number)?.toLong() ?: 0
          String.format(Locale.US, "%.1f%%", centiPercent / 100.0)
        } else {
          rawValue.toString()
        }
      }
      "ColorTemperatureMireds" -> {
        val mireds = (rawValue as? Number)?.toInt() ?: 0
        if (mireds > 0) {
          val kelvin = MatterClusterMetaRegistry.miredsToKelvin(mireds)
          "$kelvin K ($mireds Mireds)"
        } else "$mireds Mireds"
      }
      "OnOff" -> {
        if (rawValue == true) "ON" else "OFF"
      }
      "CurrentLevel", "PercentSetting", "PercentCurrent", "Volume" -> {
        "$rawValue%"
      }
      "LockState" -> {
        when ((rawValue as? Number)?.toInt()) {
          1 -> "Locked"
          2 -> "Unlocked"
          else -> "Not Fully Locked"
        }
      }
      "StateValue" -> {
        if (rawValue == true) "Open / Active" else "Closed / Normal"
      }
      else -> {
        if (attributeMeta.unit != null) "$rawValue ${attributeMeta.unit}" else rawValue.toString()
      }
    }
  }

  // --- Helper Utilities ---

  private fun findParamValue(params: Map<String, Any?>, name: String, tagId: Int): Any? {
    if (params.containsKey(name)) return params[name]
    val lowerName = name.lowercase()
    for ((k, v) in params) {
      if (k.lowercase() == lowerName) return v
    }
    val tagKey = "tag$tagId"
    if (params.containsKey(tagKey)) return params[tagKey]
    if (params.containsKey(tagId.toString())) return params[tagId.toString()]
    return null
  }

  private fun coerceToLong(value: Any, fieldMeta: MatterFieldMeta?): Long {
    return when (value) {
      is Number -> value.toLong()
      is Boolean -> if (value) 1L else 0L
      is String -> {
        // Check enum mapping
        fieldMeta?.enumValues?.get(value)?.let { return it }
        value.toDoubleOrNull()?.toLong() ?: 0L
      }
      else -> 0L
    }
  }

  private fun coerceToULong(value: Any, fieldMeta: MatterFieldMeta?): ULong {
    return when (value) {
      is Number -> value.toLong().toULong()
      is Boolean -> if (value) 1uL else 0uL
      is String -> {
        fieldMeta?.enumValues?.get(value)?.let { return it.toULong() }
        value.toDoubleOrNull()?.toLong()?.toULong() ?: 0uL
      }
      else -> 0uL
    }
  }

  private fun hexStringToByteArray(hex: String): ByteArray {
    val clean = hex.replace(" ", "").replace("0x", "")
    val len = clean.length
    val data = ByteArray(len / 2)
    var i = 0
    while (i < len) {
      data[i / 2] = ((Character.digit(clean[i], 16) shl 4) + Character.digit(clean[i + 1], 16)).toByte()
      i += 2
    }
    return data
  }
}
