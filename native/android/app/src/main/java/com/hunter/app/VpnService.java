package com.hunter.app;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import androidx.localbroadcastmanager.content.LocalBroadcastManager;

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

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.Socket;
import java.net.SocketTimeoutException;
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
    private static final String TUN_DNS = "1.1.1.1"; // DNS forwarded through proxy via tun2socks+XRay sniffing
    private static final int XRAY_SOCKS_PORT = 10810;

    // Notification states
    private enum NotificationState {
        SEARCHING, CONNECTING, CONNECTED, DISCONNECTED
    }
    
    private NotificationState currentNotificationState = NotificationState.DISCONNECTED;

    private ParcelFileDescriptor tunInterface;
    private volatile int tunFd = -1;
    private ExecutorService executor;
    private final AtomicBoolean isRunning = new AtomicBoolean(false);
    private final AtomicBoolean stopInProgress = new AtomicBoolean(false);
    private volatile Process xrayProcess;
    private volatile File xrayConfigFile;
    private volatile Thread autoSwitchThread;
    private volatile JSONObject currentConfig;
    private volatile int currentTelegramLatency = Integer.MAX_VALUE;
    private volatile int currentInstagramLatency = Integer.MAX_VALUE;

    private ConfigManager configManager;

    private volatile Engine tun2socksEngine;
    private volatile Thread tun2socksThread;

    private static VpnService instance;

    private static volatile boolean gomobileInitialized = false;

    public static final String ACTION_START = "com.hunter.app.action.START";
    public static final String ACTION_STOP = "com.hunter.app.action.STOP";
    public static final String ACTION_EXIT = "com.hunter.app.action.EXIT";
    public static final String BROADCAST_STATUS = "com.hunter.app.VPN_STATUS";

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
        configManager = FreeV2RayApplication.getInstance().getConfigManager();
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
        if (ACTION_EXIT.equals(action)) {
            stopVpn();
            stopSelf();
            return START_NOT_STICKY;
        }

        if (ACTION_START.equals(action) && !isRunning.get() && !stopInProgress.get()) {
            try {
                Notification notification = createNotification("Searching for server...", NotificationState.SEARCHING);
                if (Build.VERSION.SDK_INT >= 34) {
                    startForeground(NOTIFICATION_ID, notification,
                            ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
                } else {
                    startForeground(NOTIFICATION_ID, notification);
                }
                executor.execute(this::startVpn);
            } catch (Throwable t) {
                Log.e(TAG, "startForeground failed", t);
                stopSelf();
                return START_NOT_STICKY;
            }
        }

        return START_STICKY;
    }

    private void startVpn() {
        isRunning.set(true);
        try {
            Log.i(TAG, "[DIAG] ===== VPN startVpn() BEGIN =====");
            Log.i(TAG, "[DIAG] Device: " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")");
            Log.i(TAG, "[DIAG] ABI: " + Build.SUPPORTED_ABIS[0]);

            // Check available memory to prevent crashes on low-RAM devices
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            Log.i(TAG, "[DIAG] Memory: avail=" + (mi.availMem / 1048576) + "MB, total=" + (mi.totalMem / 1048576) + "MB, low=" + mi.lowMemory);
            if (mi.lowMemory) {
                Log.w(TAG, "Low memory detected, aborting VPN start");
                updateNotification("Low memory - cannot start VPN");
                stopVpn();
                return;
            }

            // Get application components
            FreeV2RayApplication app = FreeV2RayApplication.getInstance();
            if (app == null) {
                Log.e(TAG, "[DIAG] FAIL: Application instance is null");
                stopVpn();
                return;
            }

            ConfigManager configManager = app.getConfigManager();
            ConfigTester configTester = app.getConfigTester();
            XRayManager xrayManager = app.getXRayManager();

            if (configManager == null || configTester == null) {
                Log.e(TAG, "[DIAG] FAIL: managers null - configManager=" + (configManager != null) + " configTester=" + (configTester != null));
                stopVpn();
                return;
            }

            // Log XRay binary status
            if (xrayManager != null) {
                Log.i(TAG, "[DIAG] XRay ready=" + xrayManager.isReady() + ", binary=" + xrayManager.getBinaryPath() + ", version=" + xrayManager.getVersion());
            } else {
                Log.w(TAG, "[DIAG] XRayManager is null!");
            }

            // Setup TUN interface first
            long tunStart = System.currentTimeMillis();
            if (!setupTunInterface()) {
                Log.e(TAG, "[DIAG] FAIL: TUN interface setup failed");
                stopVpn();
                return;
            }
            Log.i(TAG, "[DIAG] TUN interface OK (" + (System.currentTimeMillis() - tunStart) + "ms), fd=" + tunFd);

            // Try to use cached working configs first (from ConfigMonitor health checks)
            // This avoids re-testing and the "Test already running" lock conflict
            final int MAX_BALANCED = 5;
            List<ConfigManager.ConfigItem> topConfigs = configManager.getTopConfigs();
            
            if (topConfigs.size() >= 1) {
                // Use cached working configs directly - much faster than re-testing
                Log.i(TAG, "Using " + topConfigs.size() + " cached working configs (skipping re-test)");
                updateNotification("Connecting with cached configs...", NotificationState.CONNECTING);
                broadcastStatus("Connecting...", "Using " + topConfigs.size() + " cached configs", true);
                
                List<JSONObject> workingConfigs = new ArrayList<>();
                for (ConfigManager.ConfigItem config : topConfigs) {
                    if (workingConfigs.size() >= MAX_BALANCED) break;
                    if (config == null || config.uri == null) continue;
                    if (!isSupportedUri(config.uri)) {
                        continue;
                    }
                    try {
                        JSONObject cfg = new JSONObject();
                        cfg.put("uri", config.uri);
                        cfg.put("ps", config.name);
                        cfg.put("protocol", config.protocol);
                        cfg.put("latency_ms", config.latency);
                        workingConfigs.add(cfg);
                        Log.i(TAG, "Cached config #" + workingConfigs.size() + ": " + config.protocol + " (" + config.latency + "ms)");
                    } catch (Exception e) {
                        Log.w(TAG, "Failed to create config JSON from cache", e);
                    }
                }
                
                if (!workingConfigs.isEmpty()) {
                    startVpnWithBalancedConfigs(workingConfigs, configManager);
                    return;
                }
            }
            
            // No cached configs - fall back to full testing
            Log.i(TAG, "No cached working configs, starting full config test");
            List<ConfigManager.ConfigItem> candidates = loadCandidateConfigs(configManager);

            if (candidates.isEmpty()) {
                Log.w(TAG, "No candidate configs available");
                updateNotification("No configs available");
                stopVpn();
                return;
            }

            broadcastStatus("Searching...", "Testing " + candidates.size() + " configs...", true);
            updateNotification("Testing configs...", NotificationState.SEARCHING);

            // Stop any existing test (e.g. ConfigMonitor health check) - VPN startup takes priority
            if (configTester.isRunning()) {
                Log.i(TAG, "Stopping existing config test to prioritize VPN startup");
                configTester.stop();
                try { Thread.sleep(500); } catch (InterruptedException ignored) {}
            }

            final List<JSONObject> workingConfigs = new ArrayList<>();

            configTester.testConfigs(candidates, new ConfigTester.TestCallback() {
                @Override
                public void onProgress(int tested, int total) {
                    updateNotification("Testing: " + tested + "/" + total + " (" + workingConfigs.size() + " working)", NotificationState.SEARCHING);
                }

                @Override
                public void onConfigTested(ConfigManager.ConfigItem config, boolean success, int latency, boolean googleAccessible) {
                    if (success && workingConfigs.size() < MAX_BALANCED) {
                        try {
                            JSONObject cfg = new JSONObject();
                            cfg.put("uri", config.uri);
                            cfg.put("ps", config.name);
                            cfg.put("protocol", config.protocol);
                            cfg.put("latency_ms", latency);
                            synchronized (workingConfigs) {
                                if (workingConfigs.size() < MAX_BALANCED) {
                                    workingConfigs.add(cfg);
                                    Log.i(TAG, "Working config #" + workingConfigs.size() + ": " + config.protocol + " (" + latency + "ms)");
                                }
                            }
                        } catch (Exception e) {
                            Log.w(TAG, "Failed to create config JSON", e);
                        }
                    }
                }

                @Override
                public void onComplete(int totalTested, int successCount) {
                    Log.i(TAG, "Config testing complete: " + successCount + "/" + totalTested + " working configs found");

                    configTester.stopTesting();

                    if (!workingConfigs.isEmpty()) {
                        startVpnWithBalancedConfigs(workingConfigs, configManager);
                    } else {
                        Log.e(TAG, "No working configs found after testing " + totalTested);
                        updateNotification("No working configs found");
                        stopVpn();
                    }
                }
            });

        } catch (Throwable e) {
            Log.e(TAG, "Failed to start VPN", e);
            stopVpn();
        }
    }
    private List<ConfigManager.ConfigItem> loadCandidateConfigs(ConfigManager configManager) {
        List<ConfigManager.ConfigItem> candidates = new ArrayList<>();

        // First, add top working configs
        List<ConfigManager.ConfigItem> topConfigs = configManager.getTopConfigs();
        candidates.addAll(topConfigs);

        // Then add other configs if we need more
        if (candidates.size() < 50) {
            List<ConfigManager.ConfigItem> allConfigs = configManager.getConfigs();
            for (ConfigManager.ConfigItem item : allConfigs) {
                if (!candidates.contains(item)) {
                    candidates.add(item);
                    if (candidates.size() >= 100) break; // Limit to 100 configs max
                }
            }
        }

        // Limit to prevent testing too many configs
        if (candidates.size() > 100) {
            candidates = candidates.subList(0, 100);
        }

        Log.i(TAG, "Loaded " + candidates.size() + " candidate configs for testing");
        return candidates;
    }

    /**
     * Start VPN with multiple working configs using XRay built-in balancer.
     * Falls back to single config if balanced generation fails.
     */
    private void startVpnWithBalancedConfigs(List<JSONObject> workingConfigs, ConfigManager configManager) {
        try {
            long flowStart = System.currentTimeMillis();
            Log.i(TAG, "[DIAG] ===== Starting VPN with " + workingConfigs.size() + " configs =====");
            Log.i(TAG, "[DIAG] Device: " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")");
            Log.i(TAG, "[DIAG] ABI: " + Build.SUPPORTED_ABIS[0]);

            // Collect URIs for balanced config
            List<String> uris = new ArrayList<>();
            for (JSONObject cfg : workingConfigs) {
                String uri = cfg.optString("uri", "");
                if (!uri.isEmpty()) uris.add(uri);
                Log.i(TAG, "[DIAG] Config: " + cfg.optString("protocol", "?") + " latency=" + cfg.optInt("latency_ms", -1) + "ms");
            }

            // Step 1: Start XRay with balanced or single config
            long stepStart = System.currentTimeMillis();
            boolean started = false;
            if (uris.size() > 1) {
                started = startXRayBalancedProcess(uris);
                if (started) {
                    Log.i(TAG, "[DIAG] Step 1: XRay balanced start OK (" + (System.currentTimeMillis() - stepStart) + "ms)");
                } else {
                    Log.w(TAG, "[DIAG] Step 1: Balanced config failed, falling back to single config");
                }
            }

            // Fallback to single config
            if (!started) {
                for (int i = 0; i < workingConfigs.size(); i++) {
                    JSONObject candidate = workingConfigs.get(i);
                    started = startXRaySocksProcess(candidate);
                    if (started) {
                        Log.i(TAG, "[DIAG] Step 1: XRay single config #" + (i + 1) + " OK (" + (System.currentTimeMillis() - stepStart) + "ms)");
                        break;
                    }
                }
            }

            if (!started) {
                Log.e(TAG, "[DIAG] FAIL at Step 1: Could not start XRay with any config");
                updateNotification("Failed to start config");
                stopVpn();
                return;
            }

            // Step 2: Wait for SOCKS port to be ready
            stepStart = System.currentTimeMillis();
            boolean portAlive = waitForPortAlive(XRAY_SOCKS_PORT, 8000);
            if (!portAlive) {
                // Check if XRay process is still alive
                Process p = xrayProcess;
                boolean alive = p != null && p.isAlive();
                Log.e(TAG, "[DIAG] FAIL at Step 2: SOCKS port " + XRAY_SOCKS_PORT + " not alive after " + (System.currentTimeMillis() - stepStart) + "ms, xray alive=" + alive);
                updateNotification("Config start failed");
                stopVpn();
                return;
            }
            Log.i(TAG, "[DIAG] Step 2: SOCKS port alive (" + (System.currentTimeMillis() - stepStart) + "ms)");

            // Step 3: Start tun2socks bridge
            stepStart = System.currentTimeMillis();
            if (!startTun2SocksBridge()) {
                Log.e(TAG, "[DIAG] FAIL at Step 3: tun2socks bridge failed (" + (System.currentTimeMillis() - stepStart) + "ms)");
                updateNotification("Bridge start failed");
                stopVpn();
                return;
            }
            Log.i(TAG, "[DIAG] Step 3: tun2socks bridge started (" + (System.currentTimeMillis() - stepStart) + "ms)");

            // Step 4: Verify VPN routing
            stepStart = System.currentTimeMillis();
            if (!verifyVpnRouting()) {
                Log.e(TAG, "[DIAG] FAIL at Step 4: Routing verification failed (" + (System.currentTimeMillis() - stepStart) + "ms)");
                updateNotification("Routing failed");
                stopVpn();
                return;
            }
            Log.i(TAG, "[DIAG] Step 4: Routing verified (" + (System.currentTimeMillis() - stepStart) + "ms)");
            Log.i(TAG, "[DIAG] ===== VPN fully started in " + (System.currentTimeMillis() - flowStart) + "ms =====");

            Log.i(TAG, "VPN started successfully with " + uris.size() + " config(s)");

            // Cache all successful configs for prioritization
            for (JSONObject cfg : workingConfigs) {
                try {
                    configManager.addToTopConfigs(
                        cfg.getString("uri"),
                        cfg.optString("ps", "Server"),
                        cfg.optString("protocol", "Unknown"),
                        cfg.optInt("latency_ms", -1),
                        currentTelegramLatency,
                        currentInstagramLatency
                    );
                } catch (Exception e) {
                    Log.w(TAG, "Failed to cache config", e);
                }
            }

            currentConfig = workingConfigs.get(0);

            // Hide progress bar and show success
            Intent hideIntent = new Intent("com.hunter.app.V2RAY_PROGRESS");
            hideIntent.putExtra("show", false);
            LocalBroadcastManager.getInstance(this).sendBroadcast(hideIntent);

            startAutoSwitchThread();
            VpnState.setActive(true);
            String statusMsg = uris.size() > 1
                ? "Connected (" + uris.size() + " balanced)"
                : "Connected";
            updateNotification(statusMsg, NotificationState.CONNECTED);
            broadcastStatus("Connected", statusMsg, true);

            // Start traffic routing
            startTrafficRouting();

            // Test connectivity
            executor.execute(this::checkConnectivity);

        } catch (Exception e) {
            Log.e(TAG, "Failed to start VPN with balanced configs", e);
            stopVpn();
        }
    }

    private boolean checkSocksDownloadOnce(int timeoutMs) {
        Socket socket = null;
        boolean isValid = false;
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", XRAY_SOCKS_PORT), timeoutMs);
            socket.setSoTimeout(timeoutMs); // Increased for XHTTP connections

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting: VER=5, NMETHODS=1, METHOD=0 (no auth)
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) {
                Log.w(TAG, "SOCKS5 handshake failed: ver=" + ver + ", method=" + method);
                return false;
            }

            // CONNECT to cp.cloudflare.com:80 (less DPI-targeted than google.com)
            String host = "cp.cloudflare.com";
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
            if (rVer != 0x05 || rep != 0x00) {
                Log.w(TAG, "SOCKS5 connect failed: ver=" + rVer + ", rep=" + rep);
                return false;
            }

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
                Log.w(TAG, "Invalid ATYP: " + atyp);
                return false;
            }
            // Skip BND.PORT
            readFully(is, new byte[2]);

            // Send HTTP request through the tunnel
            String http = "GET /generate_204 HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" +
                    "Connection: close\r\n" +
                    "User-Agent: HunterVPN\r\n\r\n";
            os.write(http.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            byte[] buf = new byte[256];
            int n;
            try {
                n = is.read(buf);
            } catch (SocketTimeoutException timeout) {
                Log.w(TAG, "SOCKS download read timed out after successful CONNECT; assuming route is OK", timeout);
                return true;
            }
            if (n <= 0) {
                Log.w(TAG, "No response from HTTP request");
                return false;
            }
            String head = new String(buf, 0, n, StandardCharsets.US_ASCII);
            isValid = head.startsWith("HTTP/");

            if (!isValid) {
                Log.w(TAG, "Invalid HTTP response: " + head.substring(0, Math.min(50, head.length())));
            } else {
                // Check for proper HTTP status
                String[] lines = head.split("\r\n");
                if (lines.length > 0) {
                    String statusLine = lines[0];
                    Log.d(TAG, "HTTP status: " + statusLine);
                    // Should be "HTTP/1.1 204 No Content" or similar
                    if (!statusLine.contains("204") && !statusLine.contains("200")) {
                        Log.w(TAG, "Unexpected HTTP status: " + statusLine);
                    }
                }
            }
            
            return isValid;
        } catch (Exception e) {
            Log.w(TAG, "SOCKS download check failed", e);
            return false;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }

    /**
     * Test download speed through SOCKS proxy with fallback servers
     * Returns speed in KB/s, or -1 if failed
     */
    private int testDownloadSpeed() {
        // Try multiple speed test servers in order of reliability for Iranian networks
        String[] testServers = {
            "cdn.jsdelivr.net",        // JSDelivr CDN (accessible in Iran)
            "cdnjs.cloudflare.com",    // Cloudflare CDN (usually accessible)
            "unpkg.com",               // UNPKG CDN (works in Iran)
            "raw.githubusercontent.com" // GitHub raw (sometimes accessible)
        };
        
        for (String server : testServers) {
            int speed = testDownloadSpeedFromServer(server);
            if (speed > 0) {
                return speed;
            }
            Log.d(TAG, "Speed test failed for " + server + ", trying next server");
        }
        
        // Fallback to simple TCP speed test
        Log.d(TAG, "HTTP speed tests failed, trying simple TCP test");
        return testTcpSpeed();
    }
    
    /**
     * Simple TCP-based speed test that doesn't rely on HTTP headers
     */
    private int testTcpSpeed() {
        Socket socket = null;
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", XRAY_SOCKS_PORT), 8000);
            socket.setSoTimeout(20000);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) {
                return -1;
            }

            // SOCKS5 CONNECT to a simple server
            String host = "1.1.1.1";
            int port = 80;
            byte[] hostBytes = host.getBytes(StandardCharsets.UTF_8);
            
            os.write(new byte[]{0x05, 0x01, 0x00, 0x03});
            os.write(hostBytes.length);
            os.write(hostBytes);
            os.write(new byte[]{(byte)(port >> 8), (byte)port});
            os.flush();

            // Read SOCKS5 response
            byte[] response = new byte[10];
            int bytesRead = 0;
            while (bytesRead < 10) {
                int n = is.read(response, bytesRead, 10 - bytesRead);
                if (n <= 0) break;
                bytesRead += n;
            }

            if (response.length < 10 || response[0] != 0x05 || response[1] != 0x00) {
                return -1;
            }

            // Send a simple HTTP request
            String http = "GET / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
            os.write(http.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            // Measure how quickly we can read data
            byte[] buffer = new byte[4096];
            long downloaded = 0;
            long downloadStart = System.nanoTime();
            
            while (true) {
                int n = is.read(buffer);
                if (n <= 0) break;
                downloaded += n;
                
                // Stop after 64KB or 5 seconds
                long elapsed = (System.nanoTime() - downloadStart) / 1_000_000_000;
                if (downloaded >= 64 * 1024 || elapsed >= 5) break;
            }
            
            long downloadEnd = System.nanoTime();
            double elapsedSeconds = (downloadEnd - downloadStart) / 1_000_000_000.0;
            int speedKbps = (int) (downloaded / 1024.0 / elapsedSeconds);
            
            Log.d(TAG, "TCP speed test: " + downloaded + " bytes in " + 
                  String.format("%.2f", elapsedSeconds) + "s = " + speedKbps + " KB/s");
            
            return speedKbps;

        } catch (Exception e) {
            Log.w(TAG, "TCP speed test failed", e);
            return -1;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }
    
    private int testDownloadSpeedFromServer(String host) {
        Socket socket = null;
        try {
            long start = System.nanoTime();
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", XRAY_SOCKS_PORT), 8000);
            socket.setSoTimeout(25000);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) {
                Log.w(TAG, "SOCKS5 authentication failed for speed test");
                return -1;
            }

            // SOCKS5 CONNECT to speed test server
            int port = 80;
            byte[] hostBytes = host.getBytes(StandardCharsets.UTF_8);
            
            os.write(new byte[]{0x05, 0x01, 0x00, 0x03});
            os.write(hostBytes.length);
            os.write(hostBytes);
            os.write(new byte[]{(byte)(port >> 8), (byte)port});
            os.flush();

            // Read SOCKS5 response
            byte[] response = new byte[10];
            int bytesRead = 0;
            while (bytesRead < 10) {
                int n = is.read(response, bytesRead, 10 - bytesRead);
                if (n <= 0) break;
                bytesRead += n;
            }

            if (response.length < 10 || response[0] != 0x05 || response[1] != 0x00) {
                Log.w(TAG, "SOCKS5 connect failed for speed test to " + host);
                return -1;
            }

            // Download a small file to measure speed (different paths for different CDNs)
            String path;
            if (host.equals("cdn.jsdelivr.net")) {
                path = "/npm/lodash@4.17.21/lodash.min.js"; // Small JS file (~25KB)
            } else if (host.equals("cdnjs.cloudflare.com")) {
                path = "/ajax/libs/jquery/3.6.0/jquery.min.js"; // Small JS file (~30KB)
            } else if (host.equals("unpkg.com")) {
                path = "/lodash@4.17.21/lodash.min.js"; // Small JS file (~25KB)
            } else if (host.equals("raw.githubusercontent.com")) {
                path = "/cdnjs/underscore.js/1.13.6/underscore-min.js"; // Small JS file (~16KB)
            } else {
                path = "/"; // Fallback
            }
            String http = "GET " + path + " HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" +
                    "Connection: close\r\n" +
                    "User-Agent: HunterVPN/1.0\r\n\r\n";
            os.write(http.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            // Skip HTTP headers with timeout
            BufferedReader reader = new BufferedReader(new InputStreamReader(is));
            String line;
            long headerStart = System.nanoTime();
            while ((line = reader.readLine()) != null) {
                if (line.isEmpty()) break; // End of headers
                
                // Timeout after 8 seconds of header reading (Iranian networks are slow)
                long headerElapsed = (System.nanoTime() - headerStart) / 1_000_000_000;
                if (headerElapsed > 8) {
                    Log.w(TAG, "Header reading timeout for " + host);
                    return -1;
                }
            }

            // Measure download speed
            byte[] buffer = new byte[8192];
            long downloaded = 0;
            long downloadStart = System.nanoTime();
            
            while (true) {
                int n = is.read(buffer);
                if (n <= 0) break;
                downloaded += n;
                
                // Stop after 256KB or 12 seconds (smaller, faster for Iranian networks)
                long elapsed = (System.nanoTime() - downloadStart) / 1_000_000_000;
                if (downloaded >= 256 * 1024 || elapsed >= 12) break;
            }
            
            long downloadEnd = System.nanoTime();
            double elapsedSeconds = (downloadEnd - downloadStart) / 1_000_000_000.0;
            int speedKbps = (int) (downloaded / 1024.0 / elapsedSeconds);
            
            Log.d(TAG, "Speed test (" + host + "): " + downloaded + " bytes in " + 
                  String.format("%.2f", elapsedSeconds) + "s = " + speedKbps + " KB/s");
            
            return speedKbps;

        } catch (Exception e) {
            Log.w(TAG, "Speed test failed for " + host, e);
            return -1;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }

    private int testServiceLatency(String host, int port) {
        Socket socket = null;
        try {
            long start = System.nanoTime();
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", XRAY_SOCKS_PORT), 8000);
            socket.setSoTimeout(15000);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) {
                return Integer.MAX_VALUE;
            }

            // CONNECT
            byte[] hostBytes = host.getBytes(StandardCharsets.UTF_8);
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

            // Read reply
            int rVer = is.read();
            int rep = is.read();
            is.read(); // RSV
            int atyp = is.read();
            if (rVer != 0x05 || rep != 0x00) {
                return Integer.MAX_VALUE;
            }

            // Skip BND.ADDR
            if (atyp == 0x01) { // IPv4
                readFully(is, new byte[4]);
            } else if (atyp == 0x04) { // IPv6
                readFully(is, new byte[16]);
            } else if (atyp == 0x03) { // DOMAIN
                int len = is.read();
                if (len < 0) return Integer.MAX_VALUE;
                readFully(is, new byte[len]);
            } else {
                return Integer.MAX_VALUE;
            }
            // Skip BND.PORT
            readFully(is, new byte[2]);

            long end = System.nanoTime();
            return (int) ((end - start) / 1000000); // ms
        } catch (Exception e) {
            return Integer.MAX_VALUE;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }

    private boolean testTcpConnectivity(JSONObject config) {
        try {
            // Extract server address and port from config
            String host = null;
            int port = 443;

            Log.d(TAG, "TCP test - config structure: " + config.toString(2));

            // Handle simplified config format (uri, protocol, ps)
            if (config.has("uri") && config.has("protocol")) {
                String uri = config.optString("uri", "");
                String protocol = config.optString("protocol", "");

                Log.d(TAG, "TCP test - simplified config protocol: " + protocol);

                if ("Shadowsocks".equals(protocol) && uri.startsWith("ss://")) {
                    try {
                        // Parse Shadowsocks URI: ss://method:password@host:port#name
                        String ssPart = uri.substring(5); // Remove "ss://"
                        int atIndex = ssPart.indexOf('@');
                        if (atIndex > 0) {
                            String afterAt = ssPart.substring(atIndex + 1);
                            int colonIndex = afterAt.lastIndexOf(':');
                            if (colonIndex > 0) {
                                host = afterAt.substring(0, colonIndex);
                                try {
                                    port = Integer.parseInt(afterAt.substring(colonIndex + 1).split("#")[0]);
                                    Log.d(TAG, "TCP test - extracted from SS URI: host=" + host + ", port=" + port);
                                } catch (NumberFormatException e) {
                                    Log.w(TAG, "TCP test - invalid port in SS URI: " + afterAt);
                                    return false;
                                }
                            }
                        }
                    } catch (Exception e) {
                        Log.w(TAG, "TCP test - failed to parse SS URI: " + uri, e);
                        return false;
                    }
                } else if ("VMess".equals(protocol) && uri.startsWith("vmess://")) {
                    try {
                        // Parse VMess URI: vmess://base64-json
                        String base64Json = uri.substring(8); // Remove "vmess://"
                        byte[] decodedBytes = android.util.Base64.decode(base64Json, android.util.Base64.DEFAULT);
                        String jsonStr = new String(decodedBytes, java.nio.charset.StandardCharsets.UTF_8);
                        
                        org.json.JSONObject vmessJson = new org.json.JSONObject(jsonStr);
                        host = vmessJson.optString("add", null);
                        port = vmessJson.optInt("port", 443);
                        
                        Log.d(TAG, "TCP test - extracted from VMess URI: host=" + host + ", port=" + port);
                    } catch (Exception e) {
                        Log.w(TAG, "TCP test - failed to parse VMess URI: " + uri, e);
                        return false;
                    }
                } else if ("VLESS".equals(protocol) && uri.startsWith("vless://")) {
                    try {
                        // Parse VLESS URI: vless://uuid@host:port?params#name
                        String vlessPart = uri.substring(8); // Remove "vless://"
                        int atIndex = vlessPart.indexOf('@');
                        if (atIndex > 0) {
                            String afterAt = vlessPart.substring(atIndex + 1);
                            int questionIndex = afterAt.indexOf('?');
                            if (questionIndex < 0) questionIndex = afterAt.length();
                            String hostPort = afterAt.substring(0, questionIndex);
                            int colonIndex = hostPort.lastIndexOf(':');
                            if (colonIndex > 0) {
                                host = hostPort.substring(0, colonIndex);
                                try {
                                    port = Integer.parseInt(hostPort.substring(colonIndex + 1));
                                    Log.d(TAG, "TCP test - extracted from VLESS URI: host=" + host + ", port=" + port);
                                } catch (NumberFormatException e) {
                                    Log.w(TAG, "TCP test - invalid port in VLESS URI: " + hostPort);
                                    return false;
                                }
                            }
                        }
                    } catch (Exception e) {
                        Log.w(TAG, "TCP test - failed to parse VLESS URI: " + uri, e);
                        return false;
                    }
                } else {
                    Log.w(TAG, "TCP test - unsupported protocol in simplified config: " + protocol);
                    return false;
                }
            }
            // Handle XRay config format (outbounds)
            else if (config.has("outbounds")) {
                JSONArray outbounds = config.getJSONArray("outbounds");
                if (outbounds.length() > 0) {
                    JSONObject outbound = outbounds.getJSONObject(0);
                    String protocol = outbound.optString("protocol", "");
                    Log.d(TAG, "TCP test - protocol: " + protocol);
                    
                    if ("vmess".equals(protocol) || "vless".equals(protocol)) {
                        // VMess/VLESS: settings.vnext[0].address/port
                        if (outbound.has("settings")) {
                            JSONObject settings = outbound.getJSONObject("settings");
                            JSONArray vnext = null;
                            if (settings.has("vnext")) {
                                vnext = settings.getJSONArray("vnext");
                            } else if (settings.has("servers")) {
                                vnext = settings.getJSONArray("servers");
                            }
                            if (vnext != null && vnext.length() > 0) {
                                JSONObject server = vnext.getJSONObject(0);
                                host = server.optString("address", null);
                                port = server.optInt("port", 443);
                            }
                        }
                    } else if ("trojan".equals(protocol)) {
                        // Trojan: settings.servers[0].address/port
                        if (outbound.has("settings")) {
                            JSONObject settings = outbound.getJSONObject("settings");
                            JSONArray servers = settings.optJSONArray("servers");
                            if (servers != null && servers.length() > 0) {
                                JSONObject server = servers.getJSONObject(0);
                                host = server.optString("address", null);
                                port = server.optInt("port", 443);
                            }
                        }
                    } else if ("shadowsocks".equals(protocol)) {
                        // Shadowsocks: settings.servers[0].address/port
                        if (outbound.has("settings")) {
                            JSONObject settings = outbound.getJSONObject("settings");
                            Log.d(TAG, "TCP test - shadowsocks settings: " + settings.toString(2));
                            JSONArray servers = settings.optJSONArray("servers");
                            if (servers != null && servers.length() > 0) {
                                JSONObject server = servers.getJSONObject(0);
                                host = server.optString("address", null);
                                port = server.optInt("port", 443);
                                Log.d(TAG, "TCP test - extracted host: " + host + ", port: " + port);
                            } else {
                                Log.w(TAG, "TCP test - no servers array in shadowsocks settings");
                            }
                        } else {
                            Log.w(TAG, "TCP test - no settings in shadowsocks outbound");
                        }
                    } else {
                        Log.w(TAG, "TCP test - unsupported protocol: " + protocol);
                    }
                }
            }
            
            if (host == null || host.isEmpty()) {
                Log.w(TAG, "Could not extract host from config for TCP test");
                return false;
            }
            
            // Test TCP connection
            Socket socket = new Socket(new Proxy(Proxy.Type.SOCKS,
                    new InetSocketAddress("127.0.0.1", XRAY_SOCKS_PORT)));
            try {
                socket.connect(new InetSocketAddress(host, port), 8000);
                socket.setSoTimeout(10000);
                Log.i(TAG, "TCP connectivity test successful: " + host + ":" + port);
                return true;
            } finally {
                try { socket.close(); } catch (Exception ignored) {}
            }
        } catch (Exception e) {
            Log.w(TAG, "TCP connectivity test failed", e);
            return false;
        }
    }

    private boolean testServiceAccessibility() {
        Log.i(TAG, "Testing Telegram and Instagram accessibility and loading performance...");
        
        // Test Telegram API accessibility and loading speed
        int telegramLatency = testServiceLatencyWithLoading("api.telegram.org", 443, "GET /method/getMe");
        boolean telegramAccessible = telegramLatency < Integer.MAX_VALUE;
        
        // Test Instagram main site
        int instagramLatency = testServiceLatencyWithLoading("instagram.com", 443, "GET /");
        boolean instagramAccessible = instagramLatency < Integer.MAX_VALUE;
        
        // Test Instagram CDN (critical for images/videos)
        int instagramCdnLatency = testServiceLatencyWithLoading("cdninstagram.com", 443, "GET /");
        boolean instagramCdnAccessible = instagramCdnLatency < Integer.MAX_VALUE;
        
        // Test Telegram CDN (for media files)
        int telegramCdnLatency = testServiceLatencyWithLoading("cdn4-1.telegram.org", 443, "GET /");
        boolean telegramCdnAccessible = telegramCdnLatency < Integer.MAX_VALUE;
        
        Log.i(TAG, String.format("Service accessibility - Telegram API: %s (%dms), Telegram CDN: %s (%dms), Instagram: %s (%dms), Instagram CDN: %s (%dms)",
            telegramAccessible ? "✓" : "✗", telegramLatency,
            telegramCdnAccessible ? "✓" : "✗", telegramCdnLatency,
            instagramAccessible ? "✓" : "✗", instagramLatency, 
            instagramCdnAccessible ? "✓" : "✗", instagramCdnLatency));
        
        // Store latencies for display (use best CDN latency)
        currentTelegramLatency = Math.min(telegramLatency, telegramCdnLatency);
        currentInstagramLatency = Math.min(instagramLatency, instagramCdnLatency);
        
        // Consider config good if:
        // 1. Telegram API is accessible (most important for messaging)
        // 2. At least one Instagram service works (for social media)
        // 3. Loading times are reasonable (<12000ms for main services in Iranian conditions)
        boolean telegramGood = telegramAccessible && telegramLatency < 12000;
        boolean instagramGood = (instagramAccessible || instagramCdnAccessible) && 
                              Math.min(instagramLatency, instagramCdnLatency) < 12000;
        
        return telegramGood && instagramGood;
    }
    
    private int testServiceLatencyWithLoading(String host, int port, String request) {
        Socket socket = null;
        try {
            long start = System.nanoTime();
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", XRAY_SOCKS_PORT), 8000);
            socket.setSoTimeout(15000);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) {
                return Integer.MAX_VALUE;
            }

            // CONNECT to service
            byte[] hostBytes = host.getBytes(StandardCharsets.UTF_8);
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
            is.read(); // RSV
            int atyp = is.read();
            if (rVer != 0x05 || rep != 0x00) {
                return Integer.MAX_VALUE;
            }

            // Skip BND.ADDR
            if (atyp == 0x01) { // IPv4
                readFully(is, new byte[4]);
            } else if (atyp == 0x04) { // IPv6
                readFully(is, new byte[16]);
            } else if (atyp == 0x03) { // DOMAIN
                int len = is.read();
                if (len < 0) return Integer.MAX_VALUE;
                readFully(is, new byte[len]);
            } else {
                return Integer.MAX_VALUE;
            }
            // Skip BND.PORT
            readFully(is, new byte[2]);

            // Send HTTP request to test loading speed
            String http = request + " HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" +
                    "Connection: close\r\n" +
                    "User-Agent: Mozilla/5.0 (compatible; HunterVPN/1.0)\r\n\r\n";
            os.write(http.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            // Read response headers and some content to measure loading time
            byte[] buf = new byte[1024];
            int totalRead = 0;
            long contentStart = System.nanoTime();
            
            while (totalRead < 512) { // Read at least 512 bytes to measure loading speed
                int n;
                try {
                    n = is.read(buf, totalRead, buf.length - totalRead);
                } catch (SocketTimeoutException timeout) {
                    Log.w(TAG, host + " loading read timed out after CONNECT; using partial data", timeout);
                    break;
                }
                if (n <= 0) break;
                totalRead += n;
            }
            
            long end = System.nanoTime();
            int latencyMs = (int) ((end - start) / 1_000_000);
            
            // Check if we got a valid HTTP response
            if (totalRead > 0) {
                String response = new String(buf, 0, Math.min(100, totalRead), StandardCharsets.US_ASCII);
                if (response.startsWith("HTTP/")) {
                    Log.d(TAG, host + " loading test: " + latencyMs + "ms, " + totalRead + " bytes");
                    return latencyMs;
                }
            } else {
                // Handshake succeeded but no payload arrived before timeout. Consider service reachable.
                Log.w(TAG, host + " loading produced no bytes but CONNECT succeeded; assuming accessible");
                return latencyMs;
            }
            
            return Integer.MAX_VALUE;
        } catch (Exception e) {
            Log.w(TAG, "Service loading test failed for " + host, e);
            return Integer.MAX_VALUE;
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
            if (app == null) return null;
            ConfigManager configManager = app.getConfigManager();
            ConfigBalancer balancer = app.getConfigBalancer();
            if (configManager == null || balancer == null) return null;

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

    private boolean startXRayBalancedProcess(List<String> uris) {
        FreeV2RayApplication app = FreeV2RayApplication.getInstance();
        if (app == null) return false;
        XRayManager xrayManager = app.getXRayManager();
        if (xrayManager == null || !xrayManager.isReady()) {
            Log.e(TAG, "XRay binary not ready");
            return false;
        }

        try {
            String jsonConfig = V2RayConfigHelper.generateBalancedConfig(uris, XRAY_SOCKS_PORT);
            if (jsonConfig == null) {
                Log.w(TAG, "Failed to generate balanced config");
                return false;
            }

            Log.i(TAG, "Generated balanced config with " + uris.size() + " proxies");

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
            Log.e(TAG, "Failed to start XRay balanced", e);
            return false;
        }
    }

    private boolean startXRaySocksProcess(JSONObject selectedConfig) {
        FreeV2RayApplication app = FreeV2RayApplication.getInstance();
        if (app == null) return false;
        XRayManager xrayManager = app.getXRayManager();
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
        if (tunFd < 0) {
            Log.e(TAG, "No TUN fd available");
            return false;
        }

        try {
            // Required by gomobile runtime.
            if (!gomobileInitialized) {
                Seq.setContext(getApplicationContext());
                gomobileInitialized = true;
                Log.i(TAG, "gomobile runtime initialized successfully");
            } else {
                Log.i(TAG, "gomobile runtime already initialized, skipping");
            }
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
            key.setLogLevel("info"); // Changed to info for better debugging
            key.setRestAPI("");
            key.setMark(0L);

            Log.i(TAG, "Starting tun2socks with fd=" + tunFd + ", proxy=socks5://127.0.0.1:" + XRAY_SOCKS_PORT);
            
            // Verify the TUN fd is valid
            if (tunFd < 0) {
                Log.e(TAG, "Invalid TUN fd: " + tunFd);
                return false;
            }
            
            Engine.insert(key);
            
            // Engine.touch() is void, just call it to ensure initialization
            Engine.touch();
            Log.i(TAG, "tun2socks configuration inserted successfully");

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

            // Wait longer for startup and add debugging
            try {
                for (int i = 0; i < 10; i++) {
                    Thread.sleep(100);
                    if (startError.get() != null) {
                        Log.e(TAG, "tun2socks startup error detected early: " + startError.get().getMessage());
                        break;
                    }
                }
            } catch (InterruptedException e) {
                Log.w(TAG, "Interrupted while waiting for tun2socks startup");
                return false;
            }

            if (startError.get() != null) {
                return false;
            }

            updateNotification("Traffic engine started", NotificationState.CONNECTED);
            Log.i(TAG, "tun2socks bridge started successfully");
            return true;
        } catch (Throwable e) {
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
            
            // Set DNS servers - these go through tun2socks → XRay SOCKS proxy
            // XRay sniffing extracts real domain from TLS/HTTP and routes correctly
            builder.addDnsServer("1.1.1.1"); // Cloudflare DNS
            builder.addDnsServer("8.8.8.8"); // Google DNS
            builder.addDnsServer("9.9.9.9"); // Quad9 DNS

            // Route all traffic through VPN - this makes it the default VPN
            builder.addRoute("0.0.0.0", 0);  // Route all IPv4 traffic
            try {
                builder.addRoute("::", 0);     // Route all IPv6 traffic
            } catch (Exception ignored) {}
            
            // Add explicit routes for common services to ensure they go through VPN
            addExplicitRoutes(builder);

            // Apply per-app settings (exclude our app from VPN to avoid loops)
            applyPerAppSettings(builder);

            // Set blocking mode for better performance on Android 10+
            if (Build.VERSION.SDK_INT >= 29) {
                builder.setBlocking(true);
            }
            
            // Set underlying networks to ensure VPN priority
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                // On Android 10+, set underlying networks to ensure proper routing
                builder.setUnderlyingNetworks(null); // Use VPN as default
            }

            tunInterface = builder.establish();
            if (tunInterface == null) {
                Log.e(TAG, "Failed to establish TUN interface");
                return false;
            }

            // detachFd() transfers fd ownership away from ParcelFileDescriptor.
            // tun2socks will own and close the fd. This prevents fdsan SIGABRT
            // from double-close when stopVpn() calls tunInterface.close().
            tunFd = tunInterface.detachFd();
            Log.i(TAG, "TUN interface established successfully with fd=" + tunFd);
            
            if (tunFd >= 0) {
                Log.i(TAG, "VPN configured as default - all traffic will be routed through VPN");
                return true;
            } else {
                Log.e(TAG, "Invalid TUN file descriptor");
                return false;
            }

        } catch (Exception e) {
            Log.e(TAG, "Failed to setup TUN interface", e);
            return false;
        }
    }
    
    private void addExplicitRoutes(Builder builder) {
        // addRoute("0.0.0.0", 0) already routes ALL IPv4 traffic through VPN
        // No additional explicit routes needed
        Log.d(TAG, "All traffic routed through VPN via default route");
    }

    private void applyPerAppSettings(Builder builder) {
        SharedPreferences prefs = getSharedPreferences("vpn_settings", MODE_PRIVATE);
        boolean perAppEnabled = prefs.getBoolean("per_app_enabled", false);
        boolean isWhitelist = prefs.getBoolean("per_app_whitelist", true);
        Set<String> selectedApps = prefs.getStringSet("selected_apps", new HashSet<>());

        try {
            // Always exclude our own app from VPN to avoid routing loops
            try {
                builder.addDisallowedApplication(getPackageName());
                Log.d(TAG, "Excluded own app from VPN: " + getPackageName());
            } catch (PackageManager.NameNotFoundException e) {
                Log.w(TAG, "Could not exclude own app from VPN", e);
            }

            // If per-app mode is enabled
            if (perAppEnabled) {
                if (isWhitelist && !selectedApps.isEmpty()) {
                    // Whitelist mode: Only route selected apps through VPN
                    Log.d(TAG, "VPN whitelist mode - routing " + selectedApps.size() + " apps through VPN");
                    for (String pkg : selectedApps) {
                        try {
                            if (getPackageName().equals(pkg)) continue; // Skip our own app
                            builder.addAllowedApplication(pkg);
                            Log.d(TAG, "Added app to VPN whitelist: " + pkg);
                        } catch (PackageManager.NameNotFoundException e) {
                            Log.w(TAG, "App not found for whitelist: " + pkg, e);
                        }
                    }
                } else if (!isWhitelist && !selectedApps.isEmpty()) {
                    // Blacklist mode: Route all apps except selected ones through VPN
                    Log.d(TAG, "VPN blacklist mode - excluding " + selectedApps.size() + " apps from VPN");
                    for (String pkg : selectedApps) {
                        try {
                            if (getPackageName().equals(pkg)) continue; // Skip our own app
                            builder.addDisallowedApplication(pkg);
                            Log.d(TAG, "Added app to VPN blacklist: " + pkg);
                        } catch (PackageManager.NameNotFoundException e) {
                            Log.w(TAG, "App not found for blacklist: " + pkg, e);
                        }
                    }
                } else {
                    // Default: Route all apps through VPN (except our own app)
                    Log.d(TAG, "VPN default mode - routing all apps through VPN");
                }
            } else {
                // Per-app mode disabled: Route all apps through VPN (except our own app)
                Log.d(TAG, "VPN per-app mode disabled - routing all apps through VPN");
            }
            
            // Add system apps that should bypass VPN for better performance
            addSystemAppExclusions(builder);
            
        } catch (Exception e) {
            Log.e(TAG, "Error applying per-app settings", e);
        }
    }
    
    private void addSystemAppExclusions(Builder builder) {
        try {
            // Only exclude system services that could cause routing loops.
            // Do NOT exclude user-facing apps (Chrome, YouTube, etc.) — users
            // expect these to work through VPN on censored networks.
            String[] systemAppsToExclude = {
                "com.android.systemui",      // System UI
                "com.google.android.gms",    // Google Play Services
                "com.google.android.gsf",    // Google Services Framework
            };
            
            for (String pkg : systemAppsToExclude) {
                try {
                    builder.addDisallowedApplication(pkg);
                    Log.d(TAG, "Excluded system app from VPN: " + pkg);
                } catch (PackageManager.NameNotFoundException e) {
                    // App not installed, ignore
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Error adding system app exclusions", e);
        }
    }

    private boolean verifyVpnRouting() {
        try {
            Log.i(TAG, "Verifying VPN routing configuration...");
            
            // Check if TUN interface is properly configured
            if (tunInterface == null || tunFd < 0) {
                Log.e(TAG, "TUN interface not available for routing verification");
                return false;
            }
            
            // On Iranian censored networks, full HTTP verification may fail due to
            // DPI/throttling even though the proxy tunnel is working fine.
            // Strategy: try full HTTP test first, fall back to SOCKS-connect-only test.
            
            // Attempt 1: Full HTTP test (8s)
            boolean basicConnectivity = checkSocksDownloadOnce(8000);
            if (basicConnectivity) {
                Log.i(TAG, "VPN routing verification successful - full HTTP test passed");
                return true;
            }
            
            // Attempt 2: Retry with longer timeout (15s) for slow Iranian networks
            Log.w(TAG, "First connectivity test failed, retrying with longer timeout...");
            try { Thread.sleep(1500); } catch (InterruptedException ignored) {}
            basicConnectivity = checkSocksDownloadOnce(15000);
            if (basicConnectivity) {
                Log.i(TAG, "VPN routing verification successful on retry");
                return true;
            }
            
            // Attempt 3: Just verify SOCKS5 CONNECT succeeds (proxy is routing traffic)
            // Even if HTTP response is slow, a successful SOCKS5 CONNECT proves the tunnel works
            Log.w(TAG, "HTTP test failed, trying SOCKS-connect-only verification...");
            try { Thread.sleep(1000); } catch (InterruptedException ignored) {}
            boolean connectOnly = checkSocksConnectOnly(10000);
            if (connectOnly) {
                Log.i(TAG, "VPN routing verified via SOCKS CONNECT (HTTP was slow but tunnel works)");
                return true;
            }
            
            Log.w(TAG, "VPN routing verification failed - SOCKS proxy not responding");
            return false;
            
        } catch (Exception e) {
            Log.e(TAG, "Error during VPN routing verification", e);
            return false;
        }
    }
    
    /**
     * Lightweight SOCKS5 CONNECT test — just verifies the proxy can establish a connection.
     * Doesn't wait for HTTP response, which may be slow on Iranian censored networks.
     */
    private boolean checkSocksConnectOnly(int timeoutMs) {
        Socket socket = null;
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", XRAY_SOCKS_PORT), timeoutMs);
            socket.setSoTimeout(timeoutMs);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) {
                Log.w(TAG, "SOCKS5 handshake failed in connect-only test");
                return false;
            }

            // SOCKS5 CONNECT to cp.cloudflare.com:80
            String host = "cp.cloudflare.com";
            byte[] hostBytes = host.getBytes(StandardCharsets.UTF_8);
            int port = 80;
            byte[] req = new byte[7 + hostBytes.length];
            req[0] = 0x05;
            req[1] = 0x01;
            req[2] = 0x00;
            req[3] = 0x03;
            req[4] = (byte) hostBytes.length;
            System.arraycopy(hostBytes, 0, req, 5, hostBytes.length);
            req[5 + hostBytes.length] = (byte) ((port >> 8) & 0xFF);
            req[6 + hostBytes.length] = (byte) (port & 0xFF);
            os.write(req);
            os.flush();

            // Read SOCKS5 reply — if REP=0x00, proxy successfully connected
            int rVer = is.read();
            int rep = is.read();
            if (rVer == 0x05 && rep == 0x00) {
                Log.i(TAG, "SOCKS5 CONNECT succeeded — proxy tunnel is working");
                return true;
            } else {
                Log.w(TAG, "SOCKS5 CONNECT failed: rep=" + rep);
                return false;
            }
        } catch (Exception e) {
            Log.w(TAG, "SOCKS connect-only test failed: " + e.getMessage());
            return false;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
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
            updateNotification("Disconnected - engine stopped", NotificationState.DISCONNECTED);
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
            if (checkSocksDownloadOnce(10000)) {
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
            VpnState.setActive(false);

            // Interrupt auto-switch thread
            if (autoSwitchThread != null) {
                autoSwitchThread.interrupt();
                autoSwitchThread = null;
            }
            currentConfig = null;
            currentTelegramLatency = Integer.MAX_VALUE;
            currentInstagramLatency = Integer.MAX_VALUE;

            stopTun2SocksOnly();
            stopXRayOnly();

            // TUN fd was detached via detachFd() and given to tun2socks.
            // Engine.stop() already closed the fd, so we just reset state.
            // tunInterface.close() is a safe no-op since fd was detached.
            tunFd = -1;
            if (tunInterface != null) {
                try {
                    tunInterface.close();
                } catch (Exception ignored) {}
                tunInterface = null;
            }

            try {
                stopForeground(true);
            } catch (Exception ignored) {}
            // Don't call stopSelf() - keep service alive for quick reconnection
            // User must use explicit "Exit" action to fully kill the service

            // Broadcast state change to UI
            broadcastStatus("Disconnected", "Tap to connect", false);

            Log.i(TAG, "VPN stopped");
        } finally {
            VpnState.setActive(false);
            stopInProgress.set(false);
        }
    }

    private void broadcastStatus(String status, String detail, boolean isConnecting) {
        try {
            Intent intent = new Intent(BROADCAST_STATUS);
            intent.putExtra("status", status);
            intent.putExtra("detail", detail);
            intent.putExtra("isConnecting", isConnecting);
            intent.putExtra("isConnected", VpnState.isActive());
            LocalBroadcastManager.getInstance(this).sendBroadcast(intent);
        } catch (Exception ignored) {}
    }

    private void stopTun2SocksOnly() {
        Log.i(TAG, "Stopping tun2socks...");
        
        // Stop the engine using global Engine.stop()
        try {
            Engine.stop();
            Log.i(TAG, "Engine.stop() called");
        } catch (Throwable e) {
            Log.w(TAG, "Engine.stop() failed", e);
        }

        Thread tt = tun2socksThread;
        tun2socksThread = null;
        if (tt == null) {
            Log.i(TAG, "tun2socks not running");
            return;
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
            channel.setDescription("VPN service activation");
            channel.setShowBadge(false);
            
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null) {
                manager.createNotificationChannel(channel);
            }
        }
    }
    
    private Notification createNotification(String text) {
        return createNotification(text, currentNotificationState);
    }
    
    private Notification createNotification(String text, NotificationState state) {
        Intent intent = new Intent(this, MainActivity.class);
        PendingIntent pendingIntent = PendingIntent.getActivity(this, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        
        Intent stopIntent = new Intent(this, VpnService.class);
        stopIntent.setAction(ACTION_STOP);
        PendingIntent stopPendingIntent = PendingIntent.getService(this, 1, stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        
        // Choose icon based on state
        int icon;
        switch (state) {
            case SEARCHING:
                icon = R.drawable.ic_searching;
                break;
            case CONNECTING:
                icon = android.R.drawable.ic_dialog_info;
                break;
            case CONNECTED:
                icon = android.R.drawable.ic_lock_lock;
                break;
            case DISCONNECTED:
            default:
                icon = android.R.drawable.ic_dialog_alert;
                break;
        }
        
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Iranian Free V2Ray")
                .setContentText(text)
                .setSmallIcon(icon)
                .setContentIntent(pendingIntent)
                .addAction(android.R.drawable.ic_media_pause, "Disconnect", stopPendingIntent)
                .setOngoing(true)
                .build();
    }
    
    private void updateNotification(String text) {
        updateNotification(text, currentNotificationState);
    }
    
    private void updateNotification(String text, NotificationState state) {
        currentNotificationState = state;
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (manager != null) {
            manager.notify(NOTIFICATION_ID, createNotification(text, state));
        }
    }

    private void startAutoSwitchThread() {
        autoSwitchThread = new Thread(() -> {
            while (isRunning.get() && !Thread.currentThread().isInterrupted()) {
                try {
                    Thread.sleep(5 * 60 * 1000); // Check every 5 minutes
                    JSONObject betterConfig = findBetterConfig();
                    if (betterConfig != null) {
                        Log.i(TAG, "Found better config, switching...");
                        switchToConfig(betterConfig);
                    }
                } catch (InterruptedException e) {
                    Log.i(TAG, "Auto-switch thread interrupted");
                    break;
                } catch (Exception e) {
                    Log.w(TAG, "Error in auto-switch thread", e);
                }
            }
        });
        autoSwitchThread.setDaemon(true);
        autoSwitchThread.start();
    }

    private JSONObject findBetterConfig() {
        try {
            List<ConfigManager.ConfigItem> topConfigs = configManager.getTopConfigs();
            if (topConfigs.isEmpty()) return null;

            ConfigManager.ConfigItem best = topConfigs.get(0);
            if (currentConfig == null) return null;

            // Calculate combined scores (latency + telegram + instagram)
            long currentScore = (long) currentConfig.optInt("latency_ms", Integer.MAX_VALUE) + currentTelegramLatency + currentInstagramLatency;
            long bestScore = (long) best.latency + best.telegram_latency + best.instagram_latency;

            // If best config has significantly better combined score (at least 20% better), switch
            if (bestScore > 0 && bestScore < currentScore * 0.8) {
                JSONObject better = new JSONObject();
                better.put("uri", best.uri);
                better.put("ps", best.name);
                better.put("protocol", best.protocol);
                better.put("latency_ms", best.latency);
                return better;
            }
        } catch (Exception e) {
            Log.w(TAG, "Error finding better config", e);
        }
        return null;
    }

    private void switchToConfig(JSONObject newConfig) {
        try {
            // Stop current XRay
            stopXRayOnly();

            // Update current config
            currentConfig = newConfig;

            // Start new XRay with new config
            if (startXRaySocksProcess(newConfig)) {
                Log.i(TAG, "Switched to new config successfully");
                updateNotification("Switched to better server", NotificationState.CONNECTED);
            } else {
                Log.w(TAG, "Failed to start new config after switch");
            }
        } catch (Exception e) {
            Log.e(TAG, "Error switching config", e);
        }
    }

}

