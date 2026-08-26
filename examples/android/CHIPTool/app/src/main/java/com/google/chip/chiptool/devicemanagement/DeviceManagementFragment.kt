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

import androidx.appcompat.app.AlertDialog
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.chip.Chip
import com.google.chip.chiptool.databinding.FragmentDeviceManagementBinding
import com.google.chip.chiptool.voice.CommissionedNode
import com.google.chip.chiptool.voice.CommissionedNodeRegistry
import com.google.chip.chiptool.voice.UniversalMatterDispatcher
import com.google.chip.chiptool.voice.VoiceControlEngine
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch

/**
 * Modern Room-Centric Matter Device & Dynamic Endpoint Management Hub Fragment.
 */
class DeviceManagementFragment : Fragment() {

  private var _binding: FragmentDeviceManagementBinding? = null
  private val binding get() = _binding!!

  private lateinit var registry: CommissionedNodeRegistry
  private lateinit var synchronizer: MatterStateSynchronizer
  private lateinit var dispatcher: UniversalMatterDispatcher
  private lateinit var viewModel: DeviceManagementViewModel
  private lateinit var voiceEngine: VoiceControlEngine

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    registry = CommissionedNodeRegistry()
    registry.loadDefaultSmartHomeFabric()
    synchronizer = MatterStateSynchronizer.getInstance(registry)
    dispatcher = UniversalMatterDispatcher(registry)
    viewModel = DeviceManagementViewModel(registry, synchronizer, dispatcher)
    voiceEngine = VoiceControlEngine.getInstance(registry)
  }

  override fun onCreateView(
    inflater: LayoutInflater,
    container: ViewGroup?,
    savedInstanceState: Bundle?
  ): View {
    _binding = FragmentDeviceManagementBinding.inflate(inflater, container, false)
    return binding.root
  }

  override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
    super.onViewCreated(view, savedInstanceState)

    setupSearchAndFilters()
    setupVoiceActions()
    setupRecyclerView()

    // Start fabric subscriptions
    viewModel.subscribeToAllFabricNodes(requireContext())
  }

  private fun setupSearchAndFilters() {
    binding.searchEditText.addTextChangedListener(object : TextWatcher {
      override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
      override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
        viewModel.setSearchQuery(s?.toString() ?: "")
      }
      override fun afterTextChanged(s: Editable?) {}
    })

    // Setup room filter chips dynamically
    viewLifecycleOwner.lifecycleScope.launch {
      viewModel.availableRooms.collectLatest { rooms ->
        binding.roomChipGroup.removeAllViews()

        val allChip = Chip(requireContext()).apply {
          text = "All Rooms"
          isCheckable = true
          isChecked = (viewModel.selectedRoomFilter.value == null)
          setOnClickListener { viewModel.setRoomFilter(null) }
        }
        binding.roomChipGroup.addView(allChip)

        for (room in rooms) {
          val chip = Chip(requireContext()).apply {
            text = room
            isCheckable = true
            isChecked = (viewModel.selectedRoomFilter.value == room)
            setOnClickListener { viewModel.setRoomFilter(room) }
          }
          binding.roomChipGroup.addView(chip)
        }
      }
    }
  }

  private fun setupVoiceActions() {
    binding.voiceSearchBtn.setOnClickListener { promptVoiceCommandDialog() }
    binding.voiceAiFab.setOnClickListener { promptVoiceCommandDialog() }
  }

  private fun promptVoiceCommandDialog() {
    val input = EditText(requireContext()).apply {
      hint = "e.g. 'Turn off all lights in living room' or 'Lock front door'"
    }

    AlertDialog.Builder(requireContext())
      .setTitle("Gemini Nano Voice Control")
      .setMessage("Speak or type a home automation command:")
      .setView(input)
      .setPositiveButton("Execute") { _, _ ->
        val prompt = input.text.toString().trim()
        if (prompt.isNotEmpty()) {
          viewLifecycleOwner.lifecycleScope.launch {
            val result = voiceEngine.processVoiceCommand(requireContext(), prompt)
            Toast.makeText(requireContext(), result.spokenResponse, Toast.LENGTH_LONG).show()
          }
        }
      }
      .setNegativeButton("Cancel", null)
      .show()
  }

  private fun setupRecyclerView() {
    binding.roomHierarchyRecyclerView.layoutManager = LinearLayoutManager(requireContext())
    // Observe room items
    viewLifecycleOwner.lifecycleScope.launch {
      viewModel.filteredRoomHierarchy.collectLatest { roomItems ->
        // Render room sections and device cards
      }
    }
  }

  fun showEndpointInspector(nodeId: Long) {
    val inspectorVm = EndpointInspectorViewModel(nodeId, registry, synchronizer, dispatcher)
    // Display Endpoint Inspector Dialog
  }

  fun showEditRoomAndAliasesDialog(node: CommissionedNode) {
    val layout = LinearLayout(requireContext()).apply {
      orientation = LinearLayout.VERTICAL
      setPadding(40, 20, 40, 20)
    }

    val roomInput = EditText(requireContext()).apply {
      hint = "Room Name"
      setText(node.roomName)
    }
    val labelInput = EditText(requireContext()).apply {
      hint = "Device Label"
      setText(node.nodeLabel)
    }
    val aliasInput = EditText(requireContext()).apply {
      hint = "Voice Aliases (comma separated)"
      setText(node.aliases.joinToString(", "))
    }

    layout.addView(roomInput)
    layout.addView(labelInput)
    layout.addView(aliasInput)

    AlertDialog.Builder(requireContext())
      .setTitle("Edit Room & Voice Metadata")
      .setView(layout)
      .setPositiveButton("Save") { _, _ ->
        val newRoom = roomInput.text.toString().trim()
        val newLabel = labelInput.text.toString().trim()
        val newAliases = aliasInput.text.toString().split(",").map { it.trim() }.filter { it.isNotEmpty() }

        if (newRoom.isNotEmpty()) viewModel.reassignDeviceRoom(node.nodeId, newRoom)
        if (newLabel.isNotEmpty()) viewModel.updateDeviceLabel(node.nodeId, newLabel)
        viewModel.updateDeviceAliases(node.nodeId, newAliases)

        Toast.makeText(requireContext(), "Updated metadata for ${node.nodeLabel}", Toast.LENGTH_SHORT).show()
      }
      .setNegativeButton("Cancel", null)
      .show()
  }

  override fun onDestroyView() {
    super.onDestroyView()
    _binding = null
  }

  companion object {
    @JvmStatic
    fun newInstance() = DeviceManagementFragment()
  }
}
