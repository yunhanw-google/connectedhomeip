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

import android.app.Activity
import android.content.Context
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import com.google.chip.chiptool.databinding.DialogPostCommissioningSetupBinding
import com.google.chip.chiptool.setuppayloadscanner.CHIPDeviceInfo
import com.google.chip.chiptool.voice.CommissionedNode
import com.google.chip.chiptool.voice.CommissionedNodeRegistry
import com.google.chip.chiptool.voice.MatterClusterMetaRegistry
import com.google.chip.chiptool.voice.UniversalMatterDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

/**
 * Helper to display the Material Post-Commissioning Device Setup and In-Place Metadata Editor Dialogs.
 */
object PostCommissioningDialogHelper {

  private const val TAG = "PostCommissioningDialog"

  val STANDARD_ROOMS = listOf(
    "Unassigned",
    "Living Room",
    "Kitchen",
    "Entryway",
    "Bedroom",
    "Bathroom",
    "Garage",
    "Hallway",
    "Office",
    "+ New Room"
  )

  /**
   * Automatically registers the newly commissioned node ID into the registry if not already present.
   */
  fun autoRegisterNode(
    registry: CommissionedNodeRegistry,
    nodeId: Long,
    deviceInfo: CHIPDeviceInfo? = null
  ): CommissionedNode {
    val defaultLabel = deviceInfo?.let { "Matter Device (VID:${it.vendorId} PID:${it.productId})" }
      ?: "Device 0x${nodeId.toString(16)}"
    val productName = deviceInfo?.let { "Matter Product ${it.productId}" } ?: "Matter Device"

    return registry.autoRegisterCommissionedNode(
      nodeId = nodeId,
      defaultLabel = defaultLabel,
      productName = productName,
      roomName = "Unassigned"
    )
  }

  /**
   * Displays the Material Post-Commissioning Setup Dialog immediately after commissioning completes.
   */
  fun showPostCommissioningDialog(
    activity: Activity,
    nodeId: Long,
    registry: CommissionedNodeRegistry,
    deviceInfo: CHIPDeviceInfo? = null,
    onComplete: ((CommissionedNode) -> Unit)? = null
  ) {
    val node = autoRegisterNode(registry, nodeId, deviceInfo)
    val binding = DialogPostCommissioningSetupBinding.inflate(LayoutInflater.from(activity))

    // Pre-populate default device name
    val defaultName = if (node.nodeLabel.isNotBlank()) {
      node.nodeLabel
    } else {
      "Device 0x${nodeId.toString(16)}"
    }
    binding.deviceNameEditText.setText(defaultName)

    // Setup room selection dropdown
    val existingRooms = registry.getAllRooms()
    val allRoomOptions = (STANDARD_ROOMS.dropLast(1) + existingRooms).distinct() + listOf("+ New Room")
    val roomAdapter = ArrayAdapter(activity, android.R.layout.simple_dropdown_item_1line, allRoomOptions)
    binding.roomAutoComplete.setAdapter(roomAdapter)
    binding.roomAutoComplete.setText("Unassigned", false)

    binding.roomAutoComplete.setOnItemClickListener { _, _, position, _ ->
      val selected = roomAdapter.getItem(position)
      if (selected == "+ New Room") {
        binding.customRoomLayout.visibility = View.VISIBLE
        binding.customRoomEditText.requestFocus()
      } else {
        binding.customRoomLayout.visibility = View.GONE
      }
    }

    // Pre-populate aliases
    binding.voiceAliasesEditText.setText(node.aliases.joinToString(", "))

    val dialog = AlertDialog.Builder(activity)
      .setTitle("Device Commissioned Successfully")
      .setMessage("Node 0x${nodeId.toString(16)} ($nodeId) is now on your Matter fabric.")
      .setView(binding.root)
      .setCancelable(false)
      .setNeutralButton("Skip (Save as Unassigned)") { _, _ ->
        val enteredName = binding.deviceNameEditText.text?.toString()?.trim()
        val finalName = if (!enteredName.isNullOrEmpty()) enteredName else defaultName
        val updated = registry.updateNodeMetadata(
          nodeId = nodeId,
          nodeLabel = finalName,
          roomName = "Unassigned",
          aliases = emptyList()
        ) ?: node
        registry.saveFabricToPreferences(activity)
        Toast.makeText(activity, "Saved $finalName to Unassigned", Toast.LENGTH_SHORT).show()
        onComplete?.invoke(updated)
      }
      .setPositiveButton("Save & Finish") { _, _ ->
        val enteredName = binding.deviceNameEditText.text?.toString()?.trim()
        val finalName = if (!enteredName.isNullOrEmpty()) enteredName else defaultName

        val selectedRoom = binding.roomAutoComplete.text?.toString()?.trim() ?: "Unassigned"
        val finalRoom = if (selectedRoom == "+ New Room") {
          val customRoom = binding.customRoomEditText.text?.toString()?.trim()
          if (!customRoom.isNullOrEmpty()) customRoom else "Unassigned"
        } else {
          selectedRoom.ifEmpty { "Unassigned" }
        }

        val rawAliases = binding.voiceAliasesEditText.text?.toString() ?: ""
        val aliasesList = rawAliases.split(",")
          .map { it.trim() }
          .filter { it.isNotEmpty() }

        val updated = registry.updateNodeMetadata(
          nodeId = nodeId,
          nodeLabel = finalName,
          roomName = finalRoom,
          aliases = aliasesList
        ) ?: node
        registry.saveFabricToPreferences(activity)

        // Optionally write NodeLabel to Basic Information cluster (0x0028) Endpoint 0
        if (binding.syncNodeLabelCheckbox.isChecked) {
          syncNodeLabelToDevice(activity, nodeId, finalName, registry)
        }

        Toast.makeText(activity, "Configured $finalName in $finalRoom", Toast.LENGTH_SHORT).show()
        onComplete?.invoke(updated)
      }
      .create()

    dialog.show()
  }

  /**
   * Displays the In-Place Device Metadata Editor Dialog from the Device Hub.
   */
  fun showEditDeviceDialog(
    context: Context,
    node: CommissionedNode,
    registry: CommissionedNodeRegistry,
    onSaved: ((CommissionedNode) -> Unit)? = null
  ) {
    val binding = DialogPostCommissioningSetupBinding.inflate(LayoutInflater.from(context))

    binding.dialogSubtitleText.text =
      "Editing metadata for Node 0x${node.nodeId.toString(16)} (${node.productName})"
    binding.deviceNameEditText.setText(node.nodeLabel)

    val existingRooms = registry.getAllRooms()
    val allRoomOptions = (STANDARD_ROOMS.dropLast(1) + existingRooms).distinct() + listOf("+ New Room")
    val roomAdapter = ArrayAdapter(context, android.R.layout.simple_dropdown_item_1line, allRoomOptions)
    binding.roomAutoComplete.setAdapter(roomAdapter)
    binding.roomAutoComplete.setText(node.roomName.ifEmpty { "Unassigned" }, false)

    binding.roomAutoComplete.setOnItemClickListener { _, _, position, _ ->
      val selected = roomAdapter.getItem(position)
      if (selected == "+ New Room") {
        binding.customRoomLayout.visibility = View.VISIBLE
        binding.customRoomEditText.requestFocus()
      } else {
        binding.customRoomLayout.visibility = View.GONE
      }
    }

    binding.voiceAliasesEditText.setText(node.aliases.joinToString(", "))

    AlertDialog.Builder(context)
      .setTitle("Edit Name & Room")
      .setView(binding.root)
      .setNegativeButton("Cancel", null)
      .setPositiveButton("Save Changes") { _, _ ->
        val enteredName = binding.deviceNameEditText.text?.toString()?.trim()
        val finalName = if (!enteredName.isNullOrEmpty()) enteredName else node.nodeLabel

        val selectedRoom = binding.roomAutoComplete.text?.toString()?.trim() ?: node.roomName
        val finalRoom = if (selectedRoom == "+ New Room") {
          val customRoom = binding.customRoomEditText.text?.toString()?.trim()
          if (!customRoom.isNullOrEmpty()) customRoom else node.roomName
        } else {
          selectedRoom.ifEmpty { "Unassigned" }
        }

        val rawAliases = binding.voiceAliasesEditText.text?.toString() ?: ""
        val aliasesList = rawAliases.split(",")
          .map { it.trim() }
          .filter { it.isNotEmpty() }

        val updated = registry.updateNodeMetadata(
          nodeId = node.nodeId,
          nodeLabel = finalName,
          roomName = finalRoom,
          aliases = aliasesList
        ) ?: node
        registry.saveFabricToPreferences(context)

        if (binding.syncNodeLabelCheckbox.isChecked) {
          syncNodeLabelToDevice(context, node.nodeId, finalName, registry)
        }

        Toast.makeText(context, "Updated $finalName", Toast.LENGTH_SHORT).show()
        onSaved?.invoke(updated)
      }
      .show()
  }

  private fun syncNodeLabelToDevice(
    context: Context,
    nodeId: Long,
    nodeLabel: String,
    registry: CommissionedNodeRegistry
  ) {
    CoroutineScope(Dispatchers.IO).launch {
      try {
        val dispatcher = UniversalMatterDispatcher(registry)
        val result = dispatcher.writeAttribute(
          context = context,
          nodeId = nodeId,
          endpointId = 0,
          clusterId = MatterClusterMetaRegistry.CLUSTER_BASIC_INFORMATION,
          attributeId = 0x0005L, // NodeLabel
          value = nodeLabel,
          targetDescription = "Node $nodeId (Basic Information)"
        )
        Log.d(TAG, "Sync NodeLabel result: ${result.message}")
      } catch (e: Exception) {
        Log.w(TAG, "Optional NodeLabel write to device failed: ${e.message}")
      }
    }
  }
}
