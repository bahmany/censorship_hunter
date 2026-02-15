package com.hunter.app;

import android.util.Log;

/**
 * JNI bridge to native C++ Hunter engine.
 * All core logic runs in C++ for maximum performance.
 * Java layer provides Android-specific APIs (HTTP, V2Ray engine, Telegram Bot API).
 */
public class HunterNative {
    private static final String TAG = "HunterNative";

    static {
        System.loadLibrary("hunter");
    }

    // Native methods
    public static native void nativeInit(String filesDir, String secretsFile, HunterCallback callback);
    public static native void nativeStart();
    public static native void nativeStop();
    public static native boolean nativeIsRunning();
    public static native String nativeGetStatus();
    public static native void nativeRunCycle();
    public static native void nativeSetConfig(String key, String value);
    public static native String nativeValidateConfig();
    public static native void nativeDestroy();
    public static native String nativeGetConfig(String key);
    public static native String nativeGetConfigs();

    /**
     * Callback interface implemented by Java to provide Android-specific functionality
     * to the native C++ layer.
     */
    public interface HunterCallback {
        /**
         * Fetch URL content via OkHttp.
         * @param url URL to fetch
         * @param userAgent User-Agent header
         * @param timeoutSeconds Connection/read timeout
         * @param proxy SOCKS5 proxy URL (empty for direct)
         * @return Response body as string, or empty on failure
         */
        String httpFetch(String url, String userAgent, int timeoutSeconds, String proxy);

        /**
         * Start a V2Ray/XRay proxy instance with the given JSON config.
         * @param configJson XRay-compatible JSON configuration
         * @param socksPort Local SOCKS port to bind
         * @return Handle ID (>= 0 on success, -1 on failure)
         */
        int startProxy(String configJson, int socksPort);

        /**
         * Stop a previously started proxy instance.
         * @param handleId Handle returned by startProxy
         */
        void stopProxy(int handleId);

        /**
         * Test a URL through a local SOCKS proxy.
         * @param url URL to test
         * @param socksPort Local SOCKS port
         * @param timeoutSeconds Timeout
         * @return double[2]: {httpStatusCode, latencyMs}
         */
        double[] testUrl(String url, int socksPort, int timeoutSeconds);

        /**
         * Fetch messages from a Telegram channel (via Bot API or public scraping).
         * @param channel Channel username
         * @param limit Max messages to fetch
         * @return Array of message text contents
         */
        String[] telegramFetch(String channel, int limit);

        /**
         * Send a text message to the configured Telegram report channel.
         * @param text Message text
         * @return true on success
         */
        boolean telegramSend(String text);

        /**
         * Send a file to the configured Telegram report channel.
         * @param filename File name
         * @param content File content as string
         * @param caption File caption
         * @return true on success
         */
        boolean telegramSendFile(String filename, String content, String caption);

        /**
         * Called by native layer to report progress.
         * @param phase Current phase name
         * @param current Current progress count
         * @param total Total count (0 if unknown)
         */
        void onProgress(String phase, int current, int total);

        /**
         * Called by native layer with JSON status updates.
         * @param statusJson JSON string with current status
         */
        void onStatusUpdate(String statusJson);
    }
}
