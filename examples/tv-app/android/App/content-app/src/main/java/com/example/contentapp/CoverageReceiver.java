package com.example.contentapp;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import java.io.FileOutputStream;
import chip.platform.AndroidChipPlatform;

public class CoverageReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        try {
            Class<?> rt = Class.forName("org.jacoco.agent.rt.RT");
            Object agent = rt.getMethod("getAgent").invoke(null);
            byte[] data = (byte[]) agent.getClass().getMethod("getExecutionData", boolean.class).invoke(agent, false);
            try (FileOutputStream fos = new FileOutputStream(context.getFilesDir() + "/coverage.ec")) {
                fos.write(data);
            }
        } catch (Exception e) {}
        try {
            AndroidChipPlatform.flushCoverageProfile(context);
        } catch (Exception e) {}
    }
}
