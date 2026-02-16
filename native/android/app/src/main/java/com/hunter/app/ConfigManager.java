package com.hunter.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Base64;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Memory-efficient config manager with streaming parsing, bounded thread pool,
 * tiered caching (top 100 tested + file-based raw cache), and crisis mode.
 */
public class ConfigManager {
    private static final String TAG = "ConfigManager";
    private static final String PREFS_NAME = "config_cache_v2";
    private static final String KEY_TOP_CONFIGS = "top_configs";
    private static final String KEY_LAST_UPDATE = "last_update";
    private static final String RAW_CACHE_DIR = "raw_sources";
    private static final String ALL_URIS_FILE = "all_uris.txt";

    private static final int CONNECT_TIMEOUT = 8000;
    private static final int READ_TIMEOUT = 12000;
    private static final int MAX_DOWNLOAD_BYTES = 512 * 1024; // 512KB per source
    private static final int MAX_CONCURRENT_DOWNLOADS = 6;
    private static final int TOP_CACHE_SIZE = 100;
    private static final int MAX_MEMORY_CONFIGS = 100000; // Cap configs kept in RAM

    // Priority 1: Iran-specific Reality configs
    private static final String[] IRAN_PRIORITY_SOURCES = {
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/reality",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/reality",
        "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/reality.txt",
        "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/reality",
        "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/reality.txt",
        "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/vless.txt",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
        "https://raw.githubusercontent.com/mahdibland/SSAggregator/master/sub/sub_merge.txt",
        "https://raw.githubusercontent.com/sarinaesmailzadeh/V2Hub/main/merged_base64",
        "https://raw.githubusercontent.com/LalatinaHub/Starter/main/Starter",
        "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
        "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/vless"
    };

    // Priority 2: Anti-censorship sources
    private static final String[] ANTI_CENSORSHIP_SOURCES = {
        "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge_base64.txt",
        "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub1.txt",
        "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub2.txt",
        "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub3.txt",
        "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vmess",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/trojan",
        "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/configtg.txt",
        "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/trojan",
        "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vmess",
        "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/all.txt",
        "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/ss"
    };

    // Priority 3: General GitHub sources
    private static final String[] GITHUB_SOURCES = {
        "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/Epodonios/v2ray-configs/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt",
        "https://raw.githubusercontent.com/coldwater-10/V2ray-Config-Lite/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/all_sub.txt",
        "https://raw.githubusercontent.com/M-Mashreghi/Free-V2ray-Collector/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/NiREvil/vless/main/subscription.txt",
        "https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/sub.txt",
        "https://raw.githubusercontent.com/skywrt/v2ray-configs/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/longlon/v2ray-config/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/ebrasha/free-v2ray-public-list/main/all_extracted_configs.txt",
        "https://raw.githubusercontent.com/hamed1124/port-based-v2ray-configs/main/all.txt",
        "https://raw.githubusercontent.com/mostafasadeghifar/v2ray-config/main/configs.txt",
        "https://raw.githubusercontent.com/Ashkan-m/v2ray/main/Sub.txt",
        "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_iOS.txt",
        "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_STARTER.txt",
        "https://raw.githubusercontent.com/AzadNetCH/Clash/main/V2Ray.txt",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/mix",
        "https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray",
        "https://raw.githubusercontent.com/freefq/free/master/v2",
        "https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2",
        "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt",
        "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
        "https://raw.githubusercontent.com/vveg26/get_proxy/main/dist/v2ray.txt"
    };

    private final Context context;
    private final SharedPreferences prefs;
    private final File cacheDir;
    private final ExecutorService executor;
    private final List<ConfigItem> configs = Collections.synchronizedList(new ArrayList<>());
    private final List<ConfigItem> topConfigs = Collections.synchronizedList(new ArrayList<>());
    private volatile boolean isRefreshing = false;

    public interface RefreshCallback {
        void onProgress(String message, int current, int total);
        void onComplete(int totalConfigs);
        void onError(String error);
    }

    public ConfigManager(Context context) {
        this.context = context.getApplicationContext();
        this.prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        this.cacheDir = new File(context.getFilesDir(), RAW_CACHE_DIR);
        if (!cacheDir.exists()) cacheDir.mkdirs();

        // Bounded thread pool: core=2, max=6, idle threads die after 30s
        this.executor = new ThreadPoolExecutor(
            2, 6, 30, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(50),
            new ThreadPoolExecutor.CallerRunsPolicy()
        );

        // Clear old prefs format
        SharedPreferences oldPrefs = context.getSharedPreferences("config_cache", Context.MODE_PRIVATE);
        if (oldPrefs.contains("cached_configs")) {
            oldPrefs.edit().clear().apply();
            Log.i(TAG, "Cleared old oversized cache");
        }

        loadTopConfigsFromCache();
        loadAllUrisFromFile();
        Log.i(TAG, "Init: " + configs.size() + " configs, " + topConfigs.size() + " top cached");
    }

    public List<ConfigItem> getConfigs() {
        synchronized (configs) {
            return new ArrayList<>(configs);
        }
    }

    public List<ConfigItem> getTopConfigs() {
        synchronized (topConfigs) {
            return new ArrayList<>(topConfigs);
        }
    }

    public int getConfigCount() {
        return configs.size();
    }

    public int getTopConfigCount() {
        return topConfigs.size();
    }

    /**
     * Update top configs cache with tested & working configs (sorted by latency).
     */
    public void updateTopConfigs(List<ConfigItem> tested) {
        synchronized (topConfigs) {
            topConfigs.clear();
            int limit = Math.min(tested.size(), TOP_CACHE_SIZE);
            for (int i = 0; i < limit; i++) {
                topConfigs.add(tested.get(i));
            }
        }
        saveTopConfigsToCache();
        Log.i(TAG, "Top configs updated: " + topConfigs.size());
    }

    public void refreshConfigs(RefreshCallback callback) {
        if (isRefreshing) {
            callback.onError("Already refreshing");
            return;
        }

        isRefreshing = true;
        executor.execute(() -> {
            try {
                // Thread-safe collection for URIs found across all sources
                ConcurrentLinkedQueue<String> foundUris = new ConcurrentLinkedQueue<>();
                AtomicInteger progress = new AtomicInteger(0);

                List<String> allSources = new ArrayList<>();
                allSources.addAll(Arrays.asList(IRAN_PRIORITY_SOURCES));
                allSources.addAll(Arrays.asList(ANTI_CENSORSHIP_SOURCES));
                allSources.addAll(Arrays.asList(GITHUB_SOURCES));

                int total = allSources.size();
                callback.onProgress("Fetching from " + total + " sources...", 0, total);

                // Semaphore limits concurrent downloads to MAX_CONCURRENT_DOWNLOADS
                Semaphore semaphore = new Semaphore(MAX_CONCURRENT_DOWNLOADS);
                List<Thread> threads = new ArrayList<>();

                for (int si = 0; si < allSources.size(); si++) {
                    final String source = allSources.get(si);
                    final int sourceIndex = si;
                    Thread t = new Thread(() -> {
                        try {
                            semaphore.acquire();
                            streamParseSource(source, sourceIndex, foundUris);
                        } catch (InterruptedException ignored) {
                        } finally {
                            semaphore.release();
                            int p = progress.incrementAndGet();
                            callback.onProgress("Fetching configs...", p, total);
                        }
                    });
                    t.setDaemon(true);
                    threads.add(t);
                    t.start();
                }

                // Wait with timeout
                for (Thread t : threads) {
                    try { t.join(25000); } catch (InterruptedException ignored) {}
                }
                // Interrupt any stragglers
                for (Thread t : threads) {
                    if (t.isAlive()) t.interrupt();
                }

                // Deduplicate using LinkedHashSet (preserves insertion order)
                Set<String> deduped = new LinkedHashSet<>(foundUris);
                Log.i(TAG, "Found " + deduped.size() + " unique URIs from " + total + " sources");

                // Parse into ConfigItems
                List<ConfigItem> newConfigs = new ArrayList<>();
                for (String uri : deduped) {
                    ConfigItem item = parseUri(uri);
                    if (item != null) {
                        newConfigs.add(item);
                    }
                }

                // Deduplicate by server address
                LinkedHashMap<String, ConfigItem> serverMap = new LinkedHashMap<>();
                for (ConfigItem item : newConfigs) {
                    String key = getServerKey(item);
                    if (key != null && !serverMap.containsKey(key)) {
                        serverMap.put(key, item);
                    }
                }
                newConfigs = new ArrayList<>(serverMap.values());

                // Sort by protocol priority
                Collections.sort(newConfigs, (a, b) -> {
                    int pa = getProtocolPriority(a.protocol);
                    int pb = getProtocolPriority(b.protocol);
                    return pb - pa;
                });

                // Replace configs atomically (cap in memory, all saved to file)
                synchronized (configs) {
                    configs.clear();
                    int limit = Math.min(newConfigs.size(), MAX_MEMORY_CONFIGS);
                    configs.addAll(newConfigs.subList(0, limit));
                }
                Log.i(TAG, "Kept " + configs.size() + "/" + newConfigs.size() + " in memory");

                // Save all URIs to file (for crisis mode)
                saveAllUrisToFile(deduped);

                isRefreshing = false;
                callback.onComplete(configs.size());

            } catch (Exception e) {
                isRefreshing = false;
                callback.onError(e.getMessage());
            }
        });
    }

    /**
     * Stream-parse a source URL: read line by line, extract URIs immediately,
     * never hold entire response in memory. Also caches raw content to file.
     */
    private void streamParseSource(String urlStr, int sourceIndex,
                                   ConcurrentLinkedQueue<String> foundUris) {
        HttpURLConnection conn = null;
        BufferedReader reader = null;
        InputStream rawStream = null;
        FileOutputStream fos = null;
        try {
            URL url = new URL(urlStr);
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(CONNECT_TIMEOUT);
            conn.setReadTimeout(READ_TIMEOUT);
            conn.setRequestProperty("User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
            conn.setInstanceFollowRedirects(true);

            int code = conn.getResponseCode();
            if (code == 200) {
                rawStream = conn.getInputStream();
            } else {
                rawStream = conn.getErrorStream();
                return;
            }

            reader = new BufferedReader(new InputStreamReader(rawStream), 8192);

            // Cache raw source to file
            File cacheFile = new File(cacheDir, "source_" + sourceIndex + ".txt");
            fos = new FileOutputStream(cacheFile);

            StringBuilder base64Buffer = null;
            boolean mightBeBase64 = true;
            int totalBytes = 0;
            String line;

            while ((line = reader.readLine()) != null) {
                totalBytes += line.length();
                if (totalBytes > MAX_DOWNLOAD_BYTES) break; // Hard limit

                // Write to cache file
                fos.write(line.getBytes("UTF-8"));
                fos.write('\n');

                String trimmed = line.trim();
                if (trimmed.isEmpty()) continue;

                // Try to extract URIs directly from this line
                if (extractUrisFromLine(trimmed, foundUris)) {
                    mightBeBase64 = false;
                } else if (mightBeBase64 && isBase64Line(trimmed)) {
                    // Accumulate for base64 decode
                    if (base64Buffer == null) base64Buffer = new StringBuilder(4096);
                    base64Buffer.append(trimmed);
                }
            }

            // If content looked like base64, decode and extract
            if (mightBeBase64 && base64Buffer != null && base64Buffer.length() > 0) {
                try {
                    byte[] decoded = Base64.decode(
                        base64Buffer.toString(), Base64.DEFAULT);
                    base64Buffer = null; // Free memory immediately
                    String decodedStr = new String(decoded, "UTF-8");
                    decoded = null; // Free byte array

                    // Parse decoded content line by line
                    for (String dLine : decodedStr.split("\n")) {
                        extractUrisFromLine(dLine.trim(), foundUris);
                    }
                    decodedStr = null; // Free
                } catch (Exception ignored) {}
            }

        } catch (Exception e) {
            Log.w(TAG, "Stream error: " + urlStr + " - " + e.getMessage());
        } finally {
            // Close reader FIRST to release the input stream, preventing OkHttp leak
            try { if (reader != null) reader.close(); } catch (Exception ignored) {}
            try { if (rawStream != null) rawStream.close(); } catch (Exception ignored) {}
            try { if (fos != null) fos.close(); } catch (Exception ignored) {}
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private boolean extractUrisFromLine(String line, ConcurrentLinkedQueue<String> uris) {
        if (line.length() < 8) return false;
        boolean found = false;
        String lower = line.toLowerCase();

        // Fast check: does this line contain a protocol prefix?
        if (lower.contains("vmess://") || lower.contains("vless://") ||
            lower.contains("trojan://") || lower.contains("ss://") ||
            lower.contains("hysteria2://") || lower.contains("hy2://")) {

            // Split by whitespace to handle multiple URIs per line
            String[] parts = line.split("[\\s]+");
            for (String part : parts) {
                String trimmed = part.trim();
                if (isValidUri(trimmed)) {
                    uris.add(trimmed);
                    found = true;
                }
            }
        }
        return found;
    }

    private boolean isBase64Line(String line) {
        if (line.length() < 4) return false;
        for (int i = 0; i < Math.min(line.length(), 100); i++) {
            char c = line.charAt(i);
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
                return false;
            }
        }
        return true;
    }

    private boolean isValidUri(String uri) {
        if (uri == null || uri.length() < 10) return false;
        String lower = uri.toLowerCase();
        return lower.startsWith("vmess://") ||
               lower.startsWith("vless://") ||
               lower.startsWith("trojan://") ||
               lower.startsWith("ss://") ||
               lower.startsWith("hysteria2://") ||
               lower.startsWith("hy2://");
    }

    private int getProtocolPriority(String protocol) {
        if (protocol == null) return 0;
        switch (protocol.toLowerCase()) {
            case "vless": return 10;
            case "vmess": return 8;
            case "trojan": return 6;
            case "hysteria2":
            case "hy2": return 5;
            case "ss": return 4;
            default: return 1;
        }
    }

    private ConfigItem parseUri(String uri) {
        try {
            ConfigItem item = new ConfigItem();
            item.uri = uri;

            if (uri.startsWith("vmess://")) {
                item.protocol = "VMess";
                item.name = parseVmessName(uri);
            } else if (uri.startsWith("vless://")) {
                item.protocol = "VLESS";
                item.name = parseStandardName(uri);
            } else if (uri.startsWith("trojan://")) {
                item.protocol = "Trojan";
                item.name = parseStandardName(uri);
            } else if (uri.startsWith("ss://")) {
                item.protocol = "Shadowsocks";
                item.name = parseStandardName(uri);
            } else if (uri.startsWith("hysteria2://") || uri.startsWith("hy2://")) {
                item.protocol = "Hysteria2";
                item.name = parseStandardName(uri);
            } else {
                return null;
            }

            if (item.name == null || item.name.isEmpty()) {
                item.name = item.protocol + " Server";
            }
            item.latency = -1;
            return item;
        } catch (Exception e) {
            return null;
        }
    }

    private String parseVmessName(String uri) {
        try {
            String base64 = uri.substring(8);
            // Limit decode size to prevent OOM on malformed URIs
            if (base64.length() > 4096) base64 = base64.substring(0, 4096);
            String json = new String(Base64.decode(base64, Base64.NO_WRAP));
            JSONObject obj = new JSONObject(json);
            return obj.optString("ps", "VMess Server");
        } catch (Exception e) {
            return "VMess Server";
        }
    }

    private String parseStandardName(String uri) {
        try {
            int hashIndex = uri.lastIndexOf('#');
            if (hashIndex > 0 && hashIndex < uri.length() - 1) {
                return java.net.URLDecoder.decode(uri.substring(hashIndex + 1), "UTF-8");
            }
        } catch (Exception ignored) {}
        return null;
    }

    private String getServerKey(ConfigItem item) {
        try {
            String uri = item.uri;
            if (uri.startsWith("vmess://")) {
                // Parse vmess
                String base64 = uri.substring(8);
                if (base64.length() > 4096) return null;
                String json = new String(Base64.decode(base64, Base64.NO_WRAP));
                JSONObject obj = new JSONObject(json);
                return obj.getString("add") + ":" + obj.getInt("port");
            } else if (uri.startsWith("vless://") || uri.startsWith("trojan://") || uri.startsWith("ss://")) {
                // Parse standard
                String withoutScheme = uri.substring(uri.indexOf("://") + 3);
                String[] parts = withoutScheme.split("@");
                if (parts.length < 2) return null;
                String[] hostParams = parts[1].split("\\?");
                String[] hostPort = hostParams[0].split(":");
                String host = hostPort[0];
                int port = hostPort.length > 1 ? Integer.parseInt(hostPort[1]) : 443;
                return host + ":" + port;
            } else if (uri.startsWith("hysteria2://") || uri.startsWith("hy2://")) {
                // Similar to vless
                String withoutScheme = uri.substring(uri.indexOf("://") + 3);
                String[] parts = withoutScheme.split("@");
                if (parts.length < 2) return null;
                String[] hostParams = parts[1].split("\\?");
                String[] hostPort = hostParams[0].split(":");
                String host = hostPort[0];
                int port = hostPort.length > 1 ? Integer.parseInt(hostPort[1]) : 443;
                return host + ":" + port;
            }
        } catch (Exception ignored) {}
        return null;
    }

    // ==================== Tiered Caching ====================

    /**
     * Save top 100 tested configs to SharedPreferences (small, fast).
     */
    private void saveTopConfigsToCache() {
        try {
            JSONArray arr = new JSONArray();
            synchronized (topConfigs) {
                for (ConfigItem item : topConfigs) {
                    JSONObject obj = new JSONObject();
                    obj.put("uri", item.uri);
                    obj.put("ps", item.name);
                    obj.put("protocol", item.protocol);
                    obj.put("latency_ms", item.latency);
                    arr.put(obj);
                }
            }
            prefs.edit()
                .putString(KEY_TOP_CONFIGS, arr.toString())
                .putLong(KEY_LAST_UPDATE, System.currentTimeMillis())
                .apply();
        } catch (Exception e) {
            Log.w(TAG, "Error saving top configs", e);
        }
    }

    private void loadTopConfigsFromCache() {
        try {
            String json = prefs.getString(KEY_TOP_CONFIGS, "[]");
            if (json.length() > 200000) { // Safety check
                prefs.edit().remove(KEY_TOP_CONFIGS).apply();
                return;
            }
            JSONArray arr = new JSONArray(json);
            synchronized (topConfigs) {
                topConfigs.clear();
                int limit = Math.min(arr.length(), TOP_CACHE_SIZE);
                for (int i = 0; i < limit; i++) {
                    JSONObject obj = arr.getJSONObject(i);
                    ConfigItem item = new ConfigItem();
                    item.uri = obj.getString("uri");
                    item.name = obj.optString("ps", "Server");
                    item.protocol = obj.optString("protocol", "Unknown");
                    item.latency = obj.optInt("latency_ms", -1);
                    topConfigs.add(item);
                }
            }
            Log.i(TAG, "Loaded " + topConfigs.size() + " top cached configs");
        } catch (Exception e) {
            Log.w(TAG, "Error loading top configs", e);
        }
    }

    /**
     * Save all URIs to a file (one per line) for crisis mode recovery.
     */
    private void saveAllUrisToFile(Set<String> uris) {
        FileOutputStream fos = null;
        try {
            File file = new File(cacheDir, ALL_URIS_FILE);
            fos = new FileOutputStream(file);
            for (String uri : uris) {
                fos.write(uri.getBytes("UTF-8"));
                fos.write('\n');
            }
            Log.i(TAG, "Saved " + uris.size() + " URIs to file cache");
        } catch (Exception e) {
            Log.w(TAG, "Error saving URIs file", e);
        } finally {
            try { if (fos != null) fos.close(); } catch (Exception ignored) {}
        }
    }

    /**
     * Load all URIs from file cache (crisis mode / offline).
     */
    private void loadAllUrisFromFile() {
        File file = new File(cacheDir, ALL_URIS_FILE);
        if (!file.exists()) return;

        BufferedReader reader = null;
        try {
            reader = new BufferedReader(new InputStreamReader(new FileInputStream(file)), 8192);
            String line;
            int count = 0;
            synchronized (configs) {
                while ((line = reader.readLine()) != null && count < MAX_MEMORY_CONFIGS) {
                    String trimmed = line.trim();
                    if (isValidUri(trimmed)) {
                        ConfigItem item = parseUri(trimmed);
                        if (item != null) {
                            configs.add(item);
                            count++;
                        }
                    }
                }
            }
            Log.i(TAG, "Loaded " + count + " configs from file cache (max " + MAX_MEMORY_CONFIGS + ")");
        } catch (Exception e) {
            Log.w(TAG, "Error loading URIs file", e);
        } finally {
            try { if (reader != null) reader.close(); } catch (Exception ignored) {}
        }
    }

    /**
     * Get all raw source cache files for sharing.
     */
    public File[] getRawCacheFiles() {
        return cacheDir.listFiles();
    }

    /**
     * Get the all-URIs file for sharing.
     */
    public File getAllUrisFile() {
        return new File(cacheDir, ALL_URIS_FILE);
    }

    public long getLastUpdateTime() {
        return prefs.getLong(KEY_LAST_UPDATE, 0);
    }

    public boolean isRefreshing() {
        return isRefreshing;
    }

    public void shutdown() {
        executor.shutdown();
        try {
            executor.awaitTermination(5, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {}
    }

    public static class ConfigItem {
        public String uri;
        public String name;
        public String protocol;
        public volatile int latency;
    }
}
