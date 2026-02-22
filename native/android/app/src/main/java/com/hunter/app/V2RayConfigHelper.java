package com.hunter.app;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Static helper for parsing V2Ray URIs into XRay-compatible outbound JSON configs.
 * Shared by VpnService (for VPN backends) and ConfigTester (for testing).
 * Supports: VMess, VLESS (with Reality), Trojan, Shadowsocks.
 */
public class V2RayConfigHelper {
    private static final String TAG = "V2RayConfigHelper";

    // ── DNS Discovery: cached results ──────────────────────────────────
    private static final Object dnsLock = new Object();
    private static volatile List<String> cachedDnsServers = null;
    private static volatile long dnsDiscoveryTime = 0;
    private static final long DNS_CACHE_TTL = 10 * 60 * 1000; // 10 min

    // ── All known DNS servers (100+) ──────────────────────────────────
    private static final String[] ALL_DNS_SERVERS = {
        // --- Iranian ISP / local DNS (highest priority — reachable without proxy) ---
        "10.202.10.10",    // Radar DNS
        "10.202.10.11",    // Radar DNS 2
        "10.202.10.202",   // 403.online
        "10.202.10.102",   // 403.online 2
        "178.22.122.100",  // Shecan
        "185.51.200.2",    // Shecan 2
        "78.157.42.100",   // Electro
        "78.157.42.101",   // Electro 2
        "185.55.226.26",   // Begzar
        "185.55.225.25",   // Begzar 2
        "85.15.1.14",      // Shatel DNS
        "85.15.1.15",      // Shatel DNS 2
        "172.29.2.100",    // HostIran
        "217.218.26.77",   // ITC Iran Telecom
        "2.189.1.1",       // ITC Iran Telecom 2
        "2.188.21.120",    // TIC Infrastructure
        "2.188.21.100",    // TIC Infrastructure 2
        "5.160.80.180",    // Respina
        "5.160.80.183",    // Respina 2
        "5.202.177.197",   // Pishgaman
        "5.202.100.101",   // Pishgaman 2
        "45.135.243.61",   // Faraso
        "109.230.83.245",  // Faraso 2
        "81.29.249.82",    // Pasargad Arian
        // --- Global public DNS (may be blocked by Iranian ISP) ---
        "8.8.8.8",         // Google
        "8.8.4.4",         // Google 2
        "1.1.1.1",         // Cloudflare
        "1.0.0.1",         // Cloudflare 2
        "9.9.9.9",         // Quad9
        "149.112.112.112", // Quad9 2
        "208.67.222.222",  // OpenDNS
        "208.67.220.220",  // OpenDNS 2
        "185.228.168.9",   // CleanBrowsing
        "185.228.169.9",   // CleanBrowsing 2
        "76.76.19.19",     // Alternate DNS
        "76.223.122.150",  // Alternate DNS 2
        "94.140.14.14",    // AdGuard
        "94.140.15.15",    // AdGuard 2
        "8.26.56.26",      // Comodo
        "8.20.247.20",     // Comodo 2
        "64.6.64.6",       // Verisign
        "64.6.65.6",       // Verisign 2
        "77.88.8.8",       // Yandex
        "77.88.8.1",       // Yandex 2
        "195.46.39.39",    // SafeDNS
        "195.46.39.40",    // SafeDNS 2
        "4.2.2.2",         // Level3
        "4.2.2.1",         // Level3 2
        "156.154.70.1",    // DNS Advantage
        "156.154.71.1",    // DNS Advantage 2
        "84.200.69.80",    // DNS.WATCH
        "84.200.70.40",    // DNS.WATCH 2
        "5.9.164.112",     // Digitalcourage
        "46.182.19.48",    // Digitalcourage 2
        "103.2.57.5",      // IIJ Japan
        "103.2.57.6",      // IIJ Japan 2
        "149.112.112.11",  // Quad9 Japan
        "9.9.9.11",        // Quad9 variant
        "185.121.177.177", // OpenNIC
        "169.239.202.202", // OpenNIC 2
        "185.231.182.126", // OpenNIC 3
        "185.43.135.1",    // OpenNIC 4
        "91.239.100.100",  // PublicDNS.org
        "89.233.43.71",    // PublicDNS.org 2
        "176.10.118.132",  // Swiss DNS
        "176.10.118.133",  // Swiss DNS 2
        "199.85.127.10",   // Norton/Symantec
        "199.85.126.10",   // Norton/Symantec 2
        "205.210.42.205",  // DNSResolvers
        "64.68.200.200",   // DNSResolvers 2
        "209.244.0.3",     // Level3 alt
        "209.244.0.4",     // Level3 alt 2
        "216.146.35.35",   // DynDNS
        "216.146.36.36",   // DynDNS 2
        "74.50.55.161",    // VisiZone
        "74.50.55.162",    // VisiZone 2
        "223.5.5.5",       // AliDNS China
        "223.6.6.6",       // AliDNS China 2
        "119.29.29.29",    // DNSPod China
        "114.114.114.114", // 114DNS China
        "114.114.115.115", // 114DNS China 2
        "180.76.76.76",    // Baidu DNS
        "101.226.4.6",     // China DNS
        "123.125.81.6",    // China DNS 2
    };

    /**
     * Get system DNS servers from Android properties.
     */
    public static List<String> getSystemDnsServers() {
        List<String> servers = new ArrayList<>();
        try {
            for (int i = 1; i <= 4; i++) {
                Process p = Runtime.getRuntime().exec("getprop net.dns" + i);
                BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line = br.readLine();
                br.close();
                p.waitFor();
                if (line != null && !line.trim().isEmpty()) {
                    String ip = line.trim();
                    if (!ip.contains(":") && !ip.startsWith("127.") && !ip.equals("0.0.0.0")) {
                        if (!servers.contains(ip)) {
                            servers.add(ip);
                        }
                    }
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Failed to get system DNS: " + e.getMessage());
        }
        return servers;
    }

    /**
     * Probe a single DNS server with a real UDP query for "example.com".
     * Returns response time in ms, or -1 if failed.
     */
    private static int probeDns(String server, int timeoutMs) {
        try {
            // Build DNS query for example.com A record
            byte[] query = buildDnsQuery("example.com");
            InetAddress addr = InetAddress.getByName(server);
            DatagramSocket sock = new DatagramSocket();
            sock.setSoTimeout(timeoutMs);
            DatagramPacket sendPkt = new DatagramPacket(query, query.length, addr, 53);
            DatagramPacket recvPkt = new DatagramPacket(new byte[512], 512);
            long start = System.currentTimeMillis();
            sock.send(sendPkt);
            sock.receive(recvPkt);
            int elapsed = (int)(System.currentTimeMillis() - start);
            sock.close();
            // Verify it's a valid DNS response (QR bit set, RCODE=0)
            byte[] resp = recvPkt.getData();
            if (recvPkt.getLength() < 12) return -1;
            boolean isResponse = (resp[2] & 0x80) != 0;
            int rcode = resp[3] & 0x0F;
            if (!isResponse || rcode != 0) return -1;
            return elapsed;
        } catch (Exception e) {
            return -1;
        }
    }

    /**
     * Build a minimal DNS query packet for a domain (A record).
     */
    private static byte[] buildDnsQuery(String domain) {
        // Header: ID=0x1234, QR=0, OPCODE=0, RD=1, QDCOUNT=1
        byte[] header = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        // Question: encode domain labels
        String[] labels = domain.split("\\.");
        int qLen = 0;
        for (String l : labels) qLen += 1 + l.length();
        qLen += 1 + 4; // null terminator + QTYPE(2) + QCLASS(2)
        byte[] question = new byte[qLen];
        int pos = 0;
        for (String l : labels) {
            question[pos++] = (byte) l.length();
            for (int i = 0; i < l.length(); i++) question[pos++] = (byte) l.charAt(i);
        }
        question[pos++] = 0x00; // end of labels
        question[pos++] = 0x00; question[pos++] = 0x01; // QTYPE = A
        question[pos++] = 0x00; question[pos++] = 0x01; // QCLASS = IN
        byte[] packet = new byte[header.length + question.length];
        System.arraycopy(header, 0, packet, 0, header.length);
        System.arraycopy(question, 0, packet, header.length, question.length);
        return packet;
    }

    /**
     * Discover the best DNS servers by probing all known servers in parallel.
     * Returns top N fastest servers that actually respond.
     * Results are cached for DNS_CACHE_TTL.
     */
    public static List<String> discoverBestDns(int maxResults) {
        synchronized (dnsLock) {
            if (cachedDnsServers != null && (System.currentTimeMillis() - dnsDiscoveryTime) < DNS_CACHE_TTL) {
                return new ArrayList<>(cachedDnsServers);
            }
        }

        Log.i(TAG, "Starting DNS discovery across " + ALL_DNS_SERVERS.length + " servers...");
        long startTime = System.currentTimeMillis();

        // Collect system DNS too
        List<String> systemDns = getSystemDnsServers();
        List<String> allToTest = new ArrayList<>(Arrays.asList(ALL_DNS_SERVERS));
        for (String s : systemDns) {
            if (!allToTest.contains(s)) allToTest.add(0, s); // system DNS first
        }

        final ConcurrentHashMap<String, Integer> results = new ConcurrentHashMap<>();
        final int PROBE_TIMEOUT = 2000; // 2s per probe
        ExecutorService pool = Executors.newFixedThreadPool(40);
        final CountDownLatch latch = new CountDownLatch(allToTest.size());

        for (String dns : allToTest) {
            pool.execute(() -> {
                try {
                    int ms = probeDns(dns, PROBE_TIMEOUT);
                    if (ms >= 0) {
                        results.put(dns, ms);
                    }
                } finally {
                    latch.countDown();
                }
            });
        }

        try {
            latch.await(5, TimeUnit.SECONDS); // max 5s total
        } catch (InterruptedException ignored) {}
        pool.shutdownNow();

        // Sort by response time
        List<Map.Entry<String, Integer>> sorted = new ArrayList<>(results.entrySet());
        sorted.sort((a, b) -> a.getValue().compareTo(b.getValue()));

        List<String> best = new ArrayList<>();
        for (int i = 0; i < Math.min(maxResults, sorted.size()); i++) {
            best.add(sorted.get(i).getKey());
            Log.i(TAG, "DNS #" + (i+1) + ": " + sorted.get(i).getKey() + " (" + sorted.get(i).getValue() + "ms)");
        }

        long elapsed = System.currentTimeMillis() - startTime;
        Log.i(TAG, "DNS discovery done: " + results.size() + "/" + allToTest.size()
            + " responded, top " + best.size() + " selected (" + elapsed + "ms)");

        // Ensure we always have at least some fallbacks
        if (best.isEmpty()) {
            for (String s : systemDns) { if (!best.contains(s)) best.add(s); }
            if (!best.contains("178.22.122.100")) best.add("178.22.122.100");
            if (!best.contains("78.157.42.100")) best.add("78.157.42.100");
            if (!best.contains("10.202.10.202")) best.add("10.202.10.202");
            Log.w(TAG, "No DNS responded in time, using fallbacks: " + best);
        }

        // Cache
        synchronized (dnsLock) {
            cachedDnsServers = new ArrayList<>(best);
            dnsDiscoveryTime = System.currentTimeMillis();
        }
        return best;
    }

    /**
     * Build DNS server list for XRay config using discovered best servers.
     */
    private static JSONArray buildDnsServers() {
        List<String> best = discoverBestDns(6); // top 6 fastest
        JSONArray servers = new JSONArray();
        for (String dns : best) {
            servers.put(dns);
        }
        return servers;
    }

    /**
     * Parse a V2Ray URI (vmess://, vless://, trojan://, ss://, hy2://, hysteria2://) into an XRay outbound JSON object.
     * Returns null if the URI is unsupported or malformed.
     */
    public static JSONObject parseUriToOutbound(String uri) {
        try {
            if (uri == null) return null;
            String u = uri.trim();
            if (u.isEmpty()) return null;
            String lower = u.toLowerCase();

            JSONObject result = null;

            if (lower.startsWith("vmess://")) {
                result = parseVmessUri("vmess://" + u.substring(8));
            } else if (lower.startsWith("vless://")) {
                result = parseVlessUri("vless://" + u.substring(8));
            } else if (lower.startsWith("trojan://")) {
                result = parseTrojanUri("trojan://" + u.substring(9));
            } else if (lower.startsWith("ss://")) {
                result = parseShadowsocksUri("ss://" + u.substring(5));
            } else if (lower.startsWith("hy2://") || lower.startsWith("hysteria2://")) {
                // Hysteria2 not supported by XRay core, skip
                Log.w(TAG, "Hysteria2 protocol not supported by XRay");
                return null;
            }

            // Since we pre-resolve proxy server hostnames to IPs in generateFullConfig/
            // generateBalancedConfig, XRay doesn't need to resolve them via DNS.
            // Using "AsIs" avoids the DNS circular dependency when tun2socks is active.
            // Previously "UseIPv4" caused rcode:3 errors because Iranian ISP DNS
            // couldn't resolve CDN hostnames like s14.fastly80-2.hosting-ip.com.
            // No sockopt.domainStrategy needed — pre-resolution handles it.

            return result;
        } catch (Exception e) {
            Log.w(TAG, "Error parsing URI: " + e.getMessage());
        }
        return null;
    }

    /**
     * Pre-resolve the proxy server hostname in an outbound config to an IP address.
     * This is critical for VPN mode: when tun2socks is active, XRay's DNS queries
     * get captured by tun2socks → sent back to XRay → circular dependency.
     * Also critical for test configs: Go's system resolver on Android defaults to
     * [::1]:53 which doesn't exist → "connection refused" for all domain lookups.
     * By resolving the hostname BEFORE starting XRay, we avoid both issues.
     *
     * Uses direct UDP DNS queries to known DNS servers (bypasses broken system resolver).
     */
    public static void preResolveOutboundHost(JSONObject outbound) {
        try {
            String protocol = outbound.optString("protocol", "");
            JSONObject settings = outbound.optJSONObject("settings");
            if (settings == null) return;

            String address = null;
            JSONObject serverObj = null;

            // Extract server address based on protocol
            if ("vmess".equals(protocol) || "vless".equals(protocol)) {
                JSONArray vnext = settings.optJSONArray("vnext");
                if (vnext != null && vnext.length() > 0) {
                    serverObj = vnext.getJSONObject(0);
                    address = serverObj.optString("address", "");
                }
            } else if ("trojan".equals(protocol) || "shadowsocks".equals(protocol)) {
                JSONArray servers = settings.optJSONArray("servers");
                if (servers != null && servers.length() > 0) {
                    serverObj = servers.getJSONObject(0);
                    address = serverObj.optString("address", "");
                }
            }

            if (address == null || address.isEmpty() || serverObj == null) return;

            // Check if it's already an IP address
            if (address.matches("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$")) {
                if (isPoisonedOrBogonIp(address)) {
                    Log.w(TAG, "Proxy address is a bogon/private IP: " + address + " — config likely poisoned");
                }
                return;
            }

            // Resolve hostname to IP using direct UDP DNS queries
            // (bypasses Android's broken [::1]:53 system resolver)
            Log.i(TAG, "Pre-resolving proxy hostname: " + address);
            String ip = resolveHostnameDirectly(address);
            if (ip != null) {
                Log.i(TAG, "Resolved " + address + " -> " + ip);
                serverObj.put("address", ip);
            } else {
                Log.w(TAG, "Could not resolve proxy hostname: " + address + " (will keep domain, XRay DNS may handle it)");
            }
        } catch (Exception e) {
            Log.w(TAG, "Error in preResolveOutboundHost", e);
        }
    }

    /**
     * Check if an IP address is poisoned/bogon (returned by Iranian censored DNS).
     * Iranian ISPs return private IPs like 10.10.34.36 for blocked domains.
     * These must NEVER be used as proxy server addresses.
     */
    private static boolean isPoisonedOrBogonIp(String ip) {
        if (ip == null || ip.isEmpty()) return true;
        try {
            String[] parts = ip.split("\\.");
            if (parts.length != 4) return true;
            int a = Integer.parseInt(parts[0]);
            int b = Integer.parseInt(parts[1]);

            // 0.0.0.0/8 — "this" network
            if (a == 0) return true;
            // 10.0.0.0/8 — private (Iranian DNS poison target: 10.10.34.36)
            if (a == 10) return true;
            // 100.64.0.0/10 — CGN shared address space
            if (a == 100 && b >= 64 && b <= 127) return true;
            // 127.0.0.0/8 — loopback
            if (a == 127) return true;
            // 169.254.0.0/16 — link-local
            if (a == 169 && b == 254) return true;
            // 172.16.0.0/12 — private
            if (a == 172 && b >= 16 && b <= 31) return true;
            // 192.0.0.0/24 — IANA special
            if (a == 192 && b == 0 && Integer.parseInt(parts[2]) == 0) return true;
            // 192.168.0.0/16 — private
            if (a == 192 && b == 168) return true;
            // 198.18.0.0/15 — benchmarking
            if (a == 198 && (b == 18 || b == 19)) return true;
            // 224.0.0.0/4 — multicast, 240.0.0.0/4 — reserved
            if (a >= 224) return true;

            return false;
        } catch (Exception e) {
            return true;
        }
    }

    /**
     * Check if an outbound's proxy server address is still a domain name (not an IP).
     * After preResolveOutboundHost(), if the address is still a domain, it means
     * DNS resolution failed (all results were poisoned). On Android, XRay's Go
     * resolver defaults to [::1]:53 which doesn't exist, so these outbounds will
     * ALWAYS fail with "connection refused" and must be excluded from balanced configs.
     */
    public static boolean isOutboundAddressUnresolved(JSONObject outbound) {
        try {
            String protocol = outbound.optString("protocol", "");
            JSONObject settings = outbound.optJSONObject("settings");
            if (settings == null) return false;

            String address = null;
            if ("vmess".equals(protocol) || "vless".equals(protocol)) {
                JSONArray vnext = settings.optJSONArray("vnext");
                if (vnext != null && vnext.length() > 0) {
                    address = vnext.getJSONObject(0).optString("address", "");
                }
            } else if ("trojan".equals(protocol) || "shadowsocks".equals(protocol)) {
                JSONArray servers = settings.optJSONArray("servers");
                if (servers != null && servers.length() > 0) {
                    address = servers.getJSONObject(0).optString("address", "");
                }
            }

            if (address == null || address.isEmpty()) return false;

            // If it's an IP address, it's resolved
            return !address.matches("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$");
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * Resolve a hostname to an IPv4 address using direct UDP DNS queries.
     * Tries multiple DNS servers in parallel to maximize success on censored networks.
     * IMPORTANT: Iranian DNS servers (Shecan, Electro, etc.) poison responses for
     * blocked/foreign domains, returning bogus IPs like 10.10.34.36. We MUST use
     * global DNS servers ONLY for proxy hostname resolution, and validate all results
     * to reject poisoned/bogon IPs.
     * Returns the first valid (non-poisoned) IP, or null if all fail.
     */
    private static String resolveHostnameDirectly(String hostname) {
        // ONLY global DNS servers for proxy hostname resolution.
        // Iranian DNS (Shecan, Electro) POISON foreign proxy domains → 10.10.34.36
        // They are useful for XRay's internal DNS (resolving sites through proxy)
        // but MUST NOT be used to resolve the proxy server address itself.
        String[] dnsServers = {
            "1.1.1.1", "8.8.8.8", "9.9.9.9",           // Global public DNS
            "208.67.222.222", "1.0.0.1",                  // OpenDNS, Cloudflare secondary
            "8.8.4.4", "149.112.112.112",                  // Google 2, Quad9 2
            "223.5.5.5", "119.29.29.29"                    // AliDNS, DNSPod (not censored for foreign domains)
        };

        byte[] query = buildDnsQuery(hostname);

        // Collect ALL results, then pick the best non-poisoned one
        final ConcurrentHashMap<String, String> dnsResults = new ConcurrentHashMap<>();
        final Object lock = new Object();
        final String[] firstGoodResult = {null};
        Thread[] threads = new Thread[dnsServers.length];

        for (int i = 0; i < dnsServers.length; i++) {
            final String dns = dnsServers[i];
            threads[i] = new Thread(() -> {
                try {
                    synchronized (lock) {
                        if (firstGoodResult[0] != null) return; // already found good result
                    }
                    InetAddress dnsAddr = InetAddress.getByName(dns);
                    DatagramSocket sock = new DatagramSocket();
                    sock.setSoTimeout(3000);
                    DatagramPacket sendPkt = new DatagramPacket(query, query.length, dnsAddr, 53);
                    DatagramPacket recvPkt = new DatagramPacket(new byte[512], 512);
                    sock.send(sendPkt);
                    sock.receive(recvPkt);
                    sock.close();

                    String ip = extractIpFromDnsResponse(recvPkt.getData(), recvPkt.getLength());
                    if (ip != null) {
                        dnsResults.put(dns, ip);
                        if (!isPoisonedOrBogonIp(ip)) {
                            synchronized (lock) {
                                if (firstGoodResult[0] == null) {
                                    firstGoodResult[0] = ip;
                                    lock.notifyAll();
                                }
                            }
                        } else {
                            Log.w(TAG, "DNS " + dns + " returned poisoned/bogon IP " + ip + " for " + hostname + " — REJECTED");
                        }
                    }
                } catch (Exception ignored) {}
            }, "DNS-resolve-" + dns);
            threads[i].setDaemon(true);
            threads[i].start();
        }

        // Wait up to 4 seconds for any valid (non-poisoned) result
        synchronized (lock) {
            if (firstGoodResult[0] == null) {
                try { lock.wait(4000); } catch (InterruptedException ignored) {}
            }
        }

        if (firstGoodResult[0] != null) {
            return firstGoodResult[0];
        }

        // Log all results for debugging if no good IP found
        if (!dnsResults.isEmpty()) {
            Log.w(TAG, "All DNS results for " + hostname + " were poisoned/bogon: " + dnsResults);
        }

        return null;
    }

    /**
     * Extract the first A record (IPv4) from a raw DNS response packet.
     */
    private static String extractIpFromDnsResponse(byte[] data, int length) {
        if (length < 12) return null;

        // Check QR=1 (response) and RCODE=0 (no error)
        boolean isResponse = (data[2] & 0x80) != 0;
        int rcode = data[3] & 0x0F;
        if (!isResponse || rcode != 0) return null;

        int qdCount = ((data[4] & 0xFF) << 8) | (data[5] & 0xFF);
        int anCount = ((data[6] & 0xFF) << 8) | (data[7] & 0xFF);
        if (anCount == 0) return null;

        // Skip question section
        int pos = 12;
        for (int q = 0; q < qdCount; q++) {
            while (pos < length) {
                int labelLen = data[pos] & 0xFF;
                if (labelLen == 0) { pos++; break; }
                if ((labelLen & 0xC0) == 0xC0) { pos += 2; break; } // pointer
                pos += 1 + labelLen;
            }
            pos += 4; // QTYPE + QCLASS
        }

        // Parse answer section — find first A record (type=1, class=1)
        for (int a = 0; a < anCount && pos < length; a++) {
            // Skip name (may be pointer)
            if ((data[pos] & 0xC0) == 0xC0) {
                pos += 2;
            } else {
                while (pos < length) {
                    int labelLen = data[pos] & 0xFF;
                    if (labelLen == 0) { pos++; break; }
                    if ((labelLen & 0xC0) == 0xC0) { pos += 2; break; }
                    pos += 1 + labelLen;
                }
            }

            if (pos + 10 > length) return null;
            int type = ((data[pos] & 0xFF) << 8) | (data[pos + 1] & 0xFF);
            int cls = ((data[pos + 2] & 0xFF) << 8) | (data[pos + 3] & 0xFF);
            int rdLength = ((data[pos + 8] & 0xFF) << 8) | (data[pos + 9] & 0xFF);
            pos += 10;

            if (type == 1 && cls == 1 && rdLength == 4 && pos + 4 <= length) {
                // A record — extract IPv4
                return (data[pos] & 0xFF) + "." + (data[pos + 1] & 0xFF) + "."
                     + (data[pos + 2] & 0xFF) + "." + (data[pos + 3] & 0xFF);
            }
            pos += rdLength;
        }
        return null;
    }

    /**
     * Apply anti-DPI settings to an outbound for Iran's new filtering system.
     * - TLS fragment: splits ClientHello to evade SNI-based DPI (most effective)
     * - uTLS fingerprint: randomize TLS fingerprint to avoid detection
     * - Mux: disabled when fragment is active (they are incompatible in XRay)
     *
     * NOTE: fragment and mux are mutually exclusive in XRay.
     * Fragment operates at TCP level and is the primary anti-DPI technique for Iran.
     */
    public static void applyAntiDpiSettings(JSONObject outbound) {
        try {
            JSONObject streamSettings = outbound.optJSONObject("streamSettings");
            if (streamSettings == null) return;

            String security = streamSettings.optString("security", "none");
            boolean isTls = "tls".equals(security) || "reality".equals(security);
            boolean fragmentApplied = false;

            // 1. TLS Fragment — split ClientHello into small pieces to bypass SNI inspection
            // This is the most effective anti-DPI technique for Iran's new filtering.
            // Only applies to TLS (not reality which uses its own anti-detection).
            if ("tls".equals(security)) {
                JSONObject sockopt = streamSettings.optJSONObject("sockopt");
                if (sockopt == null) sockopt = new JSONObject();

                if (!sockopt.has("fragment")) {
                    JSONObject fragment = new JSONObject();
                    fragment.put("packets", "tlshello");
                    fragment.put("length", "100-200");
                    fragment.put("interval", "10-20");
                    sockopt.put("fragment", fragment);
                    fragmentApplied = true;
                }

                sockopt.put("tcpNoDelay", true);
                sockopt.put("TcpKeepAliveInterval", 15);
                streamSettings.put("sockopt", sockopt);
            }

            // 2. Ensure uTLS fingerprint is set for all TLS connections
            if (isTls) {
                String settingsKey = "tls".equals(security) ? "tlsSettings" : "realitySettings";
                JSONObject tlsSettings = streamSettings.optJSONObject(settingsKey);
                if (tlsSettings != null) {
                    String fp = tlsSettings.optString("fingerprint", "");
                    if (fp.isEmpty() || "none".equals(fp)) {
                        String[] fingerprints = {"chrome", "firefox", "safari", "edge", "randomized"};
                        tlsSettings.put("fingerprint", fingerprints[(int)(Math.random() * fingerprints.length)]);
                    }
                }
            }

            // 3. Mux — MUST be disabled when fragment is active (incompatible in XRay).
            // Also disable mux for Reality — mux multiplexes ALL connections into one
            // session. If that session has ANY issue (x509 cert mismatch, timeout),
            // ALL connections fail including Telegram. The x509 errors in logs
            // ("certificate is valid for *.google.com, not tgju.org") are caused by
            // mux sessions failing on misconfigured Reality proxies.
            if (!outbound.has("mux")) {
                JSONObject mux = new JSONObject();
                mux.put("enabled", false);
                outbound.put("mux", mux);
            }

            Log.d(TAG, "Applied anti-DPI settings (security=" + security
                + ", fragment=" + fragmentApplied + ")");
        } catch (Exception e) {
            Log.w(TAG, "Error applying anti-DPI settings", e);
        }
    }

    /**
     * Generate a full V2Ray config JSON for a given URI and SOCKS port.
     * Modeled exactly after v2rayNG's proven working approach:
     * - SOCKS inbound with sniffing (http+tls, NO routeOnly, NO quic)
     * - domainStrategy "IPIfNonMatch" (NOT "AsIs") so XRay resolves domains for routing
     * - freedom outbound with "UseIP" (NOT "UseIPv4")
     * - DNS: simple DoH strings, no object form, no localhost
     * - dokodemo-door DNS inbound + dns-out outbound + routing rules
     */
    public static String generateFullConfig(String uri, int socksPort) {
        try {
            JSONObject outbound = parseUriToOutbound(uri);
            if (outbound == null) return null;

            // v2rayNG pattern: resolve proxy hostname and add to dns.hosts
            // (NOT replacing outbound address — XRay uses dns.hosts + sockopt.domainStrategy=UseIP)
            String proxyDomain = getOutboundDomain(outbound);
            String resolvedIp = null;
            if (proxyDomain != null) {
                resolvedIp = resolveHostnameDirectly(proxyDomain);
                if (resolvedIp != null) {
                    Log.i(TAG, "Resolved " + proxyDomain + " -> " + resolvedIp);
                    // v2rayNG sets sockopt.domainStrategy = "UseIP" so XRay looks up dns.hosts
                    JSONObject streamSettings = outbound.optJSONObject("streamSettings");
                    if (streamSettings != null) {
                        JSONObject sockopt = streamSettings.optJSONObject("sockopt");
                        if (sockopt == null) sockopt = new JSONObject();
                        sockopt.put("domainStrategy", "UseIP");
                        streamSettings.put("sockopt", sockopt);
                    }
                } else {
                    // All DNS poisoned — try replacing address directly as fallback
                    Log.w(TAG, "Could not resolve " + proxyDomain + " — keeping domain");
                }
            }

            // Extract IP for direct routing (either resolved or already an IP)
            String proxyServerIp = extractOutboundAddress(outbound);
            if (proxyServerIp == null && resolvedIp != null) {
                proxyServerIp = resolvedIp;
            }

            applyAntiDpiSettings(outbound);

            JSONObject config = new JSONObject();

            // ── Log ──
            JSONObject log = new JSONObject();
            log.put("loglevel", "warning");
            config.put("log", log);

            // ── Stats + Policy (v2rayNG base template) ──
            config.put("stats", new JSONObject());
            JSONObject policy = new JSONObject();
            JSONObject levels = new JSONObject();
            JSONObject level8 = new JSONObject();
            level8.put("handshake", 4);
            level8.put("connIdle", 300);
            level8.put("uplinkOnly", 1);
            level8.put("downlinkOnly", 1);
            levels.put("8", level8);
            policy.put("levels", levels);
            JSONObject system = new JSONObject();
            system.put("statsOutboundUplink", true);
            system.put("statsOutboundDownlink", true);
            policy.put("system", system);
            config.put("policy", policy);

            // ── Inbounds (v2rayNG: socks only, NO dokodemo-door for gomobile tun2socks) ──
            JSONArray inbounds = new JSONArray();
            JSONObject socksIn = new JSONObject();
            socksIn.put("tag", "socks");
            socksIn.put("port", socksPort);
            socksIn.put("listen", "127.0.0.1");
            socksIn.put("protocol", "socks");
            JSONObject socksSettings = new JSONObject();
            socksSettings.put("auth", "noauth");
            socksSettings.put("udp", true);
            socksSettings.put("userLevel", 8);
            socksIn.put("settings", socksSettings);
            JSONObject sniffing = new JSONObject();
            sniffing.put("enabled", true);
            sniffing.put("destOverride", new JSONArray().put("http").put("tls"));
            socksIn.put("sniffing", sniffing);
            inbounds.put(socksIn);
            config.put("inbounds", inbounds);

            // ── Outbounds ──
            JSONArray outbounds = new JSONArray();
            outbounds.put(outbound); // proxy (tag="proxy")

            JSONObject direct = new JSONObject();
            direct.put("tag", "direct");
            direct.put("protocol", "freedom");
            JSONObject directSettings = new JSONObject();
            directSettings.put("domainStrategy", "UseIP");
            direct.put("settings", directSettings);
            outbounds.put(direct);

            JSONObject block = new JSONObject();
            block.put("tag", "block");
            block.put("protocol", "blackhole");
            JSONObject blockSettings = new JSONObject();
            JSONObject blockResponse = new JSONObject();
            blockResponse.put("type", "http");
            blockSettings.put("response", blockResponse);
            block.put("settings", blockSettings);
            outbounds.put(block);

            // dns-out outbound (v2rayNG pattern)
            JSONObject dnsOut = new JSONObject();
            dnsOut.put("tag", "dns-out");
            dnsOut.put("protocol", "dns");
            outbounds.put(dnsOut);

            config.put("outbounds", outbounds);

            // ── Routing (v2rayNG: domainStrategy "AsIs") ──
            JSONObject routing = new JSONObject();
            routing.put("domainStrategy", "AsIs");
            JSONArray rules = new JSONArray();

            // Rule 1: Port 53 from socks → dns-out
            // (v2rayNG hev-socks5-tunnel pattern: captures DNS from tun2socks)
            JSONObject dnsFromSocks = new JSONObject();
            dnsFromSocks.put("type", "field");
            dnsFromSocks.put("inboundTag", new JSONArray().put("socks"));
            dnsFromSocks.put("port", "53");
            dnsFromSocks.put("outboundTag", "dns-out");
            rules.put(dnsFromSocks);

            // Rule 2: Proxy server IP → direct (avoid routing loop)
            if (proxyServerIp != null && !proxyServerIp.isEmpty()) {
                JSONObject proxyDirect = new JSONObject();
                proxyDirect.put("type", "field");
                proxyDirect.put("ip", new JSONArray().put(proxyServerIp));
                proxyDirect.put("outboundTag", "direct");
                rules.put(proxyDirect);
            }

            // Rule 3: Private IPs → direct
            JSONObject directPrivate = new JSONObject();
            directPrivate.put("type", "field");
            directPrivate.put("ip", new JSONArray().put("geoip:private"));
            directPrivate.put("outboundTag", "direct");
            rules.put(directPrivate);

            routing.put("rules", rules);
            config.put("routing", routing);

            // ── DNS (v2rayNG pattern: tag="dns-module" + routing sends it through proxy) ──
            // THIS IS THE KEY: dns.tag causes XRay to tag all DNS module traffic.
            // The routing rule below sends tagged DNS traffic through the proxy outbound.
            // Without this, DNS queries go directly to 1.1.1.1 which is BLOCKED by Iranian ISP.
            JSONObject dns = new JSONObject();
            JSONArray dnsServers = new JSONArray();
            dnsServers.put("1.1.1.1");
            dnsServers.put("8.8.8.8");
            dns.put("servers", dnsServers);
            dns.put("queryStrategy", "UseIPv4");
            dns.put("tag", "dns-module");  // v2rayNG: TAG_DNS = "dns-module"

            // hosts — v2rayNG hardcodes googleapis + adds resolved proxy IPs here
            JSONObject hosts = new JSONObject();
            hosts.put("domain:googleapis.cn", "googleapis.com");
            // v2rayNG resolveOutboundDomainsToHosts: add resolved proxy IP to hosts
            if (proxyDomain != null && resolvedIp != null) {
                hosts.put(proxyDomain, resolvedIp);
            }
            dns.put("hosts", hosts);

            config.put("dns", dns);

            // ── DNS routing rules (v2rayNG: getDns adds these AFTER main routing) ──
            // CRITICAL: Route DNS module traffic through proxy so DNS queries
            // reach 1.1.1.1 via the encrypted tunnel, not directly (which ISP blocks)
            JSONObject dnsModuleRule = new JSONObject();
            dnsModuleRule.put("type", "field");
            dnsModuleRule.put("inboundTag", new JSONArray().put("dns-module"));
            dnsModuleRule.put("outboundTag", "proxy");
            rules.put(dnsModuleRule);

            return config.toString(2);

        } catch (Exception e) {
            Log.e(TAG, "Error generating full config", e);
            return null;
        }
    }

    /**
     * Extract the resolved proxy server IP address from an outbound config.
     * Used to create a direct routing rule so proxy traffic doesn't loop.
     */
    public static String extractOutboundAddress(JSONObject outbound) {
        try {
            String protocol = outbound.optString("protocol", "");
            JSONObject settings = outbound.optJSONObject("settings");
            if (settings == null) return null;

            String address = null;
            if ("vmess".equals(protocol) || "vless".equals(protocol)) {
                JSONArray vnext = settings.optJSONArray("vnext");
                if (vnext != null && vnext.length() > 0) {
                    address = vnext.getJSONObject(0).optString("address", "");
                }
            } else if ("trojan".equals(protocol) || "shadowsocks".equals(protocol)) {
                JSONArray servers = settings.optJSONArray("servers");
                if (servers != null && servers.length() > 0) {
                    address = servers.getJSONObject(0).optString("address", "");
                }
            }

            // Only return if it's an IP (not a domain)
            if (address != null && address.matches("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$")) {
                return address;
            }
        } catch (Exception e) {
            Log.w(TAG, "Error extracting outbound address", e);
        }
        return null;
    }

    /**
     * Extract the proxy server domain from an outbound config.
     * Returns null if the address is already an IP or not found.
     * Used by v2rayNG's resolveOutboundDomainsToHosts pattern.
     */
    private static String getOutboundDomain(JSONObject outbound) {
        try {
            String protocol = outbound.optString("protocol", "");
            JSONObject settings = outbound.optJSONObject("settings");
            if (settings == null) return null;

            String address = null;
            if ("vmess".equals(protocol) || "vless".equals(protocol)) {
                JSONArray vnext = settings.optJSONArray("vnext");
                if (vnext != null && vnext.length() > 0) {
                    address = vnext.getJSONObject(0).optString("address", "");
                }
            } else if ("trojan".equals(protocol) || "shadowsocks".equals(protocol)) {
                JSONArray servers = settings.optJSONArray("servers");
                if (servers != null && servers.length() > 0) {
                    address = servers.getJSONObject(0).optString("address", "");
                }
            }

            // Only return if it's a domain (NOT an IP)
            if (address != null && !address.isEmpty()
                && !address.matches("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$")) {
                return address;
            }
        } catch (Exception e) {
            Log.w(TAG, "Error extracting outbound domain", e);
        }
        return null;
    }

    private static JSONObject parseVmessUri(String uri) throws Exception {
        String base64 = uri.substring(8);
        if (base64.length() > 4096) return null;
        String json = new String(android.util.Base64.decode(base64, android.util.Base64.NO_WRAP));
        JSONObject vmess = new JSONObject(json);

        JSONObject outbound = new JSONObject();
        outbound.put("tag", "proxy");
        outbound.put("protocol", "vmess");

        JSONObject settings = new JSONObject();
        JSONArray vnext = new JSONArray();
        JSONObject server = new JSONObject();
        String host = vmess.getString("add");
        server.put("address", host);
        server.put("port", vmess.getInt("port"));

        JSONArray users = new JSONArray();
        JSONObject user = new JSONObject();
        user.put("id", vmess.getString("id"));
        user.put("alterId", vmess.optInt("aid", 0));
        user.put("security", vmess.optString("scy", "auto"));
        users.put(user);
        server.put("users", users);
        vnext.put(server);
        settings.put("vnext", vnext);
        outbound.put("settings", settings);

        JSONObject streamSettings = new JSONObject();
        String net = normalizeLower(vmess.optString("net", "tcp"));
        if (!isValidNetwork(net)) net = "tcp";
        streamSettings.put("network", net);

        String tls = vmess.optString("tls", "");
        if ("tls".equals(tls)) {
            streamSettings.put("security", "tls");
            JSONObject tlsSettings = new JSONObject();
            tlsSettings.put("serverName", vmess.optString("sni", vmess.getString("add")));
            tlsSettings.put("allowInsecure", false);
            tlsSettings.put("fingerprint", "chrome");
            streamSettings.put("tlsSettings", tlsSettings);
        }

        // WebSocket settings
        if ("ws".equals(net)) {
            JSONObject wsSettings = new JSONObject();
            wsSettings.put("path", vmess.optString("path", "/"));
            String wsHost = normalizeToken(vmess.optString("host", ""));
            if (!wsHost.isEmpty()) {
                wsSettings.put("host", wsHost);
            }
            streamSettings.put("wsSettings", wsSettings);
        }

        outbound.put("streamSettings", streamSettings);
        return outbound;
    }

    private static JSONObject parseVlessUri(String uri) throws Exception {
        String withoutScheme = uri.substring(8);
        String[] parts = withoutScheme.split("#");
        String mainPart = parts[0];

        String[] userHost = mainPart.split("@");
        if (userHost.length < 2) return null;
        String uuid = userHost[0];
        String[] hostParams = userHost[1].split("\\?");
        String[] hostPort = hostParams[0].split(":");
        String host = hostPort[0];
        int port = hostPort.length > 1 ? Integer.parseInt(hostPort[1].replaceAll("[^0-9]", "")) : 443;

        Map<String, String> params = parseQueryParams(hostParams.length > 1 ? hostParams[1] : "");

        JSONObject outbound = new JSONObject();
        outbound.put("tag", "proxy");
        outbound.put("protocol", "vless");

        JSONObject settings = new JSONObject();
        JSONArray vnext = new JSONArray();
        JSONObject server = new JSONObject();
        server.put("address", host);
        server.put("port", port);

        JSONArray users = new JSONArray();
        JSONObject user = new JSONObject();
        user.put("id", uuid);
        user.put("encryption", "none");

        String flow = params.getOrDefault("flow", "");
        if (!flow.isEmpty()) {
            // xtls-rprx-vision is the standard flow for VLESS Reality in XRay 25.1.30
            if (flow.equals("xtls-rprx-vision") || flow.equals("xtls-rprx-vision-udp443")
                || flow.equals("xtls-rprx-origin") || flow.equals("xtls-rprx-direct")) {
                user.put("flow", flow);
            } else {
                Log.w(TAG, "Removing unknown flow: " + flow);
            }
        }

        users.put(user);
        server.put("users", users);
        vnext.put(server);
        settings.put("vnext", vnext);
        outbound.put("settings", settings);

        JSONObject streamSettings = new JSONObject();
        String network = normalizeLower(params.getOrDefault("type", "tcp"));
        if (!isValidNetwork(network)) network = "tcp";
        streamSettings.put("network", network);

        String security = normalizeLower(params.getOrDefault("security", "none"));
        if (security.isEmpty()) security = "none";
        streamSettings.put("security", security);

        if ("reality".equals(security)) {
            JSONObject realitySettings = new JSONObject();
            realitySettings.put("serverName", params.getOrDefault("sni", host));
            realitySettings.put("fingerprint", params.getOrDefault("fp", "chrome"));
            
            // Validate and sanitize publicKey
            String publicKey = params.getOrDefault("pbk", "");
            if (!publicKey.isEmpty()) {
                // Check if publicKey contains only valid base64 characters
                if (!publicKey.matches("^[A-Za-z0-9+/=_-]*$")) {
                    Log.w(TAG, "Invalid publicKey contains non-base64 characters, skipping Reality config");
                    // Fall back to TLS without Reality
                    streamSettings.put("security", "tls");
                    JSONObject tlsSettings = new JSONObject();
                    tlsSettings.put("serverName", params.getOrDefault("sni", host));
                    tlsSettings.put("fingerprint", params.getOrDefault("fp", "chrome"));
                    tlsSettings.put("allowInsecure", false);
                    streamSettings.put("tlsSettings", tlsSettings);
                } else {
                    realitySettings.put("publicKey", publicKey);
                    realitySettings.put("shortId", params.getOrDefault("sid", ""));
                    realitySettings.put("spiderX", params.getOrDefault("spx", ""));
                    streamSettings.put("realitySettings", realitySettings);
                }
            } else {
                Log.w(TAG, "Empty publicKey in Reality config");
                streamSettings.put("security", "tls");
                JSONObject tlsSettings = new JSONObject();
                tlsSettings.put("serverName", params.getOrDefault("sni", host));
                tlsSettings.put("fingerprint", params.getOrDefault("fp", "chrome"));
                tlsSettings.put("allowInsecure", false);
                streamSettings.put("tlsSettings", tlsSettings);
            }
        } else if ("tls".equals(security)) {
            JSONObject tlsSettings = new JSONObject();
            tlsSettings.put("serverName", params.getOrDefault("sni", host));
            tlsSettings.put("fingerprint", params.getOrDefault("fp", "chrome"));
            tlsSettings.put("allowInsecure", false);
            streamSettings.put("tlsSettings", tlsSettings);
        }

        if ("ws".equals(network)) {
            JSONObject wsSettings = new JSONObject();
            wsSettings.put("path", params.getOrDefault("path", "/"));
            String wsHost = normalizeToken(params.getOrDefault("host", ""));
            if (!wsHost.isEmpty()) {
                wsSettings.put("host", wsHost);
            }
            streamSettings.put("wsSettings", wsSettings);
        } else if ("grpc".equals(network)) {
            JSONObject grpcSettings = new JSONObject();
            grpcSettings.put("serviceName", params.getOrDefault("serviceName", ""));
            streamSettings.put("grpcSettings", grpcSettings);
        } else if ("splithttp".equals(network) || "xhttp".equals(network)) {
            JSONObject splithttpSettings = new JSONObject();
            splithttpSettings.put("path", params.getOrDefault("path", "/"));
            splithttpSettings.put("host", params.getOrDefault("host", host));
            streamSettings.put("splithttpSettings", splithttpSettings);
        } else if ("tcp".equals(network)) {
            String headerType = normalizeLower(params.getOrDefault("headertype", params.getOrDefault("headerType", "none")));
            if (headerType.isEmpty()) headerType = "none";
            if (!"none".equals(headerType) && !"http".equals(headerType)) {
                headerType = "none";
            }
            if (!"none".equals(headerType)) {
                JSONObject tcpSettings = new JSONObject();
                JSONObject header = new JSONObject();
                header.put("type", headerType);
                if ("http".equals(headerType)) {
                    JSONObject request = new JSONObject();
                    request.put("version", "1.1");
                    request.put("method", "GET");
                    request.put("path", new JSONArray().put("/"));
                    JSONObject headers = new JSONObject();
                    headers.put("Host", new JSONArray().put(params.getOrDefault("host", params.getOrDefault("sni", host))));
                    request.put("headers", headers);
                    header.put("request", request);
                }
                tcpSettings.put("header", header);
                streamSettings.put("tcpSettings", tcpSettings);
            }
        }

        outbound.put("streamSettings", streamSettings);
        return outbound;
    }

    private static JSONObject parseTrojanUri(String uri) throws Exception {
        String withoutScheme = uri.substring(9);
        String[] parts = withoutScheme.split("#");
        String mainPart = parts[0];

        String[] passHost = mainPart.split("@");
        if (passHost.length < 2) return null;
        String password = passHost[0];
        String[] hostParams = passHost[1].split("\\?");
        String[] hostPort = hostParams[0].split(":");
        String host = hostPort[0];
        int port = hostPort.length > 1 ? Integer.parseInt(hostPort[1].replaceAll("[^0-9]", "")) : 443;

        Map<String, String> params = parseQueryParams(hostParams.length > 1 ? hostParams[1] : "");

        JSONObject outbound = new JSONObject();
        outbound.put("tag", "proxy");
        outbound.put("protocol", "trojan");

        JSONObject settings = new JSONObject();
        JSONArray servers = new JSONArray();
        JSONObject server = new JSONObject();
        server.put("address", host);
        server.put("port", port);
        server.put("password", password);
        servers.put(server);
        settings.put("servers", servers);
        outbound.put("settings", settings);

        JSONObject streamSettings = new JSONObject();
        String network = normalizeLower(params.getOrDefault("type", "tcp"));
        if (!isValidNetwork(network)) network = "tcp";
        streamSettings.put("network", network);
        streamSettings.put("security", "tls");

        JSONObject tlsSettings = new JSONObject();
        tlsSettings.put("serverName", params.getOrDefault("sni", host));
        tlsSettings.put("fingerprint", params.getOrDefault("fp", "chrome"));
        tlsSettings.put("allowInsecure", false);
        streamSettings.put("tlsSettings", tlsSettings);

        if ("ws".equals(network)) {
            JSONObject wsSettings = new JSONObject();
            wsSettings.put("path", params.getOrDefault("path", "/"));
            String wsHost = normalizeToken(params.getOrDefault("host", ""));
            if (!wsHost.isEmpty()) {
                wsSettings.put("host", wsHost);
            }
            streamSettings.put("wsSettings", wsSettings);
        } else if ("grpc".equals(network)) {
            JSONObject grpcSettings = new JSONObject();
            grpcSettings.put("serviceName", params.getOrDefault("serviceName", ""));
            streamSettings.put("grpcSettings", grpcSettings);
        }

        outbound.put("streamSettings", streamSettings);
        return outbound;
    }

    private static JSONObject parseShadowsocksUri(String uri) throws Exception {
        String withoutScheme = uri.substring(5);
        String[] parts = withoutScheme.split("#");
        String mainPart = parts[0];

        String[] methodHost = mainPart.split("@");
        String host;
        int port;
        String method;
        String password;

        if (methodHost.length == 2) {
            String decoded = new String(android.util.Base64.decode(methodHost[0], android.util.Base64.NO_WRAP | android.util.Base64.URL_SAFE));
            String[] methodPass = decoded.split(":", 2);
            if (methodPass.length < 2) return null;
            method = methodPass[0];
            password = methodPass[1];
            String hostPortPart = methodHost[1];
            int q = hostPortPart.indexOf('?');
            if (q >= 0) hostPortPart = hostPortPart.substring(0, q);
            String[] hostPort = hostPortPart.split(":");
            host = hostPort[0];
            port = hostPort.length > 1 ? Integer.parseInt(hostPort[1].replaceAll("[^0-9]", "")) : 443;
        } else {
            String decoded = new String(android.util.Base64.decode(mainPart, android.util.Base64.NO_WRAP | android.util.Base64.URL_SAFE));
            String[] methodRest = decoded.split(":", 2);
            if (methodRest.length < 2) return null;
            method = methodRest[0];
            String[] passHost = methodRest[1].split("@");
            if (passHost.length < 2) return null;
            password = passHost[0];
            String hostPortPart = passHost[1];
            int q = hostPortPart.indexOf('?');
            if (q >= 0) hostPortPart = hostPortPart.substring(0, q);
            String[] hostPort = hostPortPart.split(":");
            host = hostPort[0];
            port = hostPort.length > 1 ? Integer.parseInt(hostPort[1].replaceAll("[^0-9]", "")) : 443;
        }

        if (method != null) {
            method = method.trim().toLowerCase();
        }

        // Validate cipher - must match server exactly, reject unsupported ones
        if (!isSupportedCipher(method)) {
            Log.w(TAG, "Unsupported cipher method: " + method + ", skipping config");
            return null;
        }

        JSONObject outbound = new JSONObject();
        outbound.put("tag", "proxy");
        outbound.put("protocol", "shadowsocks");

        JSONObject settings = new JSONObject();
        JSONArray servers = new JSONArray();
        JSONObject server = new JSONObject();
        server.put("address", host);
        server.put("port", port);
        server.put("method", method);
        server.put("password", password);
        servers.put(server);
        settings.put("servers", servers);
        outbound.put("settings", settings);

        return outbound;
    }

    /**
     * Check if a cipher method is supported by XRay 25.1.30.
     * Cipher must match server exactly - never remap.
     */
    private static boolean isSupportedCipher(String method) {
        if (method == null || method.isEmpty()) return false;
        
        switch (method.toLowerCase()) {
            case "aes-128-gcm":
            case "aes-256-gcm":
            case "chacha20-ietf-poly1305":
            case "xchacha20-ietf-poly1305":
            case "2022-blake3-aes-128-gcm":
            case "2022-blake3-aes-256-gcm":
            case "2022-blake3-chacha20-poly1305":
            case "aes-128-cfb":
            case "aes-192-cfb":
            case "aes-256-cfb":
            case "aes-128-ctr":
            case "aes-192-ctr":
            case "aes-256-ctr":
            case "camellia-128-cfb":
            case "camellia-192-cfb":
            case "camellia-256-cfb":
            case "bf-cfb":
            case "rc4-md5":
            case "rc4":
            case "chacha20":
            case "chacha20-ietf":
            case "salsa20":
            case "none":
            case "plain":
                return true;
            default:
                return false;
        }
    }

    /**
     * Generate a full V2Ray config with multiple outbounds and XRay built-in balancer.
     * This enables traffic load balancing between multiple working configs.
     */
    public static String generateBalancedConfig(List<String> uris, int socksPort) {
        try {
            if (uris == null || uris.isEmpty()) return null;

            // Parse all URIs into outbounds, resolve hostnames, collect hosts for DNS
            java.util.List<JSONObject> proxyOutbounds = new java.util.ArrayList<>();
            JSONObject dnsHosts = new JSONObject();
            dnsHosts.put("domain:googleapis.cn", "googleapis.com");
            List<String> proxyServerIps = new ArrayList<>();

            int tagIdx = 0;
            for (int i = 0; i < uris.size(); i++) {
                JSONObject outbound = parseUriToOutbound(uris.get(i));
                if (outbound == null) continue;

                // v2rayNG resolveOutboundDomainsToHosts pattern
                String domain = getOutboundDomain(outbound);
                if (domain != null) {
                    String ip = resolveHostnameDirectly(domain);
                    if (ip != null) {
                        dnsHosts.put(domain, ip);
                        if (!proxyServerIps.contains(ip)) proxyServerIps.add(ip);
                        JSONObject ss = outbound.optJSONObject("streamSettings");
                        if (ss != null) {
                            JSONObject so = ss.optJSONObject("sockopt");
                            if (so == null) so = new JSONObject();
                            so.put("domainStrategy", "UseIP");
                            ss.put("sockopt", so);
                        }
                    } else {
                        Log.w(TAG, "Skipping outbound #" + i + " — hostname unresolved");
                        continue;
                    }
                } else {
                    String ip = extractOutboundAddress(outbound);
                    if (ip != null && !proxyServerIps.contains(ip)) proxyServerIps.add(ip);
                }

                applyAntiDpiSettings(outbound);

                JSONObject mux = new JSONObject();
                mux.put("enabled", false);
                outbound.put("mux", mux);

                outbound.put("tag", "proxy-" + tagIdx);
                tagIdx++;
                proxyOutbounds.add(outbound);
            }

            if (proxyOutbounds.isEmpty()) return null;

            // If only 1 outbound, use simple config
            if (proxyOutbounds.size() == 1) {
                proxyOutbounds.get(0).put("tag", "proxy");
                return generateFullConfig(uris.get(0), socksPort);
            }

            JSONObject config = new JSONObject();

            // ── Log + Stats + Policy ──
            config.put("log", new JSONObject().put("loglevel", "warning"));
            config.put("stats", new JSONObject());
            JSONObject policy = new JSONObject();
            JSONObject levels = new JSONObject();
            JSONObject level8 = new JSONObject();
            level8.put("handshake", 4);
            level8.put("connIdle", 300);
            level8.put("uplinkOnly", 1);
            level8.put("downlinkOnly", 1);
            levels.put("8", level8);
            policy.put("levels", levels);
            JSONObject system = new JSONObject();
            system.put("statsOutboundUplink", true);
            system.put("statsOutboundDownlink", true);
            policy.put("system", system);
            config.put("policy", policy);

            // ── Inbounds (socks only, no dokodemo-door) ──
            JSONArray inbounds = new JSONArray();
            JSONObject socksIn = new JSONObject();
            socksIn.put("tag", "socks");
            socksIn.put("port", socksPort);
            socksIn.put("listen", "127.0.0.1");
            socksIn.put("protocol", "socks");
            JSONObject socksSettings = new JSONObject();
            socksSettings.put("auth", "noauth");
            socksSettings.put("udp", true);
            socksSettings.put("userLevel", 8);
            socksIn.put("settings", socksSettings);
            JSONObject sniffing = new JSONObject();
            sniffing.put("enabled", true);
            sniffing.put("destOverride", new JSONArray().put("http").put("tls"));
            socksIn.put("sniffing", sniffing);
            inbounds.put(socksIn);
            config.put("inbounds", inbounds);

            // ── Outbounds ──
            JSONArray outbounds = new JSONArray();
            for (JSONObject proxy : proxyOutbounds) outbounds.put(proxy);

            JSONObject direct = new JSONObject();
            direct.put("tag", "direct");
            direct.put("protocol", "freedom");
            direct.put("settings", new JSONObject().put("domainStrategy", "UseIP"));
            outbounds.put(direct);

            JSONObject block = new JSONObject();
            block.put("tag", "block");
            block.put("protocol", "blackhole");
            block.put("settings", new JSONObject().put("response", new JSONObject().put("type", "http")));
            outbounds.put(block);

            outbounds.put(new JSONObject().put("tag", "dns-out").put("protocol", "dns"));
            config.put("outbounds", outbounds);

            // ── Observatory ──
            JSONObject observatory = new JSONObject();
            observatory.put("subjectSelector", new JSONArray().put("proxy-"));
            observatory.put("probeURL", "http://1.1.1.1/generate_204");
            observatory.put("probeInterval", "30s");
            observatory.put("enableConcurrency", true);
            config.put("observatory", observatory);

            // ── Routing (v2rayNG: AsIs) ──
            JSONObject routing = new JSONObject();
            routing.put("domainStrategy", "AsIs");

            JSONArray balancers = new JSONArray();
            JSONObject balancer = new JSONObject();
            balancer.put("tag", "proxy-balancer");
            balancer.put("selector", new JSONArray().put("proxy-"));
            balancer.put("strategy", new JSONObject().put("type", "leastPing"));
            balancers.put(balancer);
            routing.put("balancers", balancers);

            JSONArray rules = new JSONArray();

            // Rule 1: Port 53 from socks → dns-out
            JSONObject dnsFromSocks = new JSONObject();
            dnsFromSocks.put("type", "field");
            dnsFromSocks.put("inboundTag", new JSONArray().put("socks"));
            dnsFromSocks.put("port", "53");
            dnsFromSocks.put("outboundTag", "dns-out");
            rules.put(dnsFromSocks);

            // Rule 2: Proxy server IPs → direct
            if (!proxyServerIps.isEmpty()) {
                JSONObject proxyDirect = new JSONObject();
                proxyDirect.put("type", "field");
                JSONArray ipArray = new JSONArray();
                for (String ip : proxyServerIps) ipArray.put(ip);
                proxyDirect.put("ip", ipArray);
                proxyDirect.put("outboundTag", "direct");
                rules.put(proxyDirect);
            }

            // Rule 3: Private IPs → direct
            JSONObject directPrivate = new JSONObject();
            directPrivate.put("type", "field");
            directPrivate.put("ip", new JSONArray().put("geoip:private"));
            directPrivate.put("outboundTag", "direct");
            rules.put(directPrivate);

            // Rule 4: All other traffic → balancer
            JSONObject balancerRule = new JSONObject();
            balancerRule.put("type", "field");
            balancerRule.put("network", "tcp,udp");
            balancerRule.put("balancerTag", "proxy-balancer");
            rules.put(balancerRule);

            routing.put("rules", rules);
            config.put("routing", routing);

            // ── DNS (v2rayNG: tag="dns-module", routed through first proxy) ──
            JSONObject dns = new JSONObject();
            dns.put("servers", new JSONArray().put("1.1.1.1").put("8.8.8.8"));
            dns.put("queryStrategy", "UseIPv4");
            dns.put("tag", "dns-module");
            dns.put("hosts", dnsHosts);
            config.put("dns", dns);

            // DNS routing: send dns-module traffic through first proxy (balancer can't be used for DNS)
            JSONObject dnsModuleRule = new JSONObject();
            dnsModuleRule.put("type", "field");
            dnsModuleRule.put("inboundTag", new JSONArray().put("dns-module"));
            dnsModuleRule.put("outboundTag", proxyOutbounds.get(0).getString("tag"));
            rules.put(dnsModuleRule);

            Log.i(TAG, "Generated balanced config with " + proxyOutbounds.size() + " proxies");
            return config.toString(2);

        } catch (Exception e) {
            Log.e(TAG, "Error generating balanced config", e);
            return null;
        }
    }

    private static Map<String, String> parseQueryParams(String queryString) {
        Map<String, String> params = new HashMap<>();
        if (queryString == null || queryString.isEmpty()) return params;

        for (String param : queryString.split("&")) {
            String[] kv = param.split("=", 2);
            if (kv.length == 2) {
                try {
                    String key = kv[0] != null ? kv[0].trim() : "";
                    String val = java.net.URLDecoder.decode(kv[1], "UTF-8");
                    params.put(key, val);
                    String lowerKey = key.toLowerCase();
                    if (!lowerKey.equals(key)) {
                        params.put(lowerKey, val);
                    }
                } catch (Exception e) {
                    String key = kv[0] != null ? kv[0].trim() : "";
                    String val = kv[1];
                    params.put(key, val);
                    String lowerKey = key.toLowerCase();
                    if (!lowerKey.equals(key)) {
                        params.put(lowerKey, val);
                    }
                }
            }
        }
        return params;
    }

    private static String normalizeToken(String v) {
        return v == null ? "" : v.trim();
    }

    private static String normalizeLower(String v) {
        return v == null ? "" : v.trim().toLowerCase();
    }

    private static boolean isValidNetwork(String network) {
        return "tcp".equals(network)
            || "ws".equals(network)
            || "grpc".equals(network)
            || "splithttp".equals(network)
            || "xhttp".equals(network);
    }
}
