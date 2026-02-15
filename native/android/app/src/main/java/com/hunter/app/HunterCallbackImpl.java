package com.hunter.app;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.ResponseBody;

/**
 * Implementation of HunterNative.HunterCallback providing Android-specific
 * functionality to the native C++ layer.
 */
public class HunterCallbackImpl implements HunterNative.HunterCallback {
    private static final String TAG = "HunterCallback";

    private final Context context;
    private final OkHttpClient directClient;
    private final AtomicInteger proxyHandleCounter = new AtomicInteger(0);
    private final ConcurrentHashMap<Integer, Process> proxyProcesses = new ConcurrentHashMap<>();

    private String botToken = "";
    private String chatId = "";
    private String xrayBinaryPath = "";

    private ProgressListener progressListener;
    private LogListener logListener;
    private final java.util.LinkedList<String> logBuffer = new java.util.LinkedList<>();
    private static final int MAX_LOG_LINES = 500;
    private long lastLogUpdateTime = 0;
    private static final long LOG_UPDATE_THROTTLE_MS = 100;

    public interface ProgressListener {
        void onProgress(String phase, int current, int total);
        void onStatusUpdate(String statusJson);
    }

    public interface LogListener {
        void onLogUpdate(String logs);
    }

    public HunterCallbackImpl(Context context) {
        this.context = context;
        try {
            ApplicationInfo appInfo = context.getApplicationInfo();
            String libDir = appInfo.nativeLibraryDir;
            if (libDir != null) {
                this.xrayBinaryPath = libDir + "/libxray.so";
            } else {
                this.xrayBinaryPath = "";
            }
        } catch (Exception ignored) {
            this.xrayBinaryPath = "";
        }
        this.directClient = new OkHttpClient.Builder()
                .connectTimeout(10, TimeUnit.SECONDS)
                .readTimeout(15, TimeUnit.SECONDS)
                .writeTimeout(10, TimeUnit.SECONDS)
                .followRedirects(true)
                .followSslRedirects(true)
                .retryOnConnectionFailure(true)
                .build();
    }

    public void setBotToken(String token) { this.botToken = token; }
    public void setChatId(String id) { this.chatId = id; }
    public void setXrayBinaryPath(String path) { this.xrayBinaryPath = path; }
    public void setProgressListener(ProgressListener listener) { this.progressListener = listener; }
    public void setLogListener(LogListener listener) { this.logListener = listener; }

    private void addLog(String message) {
        synchronized (logBuffer) {
            logBuffer.add(message);
            if (logBuffer.size() > MAX_LOG_LINES) {
                logBuffer.removeFirst();
            }
        }
        notifyLogUpdate();
    }

    private void notifyLogUpdate() {
        long now = System.currentTimeMillis();
        if (logListener != null && (now - lastLogUpdateTime) >= LOG_UPDATE_THROTTLE_MS) {
            lastLogUpdateTime = now;
            StringBuilder sb = new StringBuilder();
            synchronized (logBuffer) {
                for (String line : logBuffer) {
                    sb.append(line).append("\n");
                }
            }
            logListener.onLogUpdate(sb.toString());
        }
    }

    public String getAllLogs() {
        StringBuilder sb = new StringBuilder();
        synchronized (logBuffer) {
            for (String line : logBuffer) {
                sb.append(line).append("\n");
            }
        }
        return sb.toString();
    }

    public void clearLogs() {
        synchronized (logBuffer) {
            logBuffer.clear();
        }
        if (logListener != null) {
            logListener.onLogUpdate("");
        }
    }

    @Override
    public String httpFetch(String url, String userAgent, int timeoutSeconds, String proxy) {
        addLog("HTTP GET: " + url);
        try {
            OkHttpClient client;
            if (proxy != null && !proxy.isEmpty()) {
                // Parse SOCKS5 proxy URL
                String proxyHost = "127.0.0.1";
                int proxyPort = 10808;
                try {
                    // Format: socks5://host:port
                    String stripped = proxy.replace("socks5://", "").replace("socks5h://", "");
                    String[] parts = stripped.split(":");
                    if (parts.length >= 2) {
                        proxyHost = parts[0];
                        proxyPort = Integer.parseInt(parts[1]);
                    }
                } catch (Exception e) {
                    Log.w(TAG, "Failed to parse proxy: " + proxy);
                }
                client = directClient.newBuilder()
                        .proxy(new Proxy(Proxy.Type.SOCKS,
                                new InetSocketAddress(proxyHost, proxyPort)))
                        .connectTimeout(timeoutSeconds, TimeUnit.SECONDS)
                        .readTimeout(timeoutSeconds, TimeUnit.SECONDS)
                        .build();
            } else {
                client = directClient.newBuilder()
                        .connectTimeout(timeoutSeconds, TimeUnit.SECONDS)
                        .readTimeout(timeoutSeconds, TimeUnit.SECONDS)
                        .build();
            }

            Request request = new Request.Builder()
                    .url(url)
                    .header("User-Agent", userAgent)
                    .build();

            try (Response response = client.newCall(request).execute()) {
                if (response.isSuccessful()) {
                    ResponseBody body = response.body();
                    addLog("HTTP GET success: " + url);
                    return body != null ? body.string() : "";
                }
            }
        } catch (Exception e) {
            addLog("HTTP GET error: " + url + " - " + e.getMessage());
            Log.d(TAG, "HTTP fetch failed for " + url + ": " + e.getMessage());
        }
        return "";
    }

    @Override
    public int startProxy(String configJson, int socksPort) {
        if (xrayBinaryPath == null || xrayBinaryPath.isEmpty()) {
            addLog("XRay binary path not set");
            Log.e(TAG, "XRay binary path not set");
            return -1;
        }

        try {
            // Write config to temp file
            java.io.File tempConfig = java.io.File.createTempFile(
                    "hunter_" + socksPort + "_", ".json", context.getCacheDir());
            java.io.FileWriter writer = new java.io.FileWriter(tempConfig);
            writer.write(configJson);
            writer.close();

            // Start xray process
            ProcessBuilder pb = new ProcessBuilder(
                    xrayBinaryPath, "run", "-c", tempConfig.getAbsolutePath());
            pb.redirectErrorStream(true);
            Process process = pb.start();

            int handle = proxyHandleCounter.incrementAndGet();
            proxyProcesses.put(handle, process);

            addLog("Started proxy handle=" + handle + " port=" + socksPort);
            Log.d(TAG, "Started proxy handle=" + handle + " port=" + socksPort);
            return handle;

        } catch (Exception e) {
            addLog("Failed to start proxy on port " + socksPort + ": " + e.getMessage());
            Log.e(TAG, "Failed to start proxy on port " + socksPort + ": " + e.getMessage());
            return -1;
        }
    }

    @Override
    public void stopProxy(int handleId) {
        Process process = proxyProcesses.remove(handleId);
        if (process != null) {
            try {
                process.destroy();
                if (!process.waitFor(3, TimeUnit.SECONDS)) {
                    process.destroyForcibly();
                }
            } catch (Exception e) {
                try { process.destroyForcibly(); } catch (Exception ignored) {}
            }
            addLog("Stopped proxy handle=" + handleId);
            Log.d(TAG, "Stopped proxy handle=" + handleId);
        }
    }

    @Override
    public double[] testUrl(String url, int socksPort, int timeoutSeconds) {
        double[] result = new double[]{0, 0};
        try {
            OkHttpClient proxyClient = directClient.newBuilder()
                    .proxy(new Proxy(Proxy.Type.SOCKS,
                            new InetSocketAddress("127.0.0.1", socksPort)))
                    .connectTimeout(timeoutSeconds, TimeUnit.SECONDS)
                    .readTimeout(timeoutSeconds, TimeUnit.SECONDS)
                    .build();

            Request request = new Request.Builder()
                    .url(url)
                    .header("User-Agent",
                            "Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 Chrome/122.0.0.0 Mobile Safari/537.36")
                    .build();

            long start = System.nanoTime();
            try (Response response = proxyClient.newCall(request).execute()) {
                double latencyMs = (System.nanoTime() - start) / 1_000_000.0;
                result[0] = response.code();
                result[1] = latencyMs;
            }
        } catch (Exception e) {
            addLog("Test URL failed: " + e.getMessage());
            Log.d(TAG, "Test URL failed: " + e.getMessage());
        }
        return result;
    }

    @Override
    public String[] telegramFetch(String channel, int limit) {
        List<String> messages = new ArrayList<>();

        // Method 1: Scrape public Telegram channel preview (t.me/s/channel)
        try {
            String url = "https://t.me/s/" + channel;
            String html = httpFetch(url,
                    "Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 Chrome/122.0.0.0",
                    15, "");

            if (html != null && !html.isEmpty()) {
                // Extract message text from HTML
                Pattern pattern = Pattern.compile(
                        "<div class=\"tgme_widget_message_text[^\"]*\"[^>]*>([\\s\\S]*?)</div>",
                        Pattern.CASE_INSENSITIVE);
                Matcher matcher = pattern.matcher(html);
                while (matcher.find() && messages.size() < limit) {
                    String text = matcher.group(1);
                    // Strip HTML tags
                    text = text.replaceAll("<[^>]+>", " ");
                    text = text.replaceAll("&amp;", "&");
                    text = text.replaceAll("&lt;", "<");
                    text = text.replaceAll("&gt;", ">");
                    text = text.replaceAll("&#\\d+;", "");
                    text = text.trim();
                    if (!text.isEmpty()) {
                        messages.add(text);
                    }
                }
            }
        } catch (Exception e) {
            addLog("Telegram public scrape failed for " + channel + ": " + e.getMessage());
            Log.w(TAG, "Telegram public scrape failed for " + channel + ": " + e.getMessage());
        }

        // Method 2: If bot token is set, use Bot API getUpdates or getChat
        if (messages.isEmpty() && botToken != null && !botToken.isEmpty()) {
            try {
                String apiUrl = "https://api.telegram.org/bot" + botToken +
                        "/getChat?chat_id=@" + channel;
                String response = httpFetch(apiUrl,
                        "Mozilla/5.0", 10, "");
                // Bot API can provide channel info but not message history for non-member bots
                // This is a best-effort fallback
            } catch (Exception e) {
                addLog("Bot API fallback failed for " + channel);
                Log.d(TAG, "Bot API fallback failed for " + channel);
            }
        }

        return messages.toArray(new String[0]);
    }

    @Override
    public boolean telegramSend(String text) {
        if (botToken == null || botToken.isEmpty() || chatId == null || chatId.isEmpty()) {
            addLog("Telegram send skipped: credentials not set");
            Log.w(TAG, "Telegram send skipped: botToken or chatId not set");
            return false;
        }
        addLog("Telegram send: " + text.substring(0, Math.min(50, text.length())) + "...");

        try {
            String apiUrl = "https://api.telegram.org/bot" + botToken + "/sendMessage";
            JSONObject payload = new JSONObject();
            payload.put("chat_id", chatId);
            payload.put("text", text);

            return sendTelegramRequest(apiUrl, payload.toString());
        } catch (Exception e) {
            addLog("Telegram send failed: " + e.getMessage());
            Log.e(TAG, "Telegram send failed: " + e.getMessage());
            return false;
        }
    }

    @Override
    public boolean telegramSendFile(String filename, String content, String caption) {
        if (botToken == null || botToken.isEmpty() || chatId == null || chatId.isEmpty()) {
            return false;
        }

        try {
            // Write content to temp file and send via sendDocument
            java.io.File tempFile = new java.io.File(context.getCacheDir(), filename);
            java.io.FileWriter writer = new java.io.FileWriter(tempFile);
            writer.write(content);
            writer.close();

            String apiUrl = "https://api.telegram.org/bot" + botToken + "/sendDocument";

            // Multipart upload
            okhttp3.MultipartBody body = new okhttp3.MultipartBody.Builder()
                    .setType(okhttp3.MultipartBody.FORM)
                    .addFormDataPart("chat_id", chatId)
                    .addFormDataPart("caption", caption)
                    .addFormDataPart("document", filename,
                            okhttp3.RequestBody.create(tempFile,
                                    okhttp3.MediaType.parse("text/plain")))
                    .build();

            Request request = new Request.Builder()
                    .url(apiUrl)
                    .post(body)
                    .build();

            try (Response response = directClient.newCall(request).execute()) {
                tempFile.delete();
                if (response.isSuccessful()) {
                    addLog("Telegram message sent successfully");
                    Log.d(TAG, "Telegram message sent successfully");
                    return true;
                } else {
                    addLog("Telegram send failed: " + response.code());
                    Log.e(TAG, "Telegram send failed: " + response.code());
                    return false;
                }
            }
        } catch (Exception e) {
            addLog("Telegram send file failed: " + e.getMessage());
            Log.e(TAG, "Telegram send file failed: " + e.getMessage());
            return false;
        }
    }

    @Override
    public void onProgress(String phase, int current, int total) {
        String msg = "Progress: " + phase + " (" + current + "/" + total + ")";
        Log.d(TAG, msg);
        addLog(msg);
        if (progressListener != null) {
            progressListener.onProgress(phase, current, total);
        }
    }

    @Override
    public void onStatusUpdate(String statusJson) {
        Log.d(TAG, "Status: " + statusJson);
        addLog("Status: " + statusJson);
        if (progressListener != null) {
            progressListener.onStatusUpdate(statusJson);
        }
    }

    private boolean sendTelegramRequest(String apiUrl, String jsonPayload) {
        try {
            okhttp3.RequestBody body = okhttp3.RequestBody.create(
                    jsonPayload, okhttp3.MediaType.parse("application/json"));
            Request request = new Request.Builder()
                    .url(apiUrl)
                    .post(body)
                    .build();

            try (Response response = directClient.newCall(request).execute()) {
                return response.isSuccessful();
            }
        } catch (Exception e) {
            return false;
        }
    }

    public void destroyAllProxies() {
        for (Map.Entry<Integer, Process> entry : proxyProcesses.entrySet()) {
            try {
                entry.getValue().destroyForcibly();
            } catch (Exception ignored) {}
        }
        proxyProcesses.clear();
    }
}
