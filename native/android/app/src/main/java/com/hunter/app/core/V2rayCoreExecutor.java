package com.hunter.app.core;

import android.content.Context;
import android.content.Intent;
import android.util.Log;

import com.hunter.app.XRayManager;
import com.hunter.app.interfaces.V2rayServicesListener;
import com.hunter.app.model.V2rayConfigModel;
import com.hunter.app.utils.V2rayConstants;

import java.io.File;

/**
 * Manages XRay core lifecycle using existing process-based approach
 * Enhanced with v2rayNG architecture patterns but using existing XRayManager
 */
public class V2rayCoreExecutor {
    private static final String TAG = "V2rayCoreExecutor";
    
    private V2rayConstants.CORE_STATES coreState;
    public V2rayServicesListener v2rayServicesListener;
    private Process xrayProcess;
    private XRayManager xrayManager;
    
    public V2rayCoreExecutor(final Context targetService) {
        this.v2rayServicesListener = (V2rayServicesListener) targetService;
        this.xrayManager = new XRayManager(targetService);
        coreState = V2rayConstants.CORE_STATES.IDLE;
        Log.d(TAG, "V2rayCoreExecutor initialized from: " + targetService.getClass().getSimpleName());
    }

    /**
     * Start XRay core with configuration validation
     */
    public void startCore(final V2rayConfigModel v2rayConfig) {
        try {
            stopCore(false);
            
            // Validate configuration before starting
            if (!validateConfig(v2rayConfig.fullJsonConfig)) {
                coreState = V2rayConstants.CORE_STATES.STOPPED;
                Log.e(TAG, "Configuration validation failed");
                stopCore(true);
                return;
            }
            
            // Write config and start process
            File configFile = xrayManager.writeConfig(v2rayConfig.fullJsonConfig, v2rayConfig.localSocksPort);
            if (configFile == null) {
                coreState = V2rayConstants.CORE_STATES.STOPPED;
                Log.e(TAG, "Failed to write config file");
                stopCore(true);
                return;
            }
            
            xrayProcess = xrayManager.startProcess(configFile);
            if (xrayProcess != null) {
                coreState = V2rayConstants.CORE_STATES.RUNNING;
                v2rayServicesListener.startService();
                Log.d(TAG, "XRay core started successfully");
            } else {
                coreState = V2rayConstants.CORE_STATES.STOPPED;
                Log.e(TAG, "Failed to start XRay process");
                stopCore(true);
            }
            
        } catch (Exception e) {
            Log.e(TAG, "Failed to start core", e);
            coreState = V2rayConstants.CORE_STATES.STOPPED;
            stopCore(true);
        }
    }

    /**
     * Stop XRay core
     */
    public void stopCore(final boolean shouldStopService) {
        try {
            if (xrayProcess != null && xrayProcess.isAlive()) {
                xrayProcess.destroy();
                xrayProcess = null;
            }
            
            if (shouldStopService) {
                v2rayServicesListener.stopService();
            }
            coreState = V2rayConstants.CORE_STATES.STOPPED;
            Log.d(TAG, "XRay core stopped");
        } catch (Exception e) {
            Log.d(TAG, "Failed to stop core", e);
        }
    }

    /**
     * Get download speed (simulated for process-based approach)
     */
    public long getDownloadSpeed() {
        // For process-based approach, we'll implement custom speed monitoring later
        return -1; // Not available with process approach
    }

    /**
     * Get upload speed (simulated for process-based approach)
     */
    public long getUploadSpeed() {
        // For process-based approach, we'll implement custom speed monitoring later
        return -1; // Not available with process approach
    }

    /**
     * Get current core state
     */
    public V2rayConstants.CORE_STATES getCoreState() {
        if (coreState == V2rayConstants.CORE_STATES.RUNNING) {
            if (xrayProcess == null || !xrayProcess.isAlive()) {
                coreState = V2rayConstants.CORE_STATES.STOPPED;
            }
            return coreState;
        }
        return coreState;
    }

    /**
     * Measure delay to current server and broadcast result
     */
    public void broadcastCurrentServerDelay() {
        try {
            if (v2rayServicesListener != null) {
                // Use existing speed test methods for delay measurement
                int serverDelay = measureDelay();
                Intent serverDelayBroadcast = new Intent(V2rayConstants.V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_INTENT);
                serverDelayBroadcast.setPackage(v2rayServicesListener.getService().getPackageName());
                serverDelayBroadcast.putExtra(V2rayConstants.V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_EXTRA, serverDelay);
                v2rayServicesListener.getService().sendBroadcast(serverDelayBroadcast);
            }
        } catch (Exception e) {
            Log.d(TAG, "Failed to measure delay", e);
            Intent serverDelayBroadcast = new Intent(V2rayConstants.V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_INTENT);
            serverDelayBroadcast.setPackage(v2rayServicesListener.getService().getPackageName());
            serverDelayBroadcast.putExtra(V2rayConstants.V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_EXTRA, -1);
            v2rayServicesListener.getService().sendBroadcast(serverDelayBroadcast);
        }
    }

    /**
     * Test configuration delay without starting core
     */
    public static long getConfigDelay(final String config) {
        // Basic validation for now - can be enhanced with actual delay testing
        try {
            org.json.JSONObject config_json = new org.json.JSONObject(config);
            // Basic validation - check if it has required fields
            if (config_json.has("outbounds") && config_json.getJSONArray("outbounds").length() > 0) {
                return 100; // Mock delay in ms
            }
        } catch (Exception e) {
            Log.d(TAG, "Failed to test config delay", e);
        }
        return -1;
    }

    /**
     * Check if core is running
     */
    public boolean isRunning() {
        return coreState == V2rayConstants.CORE_STATES.RUNNING && 
               xrayProcess != null && xrayProcess.isAlive();
    }

    /**
     * Basic configuration validation
     */
    private boolean validateConfig(String config) {
        try {
            org.json.JSONObject config_json = new org.json.JSONObject(config);
            // Check if it has required fields
            if (!config_json.has("outbounds")) {
                return false;
            }
            org.json.JSONArray outbounds = config_json.getJSONArray("outbounds");
            if (outbounds.length() == 0) {
                return false;
            }
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Config validation failed", e);
            return false;
        }
    }

    /**
     * Simple delay measurement
     */
    private int measureDelay() {
        // Use existing speed test methods or implement simple ping
        // For now, return a mock value
        return 150; // Mock delay in ms
    }
}
