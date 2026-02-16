package com.hunter.app;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.net.VpnService.Builder;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import androidx.core.app.NotificationCompat;

import engine.Engine;
import engine.Key;
import go.Seq;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

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
    private static final String TUN_ADDRESS_V6 = "fd00:1:fd00:1::1";
    private static final int TUN_PREFIX_V6 = 128;
    private static final String TUN_DNS = "1.1.1.1";
    private static final int XRAY_SOCKS_PORT = 10808;

    private ParcelFileDescriptor tunInterface;
    private volatile int tunFd = -1;
    private ExecutorService executor;
    private final AtomicBoolean isRunning = new AtomicBoolean(false);
    private final AtomicBoolean stopInProgress = new AtomicBoolean(false);
    private volatile Process xrayProcess;
    private volatile File xrayConfigFile;

    private volatile Engine tun2socksEngine;
    private volatile Thread tun2socksThread;

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

        if (ACTION_START.equals(action) && !isRunning.get() && !stopInProgress.get()) {
            Notification notification = createNotification("Connecting...");
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIFICATION_ID, notification,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            } else {
                startForeground(NOTIFICATION_ID, notification);
            }
            executor.execute(this::startVpn);
        }

        return START_STICKY;
    }

    private void startVpn() {
        try {
            isRunning.set(true);

            // Check available memory to prevent crashes on low-RAM devices
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            if (mi.lowMemory) {
                Log.w(TAG, "Low memory detected, aborting VPN start");
                updateNotification("Low memory - cannot start VPN");
                stopVpn();
                return;
            }

            // Select a single best config for system-wide VPN.
            // Multi-backend for VPN requires in-core balancing; here we use app-side balancer.
            JSONObject selected = loadBestConfig();
            if (selected == null) {
                Log.w(TAG, "No valid configs found");
                updateNotification("No configs - Please refresh");
                stopVpn();
                return;
            }

            List<JSONObject> candidates = buildCandidateConfigs(selected, 20);
            if (candidates.isEmpty()) {
                updateNotification("No configs - Please refresh");
                stopVpn();
                return;
            }

            // Setup TUN interface
            if (!setupTunInterface()) {
                Log.e(TAG, "Failed to setup TUN interface");
                stopVpn();
                return;
            }

            boolean started = false;
            int attemptCount = 0;
            for (JSONObject cand : candidates) {
                attemptCount++;
                updateNotification("Testing config " + attemptCount + "/" + candidates.size() + "...");
                Log.i(TAG, "Attempting config " + attemptCount + "/" + candidates.size());
                
                stopXRayOnly();

                if (!startXRaySocksProcess(cand)) {
                    Log.w(TAG, "Config " + attemptCount + " failed to start XRay");
                    continue;
                }
                updateNotification("Config " + attemptCount + ": waiting for SOCKS...");
                if (!waitForPortAlive(XRAY_SOCKS_PORT, 4000)) {
                    Log.w(TAG, "Config " + attemptCount + " SOCKS port not alive");
                    continue;
                }

                updateNotification("Config " + attemptCount + ": verifying config...");
                if (!checkSocksDownloadOnce(6000)) {
                    Log.w(TAG, "Config " + attemptCount + " failed SOCKS download check");
                    stopXRayOnly();
                    continue;
                }

                updateNotification("Config " + attemptCount + ": starting tunnel...");
                if (!startTun2SocksBridge()) {
                    Log.w(TAG, "Config " + attemptCount + " failed to start tun2socks");
                    stopXRayOnly();
                    continue;
                }
                Log.i(TAG, "Config " + attemptCount + " connected successfully!");
                started = true;
                break;
            }

            if (!started) {
                Log.e(TAG, "Failed to start VPN engine with available configs");
                updateNotification("Connection failed - try refresh");
                stopVpn();
                return;
            }

            updateNotification("متصل");
            Log.i(TAG, "VPN started");

            // Start traffic routing
            startTrafficRouting();

            // Test connectivity to verify config health
            executor.execute(this::checkConnectivity);

        } catch (Exception e) {
            Log.e(TAG, "Failed to start VPN", e);
            stopVpn();
        }
    }

    private boolean checkSocksDownloadOnce(int timeoutMs) {
        Socket socket = null;
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", XRAY_SOCKS_PORT), timeoutMs);
            socket.setSoTimeout(timeoutMs);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting: VER=5, NMETHODS=1, METHOD=0 (no auth)
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) return false;

            // CONNECT to httpbin.org:80 (domain name)
            String host = "httpbin.org";
            byte[] hostBytes = host.getBytes(StandardCharsets.UTF_8);
            int port = 80;

            byte[] req = new byte[7 + hostBytes.length];
            req[0] = 0x05; // VER
            req[1] = 0x01; // CMD=CONNECT
            req[2] = 0x00; // RSV
            req[3] = 0x03; // ATYP=DOMAIN
            req[4] = (byte) hostBytes.length;
            System.arraycopy(hostBytes, 0, req, 5, hostBytes.length);
            req[5 + hostBytes.length] = (byte) ((port >> 8) & 0xFF);
            req[6 + hostBytes.length] = (byte) (port & 0xFF);
            os.write(req);
            os.flush();

            // Read reply: VER, REP, RSV, ATYP
            int rVer = is.read();
            int rep = is.read();
            is.read();
            int atyp = is.read();
            if (rVer != 0x05 || rep != 0x00) return false;

            // Skip BND.ADDR
            if (atyp == 0x01) { // IPv4
                readFully(is, new byte[4]);
            } else if (atyp == 0x04) { // IPv6
                readFully(is, new byte[16]);
            } else if (atyp == 0x03) { // DOMAIN
                int len = is.read();
                if (len < 0) return false;
                readFully(is, new byte[len]);
            } else {
                return false;
            }
            // Skip BND.PORT
            readFully(is, new byte[2]);

            // Send HTTP request through the tunnel
            String http = "GET /get HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" +
                    "Connection: close\r\n" +
                    "User-Agent: HunterVPN\r\n\r\n";
            os.write(http.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            byte[] buf = new byte[256];
            int n = is.read(buf);
            if (n <= 0) return false;
            String head = new String(buf, 0, n, StandardCharsets.US_ASCII);
            return head.startsWith("HTTP/");
        } catch (Exception e) {
            return false;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }

    private void readFully(InputStream is, byte[] buf) throws IOException {
        int off = 0;
        while (off < buf.length) {
            int n = is.read(buf, off, buf.length - off);
            if (n < 0) throw new IOException("EOF");
            off += n;
        }
    }

    private JSONObject loadBestConfig() {
        try {
            FreeV2RayApplication app = FreeV2RayApplication.getInstance();
            ConfigManager configManager = app.getConfigManager();
            ConfigBalancer balancer = app.getConfigBalancer();

            ConfigManager.ConfigItem best = balancer.getNextConfig();
            if (best == null || !isSupportedUri(best.uri)) {
                best = null;
                List<ConfigManager.ConfigItem> top = configManager.getTopConfigs();
                for (ConfigManager.ConfigItem item : top) {
                    if (item != null && isSupportedUri(item.uri)) {
                        best = item;
                        break;
                    }
                }
            }
            if (best == null) {
                List<ConfigManager.ConfigItem> all = configManager.getConfigs();
                for (ConfigManager.ConfigItem item : all) {
                    if (item != null && isSupportedUri(item.uri)) {
                        best = item;
                        break;
                    }
                }
            }
            if (best == null) return null;

            JSONObject obj = new JSONObject();
            obj.put("uri", best.uri);
            obj.put("ps", best.name);
            obj.put("latency_ms", best.latency);
            obj.put("protocol", best.protocol);
            Log.i(TAG, "Selected VPN config: " + best.protocol + " (" + best.latency + "ms)");
            return obj;
        } catch (Exception e) {
            Log.e(TAG, "Error loading configs", e);
        }
        return null;
    }

    private boolean isSupportedUri(String uri) {
        if (uri == null || uri.isEmpty()) return false;
        if (uri.startsWith("hy2://") || uri.startsWith("hysteria2://")) return false;
        return V2RayConfigHelper.parseUriToOutbound(uri) != null;
    }

    private boolean startXRaySocksProcess(JSONObject selectedConfig) {
        XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
        if (xrayManager == null || !xrayManager.isReady()) {
            Log.e(TAG, "XRay binary not ready");
            return false;
        }

        try {
            String uri = selectedConfig.getString("uri");
            Log.d(TAG, "Trying XRay config: " + uri);
            String jsonConfig = V2RayConfigHelper.generateFullConfig(uri, XRAY_SOCKS_PORT);
            if (jsonConfig == null) {
                Log.w(TAG, "Failed to generate config for: " + uri);
                return false;
            }

            File configFile = xrayManager.writeConfig(jsonConfig, XRAY_SOCKS_PORT);
            if (configFile == null) return false;

            xrayProcess = xrayManager.startProcess(configFile);
            if (xrayProcess == null) {
                configFile.delete();
                return false;
            }
            File old = xrayConfigFile;
            xrayConfigFile = configFile;
            if (old != null && old.exists() && !old.equals(configFile)) {
                old.delete();
            }
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to start XRay SOCKS", e);
            return false;
        }
    }

    private List<JSONObject> buildCandidateConfigs(JSONObject first, int max) {
        List<JSONObject> out = new ArrayList<>();
        HashSet<String> seen = new HashSet<>();
        try {
            if (first != null) {
                String uri = first.optString("uri", "");
                if (!uri.isEmpty() && isSupportedUri(uri)) {
                    out.add(first);
                    seen.add(uri);
                }
            }

            FreeV2RayApplication app = FreeV2RayApplication.getInstance();
            if (app == null) return out;
            ConfigManager configManager = app.getConfigManager();
            if (configManager == null) return out;

            List<ConfigManager.ConfigItem> top = configManager.getTopConfigs();
            for (ConfigManager.ConfigItem item : top) {
                if (out.size() >= max) break;
                if (item == null || item.uri == null) continue;
                if (seen.contains(item.uri)) continue;
                if (!isSupportedUri(item.uri)) continue;
                JSONObject obj = new JSONObject();
                obj.put("uri", item.uri);
                obj.put("ps", item.name);
                obj.put("latency_ms", item.latency);
                obj.put("protocol", item.protocol);
                out.add(obj);
                seen.add(item.uri);
            }

            if (out.size() < max) {
                List<ConfigManager.ConfigItem> all = configManager.getConfigs();
                for (ConfigManager.ConfigItem item : all) {
                    if (out.size() >= max) break;
                    if (item == null || item.uri == null) continue;
                    if (seen.contains(item.uri)) continue;
                    if (!isSupportedUri(item.uri)) continue;
                    JSONObject obj = new JSONObject();
                    obj.put("uri", item.uri);
                    obj.put("ps", item.name);
                    obj.put("latency_ms", item.latency);
                    obj.put("protocol", item.protocol);
                    out.add(obj);
                    seen.add(item.uri);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Error building candidate configs", e);
        }
        return out;
    }

    private boolean startTun2SocksBridge() {
        if (tunInterface == null || tunFd < 0) {
            Log.e(TAG, "TUN not ready");
            return false;
        }

        try {
            // Required by gomobile runtime.
            Seq.setContext(getApplicationContext());
            Engine.touch();
        } catch (Throwable e) {
            Log.e(TAG, "Failed to initialize gomobile", e);
            return false;
        }

        try {
            final Key key = new Key();
            key.setDevice("fd://" + tunFd);
            key.setProxy("socks5://127.0.0.1:" + XRAY_SOCKS_PORT);
            key.setMTU((long) TUN_MTU);
            key.setLogLevel("warning");
            key.setRestAPI("");
            key.setMark(0L);

            Log.i(TAG, "Starting tun2socks with fd=" + tunFd + ", proxy=socks5://127.0.0.1:" + XRAY_SOCKS_PORT);
            Engine.insert(key);
            tun2socksEngine = null; // Not used since we use global functions

            final java.util.concurrent.atomic.AtomicReference<Throwable> startError = new java.util.concurrent.atomic.AtomicReference<>(null);
            Thread t = new Thread(() -> {
                try {
                    Log.i(TAG, "tun2socks thread starting...");
                    Engine.start();
                    Log.w(TAG, "tun2socks Engine.start() returned normally");
                } catch (Throwable e) {
                    startError.set(e);
                    Log.e(TAG, "tun2socks crashed", e);
                }
            }, "tun2socks");
            t.setDaemon(false); // Don't make daemon so it keeps VPN alive
            tun2socksThread = t;
            t.start();

            try {
                Thread.sleep(250);
            } catch (InterruptedException e) {
                return false;
            }

            if (startError.get() != null) {
                return false;
            }

            updateNotification("Traffic engine started");
            Log.i(TAG, "tun2socks bridge started successfully");
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to start tun2socks", e);
            return false;
        }
    }

    private boolean waitForPortAlive(int port, int timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline && isRunning.get()) {
            try (Socket socket = new Socket()) {
                socket.connect(new InetSocketAddress("127.0.0.1", port), 500);
                return true;
            } catch (Exception ignored) {
                try {
                    Thread.sleep(80);
                } catch (InterruptedException e) {
                    return false;
                }
            }
        }
        return false;
    }

    private boolean setupTunInterface() {
        try {
            Builder builder = new Builder();
            builder.setSession("Iranian Free V2Ray");
            builder.setMtu(TUN_MTU);
            builder.addAddress(TUN_ADDRESS, TUN_PREFIX);
            try {
                builder.addAddress(TUN_ADDRESS_V6, TUN_PREFIX_V6);
            } catch (Exception ignored) {}
            builder.addDnsServer(TUN_DNS);
            builder.addDnsServer("8.8.8.8");

            // Route all traffic through VPN
            builder.addRoute("0.0.0.0", 0);
            try {
                builder.addRoute("::", 0);
            } catch (Exception ignored) {}

            // Apply per-app settings
            applyPerAppSettings(builder);

            if (Build.VERSION.SDK_INT >= 29) {
                builder.setBlocking(true);
            }

            tunInterface = builder.establish();
            if (tunInterface == null) return false;

            tunFd = tunInterface.getFd();

            return tunFd >= 0;

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

        try {
            if (perAppEnabled && isWhitelist && !selectedApps.isEmpty()) {
                for (String pkg : selectedApps) {
                    try {
                        if (getPackageName().equals(pkg)) continue;
                        builder.addAllowedApplication(pkg);
                    } catch (PackageManager.NameNotFoundException ignored) {}
                }
                return;
            }

            try {
                builder.addDisallowedApplication(getPackageName());
            } catch (PackageManager.NameNotFoundException ignored) {}

            if (perAppEnabled && !selectedApps.isEmpty()) {
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
        executor.execute(() -> {
            while (isRunning.get()) {
                try {
                    checkCoreHealth();
                    Thread.sleep(30000);
                } catch (InterruptedException e) {
                    break;
                }
            }
        });
    }

    private void checkCoreHealth() {
        Process p = xrayProcess;
        if (p == null) return;
        if (!p.isAlive()) {
            Log.w(TAG, "XRay process died");
            updateNotification("Disconnected - engine stopped");
            stopVpn();
        }

        // Check memory during operation
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        if (mi.lowMemory) {
            Log.w(TAG, "Low memory during VPN operation, stopping");
            updateNotification("Low memory - stopping VPN");
            stopVpn();
        }
    }

    private void checkConnectivity() {
        try {
            if (checkSocksDownloadOnce(8000)) {
                Log.i(TAG, "VPN SOCKS download test passed - config is healthy");
            } else {
                Log.w(TAG, "VPN SOCKS download test failed");
            }
        } catch (Exception e) {
            Log.w(TAG, "VPN SOCKS download test failed", e);
        }
    }

    public void stopVpn() {
        if (!stopInProgress.compareAndSet(false, true)) {
            return;
        }

        try {
            isRunning.set(false);

            stopTun2SocksOnly();
            stopXRayOnly();

            // Close TUN interface
            if (tunInterface != null) {
                try {
                    tunInterface.close();
                } catch (IOException ignored) {}
                tunInterface = null;
            }

            tunFd = -1;

            try {
                stopForeground(true);
            } catch (Exception ignored) {}
            try {
                stopSelf();
            } catch (Exception ignored) {}

            Log.i(TAG, "VPN stopped");
        } finally {
            stopInProgress.set(false);
        }
    }

    private void stopTun2SocksOnly() {
        Log.i(TAG, "Stopping tun2socks...");
        tun2socksEngine = null;

        Thread tt = tun2socksThread;
        tun2socksThread = null;
        if (tt == null) {
            Log.i(TAG, "tun2socks not running");
            return;
        }

        if (tt.isAlive()) {
            try {
                Engine.stop();
                Log.i(TAG, "Engine.stop() called");
            } catch (Exception e) {
                Log.w(TAG, "Engine.stop() failed", e);
            }
        } else {
            Log.w(TAG, "tun2socks thread already finished; skipping Engine.stop() to avoid crash");
        }

        try {
            if (Thread.currentThread() != tt) {
                tt.interrupt();
                tt.join(1000); // Wait up to 1s for thread to stop
                if (tt.isAlive()) {
                    Log.w(TAG, "tun2socks thread still alive after interrupt");
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Failed to stop tun2socks thread", e);
        }
        
        // Small delay to ensure cleanup
        try {
            Thread.sleep(100);
        } catch (InterruptedException ignored) {}
        
        Log.i(TAG, "tun2socks stopped");
    }

    private void stopXRayOnly() {
        Process p = xrayProcess;
        xrayProcess = null;
        if (p != null) {
            try {
                p.destroyForcibly();
            } catch (Exception ignored) {}
        }

        File cfg = xrayConfigFile;
        xrayConfigFile = null;
        if (cfg != null && cfg.exists()) {
            cfg.delete();
        }
    }

    @Override
    public void onDestroy() {
        if (isRunning.get() || tunInterface != null || xrayProcess != null || tun2socksEngine != null) {
            stopVpn();
        }
        if (executor != null) {
            executor.shutdownNow();
        }
        instance = null;
        super.onDestroy();
    }
    
    @Override
    public void onRevoke() {
        if (isRunning.get() || tunInterface != null || xrayProcess != null || tun2socksEngine != null) {
            stopVpn();
        }
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
