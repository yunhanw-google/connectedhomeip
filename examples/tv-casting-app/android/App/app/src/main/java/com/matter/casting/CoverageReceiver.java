package com.matter.casting;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import java.io.FileOutputStream;

public class CoverageReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        try {
            Class<?> rt = Class.forName("org.jacoco.agent.rt.RT");
            Object agent = rt.getMethod("getAgent").invoke(null);
            byte[] data = (byte[]) Class.forName("org.jacoco.agent.rt.IAgent").getMethod("getExecutionData", boolean.class).invoke(agent, false);
            try (FileOutputStream fos = new FileOutputStream(context.getFilesDir() + "/coverage.ec.tmp")) {
                fos.write(data);
            }
            new java.io.File(context.getFilesDir(), "coverage.ec.tmp").renameTo(new java.io.File(context.getFilesDir(), "coverage.ec"));
            Log.i("CoverageReceiver", "JaCoCo coverage dumped to " + context.getFilesDir() + "/coverage.ec");
        } catch (Throwable t) {
            Log.e("CoverageReceiver", "Failed to dump JaCoCo coverage", t);
        }
        try {
            
            Class<?> clazz = Class.forName("chip.platform.AndroidChipPlatform");
            clazz.getMethod("flushCoverageProfile", Context.class).invoke(null, context);
            Log.i("CoverageReceiver", "Native coverage flushed.");
        } catch (Throwable t) {
            Log.e("CoverageReceiver", "Failed to flush native coverage", t);
        }
    }
}
