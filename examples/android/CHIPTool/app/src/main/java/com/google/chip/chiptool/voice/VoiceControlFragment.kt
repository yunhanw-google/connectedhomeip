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

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.text.method.ScrollingMovementMethod
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.google.chip.chiptool.R
import com.google.chip.chiptool.databinding.VoiceControlFragmentBinding
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.launch

/**
 * Universal Matter Voice Control Fragment.
 *
 * Provides a push-to-talk interface and interactive console powered by
 * on-device Gemini Nano and the Universal Matter Cluster MetaRegistry.
 */
class VoiceControlFragment : Fragment() {

  private var _binding: VoiceControlFragmentBinding? = null
  private val binding
    get() = _binding!!

  private val engine: VoiceControlEngine by lazy {
    VoiceControlEngine.getInstance(requireContext().applicationContext)
  }

  private val audioPermissionLauncher =
    registerForActivityResult(ActivityResultContracts.RequestPermission()) { isGranted ->
      if (isGranted) {
        appendLog("[Audio] RECORD_AUDIO permission granted.")
      } else {
        appendLog("[Audio Warning] RECORD_AUDIO denied. Text prompt testing remains fully active.")
        Toast.makeText(requireContext(), "Microphone permission required for speech", Toast.LENGTH_SHORT).show()
      }
    }

  override fun onCreateView(
    inflater: LayoutInflater,
    container: ViewGroup?,
    savedInstanceState: Bundle?
  ): View {
    _binding = VoiceControlFragmentBinding.inflate(inflater, container, false)
    return binding.root
  }

  override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
    super.onViewCreated(view, savedInstanceState)

    binding.consoleLogsTv.movementMethod = ScrollingMovementMethod()

    setupPushToTalk()
    setupTextInput()
    setupTopologyView()
    observeEngineState()

    binding.clearLogsBtn.setOnClickListener {
      binding.consoleLogsTv.text = ""
    }

    checkAudioPermission()
  }

  private fun setupPushToTalk() {
    binding.holdToSpeakBtn.setOnTouchListener { _, event ->
      when (event.action) {
        MotionEvent.ACTION_DOWN -> {
          if (hasAudioPermission()) {
            engine.startListening(requireContext())
            binding.holdToSpeakBtn.text = "🔴 Listening... (Release to Send)"
            binding.holdToSpeakBtn.setBackgroundColor(Color.parseColor("#EA4335"))
          } else {
            audioPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
          }
          true
        }
        MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
          if (engine.state.value == EngineState.LISTENING) {
            engine.stopListening()
          }
          binding.holdToSpeakBtn.text = "🎤  Hold to Speak"
          binding.holdToSpeakBtn.setBackgroundColor(Color.parseColor("#1A73E8"))
          true
        }
        else -> false
      }
    }
  }

  private fun setupTextInput() {
    binding.sendPromptBtn.setOnClickListener {
      submitTextPrompt()
    }

    binding.textPromptEd.setOnEditorActionListener { _, actionId, _ ->
      if (actionId == EditorInfo.IME_ACTION_SEND) {
        submitTextPrompt()
        true
      } else {
        false
      }
    }
  }

  private fun submitTextPrompt() {
    val prompt = binding.textPromptEd.text.toString().trim()
    if (prompt.isEmpty()) return

    binding.textPromptEd.setText("")
    appendLog("[User Prompt] \"$prompt\"")

    viewLifecycleOwner.lifecycleScope.launch {
      val result = engine.processVoicePrompt(prompt, requireContext())
      appendLog("[Dispatch Result] Success=${result.isSuccess} | Msg=${result.dispatchResult?.message ?: if (result.isSuccess) "OK" else "Error"}")
      if (result.speechResponse.isNotEmpty()) {
        appendLog("[Voice Response] \"${result.speechResponse}\"")
      }
    }
  }


  private fun setupTopologyView() {
    val nodes = engine.nodeRegistry.getAllNodes()
    if (nodes.isEmpty()) {
      binding.fabricNodesTv.text = "No commissioned nodes found in fabric. Using default demo topology (Bedroom Lamp, Kitchen AC, Living Room TV)."
    } else {
      val sb = StringBuilder()
      for (node in nodes) {
        sb.append("• Node 0x${node.nodeId.toString(16)}: ${node.nodeLabel} in ${node.roomName} (${node.productName})\n")
        for (ep in node.endpoints) {
          val clusterHex = ep.serverClusters.joinToString { "0x" + it.toString(16) }
          sb.append("   - Endpoint ${ep.endpointId} [${ep.deviceTypeName}]: Clusters [$clusterHex]\n")
        }
      }
      binding.fabricNodesTv.text = sb.toString()
    }
  }


  private fun observeEngineState() {
    viewLifecycleOwner.lifecycleScope.launch {
      engine.state.collect { state ->
        updateStatusUi(state)
      }
    }

    viewLifecycleOwner.lifecycleScope.launch {
      engine.lastTranscription.collect { text ->
        if (text.isNotEmpty()) {
          binding.transcriptionTv.text = "\"$text\""
        }
      }
    }

    viewLifecycleOwner.lifecycleScope.launch {
      engine.lastVoiceResponse.collect { response ->
        if (response.isNotEmpty()) {
          binding.voiceResponseTv.visibility = View.VISIBLE
          binding.voiceResponseTv.text = response
        }
      }
    }
  }

  private fun updateStatusUi(state: EngineState) {
    when (state) {
      EngineState.IDLE -> {
        binding.statusIndicatorDot.setBackgroundColor(Color.parseColor("#34A853"))
        binding.statusStateTv.text = "IDLE • Ready for voice or text prompt"
      }
      EngineState.LISTENING -> {
        binding.statusIndicatorDot.setBackgroundColor(Color.parseColor("#EA4335"))
        binding.statusStateTv.text = "LISTENING • Capturing speech..."
      }
      EngineState.PROCESSING -> {
        binding.statusIndicatorDot.setBackgroundColor(Color.parseColor("#FBBC04"))
        binding.statusStateTv.text = "PROCESSING • Gemini Nano compiling intent..."
      }
      EngineState.EXECUTING -> {
        binding.statusIndicatorDot.setBackgroundColor(Color.parseColor("#4285F4"))
        binding.statusStateTv.text = "EXECUTING • Dispatching TLV to Matter fabric..."
      }
      EngineState.SUCCESS -> {
        binding.statusIndicatorDot.setBackgroundColor(Color.parseColor("#34A853"))
        binding.statusStateTv.text = "SUCCESS • Command executed and confirmed"
      }
      EngineState.ERROR -> {
        binding.statusIndicatorDot.setBackgroundColor(Color.parseColor("#EA4335"))
        binding.statusStateTv.text = "ERROR • Execution or resolution failed"
      }
    }
  }

  private fun appendLog(msg: String) {
    val time = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
    binding.consoleLogsTv.append("[$time] $msg\n")
  }

  private fun hasAudioPermission(): Boolean {
    return ContextCompat.checkSelfPermission(
      requireContext(),
      Manifest.permission.RECORD_AUDIO
    ) == PackageManager.PERMISSION_GRANTED
  }

  private fun checkAudioPermission() {
    if (!hasAudioPermission()) {
      audioPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
    }
  }

  override fun onDestroyView() {
    super.onDestroyView()
    _binding = null
  }

  companion object {
    @JvmStatic
    fun newInstance(): VoiceControlFragment = VoiceControlFragment()
  }
}
