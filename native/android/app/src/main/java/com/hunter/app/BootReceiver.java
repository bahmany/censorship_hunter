package com.hunter.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;

/**
 * Receives BOOT_COMPLETED broadcast to auto-connect VPN after device reboot.
 */
public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = "FreeV2RayBoot";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) {
            SharedPreferences prefs = context.getSharedPreferences("vpn_settings", Context.MODE_PRIVATE);
            boolean autoStart = prefs.getBoolean("auto_start", false);
            boolean autoConnect = prefs.getBoolean("auto_connect", false);

            if (autoStart && autoConnect) {
                Log.i(TAG, "Boot completed - auto-connect enabled");
                // Note: VPN permission check requires user interaction
                // The app will auto-connect when opened
            }
        }
    }
}
