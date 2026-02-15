package com.hunter.app;

import android.app.Application;
import android.util.Log;

/**
 * Application class for Hunter.
 * Handles global initialization and native library loading.
 */
public class HunterApplication extends Application {
    private static final String TAG = "HunterApp";

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "Hunter Application started (Android " + android.os.Build.VERSION.SDK_INT + ")");

        // Load native library
        System.loadLibrary("hunter");

        // Ensure runtime directories exist
        java.io.File runtimeDir = new java.io.File(getFilesDir(), "runtime");
        if (!runtimeDir.exists()) {
            runtimeDir.mkdirs();
        }
    }
}
