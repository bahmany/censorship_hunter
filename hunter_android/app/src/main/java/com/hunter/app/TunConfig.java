package com.hunter.app;

/** Pure helper (unit-tested): hev-socks5-tunnel YAML + VPN addressing constants. */
public final class TunConfig {
    public static final String TUN_V4 = "198.18.0.1";
    public static final String TUN_V6 = "fd00:198:18::1";
    public static final String MAPDNS_ADDR = "198.18.0.2";
    public static final int MTU = 1500;
    private TunConfig() {}

    public static String hevYaml(int socksPort) {
        if (socksPort < 1 || socksPort > 65535) throw new IllegalArgumentException("port " + socksPort);
        return "tunnel:\n"
             + "  mtu: " + MTU + "\n"
             + "  ipv4: " + TUN_V4 + "\n"
             + "  ipv6: '" + TUN_V6 + "'\n"
             + "socks5:\n"
             + "  port: " + socksPort + "\n"
             + "  address: 127.0.0.1\n"
             + "mapdns:\n"
             + "  address: " + MAPDNS_ADDR + "\n"
             + "  port: 53\n"
             + "  network: 100.64.0.0\n"
             + "  netmask: 255.192.0.0\n"
             + "  cache-size: 10000\n"
             + "misc:\n"
             + "  log-level: warn\n";
    }

    /** Failover policy: true when consecutive probe failures reach the threshold. */
    public static boolean shouldFailover(int consecutiveFailures, int threshold) {
        return consecutiveFailures >= threshold;
    }
}
