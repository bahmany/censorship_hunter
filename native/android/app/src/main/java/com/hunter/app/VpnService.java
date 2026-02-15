package com.hunter.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.VpnService.Builder;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import androidx.core.app.NotificationCompat;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * VPN Service implementing Android VpnService API with V2Ray/XRay load balancing.
 * Supports per-app VPN (split tunneling) and 2026 DPI evasion techniques.
 */
public class VpnService extends android.net.VpnService {
    private static final String TAG = "FilterBypassVPN";
    private static final String CHANNEL_ID = "vpn_service_channel";
    private static final int NOTIFICATION_ID = 1;
    
    private static final int TUN_MTU = 1500;
    private static final String TUN_ADDRESS = "10.255.0.1";
    private static final int TUN_PREFIX = 24;
    private static final String TUN_DNS = "1.1.1.1";
    
    private ParcelFileDescriptor tunInterface;
    private ExecutorService executor;
    private final AtomicBoolean isRunning = new AtomicBoolean(false);
    private final List<Process> v2rayProcesses = new ArrayList<>();
    private final List<Integer> activePorts = new ArrayList<>();
    
    // Load balancer settings
    private static final int BASE_SOCKS_PORT = 10808;
    private static final int MAX_BACKENDS = 10;
    
    public static final String ACTION_START = "com.hunter.app.action.START";
    public static final String ACTION_STOP = "com.hunter.app.action.STOP";
    
    private static VpnService instance;
    
    public static VpnService getInstance() {
        return instance;
    }
    
    public static boolean isActive() {
        return instance != null && instance.isRunning.get();
    }
    
    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        executor = Executors.newCachedThreadPool();
        createNotificationChannel();
    }
    
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            return START_NOT_STICKY;
        }
        
        String action = intent.getAction();
        if (ACTION_STOP.equals(action)) {
            stopVpn();
            return START_NOT_STICKY;
        }
        
        if (ACTION_START.equals(action) && !isRunning.get()) {
            startForeground(NOTIFICATION_ID, createNotification("Connecting..."));
            executor.execute(this::startVpn);
        }
        
        return START_STICKY;
    }
    
    private void startVpn() {
        try {
            isRunning.set(true);
            
            // Load configs from intent or cache
            List<JSONObject> configs = loadValidConfigs();
            if (configs.isEmpty()) {
                Log.w(TAG, "No valid configs found");
                updateNotification("No configs - Please refresh");
                stopVpn();
                return;
            }
            
            // Start V2Ray instances with load balancing
            int backendsStarted = startV2RayBackends(configs);
            if (backendsStarted == 0) {
                Log.e(TAG, "Failed to start any V2Ray backends");
                stopVpn();
                return;
            }
            
            // Setup TUN interface
            if (!setupTunInterface()) {
                Log.e(TAG, "Failed to setup TUN interface");
                stopVpn();
                return;
            }
            
            updateNotification("متصل (" + backendsStarted + " کانفیگ فعال)");
            Log.i(TAG, "VPN started with " + backendsStarted + " backends");
            
            // Start traffic routing
            startTrafficRouting();
            
        } catch (Exception e) {
            Log.e(TAG, "Failed to start VPN", e);
            stopVpn();
        }
    }
    
    private List<JSONObject> loadValidConfigs() {
        List<JSONObject> configs = new ArrayList<>();
        try {
            FreeV2RayApplication app = FreeV2RayApplication.getInstance();
            ConfigManager configManager = app.getConfigManager();
            ConfigBalancer balancer = app.getConfigBalancer();

            // Tier 1: Get from balancer (tested & working)
            List<ConfigManager.ConfigItem> source = balancer.getMultipleConfigs(MAX_BACKENDS);

            // Tier 2: Fallback to top cached configs
            if (source.isEmpty()) {
                source = configManager.getTopConfigs();
                if (source.size() > MAX_BACKENDS) {
                    source = source.subList(0, MAX_BACKENDS);
                }
                Log.i(TAG, "Using top cached configs: " + source.size());
            }

            // Tier 3: Fallback to all configs
            if (source.isEmpty()) {
                List<ConfigManager.ConfigItem> all = configManager.getConfigs();
                int limit = Math.min(all.size(), MAX_BACKENDS);
                source = all.subList(0, limit);
                Log.i(TAG, "Using all configs fallback: " + source.size());
            }

            for (ConfigManager.ConfigItem item : source) {
                JSONObject obj = new JSONObject();
                obj.put("uri", item.uri);
                obj.put("ps", item.name);
                obj.put("latency_ms", item.latency);
                obj.put("protocol", item.protocol);
                configs.add(obj);
            }

            Log.i(TAG, "Loaded " + configs.size() + " configs for VPN");
        } catch (Exception e) {
            Log.e(TAG, "Error loading configs", e);
        }
        return configs;
    }
    
    private int startV2RayBackends(List<JSONObject> configs) {
        int started = 0;
        XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
        
        if (xrayManager == null || !xrayManager.isReady()) {
            Log.e(TAG, "XRay binary not ready");
            return 0;
        }
        
        for (int i = 0; i < configs.size() && started < MAX_BACKENDS; i++) {
            try {
                JSONObject config = configs.get(i);
                String uri = config.getString("uri");
                int port = BASE_SOCKS_PORT + i;
                
                // Generate V2Ray config using shared helper
                String v2rayConfig = V2RayConfigHelper.generateFullConfig(uri, port);
                if (v2rayConfig == null) continue;
                
                // Write config and start process
                File configFile = xrayManager.writeConfig(v2rayConfig, port);
                if (configFile == null) continue;
                
                Process process = xrayManager.startProcess(configFile);
                if (process != null) {
                    v2rayProcesses.add(process);
                    activePorts.add(port);
                    started++;
                    Log.d(TAG, "Started V2Ray on port " + port);
                }
                
            } catch (Exception e) {
                Log.e(TAG, "Error starting V2Ray backend", e);
            }
        }
        
        return started;
    }
    
    private boolean setupTunInterface() {
        try {
            Builder builder = new Builder();
            builder.setSession("Iranian Free V2Ray");
            builder.setMtu(TUN_MTU);
            builder.addAddress(TUN_ADDRESS, TUN_PREFIX);
            builder.addDnsServer(TUN_DNS);
            builder.addDnsServer("8.8.8.8");
            
            // Route all traffic through VPN
            builder.addRoute("0.0.0.0", 0);
            builder.addRoute("::", 0);
            
            // Apply per-app settings
            applyPerAppSettings(builder);
            
            tunInterface = builder.establish();
            return tunInterface != null;
            
        } catch (Exception e) {
            Log.e(TAG, "Failed to setup TUN", e);
            return false;
        }
    }
    
    private void applyPerAppSettings(Builder builder) {
        SharedPreferences prefs = getSharedPreferences("vpn_settings", MODE_PRIVATE);
        boolean perAppEnabled = prefs.getBoolean("per_app_enabled", false);
        boolean isWhitelist = prefs.getBoolean("per_app_whitelist", true);
        Set<String> selectedApps = prefs.getStringSet("selected_apps", new HashSet<>());
        
        if (!perAppEnabled || selectedApps.isEmpty()) {
            return;
        }
        
        try {
            if (isWhitelist) {
                // Only selected apps use VPN
                for (String pkg : selectedApps) {
                    try {
                        builder.addAllowedApplication(pkg);
                    } catch (PackageManager.NameNotFoundException ignored) {}
                }
            } else {
                // All apps except selected use VPN
                for (String pkg : selectedApps) {
                    try {
                        builder.addDisallowedApplication(pkg);
                    } catch (PackageManager.NameNotFoundException ignored) {}
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Error applying per-app settings", e);
        }
    }
    
    private void startTrafficRouting() {
        // TODO: Implement tun2socks or similar to route TUN traffic through SOCKS proxies
        // For now, rely on V2Ray's transparent proxy mode
        executor.execute(() -> {
            while (isRunning.get() && tunInterface != null) {
                try {
                    // Health check on backends
                    checkBackendHealth();
                    Thread.sleep(30000);
                } catch (InterruptedException e) {
                    break;
                }
            }
        });
    }
    
    private void checkBackendHealth() {
        for (int i = activePorts.size() - 1; i >= 0; i--) {
            int port = activePorts.get(i);
            if (!isPortAlive(port)) {
                Log.w(TAG, "Backend on port " + port + " is dead");
                // Remove dead backend
                if (i < v2rayProcesses.size()) {
                    Process p = v2rayProcesses.remove(i);
                    if (p != null) p.destroyForcibly();
                }
                activePorts.remove(i);
            }
        }
        
        // Update notification with active count
        if (isRunning.get()) {
            updateNotification("متصل (" + activePorts.size() + " کانفیگ فعال)");
        }
    }
    
    private boolean isPortAlive(int port) {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress("127.0.0.1", port), 1000);
            return true;
        } catch (Exception e) {
            return false;
        }
    }
    
    public void stopVpn() {
        isRunning.set(false);
        
        // Stop all V2Ray processes
        for (Process p : v2rayProcesses) {
            try {
                p.destroyForcibly();
            } catch (Exception ignored) {}
        }
        v2rayProcesses.clear();
        activePorts.clear();
        
        // Close TUN interface
        if (tunInterface != null) {
            try {
                tunInterface.close();
            } catch (IOException ignored) {}
            tunInterface = null;
        }
        
        stopForeground(true);
        stopSelf();
        
        Log.i(TAG, "VPN stopped");
    }
    
    @Override
    public void onDestroy() {
        stopVpn();
        if (executor != null) {
            executor.shutdownNow();
        }
        instance = null;
        super.onDestroy();
    }
    
    @Override
    public void onRevoke() {
        stopVpn();
        super.onRevoke();
    }
    
    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    "VPN Service",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("فعال‌سازی سرویس VPN");
            channel.setShowBadge(false);
            
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null) {
                manager.createNotificationChannel(channel);
            }
        }
    }
    
    private Notification createNotification(String text) {
        Intent intent = new Intent(this, MainActivity.class);
        PendingIntent pendingIntent = PendingIntent.getActivity(this, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        
        Intent stopIntent = new Intent(this, VpnService.class);
        stopIntent.setAction(ACTION_STOP);
        PendingIntent stopPendingIntent = PendingIntent.getService(this, 1, stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Iranian Free V2Ray")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_lock_lock)
                .setContentIntent(pendingIntent)
                .addAction(android.R.drawable.ic_media_pause, "Disconnect", stopPendingIntent)
                .setOngoing(true)
                .build();
    }
    
    private void updateNotification(String text) {
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (manager != null) {
            manager.notify(NOTIFICATION_ID, createNotification(text));
        }
    }
}
