package com.hunter.app;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;

/**
 * Backend represents a running XRay process with a specific config
 */
public class Backend {
    private static final String TAG = "Backend";

    private final ConfigManager.ConfigItem config;
    private final int port;
    private Process process;
    private File configFile;
    private volatile boolean isRunning = false;

    public Backend(ConfigManager.ConfigItem config, int port) {
        this.config = config;
        this.port = port;
    }

    /**
     * Start the XRay backend
     */
    public boolean start() {
        try {
            XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
            if (xrayManager == null || !xrayManager.isReady()) {
                Log.w(TAG, "XRay manager not ready for backend on port " + port);
                return false;
            }

            // Generate config for this backend
            String v2rayConfig = generateBackendConfig(config.uri, port);
            if (v2rayConfig == null) {
                Log.w(TAG, "Failed to generate config for backend on port " + port);
                return false;
            }

            // Write config to temp file
            configFile = xrayManager.writeConfig(v2rayConfig, port);
            if (configFile == null) {
                Log.w(TAG, "Failed to write config file for backend on port " + port);
                return false;
            }

            // Start XRay process
            process = xrayManager.startProcess(configFile);
            if (process == null) {
                configFile.delete();
                Log.w(TAG, "Failed to start XRay process for backend on port " + port);
                return false;
            }

            isRunning = true;
            Log.i(TAG, "Backend started on port " + port + " with config " + config.name);
            return true;

        } catch (Exception e) {
            Log.e(TAG, "Failed to start backend on port " + port, e);
            return false;
        }
    }

    /**
     * Stop the backend
     */
    public void stop() {
        if (!isRunning) {
            return;
        }

        isRunning = false;

        if (process != null) {
            try {
                process.destroyForcibly();
                process.waitFor();
            } catch (Exception e) {
                Log.w(TAG, "Error stopping backend process on port " + port, e);
            }
        }

        if (configFile != null) {
            try {
                configFile.delete();
            } catch (Exception e) {
                Log.w(TAG, "Error deleting config file for backend on port " + port, e);
            }
        }

        Log.i(TAG, "Backend stopped on port " + port);
    }

    /**
     * Check if backend is running
     */
    public boolean isRunning() {
        return isRunning && process != null && process.isAlive();
    }

    /**
     * Get the backend port
     */
    public int getPort() {
        return port;
    }

    /**
     * Generate V2Ray config for backend
     */
    private String generateBackendConfig(String uri, int socksPort) {
        try {
            // Parse URI to outbound
            JSONObject outbound = V2RayConfigHelper.parseUriToOutbound(uri);
            if (outbound == null) return null;

            JSONObject config = new JSONObject();

            // Log
            JSONObject log = new JSONObject();
            log.put("loglevel", "none");
            config.put("log", log);

            // Inbound: SOCKS5
            JSONArray inbounds = new JSONArray();
            JSONObject socksIn = new JSONObject();
            socksIn.put("tag", "socks-in");
            socksIn.put("port", socksPort);
            socksIn.put("listen", "127.0.0.1");
            socksIn.put("protocol", "socks");
            JSONObject socksSettings = new JSONObject();
            socksSettings.put("udp", false);
            socksIn.put("settings", socksSettings);
            inbounds.put(socksIn);
            config.put("inbounds", inbounds);

            // Outbound: the config
            JSONArray outbounds = new JSONArray();
            outbounds.put(outbound);
            config.put("outbounds", outbounds);

            return config.toString();

        } catch (Exception e) {
            Log.w(TAG, "Error generating backend config", e);
            return null;
        }
    }
}
