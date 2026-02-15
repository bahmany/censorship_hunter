package com.hunter.app;

import android.app.Application;
import android.util.Log;

import java.io.File;

/**
 * Application class for Iran Filter Bypass.
 * Initializes native libraries and config on app start.
 */
public class FilterBypassApplication extends Application {
    private static final String TAG = "FilterBypassApp";
    
    private static FilterBypassApplication instance;
    private HunterCallbackImpl callbackImpl;
    
    public static FilterBypassApplication getInstance() {
        return instance;
    }
    
    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        
        // Ensure runtime directory exists
        File runtimeDir = new File(getFilesDir(), "runtime");
        if (!runtimeDir.exists()) {
            runtimeDir.mkdirs();
        }
        
        // Initialize native library
        try {
            callbackImpl = new HunterCallbackImpl(this);
            String secretsPath = new File(getFilesDir(), "secrets.env").getAbsolutePath();
            HunterNative.nativeInit(getFilesDir().getAbsolutePath(), secretsPath, callbackImpl);
            Log.i(TAG, "Native library initialized");
        } catch (Exception e) {
            Log.e(TAG, "Failed to initialize native library", e);
        }
    }
    
    public HunterCallbackImpl getCallbackImpl() {
        return callbackImpl;
    }
}
