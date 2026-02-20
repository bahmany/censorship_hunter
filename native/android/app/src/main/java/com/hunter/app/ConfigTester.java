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
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.PriorityQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Enhanced config tester with proper thread management, priority queues, and result caching.
 * Manages config testing in organized queues with different priorities for various protocols.
 */
public class ConfigTester {
    private static final String TAG = "ConfigTester";
    private static final int V2RAY_TEST_TIMEOUT_MS = 8000;
    private static final int TCP_TEST_TIMEOUT_MS = 4000;

    // Thread management
    private static final int CORE_POOL_SIZE = 6;
    private static final int MAX_POOL_SIZE = 10;
    private static final int QUEUE_CAPACITY = 200;
    private static final int TEST_SOCKS_PORT_BASE = 20808;
    private static final int TCP_PREFILTER_TIMEOUT_MS = 2000;
    private static final String TEST_URL = "http://cp.cloudflare.com/generate_204";

    // Priority queues for different config types
    private final PriorityQueue<ConfigTestTask> vmessQueue = new PriorityQueue<>(
        (a, b) -> Integer.compare(getPriority(a.config), getPriority(b.config)));
    private final PriorityQueue<ConfigTestTask> vlessQueue = new PriorityQueue<>(
        (a, b) -> Integer.compare(getPriority(a.config), getPriority(b.config)));
    private final PriorityQueue<ConfigTestTask> shadowsocksQueue = new PriorityQueue<>(
        (a, b) -> Integer.compare(getPriority(a.config), getPriority(b.config)));
    private final PriorityQueue<ConfigTestTask> otherQueue = new PriorityQueue<>(
        (a, b) -> Integer.compare(getPriority(a.config), getPriority(b.config)));

    // Thread pool and queue management
    private final ExecutorService executor;
    private final BlockingQueue<Runnable> workQueue = new LinkedBlockingQueue<>(QUEUE_CAPACITY);
    private volatile boolean isRunning = false;
    private volatile boolean stopRequested = false;
    private final AtomicInteger portCounter = new AtomicInteger(0);
    private final AtomicInteger activeTests = new AtomicInteger(0);

    // Result caching
    private final Map<String, TestResult> resultCache = new HashMap<>();
    private static final long CACHE_EXPIRY_MS = 10 * 60 * 1000; // 10 minutes

    private final Object testLock = new Object();

    // Priority levels (lower number = higher priority)
    private static final int PRIORITY_HIGH = 1;    // Recently working configs
    private static final int PRIORITY_MEDIUM = 2;  // Known working configs
    private static final int PRIORITY_LOW = 3;     // Untested or failed configs
    private static final int PRIORITY_VERY_LOW = 4; // Old failed configs

    public interface TestCallback {
        void onProgress(int tested, int total);
        void onConfigTested(ConfigManager.ConfigItem config, boolean success, int latency, boolean googleAccessible);
        void onComplete(int totalTested, int successCount);
    }

    /**
     * Represents a config testing task with priority
     */
    private static class ConfigTestTask implements Comparable<ConfigTestTask> {
        final ConfigManager.ConfigItem config;
        final long submissionTime;
        final int priority;

        ConfigTestTask(ConfigManager.ConfigItem config) {
            this.config = config;
            this.submissionTime = System.currentTimeMillis();
            this.priority = getPriority(config);
        }

        @Override
        public int compareTo(ConfigTestTask other) {
            // First compare by priority (lower = higher priority)
            int priorityCompare = Integer.compare(this.priority, other.priority);
            if (priorityCompare != 0) return priorityCompare;

            // Then by submission time (FIFO within same priority)
            return Long.compare(this.submissionTime, other.submissionTime);
        }
    }

    /**
     * Cached test result
     */
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
        // Create thread pool with proper configuration
        this.executor = new ThreadPoolExecutor(
            CORE_POOL_SIZE, MAX_POOL_SIZE, 30, TimeUnit.SECONDS,
            workQueue,
            new ThreadPoolExecutor.CallerRunsPolicy() // Caller runs if queue is full
        );

    }

    /**
     * Determine priority based on config latency (simplified since lastTestTime doesn't exist)
     */
    private static int getPriority(ConfigManager.ConfigItem config) {
        // Recently working configs get highest priority
        if (config.latency > 0 && config.latency < 2000) {
            return PRIORITY_HIGH;
        }

        // Known working configs (higher latency)
        if (config.latency > 0 && config.latency < 10000) {
            return PRIORITY_MEDIUM;
        }

        // Untested or previously failed
        if (config.latency == -1 || config.latency >= 10000) {
            return PRIORITY_LOW;
        }

        return PRIORITY_MEDIUM;
    }

    /**
     * Add configs to appropriate priority queues
     */
    public void testConfigs(List<ConfigManager.ConfigItem> configs, TestCallback callback) {
        synchronized(testLock) {
            if (isRunning) {
                Log.w(TAG, "Test already running");
                return;
            }
            isRunning = true;
        }

        stopRequested = false;

        // Clear existing queues
        vmessQueue.clear();
        vlessQueue.clear();
        shadowsocksQueue.clear();
        otherQueue.clear();

        // Categorize configs by type and add to appropriate queues
        for (ConfigManager.ConfigItem config : configs) {
            ConfigTestTask task = new ConfigTestTask(config);
            String uri = config.uri.toLowerCase();

            if (uri.startsWith("vmess://")) {
                vmessQueue.offer(task);
            } else if (uri.startsWith("vless://")) {
                vlessQueue.offer(task);
            } else if (uri.startsWith("ss://")) {
                shadowsocksQueue.offer(task);
            } else {
                otherQueue.offer(task);
            }
        }

        final int totalConfigs = configs.size();
        final AtomicInteger testedCount = new AtomicInteger(0);
        final AtomicInteger successCount = new AtomicInteger(0);

        Log.i(TAG, String.format("Starting prioritized testing: VMess=%d, VLESS=%d, SS=%d, Other=%d",
            vmessQueue.size(), vlessQueue.size(), shadowsocksQueue.size(), otherQueue.size()));

        // Process queues on a SEPARATE thread (not from executor pool)
        // This keeps all executor threads free for actual test work
        Thread queueProcessor = new Thread(() -> {
            try {
                XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
                boolean useV2Ray = xrayManager != null && xrayManager.isReady();
                Log.i(TAG, "[DIAG] XRay ready=" + useV2Ray
                    + ", binary=" + (xrayManager != null ? xrayManager.getBinaryPath() : "null")
                    + ", executor pool=" + CORE_POOL_SIZE + "/" + MAX_POOL_SIZE);

                // Process each queue in priority order
                processQueue(vmessQueue, useV2Ray, xrayManager, callback, testedCount, successCount, totalConfigs);
                processQueue(vlessQueue, useV2Ray, xrayManager, callback, testedCount, successCount, totalConfigs);
                processQueue(shadowsocksQueue, useV2Ray, xrayManager, callback, testedCount, successCount, totalConfigs);
                processQueue(otherQueue, useV2Ray, xrayManager, callback, testedCount, successCount, totalConfigs);

                callback.onComplete(testedCount.get(), successCount.get());

            } finally {
                isRunning = false;
            }
        }, "ConfigTestQueueProcessor");
        queueProcessor.setDaemon(true);
        queueProcessor.start();
    }

    /**
     * Process a single priority queue
     */
    private void processQueue(PriorityQueue<ConfigTestTask> queue, boolean useV2Ray,
                            XRayManager xrayManager, TestCallback callback,
                            AtomicInteger testedCount, AtomicInteger successCount, int total) {
        if (queue.isEmpty()) return;

        List<ConfigTestTask> batch = new ArrayList<>();
        int batchSize = useV2Ray ? 6 : 12; // Increased from 3/8 for faster testing

        while (!queue.isEmpty() && !stopRequested) {
            batch.clear();

            // Fill batch with highest priority items
            for (int i = 0; i < batchSize && !queue.isEmpty(); i++) {
                batch.add(queue.poll());
            }

            if (batch.isEmpty()) break;

            // Process batch concurrently
            CountDownLatch latch = new CountDownLatch(batch.size());

            for (ConfigTestTask task : batch) {
                executor.execute(() -> {
                    try {
                        if (stopRequested) return;

                        // Check cache first
                        String cacheKey = getCacheKey(task.config);
                        TestResult cached = resultCache.get(cacheKey);
                        if (cached != null && !cached.isExpired()) {
                            handleTestResult(task.config, cached.success, cached.latency, cached.googleAccessible,
                                           callback, testedCount, successCount, total);
                            return;
                        }

                        long startTime = System.currentTimeMillis();
                        boolean[] result;

                        if (useV2Ray) {
                            result = testThroughV2Ray(task.config, xrayManager);
                        } else {
                            boolean tcpSuccess = testTcpReachability(task.config);
                            result = new boolean[]{tcpSuccess, false};
                        }

                        int latency = (int) (System.currentTimeMillis() - startTime);

                        handleTestResult(task.config, result[0], latency, result[1],
                                       callback, testedCount, successCount, total);

                    } catch (Exception e) {
                        handleTestResult(task.config, false, -1, false, callback,
                                       testedCount, successCount, total);
                    } finally {
                        latch.countDown();
                    }
                });
            }

            // Wait for batch to complete with timeout
            try {
                long timeoutMs = useV2Ray ? 20000L : 12000L; // Increased timeouts for larger batches
                latch.await(timeoutMs, TimeUnit.MILLISECONDS);
            } catch (InterruptedException ignored) {}

            // Throttle to prevent overwhelming the system
            if (!queue.isEmpty()) {
                try {
                    Thread.sleep(100); // Reduced from 200ms for faster processing
                } catch (InterruptedException ignored) {}
            }
        }
    }

    private String getCacheKey(ConfigManager.ConfigItem config) {
        return config.uri + "_" + config.protocol;
    }

    private void handleTestResult(ConfigManager.ConfigItem config, boolean success, int latency,
                                boolean googleAccessible, TestCallback callback, AtomicInteger testedCount,
                                AtomicInteger successCount, int total) {
        config.googleAccessible = googleAccessible;

        if (success) {
            config.latency = latency;
            successCount.incrementAndGet();
        } else {
            config.latency = -1;
        }

        callback.onConfigTested(config, success, latency, googleAccessible);

        int tested = testedCount.incrementAndGet();
        if (tested % 10 == 0 || tested == total) { // Update progress less frequently
            callback.onProgress(tested, total);
        }
    }

    /**
     * Test a config by starting a real XRay process, sending HTTP through its SOCKS proxy.
     * This is the real test — verifies the config actually works end-to-end.
     * Returns {basicSuccess, googleAccessible}
     */
    private boolean[] testThroughV2Ray(ConfigManager.ConfigItem config, XRayManager xrayManager) {
        int testPort = TEST_SOCKS_PORT_BASE + portCounter.getAndIncrement() % 200;
        Process xrayProcess = null;

        try {
            // Quick TCP pre-filter: skip dead servers without spawning XRay
            if (!quickTcpCheck(config)) {
                Log.d(TAG, "TCP pre-filter FAIL: " + config.name + " (server unreachable)");
                return new boolean[]{false, false};
            }

            // Generate minimal V2Ray config for this test
            String v2rayConfig = generateTestConfig(config.uri, testPort);
            if (v2rayConfig == null) {
                Log.d(TAG, "Config generation failed for: " + config.name);
                return new boolean[]{false, false};
            }

            // Write config to temp file
            File configFile = xrayManager.writeConfig(v2rayConfig, testPort);
            if (configFile == null) return new boolean[]{false, false};

            // Start xray process
            xrayProcess = xrayManager.startProcess(configFile);
            if (xrayProcess == null) {
                configFile.delete();
                return new boolean[]{false, false};
            }

            // Wait for SOCKS port to become alive (instead of fixed sleep)
            boolean portAlive = waitForPort(testPort, xrayProcess, 3000);
            if (!portAlive) {
                Log.d(TAG, "XRay SOCKS port " + testPort + " never came alive for: " + config.name);
                configFile.delete();
                return new boolean[]{false, false};
            }

            // Verify XRay process is still alive
            if (!xrayProcess.isAlive()) {
                Log.d(TAG, "XRay process died before test for: " + config.name);
                configFile.delete();
                return new boolean[]{false, false};
            }

            // Test basic connectivity through SOCKS proxy
            boolean basicSuccess = testHttpThroughSocks(testPort, config.name);

            // Test Google access if basic test passed
            boolean googleAccessible = false;
            if (basicSuccess) {
                googleAccessible = testGoogleThroughSocks(testPort, config.name);
            }

            configFile.delete();
            return new boolean[]{basicSuccess, googleAccessible};

        } catch (Exception e) {
            Log.w(TAG, "V2Ray test error for " + config.name + ": " + e.getMessage());
            return new boolean[]{false, false};
        } finally {
            if (xrayProcess != null) {
                try {
                    xrayProcess.destroyForcibly();
                } catch (Exception ignored) {}
            }
        }
    }

    /**
     * Quick TCP connect test to proxy server. Filters out clearly dead servers
     * before spawning an expensive XRay process.
     */
    private boolean quickTcpCheck(ConfigManager.ConfigItem config) {
        try {
            String uri = config.uri;
            String host = null;
            int port = 443;
            String lower = uri.toLowerCase();

            if (lower.startsWith("vmess://")) {
                String base64 = uri.substring(8);
                if (base64.length() > 4096) return false;
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

            if (host == null || host.isEmpty()) return false;

            // Only TCP-check IP addresses. Domain names skip pre-filter because
            // local DNS is censored in Iran, but XRay can resolve via its own DNS config.
            if (!isIpAddress(host)) return true;

            Socket socket = new Socket();
            try {
                socket.connect(new InetSocketAddress(host, port), TCP_PREFILTER_TIMEOUT_MS);
                return true;
            } finally {
                try { socket.close(); } catch (Exception ignored) {}
            }
        } catch (Exception e) {
            return false;
        }
    }

    private static boolean isIpAddress(String host) {
        // Quick check: if first char is digit or colon (IPv6), it's likely an IP
        if (host.isEmpty()) return false;
        char c = host.charAt(0);
        if (c == '[') return true; // IPv6 bracket notation
        if (Character.isDigit(c)) {
            // Check for IPv4 pattern (digits and dots only)
            for (int i = 0; i < host.length(); i++) {
                char ch = host.charAt(i);
                if (ch != '.' && !Character.isDigit(ch)) return false;
            }
            return true;
        }
        return host.contains(":"); // IPv6
    }

    /**
     * Wait for a SOCKS port to become alive, polling every 100ms.
     * Returns as soon as port is connectable, or false after maxWaitMs.
     */
    private boolean waitForPort(int port, Process process, int maxWaitMs) {
        long deadline = System.currentTimeMillis() + maxWaitMs;
        while (System.currentTimeMillis() < deadline) {
            if (!process.isAlive()) return false;
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress("127.0.0.1", port), 100);
                return true;
            } catch (Exception ignored) {}
            try { Thread.sleep(100); } catch (InterruptedException e) { return false; }
        }
        return false;
    }

    /**
     * Test basic HTTP connectivity through SOCKS5 proxy.
     * Uses raw SOCKS5 protocol with domain name (ATYP=0x03) so DNS is resolved
     * remotely by XRay, bypassing local censored DNS on Iranian networks.
     */
    private boolean testHttpThroughSocks(int socksPort, String configName) {
        Socket socket = null;
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", socksPort), V2RAY_TEST_TIMEOUT_MS);
            socket.setSoTimeout(V2RAY_TEST_TIMEOUT_MS);

            OutputStream os = socket.getOutputStream();
            InputStream is = socket.getInputStream();

            // SOCKS5 greeting: VER=5, NMETHODS=1, METHOD=0 (no auth)
            os.write(new byte[]{0x05, 0x01, 0x00});
            os.flush();
            int ver = is.read();
            int method = is.read();
            if (ver != 0x05 || method != 0x00) {
                Log.d(TAG, "SOCKS5 handshake FAIL for " + configName + ": ver=" + ver + " method=" + method);
                return false;
            }

            // SOCKS5 CONNECT using domain name (ATYP=0x03) — avoids local DNS
            String host = "cp.cloudflare.com";
            int port = 80;
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

            // Read SOCKS5 reply
            int rVer = is.read();
            int rep = is.read();
            is.read(); // RSV
            int atyp = is.read();
            if (rVer != 0x05 || rep != 0x00) {
                // rep codes: 1=general failure, 2=not allowed, 3=network unreachable,
                // 4=host unreachable, 5=connection refused, 6=TTL expired, 7=cmd not supported
                Log.d(TAG, "SOCKS5 CONNECT rejected for " + configName + ": rep=" + rep + " (proxy can't reach cp.cloudflare.com)");
                return false;
            }

            // Skip BND.ADDR + BND.PORT
            skipSocksAddress(is, atyp);
            readNBytes(is, 2);

            // Send HTTP request
            String http = "GET /generate_204 HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
            os.write(http.getBytes(StandardCharsets.US_ASCII));
            os.flush();

            // Read response
            byte[] buf = new byte[256];
            int n = is.read(buf);
            if (n <= 0) {
                Log.d(TAG, "No HTTP response for " + configName + " (proxy connected but no data)");
                return false;
            }
            String head = new String(buf, 0, n, StandardCharsets.US_ASCII);
            boolean ok = head.startsWith("HTTP/") && (head.contains(" 204") || head.contains(" 200"));
            if (!ok) {
                String firstLine = head.contains("\r\n") ? head.substring(0, head.indexOf("\r\n")) : head.substring(0, Math.min(head.length(), 80));
                Log.d(TAG, "HTTP test FAIL for " + configName + ": " + firstLine);
            } else {
                Log.i(TAG, "HTTP test PASS for " + configName + " on port " + socksPort);
            }
            return ok;

        } catch (java.net.SocketTimeoutException e) {
            Log.d(TAG, "SOCKS test TIMEOUT for " + configName + " on port " + socksPort);
            return false;
        } catch (java.net.ConnectException e) {
            Log.d(TAG, "SOCKS port not listening for " + configName + " on port " + socksPort);
            return false;
        } catch (Exception e) {
            Log.d(TAG, "SOCKS test ERROR for " + configName + ": " + e.getClass().getSimpleName() + ": " + e.getMessage());
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
            socket.connect(new InetSocketAddress("127.0.0.1", socksPort), V2RAY_TEST_TIMEOUT_MS);
            socket.setSoTimeout(V2RAY_TEST_TIMEOUT_MS);

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
     * Skip a SOCKS5 BND.ADDR based on address type.
     */
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

    /**
     * Read exactly count bytes from the stream.
     */
    private void readNBytes(InputStream is, int count) throws IOException {
        byte[] buf = new byte[count];
        int off = 0;
        while (off < count) {
            int n = is.read(buf, off, count - off);
            if (n < 0) throw new IOException("EOF");
            off += n;
        }
    }

    /**
     * Generate a minimal V2Ray JSON config for testing a single URI.
     */
    private String generateTestConfig(String uri, int socksPort) {
        try {
            // Use VpnService's URI parsers via a static helper
            JSONObject outbound = V2RayConfigHelper.parseUriToOutbound(uri);
            if (outbound == null) return null;

            JSONObject config = new JSONObject();

            // Log
            JSONObject log = new JSONObject();
            log.put("loglevel", "warning");
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

            // Sniffing helps XRay detect destination domains from TLS SNI
            JSONObject sniffing = new JSONObject();
            sniffing.put("enabled", true);
            sniffing.put("destOverride", new JSONArray().put("http").put("tls"));
            sniffing.put("routeOnly", true);
            socksIn.put("sniffing", sniffing);

            inbounds.put(socksIn);
            config.put("inbounds", inbounds);

            // Outbounds: proxy + direct (needed for routing private IPs)
            JSONArray outbounds = new JSONArray();
            outbounds.put(outbound);

            JSONObject direct = new JSONObject();
            direct.put("tag", "direct");
            direct.put("protocol", "freedom");
            outbounds.put(direct);

            config.put("outbounds", outbounds);

            // Routing - route private IPs and DNS traffic directly
            JSONObject routing = new JSONObject();
            routing.put("domainStrategy", "IPIfNonMatch");
            JSONArray rules = new JSONArray();

            // CRITICAL: Route XRay's internal DNS module queries direct.
            // Without "tag" in DNS config + this rule, DNS queries bypass routing
            // and go to the first outbound (proxy), creating a chicken-and-egg loop.
            JSONObject directDnsTag = new JSONObject();
            directDnsTag.put("type", "field");
            directDnsTag.put("inboundTag", new JSONArray().put("dns-internal"));
            directDnsTag.put("outboundTag", "direct");
            rules.put(directDnsTag);

            // Also catch any other port-53 traffic and send direct
            JSONObject directDns = new JSONObject();
            directDns.put("type", "field");
            directDns.put("port", "53");
            directDns.put("outboundTag", "direct");
            rules.put(directDns);

            JSONObject directPrivate = new JSONObject();
            directPrivate.put("type", "field");
            directPrivate.put("ip", new JSONArray()
                .put("192.168.0.0/16").put("172.16.0.0/12")
                .put("10.0.0.0/8").put("127.0.0.0/8")
                .put("178.22.122.100").put("78.157.42.100")
                .put("1.1.1.1").put("8.8.8.8"));
            directPrivate.put("outboundTag", "direct");
            rules.put(directPrivate);
            routing.put("rules", rules);
            config.put("routing", routing);

            // DNS - needed to resolve proxy server domains on censored networks
            JSONObject dns = new JSONObject();
            JSONArray servers = new JSONArray();
            servers.put("178.22.122.100"); // Shecan (Iranian DNS, fast & uncensored for foreign domains)
            servers.put("78.157.42.100");  // Electro (Iranian DNS)
            servers.put("1.1.1.1");        // Cloudflare
            servers.put("8.8.8.8");        // Google
            dns.put("servers", servers);
            dns.put("queryStrategy", "UseIPv4");
            dns.put("disableCache", false);
            dns.put("tag", "dns-internal");
            config.put("dns", dns);

            return config.toString();

        } catch (Exception e) {
            Log.w(TAG, "Error generating test config", e);
            return null;
        }
    }

    /**
     * Fallback: TCP connect test to server host:port.
     */
    private boolean testTcpReachability(ConfigManager.ConfigItem config) {
        try {
            String uri = config.uri;
            String host = null;
            int port = 443;

            if (uri.startsWith("vmess://")) {
                String base64 = uri.substring(8);
                if (base64.length() > 4096) return false;
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

            if (host == null || host.isEmpty()) return false;

            Socket socket = new Socket();
            try {
                socket.connect(new InetSocketAddress(host, port), TCP_TEST_TIMEOUT_MS);
                return true;
            } finally {
                try { socket.close(); } catch (Exception ignored) {}
            }
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * Get working configs sorted by latency (fastest first).
     */
    public List<ConfigManager.ConfigItem> getWorkingConfigs(List<ConfigManager.ConfigItem> allConfigs) {
        List<ConfigManager.ConfigItem> working = new ArrayList<>();
        for (ConfigManager.ConfigItem config : allConfigs) {
            if (config.latency > 0 && config.latency < 15000) {
                working.add(config);
            }
        }
        Collections.sort(working, (a, b) -> Integer.compare(a.latency, b.latency));
        return working;
    }

    /**
     * Clear expired cache entries
     */
    public void cleanCache() {
        resultCache.entrySet().removeIf(entry -> entry.getValue().isExpired());
        Log.d(TAG, "Cleaned expired cache entries, remaining: " + resultCache.size());
    }

    /**
     * Get cache statistics
     */
    public int getCacheSize() {
        return resultCache.size();
    }

    /**
     * Get active queue sizes
     */
    public String getQueueStatus() {
        return String.format("Queues - VMess:%d, VLESS:%d, SS:%d, Other:%d",
            vmessQueue.size(), vlessQueue.size(), shadowsocksQueue.size(), otherQueue.size());
    }

    public boolean isRunning() {
        return isRunning;
    }

    public void stop() {
        stopRequested = true;
        isRunning = false;
        // Clear queues
        vmessQueue.clear();
        vlessQueue.clear();
        shadowsocksQueue.clear();
        otherQueue.clear();
    }

    public void stopTesting() {
        isRunning = false;
    }

}
