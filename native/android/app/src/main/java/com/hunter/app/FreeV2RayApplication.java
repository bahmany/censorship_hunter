package com.hunter.app;

import android.app.Application;
import android.util.Log;

/**
 * Application class for Iranian Free V2Ray.
 * Pure Java implementation - no native dependencies.
 */
public class FreeV2RayApplication extends Application {
    private static final String TAG = "FreeV2Ray";
    
    private static FreeV2RayApplication instance;
    private ConfigManager configManager;
    private ConfigTester configTester;
    private ConfigBalancer configBalancer;
    private XRayManager xrayManager;
    
    public static FreeV2RayApplication getInstance() {
        return instance;
    }
    
    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        
        // Initialize config manager (pure Java)
        configManager = new ConfigManager(this);
        configTester = new ConfigTester();
        configBalancer = new ConfigBalancer();
        xrayManager = new XRayManager(this);
        
        // Extract XRay binary from bundled assets on first run
        if (xrayManager.needsUpdate()) {
            xrayManager.ensureInstalled(new XRayManager.InstallCallback() {
                @Override
                public void onProgress(String message, int percent) {
                    Log.i(TAG, "XRay: " + message + " (" + percent + "%)");
                }
                @Override
                public void onComplete(String binaryPath) {
                    Log.i(TAG, "XRay engine ready at: " + binaryPath);
                }
                @Override
                public void onError(String error) {
                    Log.e(TAG, "XRay install failed: " + error);
                }
            });
        }
        
        Log.i(TAG, "Iranian Free V2Ray initialized");
    }
    
    public ConfigManager getConfigManager() {
        return configManager;
    }
    
    public ConfigTester getConfigTester() {
        return configTester;
    }
    
    public ConfigBalancer getConfigBalancer() {
        return configBalancer;
    }
    
    public XRayManager getXRayManager() {
        return xrayManager;
    }
    
    @Override
    public void onTerminate() {
        if (configTester != null) configTester.shutdown();
        if (configManager != null) configManager.shutdown();
        if (xrayManager != null) xrayManager.shutdown();
        super.onTerminate();
    }
}
