package com.hunter.app;

/** JNI surface of hev-socks5-tunnel (natives are registered in its JNI_OnLoad). */
public final class TProxyService {
    static { System.loadLibrary("hev-socks5-tunnel"); }
    private TProxyService() {}
    public static native void TProxyStartService(String configPath, int fd);
    public static native void TProxyStopService();
    public static native long[] TProxyGetStats();
}
