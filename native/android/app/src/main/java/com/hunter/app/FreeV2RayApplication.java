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
    private ProxyBalancer proxyBalancer;
    
    public static FreeV2RayApplication getInstance() {
        return instance;
    }
    
    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;

        // Global crash handler for better debugging
        final Thread.UncaughtExceptionHandler defaultHandler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            Log.e(TAG, "FATAL CRASH in thread " + thread.getName(), throwable);
            if (defaultHandler != null) {
                defaultHandler.uncaughtException(thread, throwable);
            }
        });

        try {
            // Initialize config manager (pure Java)
            configManager = new ConfigManager(this);
            configTester = new ConfigTester();
            configBalancer = new ConfigBalancer();
            xrayManager = new XRayManager(this);
            proxyBalancer = new ProxyBalancer();

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
        } catch (Exception e) {
            Log.e(TAG, "CRITICAL: Failed to initialize app", e);
        }
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
    
    public ProxyBalancer getProxyBalancer() {
        return proxyBalancer;
    }
    
    @Override
    public void onTerminate() {
        if (configTester != null) configTester.stopTesting();
        if (configManager != null) configManager.shutdown();
        if (xrayManager != null) xrayManager.shutdown();
        if (proxyBalancer != null) proxyBalancer.stop();
        super.onTerminate();
    }
}
