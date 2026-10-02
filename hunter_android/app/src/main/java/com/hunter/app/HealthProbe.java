package com.hunter.app;

import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.URL;

/** HTTP probe through the local SOCKS5 (this app is excluded from the VPN, so it hits SOCKS directly). */
public final class HealthProbe {
    private HealthProbe() {}

    public static boolean viaSocks(int port, int timeoutMs) {
        HttpURLConnection c = null;
        try {
            Proxy p = new Proxy(Proxy.Type.SOCKS, new InetSocketAddress("127.0.0.1", port));
            c = (HttpURLConnection) new URL("https://www.gstatic.com/generate_204").openConnection(p);
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setInstanceFollowRedirects(false);
            return c.getResponseCode() == 204;
        } catch (Exception e) {
            return false;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
