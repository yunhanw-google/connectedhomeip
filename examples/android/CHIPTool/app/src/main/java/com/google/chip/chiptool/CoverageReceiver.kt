package com.google.chip.chiptool

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import chip.platform.AndroidChipPlatform
import java.io.FileOutputStream

class CoverageReceiver : BroadcastReceiver() {
  override fun onReceive(context: Context, intent: Intent) {
    try {
      val rt = Class.forName("org.jacoco.agent.rt.RT")
      val agent = rt.getMethod("getAgent").invoke(null)
      val data =
        agent.javaClass
          .getMethod("getExecutionData", Boolean::class.javaPrimitiveType)
          .invoke(agent, false) as ByteArray
      FileOutputStream(context.filesDir.absolutePath + "/coverage.ec").use { fos ->
        fos.write(data)
      }
      Log.i(
        "CoverageReceiver",
        "JaCoCo coverage dumped to " + context.filesDir.absolutePath + "/coverage.ec",
      )
    } catch (e: Exception) {
      Log.e("CoverageReceiver", "Failed to dump JaCoCo coverage", e)
    }
    try {
      AndroidChipPlatform.flushCoverageProfile(context)
      Log.i("CoverageReceiver", "Native coverage flushed.")
    } catch (e: Exception) {
      Log.e("CoverageReceiver", "Failed to flush native coverage", e)
    }
  }
}
