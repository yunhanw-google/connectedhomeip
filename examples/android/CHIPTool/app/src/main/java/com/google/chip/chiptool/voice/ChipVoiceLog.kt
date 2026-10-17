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

import android.util.Log

object ChipVoiceLog {
  fun utteranceLine(
    utterance: String,
    kind: String,
    route: String,
    fastMs: Long,
    nanoMs: Long,
    totalMs: Long,
    wrote: Boolean,
    answer: String,
  ): String {
    val cleanAnswer = if (answer.length > 120) answer.substring(0, 120) else answer
    return "utterance=\"$utterance\" kind=$kind route=$route fastMs=$fastMs nanoMs=$nanoMs totalMs=$totalMs wrote=$wrote answer=\"$cleanAnswer\""
  }

  fun logUtterance(
    utterance: String,
    kind: String,
    route: String,
    fastMs: Long,
    nanoMs: Long,
    totalMs: Long,
    wrote: Boolean,
    answer: String,
  ) {
    val line = utteranceLine(utterance, kind, route, fastMs, nanoMs, totalMs, wrote, answer)
    try {
      Log.i("ChipVoice", line)
    } catch (e: Exception) {
      println("ChipVoice: $line")
    }
  }

  fun logNanoStatus(status: String) {
    try {
      Log.i("ChipVoice", "nano status=<$status>")
    } catch (e: Exception) {
      println("ChipVoice: nano status=<$status>")
    }
  }

  fun logNanoDownload(pct: Int) {
    try {
      Log.i("ChipVoice", "nano download=<$pct>")
    } catch (e: Exception) {
      println("ChipVoice: nano download=<$pct>")
    }
  }
}
