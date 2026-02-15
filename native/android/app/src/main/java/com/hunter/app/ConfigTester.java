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
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Tests V2Ray configs by starting a real XRay process per config,
 * routing HTTP requests through its SOCKS5 proxy, and measuring latency.
 * Falls back to TCP connect test if XRay binary is not available.
 */
public class ConfigTester {
    private static final String TAG = "ConfigTester";
    private static final int V2RAY_TEST_TIMEOUT_MS = 8000;
    private static final int TCP_TEST_TIMEOUT_MS = 4000;
    private static final int XRAY_STARTUP_WAIT_MS = 1500;
    private static final int BATCH_SIZE = 5; // Small batches for V2Ray testing (each starts a process)
    private static final int MAX_TEST_THREADS = 3; // Limited: each thread runs an xray process
    private static final int TEST_SOCKS_PORT_BASE = 20808;
    private static final String TEST_URL = "http://cp.cloudflare.com/generate_204";

    private final ExecutorService executor;
    private volatile boolean isRunning = false;
    private volatile boolean stopRequested = false;
    private final AtomicInteger portCounter = new AtomicInteger(0);

    public interface TestCallback {
        void onProgress(int tested, int total);
        void onConfigTested(ConfigManager.ConfigItem config, boolean success, int latency);
        void onComplete(int totalTested, int successCount);
    }

    public ConfigTester() {
        this.executor = new ThreadPoolExecutor(
            2, MAX_TEST_THREADS, 30, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(50),
            new ThreadPoolExecutor.DiscardOldestPolicy()
        );
    }

    /**
     * Test configs in batches. If XRay is available, tests through real V2Ray proxy.
     * Otherwise falls back to TCP connect test.
     */
    public void testConfigs(List<ConfigManager.ConfigItem> configs, TestCallback callback) {
        if (isRunning) {
            Log.w(TAG, "Test already running");
            return;
        }

        isRunning = true;
        stopRequested = false;
        portCounter.set(0);

        executor.execute(() -> {
            try {
                XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
                boolean useV2Ray = xrayManager != null && xrayManager.isReady();

                int total = configs.size();
                AtomicInteger testedCount = new AtomicInteger(0);
                AtomicInteger successCount = new AtomicInteger(0);

                Log.i(TAG, "Starting test of " + total + " configs, V2Ray engine: " + useV2Ray);

                int batchSize = useV2Ray ? BATCH_SIZE : 20;

                for (int batchStart = 0; batchStart < total && !stopRequested; batchStart += batchSize) {
                    int batchEnd = Math.min(batchStart + batchSize, total);
                    int currentBatchSize = batchEnd - batchStart;

                    CountDownLatch latch = new CountDownLatch(currentBatchSize);

                    for (int i = batchStart; i < batchEnd; i++) {
                        final ConfigManager.ConfigItem config = configs.get(i);
                        final boolean v2ray = useV2Ray;

                        executor.execute(() -> {
                            try {
                                if (stopRequested) return;

                                long startTime = System.currentTimeMillis();
                                boolean success;

                                if (v2ray) {
                                    success = testThroughV2Ray(config, xrayManager);
                                } else {
                                    success = testTcpReachability(config);
                                }

                                int latency = (int) (System.currentTimeMillis() - startTime);

                                if (success) {
                                    config.latency = latency;
                                    successCount.incrementAndGet();
                                } else {
                                    config.latency = -1;
                                }

                                callback.onConfigTested(config, success, latency);
                            } catch (Exception e) {
                                config.latency = -1;
                                callback.onConfigTested(config, false, -1);
                            } finally {
                                testedCount.incrementAndGet();
                                callback.onProgress(testedCount.get(), total);
                                latch.countDown();
                            }
                        });
                    }

                    long waitTime = useV2Ray ?
                        (V2RAY_TEST_TIMEOUT_MS + XRAY_STARTUP_WAIT_MS) * 2L :
                        TCP_TEST_TIMEOUT_MS * 2L;
                    try {
                        latch.await(waitTime, TimeUnit.MILLISECONDS);
                    } catch (InterruptedException ignored) {}
                }

                callback.onComplete(testedCount.get(), successCount.get());

            } finally {
                isRunning = false;
            }
        });
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
     */
    private boolean testHttpThroughSocks(int socksPort) {
        HttpURLConnection conn = null;
        try {
            Proxy proxy = new Proxy(Proxy.Type.SOCKS,
                new InetSocketAddress("127.0.0.1", socksPort));

            URL url = new URL(TEST_URL);
            conn = (HttpURLConnection) url.openConnection(proxy);
            conn.setConnectTimeout(V2RAY_TEST_TIMEOUT_MS);
            conn.setReadTimeout(V2RAY_TEST_TIMEOUT_MS);
            conn.setRequestMethod("GET");
            conn.setInstanceFollowRedirects(false);

            int responseCode = conn.getResponseCode();
            return responseCode == 204 || responseCode == 200;

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

    public boolean isRunning() {
        return isRunning;
    }

    public void stop() {
        stopRequested = true;
    }

    public void shutdown() {
        stop();
        executor.shutdown();
        try {
            executor.awaitTermination(5, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {}
    }
}
