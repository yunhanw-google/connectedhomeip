package com.google.chip.chiptool

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import java.io.File
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
      val tmpFile = File(context.filesDir, "coverage.ec.tmp")
      val outFile = File(context.filesDir, "coverage.ec")
      FileOutputStream(tmpFile).use { fos ->
        fos.write(data)
      }
      tmpFile.renameTo(outFile)
      Log.i(
        "CoverageReceiver",
        "JaCoCo coverage dumped to " + outFile.absolutePath,
      )
    } catch (e: Throwable) {
      Log.e("CoverageReceiver", "Failed to dump JaCoCo coverage", e)
    }
    try {
      val clazz = Class.forName("chip.platform.AndroidChipPlatform")
      clazz.getMethod("flushCoverageProfile", Context::class.java).invoke(null, context)
      Log.i("CoverageReceiver", "Native coverage flushed.")
    } catch (e: Throwable) {
      Log.e("CoverageReceiver", "Failed to flush native coverage", e)
    }
  }
}
