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
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.chip.Chip
import com.google.android.material.slider.Slider
import com.google.android.material.tabs.TabLayout
import com.google.chip.chiptool.R
import com.google.chip.chiptool.databinding.DialogEndpointInspectorBinding
import com.google.chip.chiptool.databinding.FragmentDeviceManagementBinding
import com.google.chip.chiptool.databinding.ItemClusterAttributeEditorBinding
import com.google.chip.chiptool.databinding.ItemDeviceCardBinding
import com.google.chip.chiptool.databinding.ItemRoomSectionBinding
import com.google.chip.chiptool.setuppayloadscanner.BarcodeFragment
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

  private lateinit var roomHierarchyAdapter: RoomHierarchyAdapter

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    registry = CommissionedNodeRegistry(context)
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

    setupToolbarMenu()
    setupSearchAndFilters()
    setupVoiceActions()
    setupEmptyStateActions()
    setupRecyclerView()

    // Start fabric subscriptions safely
    context?.let { ctx ->
      try {
        viewModel.subscribeToAllFabricNodes(ctx)
      } catch (e: Exception) {
        // Safe fallback
      }
    }
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
        if (!isAdded || _binding == null) return@collectLatest
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

  private fun setupToolbarMenu() {
    binding.topAppBar.inflateMenu(R.menu.menu_device_management)
    binding.topAppBar.setOnMenuItemClickListener { menuItem ->
      when (menuItem.itemId) {
        R.id.action_load_demo -> {
          registry.loadDefaultDemoFabric()
          context?.let { ctx -> registry.saveFabricToPreferences(ctx) }
          viewModel.refreshFabric()
          context?.let { ctx -> viewModel.subscribeToAllFabricNodes(ctx) }
          Toast.makeText(requireContext(), "Loaded demo Matter smart home fabric", Toast.LENGTH_SHORT).show()
          true
        }
        R.id.action_clear_all -> {
          registry.clearAllNodes(requireContext())
          viewModel.refreshFabric()
          Toast.makeText(requireContext(), "Cleared all devices", Toast.LENGTH_SHORT).show()
          true
        }
        else -> false
      }
    }
  }

  private fun setupVoiceActions() {
    binding.voiceSearchBtn.setOnClickListener { promptVoiceCommandDialog() }
    binding.voiceAiFab.setOnClickListener { promptVoiceCommandDialog() }
  }

  private fun setupEmptyStateActions() {
    binding.commissionDeviceBtn.setOnClickListener {
      parentFragmentManager
        .beginTransaction()
        .replace(R.id.nav_host_fragment, BarcodeFragment.newInstance(), BarcodeFragment::class.java.simpleName)
        .addToBackStack(null)
        .commit()
    }

    binding.loadDemoDevicesBtn.setOnClickListener {
      registry.loadDefaultDemoFabric()
      context?.let { ctx -> registry.saveFabricToPreferences(ctx) }
      viewModel.refreshFabric()
      context?.let { ctx -> viewModel.subscribeToAllFabricNodes(ctx) }
      Toast.makeText(requireContext(), "Loaded demo Matter smart home fabric", Toast.LENGTH_SHORT).show()
    }
  }

  private fun setupRecyclerView() {
    roomHierarchyAdapter = RoomHierarchyAdapter(
      onBulkOffClicked = { roomName ->
        context?.let { ctx ->
          viewModel.executeBulkRoomAction(ctx, roomName, BulkRoomAction.TURN_ALL_OFF)
        }
      },
      onTogglePower = { card ->
        context?.let { ctx ->
          val node = registry.getNode(card.nodeId)
          if (node != null) {
            val cardVm = DeviceCardViewModel(node, registry, synchronizer, dispatcher)
            cardVm.togglePower(ctx)
          }
        }
      },
      onToggleLock = { card ->
        context?.let { ctx ->
          val node = registry.getNode(card.nodeId)
          if (node != null) {
            val cardVm = DeviceCardViewModel(node, registry, synchronizer, dispatcher)
            cardVm.toggleLock(ctx)
          }
        }
      },
      onBrightnessChanged = { card, percent ->
        context?.let { ctx ->
          val node = registry.getNode(card.nodeId)
          if (node != null) {
            val cardVm = DeviceCardViewModel(node, registry, synchronizer, dispatcher)
            cardVm.setBrightness(ctx, percent)
          }
        }
      },
      onEditAliasesClicked = { card ->
        val node = registry.getNode(card.nodeId)
        if (node != null) {
          showEditRoomAndAliasesDialog(node)
        }
      },
      onInspectEndpointsClicked = { card ->
        showEndpointInspector(card.nodeId)
      }
    )

    binding.roomHierarchyRecyclerView.layoutManager = LinearLayoutManager(requireContext())
    binding.roomHierarchyRecyclerView.adapter = roomHierarchyAdapter

    viewLifecycleOwner.lifecycleScope.launch {
      viewModel.filteredRoomHierarchy.collectLatest { roomItems ->
        if (!isAdded || _binding == null) return@collectLatest
        if (roomItems.isEmpty()) {
          binding.emptyStateLayout.visibility = View.VISIBLE
          binding.roomHierarchyRecyclerView.visibility = View.GONE
        } else {
          binding.emptyStateLayout.visibility = View.GONE
          binding.roomHierarchyRecyclerView.visibility = View.VISIBLE
          roomHierarchyAdapter.submitList(roomItems)
        }
      }
    }
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

  fun showEndpointInspector(nodeId: Long) {
    val inspectorVm = EndpointInspectorViewModel(nodeId, registry, synchronizer, dispatcher)
    val dialogBinding = DialogEndpointInspectorBinding.inflate(layoutInflater)

    val dialog = AlertDialog.Builder(requireContext())
      .setView(dialogBinding.root)
      .create()

    dialogBinding.inspectorToolbar.setNavigationOnClickListener { dialog.dismiss() }

    val clusterAdapter = ClusterAttributeAdapter(
      onSwitchToggled = { epId, clusterId, attrId, isChecked ->
        inspectorVm.inPlaceWriteAttribute(requireContext(), epId, clusterId, attrId, isChecked)
      },
      onSliderChanged = { epId, clusterId, attrId, value ->
        inspectorVm.inPlaceWriteAttribute(requireContext(), epId, clusterId, attrId, value)
      },
      onReadClicked = { epId, clusterId, attrId ->
        inspectorVm.readAttribute(requireContext(), epId, clusterId, attrId)
      },
      onEditClicked = { epId, clusterId, attrId, attrName, currentVal ->
        val input = EditText(requireContext()).apply {
          hint = "New value"
          setText(currentVal?.toString() ?: "")
        }
        AlertDialog.Builder(requireContext())
          .setTitle("Write $attrName")
          .setView(input)
          .setPositiveButton("Write") { _, _ ->
            val text = input.text.toString().trim()
            if (text.isNotEmpty()) {
              inspectorVm.inPlaceWriteAttribute(requireContext(), epId, clusterId, attrId, text)
            }
          }
          .setNegativeButton("Cancel", null)
          .show()
      }
    )

    dialogBinding.clustersRecyclerView.layoutManager = LinearLayoutManager(requireContext())
    dialogBinding.clustersRecyclerView.adapter = clusterAdapter

    viewLifecycleOwner.lifecycleScope.launch {
      inspectorVm.node.collectLatest { node ->
        if (node != null) {
          dialogBinding.inspectorDeviceLabel.text = node.nodeLabel
          dialogBinding.inspectorDeviceMeta.text =
            "Node 0x${node.nodeId.toString(16)} • ${node.vendorName} • Room: ${node.roomName}"
        }
      }
    }

    viewLifecycleOwner.lifecycleScope.launch {
      inspectorVm.operationStatus.collectLatest { status ->
        if (!status.isNullOrEmpty()) {
          dialogBinding.inspectorStatusBanner.text = status
        }
      }
    }

    viewLifecycleOwner.lifecycleScope.launch {
      inspectorVm.endpoints.collectLatest { endpoints ->
        dialogBinding.endpointTabLayout.removeAllTabs()
        for ((idx, ep) in endpoints.withIndex()) {
          val tab = dialogBinding.endpointTabLayout.newTab().setText("EP ${ep.endpointId}: ${ep.deviceTypeName}")
          dialogBinding.endpointTabLayout.addTab(tab)
        }
      }
    }

    dialogBinding.endpointTabLayout.addOnTabSelectedListener(object : TabLayout.OnTabSelectedListener {
      override fun onTabSelected(tab: TabLayout.Tab?) {
        tab?.let { inspectorVm.selectEndpoint(it.position) }
      }
      override fun onTabUnselected(tab: TabLayout.Tab?) {}
      override fun onTabReselected(tab: TabLayout.Tab?) {}
    })

    viewLifecycleOwner.lifecycleScope.launch {
      inspectorVm.currentEndpoint.collectLatest { ep ->
        if (ep != null) {
          dialogBinding.endpointTitleText.text =
            "Endpoint ${ep.endpointId}: ${ep.deviceTypeName} (0x${ep.deviceTypeId.toString(16)})"
          dialogBinding.clusterCountText.text = "${ep.serverClusters.size} Server Clusters"
          clusterAdapter.submitClusters(ep.endpointId, ep.serverClusters)
        }
      }
    }

    dialog.show()
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
        context?.let { ctx -> registry.saveFabricToPreferences(ctx) }

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

class RoomHierarchyAdapter(
  private val onBulkOffClicked: (String) -> Unit,
  private val onTogglePower: (DeviceCardState) -> Unit,
  private val onToggleLock: (DeviceCardState) -> Unit,
  private val onBrightnessChanged: (DeviceCardState, Int) -> Unit,
  private val onEditAliasesClicked: (DeviceCardState) -> Unit,
  private val onInspectEndpointsClicked: (DeviceCardState) -> Unit
) : RecyclerView.Adapter<RoomHierarchyAdapter.RoomViewHolder>() {

  private val items = mutableListOf<RoomHierarchyItem>()

  fun submitList(newItems: List<RoomHierarchyItem>) {
    items.clear()
    items.addAll(newItems)
    notifyDataSetChanged()
  }

  override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RoomViewHolder {
    val binding = ItemRoomSectionBinding.inflate(LayoutInflater.from(parent.context), parent, false)
    return RoomViewHolder(binding)
  }

  override fun onBindViewHolder(holder: RoomViewHolder, position: Int) {
    holder.bind(items[position])
  }

  override fun getItemCount(): Int = items.size

  inner class RoomViewHolder(private val binding: ItemRoomSectionBinding) :
    RecyclerView.ViewHolder(binding.root) {

    fun bind(item: RoomHierarchyItem) {
      binding.roomNameText.text = item.roomName
      binding.roomCountBadge.text = "${item.totalCount} devices • ${item.activeCount} online"
      binding.bulkOffButton.setOnClickListener { onBulkOffClicked(item.roomName) }

      val deviceAdapter = DeviceCardAdapter(
        onTogglePower = onTogglePower,
        onToggleLock = onToggleLock,
        onBrightnessChanged = onBrightnessChanged,
        onEditAliasesClicked = onEditAliasesClicked,
        onInspectEndpointsClicked = onInspectEndpointsClicked
      )
      binding.roomDevicesRecyclerView.layoutManager = LinearLayoutManager(binding.root.context)
      binding.roomDevicesRecyclerView.adapter = deviceAdapter
      deviceAdapter.submitList(item.deviceCards)
    }
  }
}

class DeviceCardAdapter(
  private val onTogglePower: (DeviceCardState) -> Unit,
  private val onToggleLock: (DeviceCardState) -> Unit,
  private val onBrightnessChanged: (DeviceCardState, Int) -> Unit,
  private val onEditAliasesClicked: (DeviceCardState) -> Unit,
  private val onInspectEndpointsClicked: (DeviceCardState) -> Unit
) : RecyclerView.Adapter<DeviceCardAdapter.DeviceViewHolder>() {

  private val items = mutableListOf<DeviceCardState>()

  fun submitList(newItems: List<DeviceCardState>) {
    items.clear()
    items.addAll(newItems)
    notifyDataSetChanged()
  }

  override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): DeviceViewHolder {
    val binding = ItemDeviceCardBinding.inflate(LayoutInflater.from(parent.context), parent, false)
    return DeviceViewHolder(binding)
  }

  override fun onBindViewHolder(holder: DeviceViewHolder, position: Int) {
    holder.bind(items[position])
  }

  override fun getItemCount(): Int = items.size

  inner class DeviceViewHolder(private val binding: ItemDeviceCardBinding) :
    RecyclerView.ViewHolder(binding.root) {

    fun bind(card: DeviceCardState) {
      binding.deviceLabelText.text = card.nodeLabel
      binding.deviceSubtitleText.text =
        "${card.vendorName} • ${card.productName} • Node 0x${card.nodeId.toString(16)}"

      // Badges
      binding.badgeOnline.text = if (card.isOnline) "Online" else "Offline"
      if (card.batteryPercent != null) {
        binding.badgeBattery.visibility = View.VISIBLE
        binding.badgeBattery.text = "${card.batteryPercent}%"
      } else {
        binding.badgeBattery.visibility = View.GONE
      }

      val stateBadge = card.badges.firstOrNull {
        it.type == DeviceBadgeType.STATE_ON || it.type == DeviceBadgeType.STATE_OFF ||
        it.type == DeviceBadgeType.STATE_LOCKED || it.type == DeviceBadgeType.STATE_UNLOCKED ||
        it.type == DeviceBadgeType.TEMPERATURE
      }
      if (stateBadge != null) {
        binding.badgeState.visibility = View.VISIBLE
        binding.badgeState.text = stateBadge.label
      } else {
        binding.badgeState.visibility = View.GONE
      }

      // Aliases
      if (card.aliases.isNotEmpty()) {
        binding.aliasesText.visibility = View.VISIBLE
        binding.aliasesText.text = "Voice Aliases: " + card.aliases.joinToString(", ")
      } else {
        binding.aliasesText.visibility = View.GONE
      }

      // Quick Controls
      when (val qc = card.quickControl) {
        is QuickControlAction.ToggleSwitch -> {
          binding.quickToggleSwitch.visibility = View.VISIBLE
          binding.quickLockButton.visibility = View.GONE
          binding.sliderContainer.visibility = View.GONE
          binding.quickToggleSwitch.setOnCheckedChangeListener(null)
          binding.quickToggleSwitch.isChecked = qc.isOn
          binding.quickToggleSwitch.setOnCheckedChangeListener { _, _ ->
            onTogglePower(card)
          }
        }
        is QuickControlAction.LockToggle -> {
          binding.quickToggleSwitch.visibility = View.GONE
          binding.quickLockButton.visibility = View.VISIBLE
          binding.sliderContainer.visibility = View.GONE
          binding.quickLockButton.text = if (qc.isLocked) "Unlock" else "Lock"
          binding.quickLockButton.setOnClickListener {
            onToggleLock(card)
          }
        }
        is QuickControlAction.BrightnessSlider -> {
          binding.quickToggleSwitch.visibility = View.VISIBLE
          binding.quickLockButton.visibility = View.GONE
          binding.sliderContainer.visibility = View.VISIBLE
          binding.quickLevelSlider.value = qc.levelPercent.toFloat().coerceIn(0f, 100f)
          binding.sliderValueText.text = "${qc.levelPercent}%"
          binding.quickLevelSlider.clearOnChangeListeners()
          binding.quickLevelSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
              binding.sliderValueText.text = "${value.toInt()}%"
              onBrightnessChanged(card, value.toInt())
            }
          }
        }
        else -> {
          binding.quickToggleSwitch.visibility = View.GONE
          binding.quickLockButton.visibility = View.GONE
          binding.sliderContainer.visibility = View.GONE
        }
      }

      binding.editAliasesButton.setOnClickListener { onEditAliasesClicked(card) }
      binding.inspectEndpointsButton.setOnClickListener { onInspectEndpointsClicked(card) }
    }
  }
}

class ClusterAttributeAdapter(
  private val onSwitchToggled: (endpointId: Int, clusterId: Long, attributeId: Long, isChecked: Boolean) -> Unit,
  private val onSliderChanged: (endpointId: Int, clusterId: Long, attributeId: Long, value: Any) -> Unit,
  private val onReadClicked: (endpointId: Int, clusterId: Long, attributeId: Long) -> Unit,
  private val onEditClicked: (endpointId: Int, clusterId: Long, attributeId: Long, attrName: String, currentValue: Any?) -> Unit
) : RecyclerView.Adapter<ClusterAttributeAdapter.ClusterViewHolder>() {

  private var endpointId: Int = 1
  private val clusters = mutableListOf<ClusterDescriptor>()

  fun submitClusters(epId: Int, newClusters: List<ClusterDescriptor>) {
    endpointId = epId
    clusters.clear()
    clusters.addAll(newClusters)
    notifyDataSetChanged()
  }

  override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ClusterViewHolder {
    val binding = ItemClusterAttributeEditorBinding.inflate(LayoutInflater.from(parent.context), parent, false)
    return ClusterViewHolder(binding)
  }

  override fun onBindViewHolder(holder: ClusterViewHolder, position: Int) {
    holder.bind(clusters[position])
  }

  override fun getItemCount(): Int = clusters.size

  inner class ClusterViewHolder(private val binding: ItemClusterAttributeEditorBinding) :
    RecyclerView.ViewHolder(binding.root) {

    fun bind(cluster: ClusterDescriptor) {
      binding.clusterNameText.text = "${cluster.clusterName} (0x${cluster.clusterId.toString(16)})"
      binding.clusterCategoryBadge.text = cluster.category
      binding.clusterDescriptionText.text = cluster.description

      binding.attributesContainer.removeAllViews()

      for (attr in cluster.attributes) {
        val row = createAttributeRow(cluster.clusterId, attr)
        binding.attributesContainer.addView(row)
      }
    }

    private fun createAttributeRow(clusterId: Long, attr: WritableAttributeState): View {
      val ctx = binding.root.context
      return when (attr.controlType) {
        AttributeControlType.SWITCH -> {
          val layout = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            setPadding(0, 8, 0, 8)
          }
          val textLayout = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
          }
          val nameTv = TextView(ctx).apply {
            text = "${attr.attributeName} (0x${attr.attributeId.toString(16)})"
            textSize = 13f
            typeface = android.graphics.Typeface.DEFAULT_BOLD
          }
          val metaTv = TextView(ctx).apply {
            text = "${attr.dataType} • ${if (attr.isWritable) "Writable" else "Read-Only"} • Value: ${attr.formattedValue}"
            textSize = 10f
          }
          textLayout.addView(nameTv)
          textLayout.addView(metaTv)

          val switch = com.google.android.material.materialswitch.MaterialSwitch(ctx).apply {
            showText = false
            textOn = ""
            textOff = ""
            isChecked = (attr.currentValue as? Boolean) == true
            isEnabled = attr.isWritable
            setOnCheckedChangeListener { _, isChecked ->
              onSwitchToggled(endpointId, clusterId, attr.attributeId, isChecked)
            }
          }
          layout.addView(textLayout)
          layout.addView(switch)
          layout
        }

        AttributeControlType.SLIDER_COLOR_TEMP,
        AttributeControlType.SLIDER_NUMERIC -> {
          val layout = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 8, 0, 8)
          }
          val header = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
          }
          val nameTv = TextView(ctx).apply {
            text = "${attr.attributeName} (0x${attr.attributeId.toString(16)})"
            textSize = 13f
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
          }
          val valTv = TextView(ctx).apply {
            text = attr.formattedValue
            textSize = 12f
            typeface = android.graphics.Typeface.DEFAULT_BOLD
          }
          header.addView(nameTv)
          header.addView(valTv)
          layout.addView(header)

          val slider = Slider(ctx).apply {
            valueFrom = (attr.minValue ?: 0.0).toFloat()
            valueTo = (attr.maxValue ?: 254.0).toFloat()
            val raw = (attr.currentValue as? Number)?.toFloat() ?: valueFrom
            value = raw.coerceIn(valueFrom, valueTo)
            stepSize = (attr.step ?: 1.0).toFloat()
            isEnabled = attr.isWritable
            addOnChangeListener { _, v, fromUser ->
              if (fromUser) {
                valTv.text = "$v"
                onSliderChanged(endpointId, clusterId, attr.attributeId, v)
              }
            }
          }
          layout.addView(slider)
          layout
        }

        else -> {
          val layout = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            setPadding(0, 8, 0, 8)
          }
          val textLayout = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
          }
          val nameTv = TextView(ctx).apply {
            text = "${attr.attributeName} (0x${attr.attributeId.toString(16)})"
            textSize = 13f
            typeface = android.graphics.Typeface.DEFAULT_BOLD
          }
          val metaTv = TextView(ctx).apply {
            text = "${attr.dataType} • Value: ${attr.formattedValue}"
            textSize = 10f
          }
          textLayout.addView(nameTv)
          textLayout.addView(metaTv)
          layout.addView(textLayout)

          val readBtn = com.google.android.material.button.MaterialButton(
            ctx,
            null,
            com.google.android.material.R.attr.borderlessButtonStyle
          ).apply {
            text = "Read"
            textSize = 11f
            setOnClickListener { onReadClicked(endpointId, clusterId, attr.attributeId) }
          }
          layout.addView(readBtn)

          if (attr.isWritable) {
            val editBtn = com.google.android.material.button.MaterialButton(
              ctx,
              null,
              com.google.android.material.R.attr.materialButtonOutlinedStyle
            ).apply {
              text = "Edit"
              textSize = 11f
              setOnClickListener {
                onEditClicked(endpointId, clusterId, attr.attributeId, attr.attributeName, attr.currentValue)
              }
            }
            layout.addView(editBtn)
          }
          layout
        }
      }
    }
  }
}

