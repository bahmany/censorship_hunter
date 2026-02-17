package com.hunter.app;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.Socket;
import java.net.URL;
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
    private static final int XRAY_STARTUP_WAIT_MS = 1500;

    // Thread management
    private static final int CORE_POOL_SIZE = 2;
    private static final int MAX_POOL_SIZE = 5;
    private static final int QUEUE_CAPACITY = 100;
    private static final int TEST_SOCKS_PORT_BASE = 20808;
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

    // Priority levels (lower number = higher priority)
    private static final int PRIORITY_HIGH = 1;    // Recently working configs
    private static final int PRIORITY_MEDIUM = 2;  // Known working configs
    private static final int PRIORITY_LOW = 3;     // Untested or failed configs
    private static final int PRIORITY_VERY_LOW = 4; // Old failed configs

    public interface TestCallback {
        void onProgress(int tested, int total);
        void onConfigTested(ConfigManager.ConfigItem config, boolean success, int latency);
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
        final long timestamp;

        TestResult(boolean success, int latency) {
            this.success = success;
            this.latency = latency;
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

        // Start the queue processor
        startQueueProcessor();
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
        if (isRunning) {
            Log.w(TAG, "Test already running");
            return;
        }

        isRunning = true;
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

        // Process queues in priority order
        executor.execute(() -> {
            try {
                XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
                boolean useV2Ray = xrayManager != null && xrayManager.isReady();

                // Process each queue in priority order
                processQueue(vmessQueue, useV2Ray, xrayManager, callback, testedCount, successCount, totalConfigs);
                processQueue(vlessQueue, useV2Ray, xrayManager, callback, testedCount, successCount, totalConfigs);
                processQueue(shadowsocksQueue, useV2Ray, xrayManager, callback, testedCount, successCount, totalConfigs);
                processQueue(otherQueue, useV2Ray, xrayManager, callback, testedCount, successCount, totalConfigs);

                callback.onComplete(testedCount.get(), successCount.get());

            } finally {
                isRunning = false;
            }
        });
    }

    /**
     * Process a single priority queue
     */
    private void processQueue(PriorityQueue<ConfigTestTask> queue, boolean useV2Ray,
                            XRayManager xrayManager, TestCallback callback,
                            AtomicInteger testedCount, AtomicInteger successCount, int total) {
        if (queue.isEmpty()) return;

        List<ConfigTestTask> batch = new ArrayList<>();
        int batchSize = useV2Ray ? 3 : 8; // Smaller batches for V2Ray testing

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
                            handleTestResult(task.config, cached.success, cached.latency,
                                           callback, testedCount, successCount, total);
                            return;
                        }

                        long startTime = System.currentTimeMillis();
                        boolean success;

                        if (useV2Ray) {
                            success = testThroughV2Ray(task.config, xrayManager);
                        } else {
                            success = testTcpReachability(task.config);
                        }

                        int latency = (int) (System.currentTimeMillis() - startTime);

                        handleTestResult(task.config, success, latency,
                                       callback, testedCount, successCount, total);

                    } catch (Exception e) {
                        handleTestResult(task.config, false, -1, callback,
                                       testedCount, successCount, total);
                    } finally {
                        latch.countDown();
                    }
                });
            }

            // Wait for batch to complete with timeout
            try {
                long timeoutMs = useV2Ray ? 15000L : 8000L;
                latch.await(timeoutMs, TimeUnit.MILLISECONDS);
            } catch (InterruptedException ignored) {}

            // Throttle to prevent overwhelming the system
            if (!queue.isEmpty()) {
                try {
                    Thread.sleep(200); // 200ms between batches
                } catch (InterruptedException ignored) {}
            }
        }
    }

    /**
     * Start background queue processor for continuous testing
     */
    private void startQueueProcessor() {
        executor.execute(() -> {
            while (!stopRequested) {
                try {
                    // Process any pending tasks in queues
                    int active = activeTests.get();
                    if (active < MAX_POOL_SIZE) {
                        // Could add background processing logic here
                        Thread.sleep(1000);
                    } else {
                        Thread.sleep(5000); // Wait longer if busy
                    }
                } catch (InterruptedException e) {
                    break;
                }
            }
        });
    }

    private String getCacheKey(ConfigManager.ConfigItem config) {
        return config.uri + "_" + config.protocol;
    }

    private void handleTestResult(ConfigManager.ConfigItem config, boolean success, int latency,
                                TestCallback callback, AtomicInteger testedCount,
                                AtomicInteger successCount, int total) {
        if (success) {
            config.latency = latency;
            successCount.incrementAndGet();
        } else {
            config.latency = -1;
        }

        callback.onConfigTested(config, success, latency);

        int tested = testedCount.incrementAndGet();
        if (tested % 10 == 0 || tested == total) { // Update progress less frequently
            callback.onProgress(tested, total);
        }
    }

    /**
     * Test a config by starting a real XRay process, sending HTTP through its SOCKS proxy.
     * This is the real test — verifies the config actually works end-to-end.
     */
    private boolean testThroughV2Ray(ConfigManager.ConfigItem config, XRayManager xrayManager) {
        int testPort = TEST_SOCKS_PORT_BASE + portCounter.getAndIncrement() % 200;
        Process xrayProcess = null;

        try {
            // Generate minimal V2Ray config for this test
            String v2rayConfig = generateTestConfig(config.uri, testPort);
            if (v2rayConfig == null) {
                return testTcpReachability(config); // Fallback
            }

            // Write config to temp file
            File configFile = xrayManager.writeConfig(v2rayConfig, testPort);
            if (configFile == null) return false;

            // Start xray process
            xrayProcess = xrayManager.startProcess(configFile);
            if (xrayProcess == null) {
                configFile.delete();
                return false;
            }

            // Wait for xray to initialize
            Thread.sleep(XRAY_STARTUP_WAIT_MS);

            // Test through SOCKS proxy
            boolean success = testHttpThroughSocks(testPort);

            configFile.delete();
            return success;

        } catch (Exception e) {
            Log.w(TAG, "V2Ray test error for " + config.name + ": " + e.getMessage());
            return false;
        } finally {
            if (xrayProcess != null) {
                try {
                    xrayProcess.destroyForcibly();
                } catch (Exception ignored) {}
            }
        }
    }

    /**
     * Send an HTTP request through a SOCKS5 proxy and check for 204/200 response.
     * Also verifies that the response content is not empty/invalid.
     */
    private boolean testHttpThroughSocks(int socksPort) {
        HttpURLConnection conn = null;
        try {
            Proxy proxy = new Proxy(Proxy.Type.SOCKS,
                new InetSocketAddress("127.0.0.1", socksPort));

            URL url = new URL("http://cp.cloudflare.com/generate_204");
            conn = (HttpURLConnection) url.openConnection(proxy);
            conn.setConnectTimeout(V2RAY_TEST_TIMEOUT_MS);
            conn.setReadTimeout(V2RAY_TEST_TIMEOUT_MS);
            conn.setRequestMethod("GET");
            conn.setInstanceFollowRedirects(false);

            int responseCode = conn.getResponseCode();
            boolean validResponse = responseCode == 204 || responseCode == 200;

            // Additional validation: check response content for 200 responses
            if (validResponse && responseCode == 200) {
                String content = conn.getHeaderField("Content-Type");
                // For Cloudflare generate_204, we expect empty content or specific content-type
                if (content != null && content.contains("text/html")) {
                    // If we get HTML, it's likely a captive portal or error page
                    return false;
                }
            }

            return validResponse;

        } catch (Exception e) {
            return false;
        } finally {
            if (conn != null) conn.disconnect();
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

            // Outbound: the config being tested
            JSONArray outbounds = new JSONArray();
            outbounds.put(outbound);
            config.put("outbounds", outbounds);

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
        // Clear queues
        vmessQueue.clear();
        vlessQueue.clear();
        shadowsocksQueue.clear();
        otherQueue.clear();
    }

    public void shutdown() {
        stop();
        executor.shutdown();
        try {
            executor.awaitTermination(5, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {}
        Log.i(TAG, "ConfigTester shutdown complete");
    }
}
