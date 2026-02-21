package com.hunter.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.util.Log;

import androidx.core.app.NotificationCompat;

import java.util.ArrayList;
import java.util.List;

/**
 * Background service for continuous config monitoring, testing, and caching.
 * Uses a dedicated HandlerThread to avoid blocking the main thread.
 * Integrates with tiered caching: top 100 tested configs + file-based raw cache.
 */
public class ConfigMonitorService extends Service {
    private static final String TAG = "ConfigMonitor";
    private static final String CHANNEL_ID = "config_monitor";
    private static final int NOTIFICATION_ID = 2;

    private static final long REFRESH_INTERVAL = 10 * 60 * 1000; // 10 minutes (reduced from 20)
    private static final long TEST_INTERVAL = 90 * 1000;     // 90 seconds (reduced from 3 minutes)
    private static final long INITIAL_TEST_DELAY = 3000;      // 3 seconds (reduced from 8)
    private static final long NOTIFICATION_THROTTLE_MS = 2000;     // 2 seconds minimum between notifications

    private HandlerThread workerThread;
    private Handler workerHandler;
    private ConfigManager configManager;
    private ConfigTester configTester;
    private ConfigBalancer configBalancer;
    private ProxyBalancer proxyBalancer;
    private volatile boolean isDestroyed = false;
    
    // Notification rate limiting
    private volatile long lastNotificationTime = 0;
    private volatile String lastNotificationText = "";

    @Override
    public void onCreate() {
        super.onCreate();

        createNotificationChannel();
        Notification notification = createNotification("Starting monitor...");
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            } else {
                startForeground(NOTIFICATION_ID, notification);
            }
        } catch (Throwable t) {
            Log.e(TAG, "startForeground failed", t);
            stopSelf();
            return;
        }

        // Dedicated background thread for scheduling
        workerThread = new HandlerThread("ConfigMonitorWorker");
        workerThread.start();
        workerHandler = new Handler(workerThread.getLooper());

        FreeV2RayApplication app = FreeV2RayApplication.getInstance();
        if (app == null) {
            Log.e(TAG, "Application instance is null");
            stopSelf();
            return;
        }
        configManager = app.getConfigManager();
        configTester = app.getConfigTester();
        configBalancer = app.getConfigBalancer();
        proxyBalancer = app.getProxyBalancer();
        if (configManager == null || configTester == null || configBalancer == null || proxyBalancer == null) {
            Log.e(TAG, "Managers not ready");
            stopSelf();
            return;
        }

        // Start proxy balancers early to make them always active
        if (!proxyBalancer.isRunning()) {
            proxyBalancer.start();
            Log.i(TAG, "Proxy balancers started early");
        }

        // Don't refresh immediately - MainActivity may already be refreshing
        // Wait a bit, then check if refresh is needed
        workerHandler.postDelayed(this::maybeRefresh, 5000);
        workerHandler.postDelayed(this::doTest, INITIAL_TEST_DELAY + 5000);

        Log.i(TAG, "Config monitor service started");
    }

    private void maybeRefresh() {
        if (isDestroyed) return;
        // Skip if ConfigManager is already refreshing (e.g. from MainActivity)
        if (configManager.isRefreshing()) {
            Log.i(TAG, "Skipping refresh - already in progress");
            updateNotification("Waiting for refresh to complete...");
            scheduleNextRefresh();
            return;
        }
        // Skip if configs were recently refreshed (within 2 minutes)
        long lastUpdate = configManager.getLastUpdateTime();
        if (lastUpdate > 0 && System.currentTimeMillis() - lastUpdate < 120000) {
            Log.i(TAG, "Skipping refresh - recently updated");
            scheduleNextRefresh();
            return;
        }
        doRefresh();
    }

    private void doRefresh() {
        if (isDestroyed) return;

        Log.i(TAG, "Starting periodic config refresh");
        updateNotification("Refreshing configs...");

        configManager.refreshConfigs(new ConfigManager.RefreshCallback() {
            @Override
            public void onProgress(String message, int current, int total) {
                if (!isDestroyed) {
                    // Update less frequently for refresh progress
                    if (current % 50 == 0 || current == total) {
                        updateNotification("Refreshing: " + current + "/" + total);
                    }
                }
            }

            @Override
            public void onComplete(int totalConfigs) {
                Log.i(TAG, "Refresh complete: " + totalConfigs + " configs");
                updateNotification(totalConfigs + " configs loaded");

                // Test after refresh completes
                if (!isDestroyed) {
                    workerHandler.postDelayed(() -> doTest(), 3000);
                }
            }

            @Override
            public void onError(String error) {
                Log.e(TAG, "Refresh error: " + error);
                updateNotification("Refresh failed, using cache");
            }
        });

        scheduleNextRefresh();
    }

    private void scheduleNextRefresh() {
        if (!isDestroyed) {
            workerHandler.postDelayed(this::maybeRefresh, REFRESH_INTERVAL);
        }
    }

    private void doTest() {
        if (isDestroyed) return;
        if (configTester.isRunning()) {
            Log.w(TAG, "Test already running, skipping");
            scheduleNextTest();
            return;
        }
        if (configManager.isRefreshing() && configManager.getConfigCount() == 0) {
            Log.w(TAG, "Refresh in progress and no cached configs, postponing test");
            workerHandler.postDelayed(this::doTest, 10000);
            return;
        }

        // Check if XRay engine is ready (bundled in APK, extracts on first run)
        XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
        if (xrayManager != null && !xrayManager.isReady() && !xrayManager.isDownloading()) {
            Log.i(TAG, "XRay not ready, extracting from assets");
            updateNotification("Installing V2Ray engine...");
            xrayManager.ensureInstalled(new XRayManager.InstallCallback() {
                @Override
                public void onProgress(String message, int percent) {
                    // Only update on significant progress changes
                    if (percent % 25 == 0 || percent == 100) {
                        updateNotification(message);
                    }
                }
                @Override
                public void onComplete(String binaryPath) {
                    Log.i(TAG, "XRay ready, starting tests");
                    if (!isDestroyed) workerHandler.post(() -> doTest());
                }
                @Override
                public void onError(String error) {
                    Log.e(TAG, "XRay install failed: " + error);
                    updateNotification("Engine install failed");
                    scheduleNextTest();
                }
            });
            return;
        }

        // Pass ALL configs — ConfigTester handles dedup + 3-phase pipeline internally.
        // getPrioritizedConfigs() puts previously-working configs first.
        List<ConfigManager.ConfigItem> configs = configManager.getPrioritizedConfigs();
        if (configs.isEmpty()) {
            Log.w(TAG, "No configs to test");
            updateNotification("No configs - waiting for refresh");
            scheduleNextTest();
            return;
        }

        final int totalToTest = configs.size();
        Log.i(TAG, "Starting health check: " + totalToTest + " configs (pipeline will dedup)");
        updateNotification("Testing " + totalToTest + " configs...");

        // Keep reference to tested configs - config refresh may replace configManager's list
        final List<ConfigManager.ConfigItem> testedConfigs = new ArrayList<>(configs);

        configTester.testConfigs(configs, new ConfigTester.TestCallback() {
            @Override
            public void onProgress(int tested, int total) {
                // Update less frequently to reduce notification spam (thousands of configs now)
                if (!isDestroyed && (tested % 200 == 0 || tested == total)) {
                    updateNotification("Testing: " + tested + "/" + total);
                }
            }

            @Override
            public void onConfigTested(ConfigManager.ConfigItem config, boolean success, int latency, boolean googleAccessible) {
                // Individual results handled by latency field update
            }

            @Override
            public void onComplete(int totalTested, int successCount) {
                if (isDestroyed) return;

                Log.i(TAG, "Health check: " + successCount + "/" + totalTested + " working");

                // Use the TESTED configs (which have latency set), not re-fetched configs
                // Config refresh may have replaced configManager's list with fresh objects (latency=-1)
                List<ConfigManager.ConfigItem> working = configTester.getWorkingConfigs(testedConfigs);

                // Update balancer with working configs
                configBalancer.updateConfigs(working);

                // Update proxy balancer with tested configs
                ProxyBalancer proxyBalancer = FreeV2RayApplication.getInstance().getProxyBalancer();
                if (proxyBalancer != null) {
                    proxyBalancer.updateConfigs(testedConfigs);
                    Log.i(TAG, "Proxy balancer updated with " + testedConfigs.size() + " tested configs");
                }

                // Update top 100 cache
                configManager.updateTopConfigs(working);

                // Trigger immediate network condition check if we have good configs
                if (successCount > 0 && working.size() > 0) {
                    triggerNetworkConditionCheck();
                }

                updateNotification(successCount + " working / " +
                    configManager.getConfigCount() + " total");

                scheduleNextTest();
            }
        });
    }

    private void scheduleNextTest() {
        if (!isDestroyed) {
            workerHandler.postDelayed(this::doTest, TEST_INTERVAL);
        }
    }

    /**
     * Trigger immediate network condition check when good configs are found
     */
    private void triggerNetworkConditionCheck() {
        if (!isDestroyed) {
            workerHandler.post(() -> {
                try {
                    // This would integrate with the network detection system
                    // For now, we'll just log that we're triggering a check
                    Log.i(TAG, "Triggering immediate network condition check");
                    // TODO: Call actual network condition detection system
                } catch (Exception e) {
                    Log.w(TAG, "Error triggering network condition check", e);
                }
            });
        }
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "Config Monitor", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Monitors and tests V2Ray configs");
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null) manager.createNotificationChannel(channel);
        }
    }

    private Notification createNotification(String text) {
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Iranian Free V2Ray")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_menu_info_details)
                .setOngoing(true)
                .build();
    }

    private void updateNotification(String text) {
        try {
            // Rate limiting: only update if text changed or enough time passed
            long currentTime = System.currentTimeMillis();
            if (text.equals(lastNotificationText) && 
                (currentTime - lastNotificationTime) < NOTIFICATION_THROTTLE_MS) {
                return; // Skip duplicate notification within throttle period
            }
            
            NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (manager != null && !isDestroyed) {
                manager.notify(NOTIFICATION_ID, createNotification(text));
                lastNotificationTime = currentTime;
                lastNotificationText = text;
            }
        } catch (Exception ignored) {}
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        isDestroyed = true;
        if (workerHandler != null) {
            workerHandler.removeCallbacksAndMessages(null);
        }
        if (workerThread != null) {
            workerThread.quitSafely();
        }
        Log.i(TAG, "Config monitor service stopped");
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
