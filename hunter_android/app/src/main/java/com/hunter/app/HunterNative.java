package com.hunter.app;

import android.content.Context;

/** Thin wrapper over libhunter.so core lifecycle. Core is process-wide and idempotent. */
public final class HunterNative {
    static { System.loadLibrary("hunter"); }
    private HunterNative() {}

    /** Initialise paths (absolute filesDir, nativeLibraryDir engines) and start the core if needed. */
    public static synchronized boolean init(Context ctx) {
        Context app = ctx.getApplicationContext();
        return nativeInit(app.getFilesDir().getAbsolutePath(),
                app.getApplicationInfo().nativeLibraryDir, app.getAssets());
    }
    public static synchronized void stopCore() { nativeStopCore(); }
    public static boolean isCoreRunning() { return nativeIsCoreRunning(); }
    /** Starts local SOCKS5 on best config; skip>0 fails over to later candidates. Returns port or 0. */
    public static int startSocks(int skip) { return nativeStartSocks(skip); }
    public static void stopSocks() { nativeStopSocks(); }
    public static int socksPort() { return nativeSocksPort(); }

    private static native boolean nativeInit(String filesDir, String libDir, android.content.res.AssetManager am);
    private static native void nativeStopCore();
    private static native boolean nativeIsCoreRunning();
    private static native int nativeStartSocks(int skip);
    private static native void nativeStopSocks();
    private static native int nativeSocksPort();
}
