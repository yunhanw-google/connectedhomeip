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
import android.content.Intent
import android.os.Bundle
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.util.Log
import java.util.Locale
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * State machine for the on-device voice control engine.
 */
enum class EngineState {
  IDLE,
  LISTENING,
  PROCESSING,
  EXECUTING,
  SUCCESS,
  ERROR
}

/**
 * High-level Result of Voice Control Processing.
 */
data class VoiceExecutionResponse(
  val prompt: String,
  val intent: VoiceIntent?,
  val dispatchResult: MatterDispatchResult?,
  val speechResponse: String,
  val isSuccess: Boolean
) {
  val spokenResponse: String
    get() = speechResponse
}

/**
 * Universal All-Cluster Matter Voice Control Engine.
 * Top-level orchestrator interfacing on-device Gemini Nano / AICore with standard Matter clusters,
 * dynamic tool generation, dynamic TLV parameter encoding, and polymorphic IM dispatching.
 */
class VoiceControlEngine private constructor(
  val nodeRegistry: CommissionedNodeRegistry
) {
  val toolGenerator = DynamicNanoToolGenerator(nodeRegistry)
  val dispatcher = UniversalMatterDispatcher(nodeRegistry)

  private val _state = MutableStateFlow(EngineState.IDLE)
  val state: StateFlow<EngineState> = _state.asStateFlow()

  private val _lastTranscription = MutableStateFlow("")
  val lastTranscription: StateFlow<String> = _lastTranscription.asStateFlow()

  private val _lastVoiceResponse = MutableStateFlow("")
  val lastVoiceResponse: StateFlow<String> = _lastVoiceResponse.asStateFlow()

  private var speechRecognizer: SpeechRecognizer? = null
  private val scope = CoroutineScope(Dispatchers.Main)

  companion object {
    private const val TAG = "VoiceControlEngine"

    @Volatile
    private var instance: VoiceControlEngine? = null

    fun getInstance(nodeRegistry: CommissionedNodeRegistry = CommissionedNodeRegistry()): VoiceControlEngine {
      return instance ?: synchronized(this) {
        instance ?: VoiceControlEngine(nodeRegistry).also {
          if (it.nodeRegistry.count() == 0) {
            it.nodeRegistry.loadDefaultSmartHomeFabric()
          }
          instance = it
        }
      }
    }

    fun getInstance(context: Context): VoiceControlEngine {
      val reg = CommissionedNodeRegistry()
      reg.loadFabricFromPreferences(context)
      if (reg.count() == 0) {
        reg.loadDefaultSmartHomeFabric()
      }
      return getInstance(reg)
    }
  }

  /**
   * Starts listening to audio input via SpeechRecognizer.
   */
  fun startListening(context: Context) {
    if (!SpeechRecognizer.isRecognitionAvailable(context)) {
      Log.w(TAG, "SpeechRecognizer is not available on this device.")
      _state.value = EngineState.IDLE
      return
    }

    _state.value = EngineState.LISTENING
    _lastTranscription.value = ""

    speechRecognizer?.destroy()
    speechRecognizer = SpeechRecognizer.createSpeechRecognizer(context).apply {
      setRecognitionListener(object : RecognitionListener {
        override fun onReadyForSpeech(params: Bundle?) {
          Log.d(TAG, "SpeechRecognizer ready for speech")
        }

        override fun onBeginningOfSpeech() {
          _state.value = EngineState.LISTENING
        }

        override fun onRmsChanged(rmsdB: Float) {}
        override fun onBufferReceived(buffer: ByteArray?) {}
        override fun onEndOfSpeech() {
          _state.value = EngineState.PROCESSING
        }

        override fun onError(error: Int) {
          Log.w(TAG, "SpeechRecognizer error: $error")
          _state.value = EngineState.ERROR
        }

        override fun onResults(results: Bundle?) {
          val matches = results?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
          val spokenText = matches?.firstOrNull() ?: ""
          Log.d(TAG, "Speech recognition result: $spokenText")
          _lastTranscription.value = spokenText

          if (spokenText.isNotEmpty()) {
            scope.launch {
              processVoicePrompt(spokenText, context)
            }
          } else {
            _state.value = EngineState.IDLE
          }
        }

        override fun onPartialResults(partialResults: Bundle?) {
          val partial = partialResults?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull()
          if (!partial.isNullOrEmpty()) {
            _lastTranscription.value = partial
          }
        }

        override fun onEvent(eventType: Int, params: Bundle?) {}
      })
    }

    val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
      putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
      putExtra(RecognizerIntent.EXTRA_LANGUAGE, Locale.getDefault())
      putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
      putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1)
    }

    try {
      speechRecognizer?.startListening(intent)
    } catch (e: Exception) {
      Log.e(TAG, "Error starting SpeechRecognizer", e)
      _state.value = EngineState.ERROR
    }
  }

  /**
   * Stops listening to audio input.
   */
  fun stopListening() {
    try {
      speechRecognizer?.stopListening()
    } catch (e: Exception) {
      Log.e(TAG, "Error stopping SpeechRecognizer", e)
    }
  }

  /**
   * Processes a natural language text prompt directly into Matter execution.
   */
  suspend fun processVoicePrompt(prompt: String, context: Context): VoiceExecutionResponse {
    _state.value = EngineState.PROCESSING
    _lastTranscription.value = prompt

    return try {
      // 1. Compile prompt using natural language matching / Gemini Nano compiler
      val intent = toolGenerator.matchPromptToIntent(prompt)

      _state.value = EngineState.EXECUTING
      // 2. Dispatch Matter interaction
      val dispatchResult = dispatcher.dispatchIntent(context, intent)

      _state.value = if (dispatchResult.isSuccess) EngineState.SUCCESS else EngineState.ERROR
      _lastVoiceResponse.value = dispatchResult.voiceResponse

      VoiceExecutionResponse(
        prompt = prompt,
        intent = intent,
        dispatchResult = dispatchResult,
        speechResponse = dispatchResult.voiceResponse,
        isSuccess = dispatchResult.isSuccess
      )
    } catch (e: Exception) {
      Log.e(TAG, "Error processing prompt \"$prompt\"", e)
      _state.value = EngineState.ERROR
      val errMsg = "Failed to process voice command: ${e.message}"
      _lastVoiceResponse.value = errMsg
      VoiceExecutionResponse(
        prompt = prompt,
        intent = null,
        dispatchResult = null,
        speechResponse = errMsg,
        isSuccess = false
      )
    }
  }

  suspend fun processVoiceCommand(context: Context, prompt: String): VoiceExecutionResponse {
    return processVoicePrompt(prompt, context)
  }

  /**
   * Generates complete AICore Gemini Nano prompt and tools configuration for the active fabric.
   */
  fun getAICoreConfiguration(): Pair<String, NanoTool> {
    val prompt = toolGenerator.generateSystemPrompt()
    val tools = toolGenerator.generateGeminiTools()
    return Pair(prompt, tools)
  }

  /**
   * Executes a tool call returned by Gemini Nano / AICore.
   */
  suspend fun executeNanoToolCall(
    context: Context,
    functionName: String,
    arguments: Map<String, Any?>,
    userPrompt: String
  ): VoiceExecutionResponse {
    _state.value = EngineState.EXECUTING
    Log.d(TAG, "Executing Gemini Nano tool call: $functionName with args: $arguments for prompt: \"$userPrompt\"")
    return try {
      val intent = toolGenerator.compileToolCall(functionName, arguments, userPrompt)
      val dispatchResult = dispatcher.dispatchIntent(context, intent)
      _state.value = if (dispatchResult.isSuccess) EngineState.SUCCESS else EngineState.ERROR
      _lastVoiceResponse.value = dispatchResult.voiceResponse
      VoiceExecutionResponse(
        prompt = userPrompt,
        intent = intent,
        dispatchResult = dispatchResult,
        speechResponse = dispatchResult.voiceResponse,
        isSuccess = dispatchResult.isSuccess
      )
    } catch (e: Exception) {
      Log.e(TAG, "Error executing tool call $functionName", e)
      _state.value = EngineState.ERROR
      VoiceExecutionResponse(
        prompt = userPrompt,
        intent = null,
        dispatchResult = null,
        speechResponse = "Sorry, I couldn't execute that command: ${e.message}",
        isSuccess = false
      )
    }
  }

  /**
   * Executes a tool call from a raw JSON string returned by the model.
   */
  suspend fun executeNanoToolCallJson(
    context: Context,
    toolCallJson: String,
    userPrompt: String
  ): VoiceExecutionResponse {
    _state.value = EngineState.EXECUTING
    return try {
      val intent = toolGenerator.parseToolCallJson(toolCallJson, userPrompt)
      val dispatchResult = dispatcher.dispatchIntent(context, intent)
      _state.value = if (dispatchResult.isSuccess) EngineState.SUCCESS else EngineState.ERROR
      _lastVoiceResponse.value = dispatchResult.voiceResponse
      VoiceExecutionResponse(
        prompt = userPrompt,
        intent = intent,
        dispatchResult = dispatchResult,
        speechResponse = dispatchResult.voiceResponse,
        isSuccess = dispatchResult.isSuccess
      )
    } catch (e: Exception) {
      Log.e(TAG, "Error parsing tool call JSON", e)
      _state.value = EngineState.ERROR
      VoiceExecutionResponse(
        prompt = userPrompt,
        intent = null,
        dispatchResult = null,
        speechResponse = "Failed to parse model response: ${e.message}",
        isSuccess = false
      )
    }
  }
}
