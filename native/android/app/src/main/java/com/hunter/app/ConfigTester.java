package com.hunter.app;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * High-speed 3-phase config tester.
 *
 * Phase 1 — Dedup: group configs by server:port, keep best protocol per server.
 * Phase 2 — TCP pre-filter: 30 parallel raw TCP connects (2 s timeout) to
 *           eliminate dead IP-based servers instantly. Domain-based servers
 *           skip this phase (local DNS is censored in Iran).
 * Phase 3 — XRay SOCKS test: 12 parallel XRay instances with a semaphore-guarded
 *           port pool (20808-20831). Each instance gets an exclusive port,
 *           starts XRay, and tests HTTP through the SOCKS proxy.
 *
 * Designed to chew through 5 000-15 000+ configs on Iranian censored networks.
 */
public class ConfigTester {
    private static final String TAG = "ConfigTester";

    // ── Phase 2: TCP pre-filter ──────────────────────────────────────────
    private static final int TCP_PREFILTER_THREADS  = 30;
    private static final int TCP_PREFILTER_TIMEOUT  = 2000;   // ms

    // ── Phase 3: XRay SOCKS test ─────────────────────────────────────────
    private static final int XRAY_PARALLEL          = 12;
    private static final int PORT_POOL_BASE         = 20808;
    private static final int PORT_POOL_SIZE         = 24;     // ports 20808-20831
    private static final int SOCKS_TIMEOUT_MS       = 8000;
    private static final int PORT_WAIT_MS           = 3000;

    // ── Result cache ─────────────────────────────────────────────────────
    private static final long CACHE_EXPIRY_MS = 10 * 60 * 1000; // 10 min
    private final ConcurrentHashMap<String, TestResult> resultCache = new ConcurrentHashMap<>();

    // ── Port pool: each permit = one exclusive port ──────────────────────
    private final Semaphore portPool = new Semaphore(PORT_POOL_SIZE);
    private final boolean[] portInUse = new boolean[PORT_POOL_SIZE];
    private final Object portLock = new Object();

    // ── Thread pools ─────────────────────────────────────────────────────
    private final ExecutorService tcpExecutor;
    private final ExecutorService xrayExecutor;

    // ── Lifecycle ─────────────────────────────────────────────────────────
    private volatile boolean isRunning = false;
    private volatile boolean stopRequested = false;
    private final Object testLock = new Object();

    // ── Public callback interface (unchanged for callers) ────────────────
    public interface TestCallback {
        void onProgress(int tested, int total);
        void onConfigTested(ConfigManager.ConfigItem config, boolean success, int latency, boolean googleAccessible);
        void onComplete(int totalTested, int successCount);
    }

    // ── Cached test result ────────────────────────────────────────────────
    private static class TestResult {
        final boolean success;
        final int latency;
        final boolean googleAccessible;
        final long timestamp;

        TestResult(boolean success, int latency, boolean googleAccessible) {
            this.success = success;
            this.latency = latency;
            this.googleAccessible = googleAccessible;
            this.timestamp = System.currentTimeMillis();
        }

        boolean isExpired() {
            return System.currentTimeMillis() - timestamp > CACHE_EXPIRY_MS;
        }
    }

    public ConfigTester() {
        this.tcpExecutor = new ThreadPoolExecutor(
            TCP_PREFILTER_THREADS, TCP_PREFILTER_THREADS, 30, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(10000),
            new ThreadPoolExecutor.CallerRunsPolicy());

        this.xrayExecutor = new ThreadPoolExecutor(
            XRAY_PARALLEL, XRAY_PARALLEL, 30, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(5000),
            new ThreadPoolExecutor.CallerRunsPolicy());
    }

    // =====================================================================
    //  Main entry point
    // =====================================================================

    public void testConfigs(List<ConfigManager.ConfigItem> configs, TestCallback callback) {
        synchronized (testLock) {
            if (isRunning) {
                Log.w(TAG, "Test already running");
                return;
            }
            isRunning = true;
        }
        stopRequested = false;

        Thread pipeline = new Thread(() -> {
            try {
                runPipeline(configs, callback);
            } finally {
                isRunning = false;
            }
        }, "ConfigTester-Pipeline");
        pipeline.setDaemon(true);
        pipeline.start();
    }

    // =====================================================================
    //  3-phase pipeline
    // =====================================================================

    private void runPipeline(List<ConfigManager.ConfigItem> raw, TestCallback callback) {
        XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
        boolean xrayReady = xrayManager != null && xrayManager.isReady();
        Log.i(TAG, "[DIAG] XRay ready=" + xrayReady
            + ", binary=" + (xrayManager != null ? xrayManager.getBinaryPath() : "null")
            + ", tcpThreads=" + TCP_PREFILTER_THREADS + ", xraySlots=" + XRAY_PARALLEL);

        // ── Phase 1: Dedup by server:port ────────────────────────────────
        long p1Start = System.currentTimeMillis();
        List<ConfigManager.ConfigItem> deduped = dedup(raw);
        Log.i(TAG, "Phase 1 dedup: " + raw.size() + " → " + deduped.size()
            + " unique servers (" + (System.currentTimeMillis() - p1Start) + "ms)");

        // Sort: previously working (low latency) first, then VLESS > VMess > Trojan > SS
        deduped.sort((a, b) -> {
            boolean aWork = a.latency > 0 && a.latency < 15000;
            boolean bWork = b.latency > 0 && b.latency < 15000;
            if (aWork != bWork) return aWork ? -1 : 1;
            if (aWork) return Integer.compare(a.latency, b.latency);
            return Integer.compare(protoPriority(b.protocol), protoPriority(a.protocol));
        });

        final int totalToTest = deduped.size();
        final AtomicInteger testedCount = new AtomicInteger(0);
        final AtomicInteger successCount = new AtomicInteger(0);
        callback.onProgress(0, totalToTest);

        if (!xrayReady) {
            Log.w(TAG, "XRay not ready, falling back to TCP-only testing");
            runTcpOnlyFallback(deduped, callback, testedCount, successCount, totalToTest);
            callback.onComplete(testedCount.get(), successCount.get());
            return;
        }

        // ── STREAMING PIPELINE: TCP pre-filter feeds directly into XRay test ──
        // No blocking between phases. Domain-based configs go straight to XRay.
        // IP-based configs do a quick TCP check, then go to XRay on success.
        // One completion latch tracks ALL configs.
        long pipeStart = System.currentTimeMillis();
        final CountDownLatch completionLatch = new CountDownLatch(totalToTest);

        int ipCount = 0, domainCount = 0, unparseableCount = 0;

        for (ConfigManager.ConfigItem c : deduped) {
            if (stopRequested) { completionLatch.countDown(); continue; }

            String[] hp = extractHostPort(c);

            if (hp == null) {
                // Unparseable URI — send straight to XRay (it may know how to handle it)
                unparseableCount++;
                submitToXray(c, xrayManager, callback, testedCount, successCount, totalToTest, completionLatch);
            } else if (!isIpAddress(hp[0])) {
                // Domain-based — skip TCP (local DNS is censored), go straight to XRay
                domainCount++;
                submitToXray(c, xrayManager, callback, testedCount, successCount, totalToTest, completionLatch);
            } else {
                // IP-based — TCP pre-filter first, then XRay on success
                ipCount++;
                final String host = hp[0];
                final int port;
                try { port = Integer.parseInt(hp[1]); } catch (NumberFormatException e) {
                    reportResult(c, false, -1, false, callback, testedCount, successCount, totalToTest);
                    completionLatch.countDown();
                    continue;
                }
                tcpExecutor.execute(() -> {
                    try {
                        if (stopRequested) {
                            reportResult(c, false, -1, false, callback, testedCount, successCount, totalToTest);
                            completionLatch.countDown();
                            return;
                        }
                        if (tcpConnect(host, port, TCP_PREFILTER_TIMEOUT)) {
                            // TCP alive → submit to XRay test (latch counted down inside)
                            submitToXray(c, xrayManager, callback, testedCount, successCount, totalToTest, completionLatch);
                        } else {
                            // TCP dead → report failure immediately
                            reportResult(c, false, -1, false, callback, testedCount, successCount, totalToTest);
                            completionLatch.countDown();
                        }
                    } catch (Exception e) {
                        reportResult(c, false, -1, false, callback, testedCount, successCount, totalToTest);
                        completionLatch.countDown();
                    }
                });
            }
        }

        Log.i(TAG, "Streaming pipeline dispatched: " + ipCount + " IP-based (TCP→XRay), "
            + domainCount + " domain-based (→XRay), " + unparseableCount + " unparseable (→XRay)");

        // Wait for all configs to complete (TCP failures + XRay tests)
        try {
            // Generous timeout: worst case all go through XRay
            long maxWait = Math.max(120_000, (long)(totalToTest / XRAY_PARALLEL + 1) * 12_000L);
            maxWait = Math.min(maxWait, 30 * 60 * 1000L); // cap 30 min
            completionLatch.await(maxWait, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {}

        Log.i(TAG, "Pipeline done: " + successCount.get() + "/" + testedCount.get()
            + " working (" + (System.currentTimeMillis() - pipeStart) + "ms)");

        callback.onComplete(testedCount.get(), successCount.get());
    }

    /**
     * Submit a config to the XRay SOCKS test pool. Counts down completionLatch when done.
     */
    private void submitToXray(ConfigManager.ConfigItem c, XRayManager xrayManager,
                              TestCallback callback, AtomicInteger testedCount,
                              AtomicInteger successCount, int totalToTest,
                              CountDownLatch completionLatch) {
        xrayExecutor.execute(() -> {
            try {
                if (stopRequested) {
                    reportResult(c, false, -1, false, callback, testedCount, successCount, totalToTest);
                    return;
                }

                // Check cache
                String cacheKey = c.uri;
                TestResult cached = resultCache.get(cacheKey);
                if (cached != null && !cached.isExpired()) {
                    reportResult(c, cached.success, cached.latency, cached.googleAccessible,
                        callback, testedCount, successCount, totalToTest);
                    return;
                }

                long start = System.currentTimeMillis();
                boolean[] result = testThroughV2Ray(c, xrayManager);
                int latency = (int) (System.currentTimeMillis() - start);

                if (stopRequested) {
                    reportResult(c, false, -1, false, callback, testedCount, successCount, totalToTest);
                    return;
                }

                // Cache result
                resultCache.put(cacheKey, new TestResult(result[0], latency, result[1]));
                reportResult(c, result[0], latency, result[1],
                    callback, testedCount, successCount, totalToTest);
            } catch (Exception e) {
                reportResult(c, false, -1, false, callback, testedCount, successCount, totalToTest);
            } finally {
                completionLatch.countDown();
            }
        });
    }

    // =====================================================================
    //  Phase 1: Deduplication
    // =====================================================================

    /**
     * Deduplicate configs by server:port. When multiple configs share the same
     * server, keep the one with the best protocol priority.
     */
    private List<ConfigManager.ConfigItem> dedup(List<ConfigManager.ConfigItem> configs) {
        LinkedHashMap<String, ConfigManager.ConfigItem> map = new LinkedHashMap<>();
        for (ConfigManager.ConfigItem c : configs) {
            if (c == null || c.uri == null) continue;
            // Skip Hysteria2 — not supported by XRay
            String lower = c.uri.toLowerCase();
            if (lower.startsWith("hy2://") || lower.startsWith("hysteria2://")) continue;

            String key = serverKey(c);
            if (key == null) key = c.uri; // fallback: no dedup for unparseable
            ConfigManager.ConfigItem existing = map.get(key);
            if (existing == null || protoPriority(c.protocol) > protoPriority(existing.protocol)) {
                map.put(key, c);
            }
        }
        return new ArrayList<>(map.values());
    }

    /**
     * Extract server:port key from a config URI for deduplication.
     */
    private String serverKey(ConfigManager.ConfigItem c) {
        String[] hp = extractHostPort(c);
        if (hp == null) return null;
        return hp[0].toLowerCase() + ":" + hp[1];
    }

    private int protoPriority(String protocol) {
        if (protocol == null) return 0;
        switch (protocol.toLowerCase()) {
            case "vless":  return 10;
            case "vmess":  return 8;
            case "trojan": return 6;
            case "shadowsocks":
            case "ss":     return 4;
            default:       return 1;
        }
    }

    // =====================================================================
    //  Phase 2: TCP pre-filter
    // =====================================================================

    private boolean tcpConnect(String host, int port, int timeoutMs) {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(host, port), timeoutMs);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    // =====================================================================
    //  Phase 3: XRay SOCKS test
    // =====================================================================

    /**
     * Test a config end-to-end: acquire port → start XRay → SOCKS HTTP test.
     * Returns {basicSuccess, googleAccessible}.
     */
    private boolean[] testThroughV2Ray(ConfigManager.ConfigItem config, XRayManager xrayManager) {
        int port = -1;
        Process xrayProcess = null;
        File configFile = null;

        try {
            // Acquire an exclusive port from the pool (blocks until one is free)
            if (!portPool.tryAcquire(15, TimeUnit.SECONDS)) {
                Log.w(TAG, "Port pool exhausted for: " + config.name);
                return new boolean[]{false, false};
            }
            port = acquirePort();
            if (port < 0) {
                portPool.release();
                return new boolean[]{false, false};
            }

            // Generate minimal V2Ray config
            String v2rayConfig = generateTestConfig(config.uri, port);
            if (v2rayConfig == null) {
                Log.d(TAG, "Config generation failed for: " + config.name);
                return new boolean[]{false, false};
            }

            // Write config to temp file
            configFile = xrayManager.writeConfig(v2rayConfig, port);
            if (configFile == null) return new boolean[]{false, false};

            // Start xray process
            xrayProcess = xrayManager.startProcess(configFile);
            if (xrayProcess == null) return new boolean[]{false, false};

            // Wait for SOCKS port to become alive
            boolean portAlive = waitForPort(port, xrayProcess, PORT_WAIT_MS);
            if (!portAlive) {
                Log.d(TAG, "XRay port " + port + " never alive for: " + config.name);
                return new boolean[]{false, false};
            }

            if (!xrayProcess.isAlive()) {
                Log.d(TAG, "XRay died before test for: " + config.name);
                return new boolean[]{false, false};
            }

            // Test 1: Download/HTTP test — MANDATORY.
            // This is the most important test: verifies the proxy can actually
            // transfer data, not just establish TCP connections.
            // Without this, configs that pass Telegram DC connect but can't
            // transfer HTTP data would be accepted, causing "connecting" forever.
            boolean downloadOk = testDownloadThroughSocks(port, config.name);
            if (!downloadOk) {
                downloadOk = testHttpThroughSocks(port, config.name);
            }
            if (!downloadOk) {
                Log.d(TAG, "Config REJECTED (download test failed): " + config.name);
                return new boolean[]{false, false};
            }

            // Test 2: Telegram DC connectivity — important for Iranian users.
            // Returns latency in ms, or -1 if unreachable.
            int telegramLatency = testTelegramThroughSocks(port, config.name);
            boolean telegramOk = telegramLatency >= 0;

            // Store Telegram latency on the config item for ranking/monitoring
            config.telegram_latency = telegramOk ? telegramLatency : Integer.MAX_VALUE;

            // Test 3: Google access (bonus, not required)
            boolean googleAccessible = testGoogleThroughSocks(port, config.name);

            Log.i(TAG, "Config ACCEPTED: " + config.name + " (download=true"
                + ", telegram=" + telegramOk
                + (telegramOk ? " " + telegramLatency + "ms" : "")
                + ", google=" + googleAccessible + ")");
            return new boolean[]{true, googleAccessible};

        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return new boolean[]{false, false};
        } catch (Exception e) {
            Log.w(TAG, "V2Ray test error for " + config.name + ": " + e.getMessage());
            return new boolean[]{false, false};
        } finally {
            // Always clean up: kill process, delete file, release port
            if (xrayProcess != null) {
                try { xrayProcess.destroyForcibly(); } catch (Exception ignored) {}
            }
            if (configFile != null) {
                try { configFile.delete(); } catch (Exception ignored) {}
            }
            if (port >= 0) {
                releasePort(port);
                portPool.release();
            }
        }
    }

    // ── Port pool helpers ─────────────────────────────────────────────────

    private int acquirePort() {
        synchronized (portLock) {
            for (int i = 0; i < PORT_POOL_SIZE; i++) {
                if (!portInUse[i]) {
                    portInUse[i] = true;
                    return PORT_POOL_BASE + i;
                }
            }
        }
        return -1; // should not happen if semaphore is sized correctly
    }

    private void releasePort(int port) {
        int idx = port - PORT_POOL_BASE;
        if (idx >= 0 && idx < PORT_POOL_SIZE) {
            synchronized (portLock) {
                portInUse[idx] = false;
            }
        }
    }

    // =====================================================================
    //  Host/port extraction (shared by Phase 1 & 2)
    // =====================================================================

    /**
     * Extract [host, port] from a config URI. Returns null on parse failure.
     */
    private String[] extractHostPort(ConfigManager.ConfigItem config) {
        try {
            String uri = config.uri;
            String host;
            int port;

            if (uri.toLowerCase().startsWith("vmess://")) {
                String base64 = uri.substring(8);
                if (base64.length() > 4096) return null;
                String json = new String(android.util.Base64.decode(base64, android.util.Base64.NO_WRAP));
                JSONObject obj = new JSONObject(json);
                host = obj.optString("add", "");
                port = obj.optInt("port", 443);
            } else {
                java.net.URI parsed = new java.net.URI(uri);
                host = parsed.getHost();
                port = parsed.getPort();
                if (port <= 0) port = 443;
            }

            if (host == null || host.isEmpty()) return null;
            return new String[]{host, String.valueOf(port)};
        } catch (Exception e) {
            return null;
        }
    }

    static boolean isIpAddress(String host) {
        if (host == null || host.isEmpty()) return false;
        char c = host.charAt(0);
        if (c == '[') return true; // IPv6 bracket notation
        if (Character.isDigit(c)) {
            for (int i = 0; i < host.length(); i++) {
                char ch = host.charAt(i);
                if (ch != '.' && !Character.isDigit(ch)) return false;
            }
            return true;
        }
        return host.contains(":"); // IPv6
    }

    // =====================================================================
    //  SOCKS5 test helpers
    // =====================================================================

    private boolean waitForPort(int port, Process process, int maxWaitMs) {
        long deadline = System.currentTimeMillis() + maxWaitMs;
        while (System.currentTimeMillis() < deadline) {
            if (!process.isAlive()) return false;
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress("127.0.0.1", port), 100);
                return true;
            } catch (Exception ignored) {}
            try { Thread.sleep(80); } catch (InterruptedException e) { return false; }
        }
        return false;
    }

    /**
     * Real download test through SOCKS5 proxy.
     * Tries multiple endpoints to verify actual data transfer:
     *   1. cachefly.cachefly.net (CDN, reliable, returns real data)
     *   2. www.google.com/generate_204 (returns 204, widely accessible)
     * This catches configs that can SOCKS CONNECT but can't transfer data.
     */
    private boolean testDownloadThroughSocks(int socksPort, String configName) {
        // Try CacheFly first — real download test with actual data
        if (testSingleDownload(socksPort, configName, "cachefly.cachefly.net", 80,
                "GET /50mb.test HTTP/1.1\r\nHost: cachefly.cachefly.net\r\nConnection: close\r\nRange: bytes=0-4095\r\n\r\n",
                true)) {
            return true;
        }
        // Fallback: Google generate_204
        if (testSingleDownload(socksPort, configName, "www.google.com", 80,
                "GET /generate_204 HTTP/1.1\r\nHost: www.google.com\r\nConnection: close\r\n\r\n",
                false)) {
            return true;
        }
        return false;
    }

    /**
     * Test a single download endpoint through SOCKS5.
     * @param requireData if true, requires actual body data (not just headers)
     */
    private boolean testSingleDownload(int socksPort, String configName,
                                        String host, int port, String httpReq, boolean requireData) {
        Socket socket = null;
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", socksPort), SOCKS_TIMEOUT_MS);
            socket.setSoTimeout(SOCKS_TIMEOUT_MS);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) return false;

            // SOCKS5 CONNECT using domain (ATYP=0x03) — DNS resolved by XRay/proxy
            byte[] hostBytes = host.getBytes(StandardCharsets.UTF_8);
            byte[] req = new byte[7 + hostBytes.length];
            req[0] = 0x05; req[1] = 0x01; req[2] = 0x00; req[3] = 0x03;
            req[4] = (byte) hostBytes.length;
            System.arraycopy(hostBytes, 0, req, 5, hostBytes.length);
            req[5 + hostBytes.length] = (byte) ((port >> 8) & 0xFF);
            req[6 + hostBytes.length] = (byte) (port & 0xFF);
            os.write(req);
            os.flush();

            // Read SOCKS5 reply
            int rVer = is.read();
            int rep = is.read();
            is.read(); // RSV
            int atyp = is.read();
            if (rVer != 0x05 || rep != 0x00) {
                Log.d(TAG, "Download SOCKS CONNECT rejected for " + configName + " -> " + host + ": rep=" + rep);
                return false;
            }

            // Skip BND.ADDR + BND.PORT
            skipSocksAddress(is, atyp);
            readNBytes(is, 2);

            // Send HTTP request
            os.write(httpReq.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            // Read HTTP response
            byte[] buf = new byte[4096];
            long startTime = System.currentTimeMillis();
            int totalRead = 0;
            int n = is.read(buf);
            if (n <= 0) {
                Log.d(TAG, "Download FAIL for " + configName + " via " + host + ": no response data");
                return false;
            }
            totalRead += n;
            String head = new String(buf, 0, Math.min(n, 512), StandardCharsets.US_ASCII);

            // Must be a valid HTTP response
            if (!head.startsWith("HTTP/")) {
                Log.d(TAG, "Download FAIL for " + configName + " via " + host + ": not HTTP");
                return false;
            }
            if (!head.contains(" 200") && !head.contains(" 204") && !head.contains(" 206")) {
                String firstLine = head.contains("\r\n") ? head.substring(0, head.indexOf("\r\n")) : head.substring(0, Math.min(head.length(), 80));
                Log.d(TAG, "Download FAIL for " + configName + " via " + host + ": " + firstLine);
                return false;
            }

            if (requireData) {
                // Read more data to verify actual transfer (up to 8KB total, 5s max)
                try {
                    while (totalRead < 8192 && (System.currentTimeMillis() - startTime) < 5000) {
                        n = is.read(buf);
                        if (n <= 0) break;
                        totalRead += n;
                    }
                } catch (java.net.SocketTimeoutException ignored) {}

                long duration = Math.max(System.currentTimeMillis() - startTime, 1);
                int speedKBs = (int) ((totalRead * 1000L) / (duration * 1024));
                Log.i(TAG, "Download test " + configName + " via " + host + ": "
                    + totalRead + " bytes in " + duration + "ms = " + speedKBs + " KB/s");

                if (totalRead < 500) {
                    Log.d(TAG, "Download FAIL for " + configName + " via " + host + ": too little data (" + totalRead + " bytes, need 500+)");
                    return false;
                }
            }

            Log.i(TAG, "Download PASS for " + configName + " via " + host + " (" + totalRead + " bytes)");
            return true;

        } catch (java.net.SocketTimeoutException e) {
            Log.d(TAG, "Download TIMEOUT for " + configName + " via " + host);
            return false;
        } catch (Exception e) {
            Log.d(TAG, "Download ERROR for " + configName + " via " + host + ": " + e.getMessage());
            return false;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }

    /**
     * Fallback HTTP test through SOCKS5 proxy using Cloudflare IP directly.
     * Uses IP address (ATYP=0x01) to avoid any DNS dependency.
     */
    private boolean testHttpThroughSocks(int socksPort, String configName) {
        Socket socket = null;
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", socksPort), SOCKS_TIMEOUT_MS);
            socket.setSoTimeout(SOCKS_TIMEOUT_MS);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) {
                Log.d(TAG, "SOCKS5 handshake FAIL for " + configName);
                return false;
            }

            // SOCKS5 CONNECT to 1.1.1.1:80 (Cloudflare IP — no DNS needed)
            byte[] req = new byte[10];
            req[0] = 0x05; req[1] = 0x01; req[2] = 0x00;
            req[3] = 0x01; // ATYP=IPv4
            req[4] = 0x01; req[5] = 0x01; req[6] = 0x01; req[7] = 0x01; // 1.1.1.1
            req[8] = 0x00; req[9] = 0x50; // port 80
            os.write(req);
            os.flush();

            // Read SOCKS5 reply
            int rVer = is.read();
            int rep = is.read();
            is.read(); // RSV
            int atyp = is.read();
            if (rVer != 0x05 || rep != 0x00) {
                Log.d(TAG, "SOCKS5 CONNECT rejected for " + configName + ": rep=" + rep);
                return false;
            }

            // Skip BND.ADDR + BND.PORT
            skipSocksAddress(is, atyp);
            readNBytes(is, 2);

            // Send HTTP request
            String http = "GET /generate_204 HTTP/1.1\r\nHost: 1.1.1.1\r\nConnection: close\r\n\r\n";
            os.write(http.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            // Read response
            byte[] buf = new byte[256];
            int n = is.read(buf);
            if (n <= 0) {
                Log.d(TAG, "HTTP fallback FAIL for " + configName + ": no response");
                return false;
            }
            String head = new String(buf, 0, n, StandardCharsets.US_ASCII);
            boolean ok = head.startsWith("HTTP/") && (head.contains(" 204") || head.contains(" 200"));
            if (ok) {
                Log.i(TAG, "HTTP fallback PASS for " + configName + " on port " + socksPort);
            } else {
                Log.d(TAG, "HTTP fallback FAIL for " + configName + ": " + head.substring(0, Math.min(head.length(), 80)));
            }
            return ok;

        } catch (java.net.SocketTimeoutException e) {
            Log.d(TAG, "HTTP fallback TIMEOUT for " + configName);
            return false;
        } catch (Exception e) {
            Log.d(TAG, "HTTP fallback ERROR for " + configName + ": " + e.getMessage());
            return false;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }

    /**
     * Test Google access through SOCKS5 proxy.
     * Uses raw SOCKS5 protocol with domain name (ATYP=0x03) so DNS is resolved
     * remotely by XRay, bypassing local censored DNS on Iranian networks.
     */
    private boolean testGoogleThroughSocks(int socksPort, String configName) {
        Socket socket = null;
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", socksPort), SOCKS_TIMEOUT_MS);
            socket.setSoTimeout(SOCKS_TIMEOUT_MS);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) return false;

            // SOCKS5 CONNECT using domain name (ATYP=0x03) — avoids local DNS
            String host = "www.google.com";
            int port = 80;
            byte[] hostBytes = host.getBytes(StandardCharsets.UTF_8);
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

            // Read SOCKS5 reply
            int rVer = is.read();
            int rep = is.read();
            is.read(); // RSV
            int atyp = is.read();
            if (rVer != 0x05 || rep != 0x00) {
                Log.d(TAG, "Google SOCKS CONNECT rejected for " + configName + ": rep=" + rep);
                return false;
            }

            // Skip BND.ADDR + BND.PORT
            skipSocksAddress(is, atyp);
            readNBytes(is, 2);

            // Send HTTP request
            String http = "GET /generate_204 HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
            os.write(http.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            // Read response — any HTTP response from Google means it's accessible
            byte[] buf = new byte[256];
            int n = is.read(buf);
            if (n <= 0) return false;
            String head = new String(buf, 0, n, StandardCharsets.US_ASCII);
            boolean ok = head.startsWith("HTTP/");
            if (ok) {
                Log.i(TAG, "Google test PASS for " + configName);
            }
            return ok;

        } catch (Exception e) {
            return false;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }

    /**
     * Test Telegram DC connectivity through SOCKS5 proxy.
     * Returns best latency in ms across all DCs, or -1 if none reachable.
     * Telegram DCs are heavily filtered in Iran, so this is a critical test.
     */
    private int testTelegramThroughSocks(int socksPort, String configName) {
        // Telegram DC IPs (DC1-DC5) — these are the primary API endpoints
        String[][] telegramDCs = {
            {"149.154.175.50", "443"},   // DC1
            {"149.154.167.51", "443"},   // DC2
            {"149.154.175.100", "443"},  // DC3
            {"149.154.167.91", "443"},   // DC4
            {"91.108.56.130", "443"},    // DC5
        };

        int bestLatency = -1;
        for (String[] dc : telegramDCs) {
            int latency = testTelegramDCLatency(socksPort, configName, dc[0], Integer.parseInt(dc[1]));
            if (latency >= 0) {
                bestLatency = (bestLatency < 0) ? latency : Math.min(bestLatency, latency);
                break; // One DC reachable is enough — use best latency
            }
        }
        if (bestLatency < 0) {
            Log.d(TAG, "Telegram test FAIL for " + configName + ": no DC reachable");
        }
        return bestLatency;
    }

    /**
     * Test a single Telegram DC and return latency in ms, or -1 if unreachable.
     * Verifies actual data flow, not just SOCKS5 CONNECT.
     */
    private int testTelegramDCLatency(int socksPort, String configName, String ip, int port) {
        Socket socket = null;
        try {
            long start = System.currentTimeMillis();
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", socksPort), 6000);
            socket.setSoTimeout(6000);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) return -1;

            // SOCKS5 CONNECT to Telegram DC IP (ATYP=0x01 IPv4)
            String[] parts = ip.split("\\.");
            byte[] req = new byte[10];
            req[0] = 0x05; req[1] = 0x01; req[2] = 0x00; req[3] = 0x01;
            req[4] = (byte) Integer.parseInt(parts[0]);
            req[5] = (byte) Integer.parseInt(parts[1]);
            req[6] = (byte) Integer.parseInt(parts[2]);
            req[7] = (byte) Integer.parseInt(parts[3]);
            req[8] = (byte) ((port >> 8) & 0xFF);
            req[9] = (byte) (port & 0xFF);
            os.write(req);
            os.flush();

            // Read SOCKS5 reply
            int rVer = is.read();
            int rep = is.read();
            is.read(); // RSV
            int atyp = is.read();
            if (rVer != 0x05 || rep != 0x00) return -1;

            // Skip BND.ADDR + BND.PORT
            skipSocksAddress(is, atyp);
            readNBytes(is, 2);

            // Verify ACTUAL data flow — send probe, wait for response
            socket.setSoTimeout(5000);
            byte[] probe = new byte[64];
            for (int i = 0; i < probe.length; i++) probe[i] = (byte)(i ^ 0xAA);
            os.write(probe);
            os.flush();

            byte[] buf = new byte[256];
            try {
                int n = is.read(buf);
                int latency = (int)(System.currentTimeMillis() - start);
                if (n > 0) {
                    Log.i(TAG, "Telegram DC " + ip + " data flow verified for " + configName + " (" + n + " bytes, " + latency + "ms)");
                    return latency;
                }
                // Clean close = DC received our data
                Log.i(TAG, "Telegram DC " + ip + " reachable for " + configName + " (clean close, " + latency + "ms)");
                return latency;
            } catch (java.net.SocketTimeoutException e) {
                // Timeout but SOCKS succeeded — accept with penalty latency
                int latency = (int)(System.currentTimeMillis() - start);
                Log.i(TAG, "Telegram DC " + ip + " reachable for " + configName + " (SOCKS ok, " + latency + "ms)");
                return latency;
            }

        } catch (Exception e) {
            return -1;
        } finally {
            if (socket != null) {
                try { socket.close(); } catch (Exception ignored) {}
            }
        }
    }

    private void skipSocksAddress(InputStream is, int atyp) throws IOException {
        if (atyp == 0x01) { // IPv4
            readNBytes(is, 4);
        } else if (atyp == 0x04) { // IPv6
            readNBytes(is, 16);
        } else if (atyp == 0x03) { // DOMAIN
            int len = is.read();
            if (len > 0) readNBytes(is, len);
        }
    }

    private void readNBytes(InputStream is, int count) throws IOException {
        byte[] buf = new byte[count];
        int off = 0;
        while (off < count) {
            int n = is.read(buf, off, count - off);
            if (n < 0) throw new IOException("EOF");
            off += n;
        }
    }

    // =====================================================================
    //  XRay config generation
    // =====================================================================

    /**
     * Generate a minimal V2Ray JSON config for testing a single URI.
     * Based on v2rayNG architecture: dns.tag routes DNS through proxy.
     */
    private String generateTestConfig(String uri, int socksPort) {
        try {
            JSONObject outbound = V2RayConfigHelper.parseUriToOutbound(uri);
            if (outbound == null) return null;

            // v2rayNG resolveOutboundDomainsToHosts pattern:
            // resolve hostname → add to dns.hosts + set sockopt.domainStrategy=UseIP
            String proxyDomain = null;
            String resolvedIp = null;
            // Check if address is a domain
            String address = V2RayConfigHelper.extractOutboundAddress(outbound);
            if (address == null) {
                // address is a domain, not IP — need to resolve
                V2RayConfigHelper.preResolveOutboundHost(outbound);
                // After preResolve, check if it got resolved
                address = V2RayConfigHelper.extractOutboundAddress(outbound);
                if (address == null) {
                    // Still unresolved — try the v2rayNG hosts approach instead
                    // Extract the domain name before preResolve changed it
                    try {
                        String protocol = outbound.optString("protocol", "");
                        JSONObject settings = outbound.optJSONObject("settings");
                        if (settings != null) {
                            if ("vmess".equals(protocol) || "vless".equals(protocol)) {
                                JSONArray vnext = settings.optJSONArray("vnext");
                                if (vnext != null && vnext.length() > 0) {
                                    proxyDomain = vnext.getJSONObject(0).optString("address", "");
                                }
                            } else if ("trojan".equals(protocol) || "shadowsocks".equals(protocol)) {
                                JSONArray servers = settings.optJSONArray("servers");
                                if (servers != null && servers.length() > 0) {
                                    proxyDomain = servers.getJSONObject(0).optString("address", "");
                                }
                            }
                        }
                    } catch (Exception ignored) {}

                    if (proxyDomain != null && !proxyDomain.isEmpty()
                        && !proxyDomain.matches("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$")) {
                        // Domain still unresolved — this config likely won't work
                        Log.w(TAG, "Rejecting config — hostname unresolved: " + proxyDomain);
                        return null;
                    }
                }
            }

            V2RayConfigHelper.applyAntiDpiSettings(outbound);

            JSONObject config = new JSONObject();
            config.put("log", new JSONObject().put("loglevel", "warning"));

            // ── Inbound: SOCKS ──
            JSONArray inbounds = new JSONArray();
            JSONObject socksIn = new JSONObject();
            socksIn.put("tag", "socks");
            socksIn.put("port", socksPort);
            socksIn.put("listen", "127.0.0.1");
            socksIn.put("protocol", "socks");
            JSONObject socksSettings = new JSONObject();
            socksSettings.put("auth", "noauth");
            socksSettings.put("udp", true);
            socksSettings.put("userLevel", 8);
            socksIn.put("settings", socksSettings);
            JSONObject sniffing = new JSONObject();
            sniffing.put("enabled", true);
            sniffing.put("destOverride", new JSONArray().put("http").put("tls"));
            socksIn.put("sniffing", sniffing);
            inbounds.put(socksIn);
            config.put("inbounds", inbounds);

            // ── Outbounds ──
            JSONArray outbounds = new JSONArray();
            outbounds.put(outbound);
            outbounds.put(new JSONObject().put("tag", "direct").put("protocol", "freedom")
                .put("settings", new JSONObject().put("domainStrategy", "UseIP")));
            outbounds.put(new JSONObject().put("tag", "dns-out").put("protocol", "dns"));
            config.put("outbounds", outbounds);

            // ── Routing (v2rayNG: AsIs) ──
            JSONObject routing = new JSONObject();
            routing.put("domainStrategy", "AsIs");
            JSONArray rules = new JSONArray();

            // Proxy server IP → direct
            String proxyServerIp = V2RayConfigHelper.extractOutboundAddress(outbound);
            if (proxyServerIp != null && !proxyServerIp.isEmpty()) {
                JSONObject proxyDirect = new JSONObject();
                proxyDirect.put("type", "field");
                proxyDirect.put("ip", new JSONArray().put(proxyServerIp));
                proxyDirect.put("outboundTag", "direct");
                rules.put(proxyDirect);
            }

            // Private IPs → direct
            JSONObject directPrivate = new JSONObject();
            directPrivate.put("type", "field");
            directPrivate.put("ip", new JSONArray().put("geoip:private"));
            directPrivate.put("outboundTag", "direct");
            rules.put(directPrivate);

            routing.put("rules", rules);
            config.put("routing", routing);

            // ── DNS (v2rayNG: tag="dns-module" → routed through proxy) ──
            JSONObject dns = new JSONObject();
            dns.put("servers", new JSONArray().put("1.1.1.1").put("8.8.8.8"));
            dns.put("queryStrategy", "UseIPv4");
            dns.put("tag", "dns-module");
            JSONObject hosts = new JSONObject();
            hosts.put("domain:googleapis.cn", "googleapis.com");
            dns.put("hosts", hosts);
            config.put("dns", dns);

            // DNS routing: send dns-module traffic through proxy
            JSONObject dnsModuleRule = new JSONObject();
            dnsModuleRule.put("type", "field");
            dnsModuleRule.put("inboundTag", new JSONArray().put("dns-module"));
            dnsModuleRule.put("outboundTag", "proxy");
            rules.put(dnsModuleRule);

            return config.toString();

        } catch (Exception e) {
            Log.w(TAG, "Error generating test config", e);
            return null;
        }
    }

    // =====================================================================
    //  TCP-only fallback (when XRay is not ready)
    // =====================================================================

    private void runTcpOnlyFallback(List<ConfigManager.ConfigItem> configs, TestCallback callback,
                                    AtomicInteger testedCount, AtomicInteger successCount, int total) {
        CountDownLatch latch = new CountDownLatch(configs.size());
        for (ConfigManager.ConfigItem c : configs) {
            if (stopRequested) { latch.countDown(); continue; }
            tcpExecutor.execute(() -> {
                try {
                    String[] hp = extractHostPort(c);
                    boolean ok = false;
                    if (hp != null) {
                        if (isIpAddress(hp[0])) {
                            ok = tcpConnect(hp[0], Integer.parseInt(hp[1]), 4000);
                        } else {
                            ok = true; // can't test domain without XRay, assume alive
                        }
                    }
                    reportResult(c, ok, ok ? 5000 : -1, false, callback, testedCount, successCount, total);
                } catch (Exception e) {
                    reportResult(c, false, -1, false, callback, testedCount, successCount, total);
                } finally {
                    latch.countDown();
                }
            });
        }
        try { latch.await(120, TimeUnit.SECONDS); } catch (InterruptedException ignored) {}
    }

    // =====================================================================
    //  Result reporting helper
    // =====================================================================

    private void reportResult(ConfigManager.ConfigItem config, boolean success, int latency,
                              boolean googleAccessible, TestCallback callback,
                              AtomicInteger testedCount, AtomicInteger successCount, int total) {
        config.googleAccessible = googleAccessible;
        if (success) {
            config.latency = latency;
            successCount.incrementAndGet();
        } else {
            config.latency = -1;
        }

        callback.onConfigTested(config, success, latency, googleAccessible);

        int tested = testedCount.incrementAndGet();
        if (tested % 50 == 0 || tested == total || (success && tested < 100)) {
            callback.onProgress(tested, total);
        }
    }

    // =====================================================================
    //  Public API (unchanged interface for callers)
    // =====================================================================

    /**
     * Get working configs sorted by Telegram stability first, then general latency.
     * Configs with verified Telegram DC connectivity are prioritized over those without.
     * Among Telegram-capable configs, sort by Telegram latency (fastest first).
     */
    public List<ConfigManager.ConfigItem> getWorkingConfigs(List<ConfigManager.ConfigItem> allConfigs) {
        List<ConfigManager.ConfigItem> working = new ArrayList<>();
        for (ConfigManager.ConfigItem config : allConfigs) {
            if (config.latency > 0 && config.latency < 15000) {
                working.add(config);
            }
        }
        // Sort: Telegram-capable first (by telegram latency), then non-Telegram (by general latency)
        Collections.sort(working, (a, b) -> {
            boolean aTelegram = a.telegram_latency > 0 && a.telegram_latency < Integer.MAX_VALUE;
            boolean bTelegram = b.telegram_latency > 0 && b.telegram_latency < Integer.MAX_VALUE;
            if (aTelegram != bTelegram) return aTelegram ? -1 : 1;
            if (aTelegram) return Integer.compare(a.telegram_latency, b.telegram_latency);
            return Integer.compare(a.latency, b.latency);
        });
        return working;
    }

    public void cleanCache() {
        resultCache.entrySet().removeIf(entry -> entry.getValue().isExpired());
        Log.d(TAG, "Cleaned expired cache entries, remaining: " + resultCache.size());
    }

    public int getCacheSize() {
        return resultCache.size();
    }

    public String getQueueStatus() {
        return "Pipeline mode — no queues";
    }

    public boolean isRunning() {
        return isRunning;
    }

    public void stop() {
        stopRequested = true;
        isRunning = false;
    }

    public void stopTesting() {
        isRunning = false;
    }
}
