package com.hunter.app;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Static helper for parsing V2Ray URIs into XRay-compatible outbound JSON configs.
 * Shared by VpnService (for VPN backends) and ConfigTester (for testing).
 * Supports: VMess, VLESS (with Reality), Trojan, Shadowsocks.
 */
public class V2RayConfigHelper {
    private static final String TAG = "V2RayConfigHelper";

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

            // CRITICAL: Force XRay to resolve proxy server domains via its built-in
            // DNS module instead of Go's system resolver. On Android, Go defaults to
            // [::1]:53 which doesn't exist, causing ALL domain lookups to fail.
            if (result != null) {
                JSONObject streamSettings = result.optJSONObject("streamSettings");
                if (streamSettings == null) {
                    streamSettings = new JSONObject();
                    result.put("streamSettings", streamSettings);
                }
                JSONObject sockopt = streamSettings.optJSONObject("sockopt");
                if (sockopt == null) {
                    sockopt = new JSONObject();
                    streamSettings.put("sockopt", sockopt);
                }
                sockopt.put("domainStrategy", "UseIPv4");
            }

            return result;
        } catch (Exception e) {
            Log.w(TAG, "Error parsing URI: " + e.getMessage());
        }
        return null;
    }

    /**
     * Generate a full V2Ray config JSON for a given URI and SOCKS port.
     */
    public static String generateFullConfig(String uri, int socksPort) {
        try {
            JSONObject outbound = parseUriToOutbound(uri);
            if (outbound == null) return null;

            JSONObject config = new JSONObject();

            // Log
            JSONObject log = new JSONObject();
            log.put("loglevel", "warning");
            config.put("log", log);

            // Inbounds - SOCKS5 proxy
            JSONArray inbounds = new JSONArray();
            JSONObject socksIn = new JSONObject();
            socksIn.put("tag", "socks-in");
            socksIn.put("port", socksPort);
            socksIn.put("listen", "127.0.0.1");
            socksIn.put("protocol", "socks");
            JSONObject socksSettings = new JSONObject();
            socksSettings.put("udp", true);
            socksIn.put("settings", socksSettings);

            JSONObject sniffing = new JSONObject();
            sniffing.put("enabled", true);
            sniffing.put("destOverride", new JSONArray().put("http").put("tls").put("quic"));
            sniffing.put("routeOnly", true);
            socksIn.put("sniffing", sniffing);

            inbounds.put(socksIn);
            config.put("inbounds", inbounds);

            // Outbounds
            JSONArray outbounds = new JSONArray();
            outbounds.put(outbound);

            JSONObject direct = new JSONObject();
            direct.put("tag", "direct");
            direct.put("protocol", "freedom");
            outbounds.put(direct);

            JSONObject block = new JSONObject();
            block.put("tag", "block");
            block.put("protocol", "blackhole");
            outbounds.put(block);

            config.put("outbounds", outbounds);

            // Routing
            JSONObject routing = new JSONObject();
            routing.put("domainStrategy", "IPIfNonMatch");
            JSONArray rules = new JSONArray();

            // CRITICAL: Route XRay's internal DNS module queries direct.
            // Without "tag" in DNS config + this rule, DNS queries bypass routing
            // and go to the first outbound (proxy), creating a chicken-and-egg loop.
            JSONObject directDnsTag = new JSONObject();
            directDnsTag.put("type", "field");
            directDnsTag.put("inboundTag", new JSONArray().put("dns-internal"));
            directDnsTag.put("outboundTag", "direct");
            rules.put(directDnsTag);

            // Route client app DNS (from socks-in) through proxy
            // so censored domains are resolved correctly on the remote server
            JSONObject proxyDns = new JSONObject();
            proxyDns.put("type", "field");
            proxyDns.put("inboundTag", new JSONArray().put("socks-in"));
            proxyDns.put("port", "53");
            proxyDns.put("outboundTag", "proxy");
            rules.put(proxyDns);

            // Catch any remaining port-53 traffic and send direct
            JSONObject directDns = new JSONObject();
            directDns.put("type", "field");
            directDns.put("port", "53");
            directDns.put("outboundTag", "direct");
            rules.put(directDns);

            // Route private IPs and DNS server IPs directly to avoid loops
            JSONObject directPrivate = new JSONObject();
            directPrivate.put("type", "field");
            directPrivate.put("ip", new JSONArray()
                .put("192.168.0.0/16").put("172.16.0.0/12")
                .put("10.0.0.0/8").put("127.0.0.0/8")
                .put("178.22.122.100").put("78.157.42.100")
                .put("1.1.1.1").put("8.8.8.8"));
            directPrivate.put("outboundTag", "direct");
            rules.put(directPrivate);

            routing.put("rules", rules);
            config.put("routing", routing);

            // DNS - optimized for Iranian censorship with fast fallbacks
            JSONObject dns = new JSONObject();
            JSONArray servers = new JSONArray();
            
            // Fast traditional DNS servers (prioritize speed over privacy in Iran)
            JSONObject shecan = new JSONObject(); // Iranian DNS service
            shecan.put("address", "178.22.122.100");
            shecan.put("port", 53);
            servers.put(shecan);
            
            JSONObject electro = new JSONObject(); // Another Iranian DNS
            electro.put("address", "78.157.42.100");
            electro.put("port", 53);
            servers.put(electro);
            
            // Cloudflare (usually works)
            JSONObject cloudflare = new JSONObject();
            cloudflare.put("address", "1.1.1.1");
            cloudflare.put("port", 53);
            servers.put(cloudflare);
            
            // Google (sometimes blocked)
            JSONObject google = new JSONObject();
            google.put("address", "8.8.8.8");
            google.put("port", 53);
            servers.put(google);
            
            // Fast DoH servers with shorter timeouts (Iranian networks are slow)
            JSONObject cloudflareDoh = new JSONObject();
            cloudflareDoh.put("address", "https://1.1.1.1/dns-query");
            cloudflareDoh.put("port", 443);
            JSONObject dohSettings1 = new JSONObject();
            dohSettings1.put("path", "/dns-query");
            dohSettings1.put("queryStrategy", "UseIPv4");
            cloudflareDoh.put("settings", dohSettings1);
            servers.put(cloudflareDoh);
            
            dns.put("servers", servers);
            dns.put("disableCache", false);  // Enable cache for performance
            dns.put("queryStrategy", "UseIPv4");  // Force IPv4 to avoid IPv6 issues
            dns.put("disableFallback", false);  // Allow fallback to other DNS servers
            dns.put("final", "178.22.122.100");  // Final fallback to Iranian DNS
            dns.put("tag", "dns-internal");  // CRITICAL: enables routing for DNS module queries
            
            config.put("dns", dns);

            return config.toString(2);

        } catch (Exception e) {
            Log.e(TAG, "Error generating full config", e);
            return null;
        }
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
        int port = hostPort.length > 1 ? Integer.parseInt(hostPort[1]) : 443;

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
        int port = hostPort.length > 1 ? Integer.parseInt(hostPort[1]) : 443;

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
            port = hostPort.length > 1 ? Integer.parseInt(hostPort[1]) : 443;
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
            port = hostPort.length > 1 ? Integer.parseInt(hostPort[1]) : 443;
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

            // Parse all URIs into outbounds
            java.util.List<JSONObject> proxyOutbounds = new java.util.ArrayList<>();
            for (int i = 0; i < uris.size(); i++) {
                JSONObject outbound = parseUriToOutbound(uris.get(i));
                if (outbound != null) {
                    outbound.put("tag", "proxy-" + i);
                    proxyOutbounds.add(outbound);
                }
            }

            if (proxyOutbounds.isEmpty()) return null;

            // If only 1 outbound, use simple config
            if (proxyOutbounds.size() == 1) {
                proxyOutbounds.get(0).put("tag", "proxy");
                return generateFullConfig(uris.get(0), socksPort);
            }

            JSONObject config = new JSONObject();

            // Log
            JSONObject log = new JSONObject();
            log.put("loglevel", "warning");
            config.put("log", log);

            // Inbounds - SOCKS5 proxy
            JSONArray inbounds = new JSONArray();
            JSONObject socksIn = new JSONObject();
            socksIn.put("tag", "socks-in");
            socksIn.put("port", socksPort);
            socksIn.put("listen", "127.0.0.1");
            socksIn.put("protocol", "socks");
            JSONObject socksSettings = new JSONObject();
            socksSettings.put("udp", true);
            socksIn.put("settings", socksSettings);

            JSONObject sniffing = new JSONObject();
            sniffing.put("enabled", true);
            sniffing.put("destOverride", new JSONArray().put("http").put("tls").put("quic"));
            sniffing.put("routeOnly", true);
            socksIn.put("sniffing", sniffing);

            inbounds.put(socksIn);
            config.put("inbounds", inbounds);

            // Outbounds - multiple proxies + direct + block
            JSONArray outbounds = new JSONArray();
            for (JSONObject proxy : proxyOutbounds) {
                outbounds.put(proxy);
            }

            JSONObject direct = new JSONObject();
            direct.put("tag", "direct");
            direct.put("protocol", "freedom");
            outbounds.put(direct);

            JSONObject block = new JSONObject();
            block.put("tag", "block");
            block.put("protocol", "blackhole");
            outbounds.put(block);

            config.put("outbounds", outbounds);

            // Observatory - monitors proxy health with periodic probes
            JSONObject observatory = new JSONObject();
            JSONArray subjectSelector = new JSONArray();
            subjectSelector.put("proxy-");
            observatory.put("subjectSelector", subjectSelector);
            observatory.put("probeURL", "http://cp.cloudflare.com/generate_204");
            observatory.put("probeInterval", "30s");
            observatory.put("enableConcurrency", true);
            config.put("observatory", observatory);

            // Routing with balancer
            JSONObject routing = new JSONObject();
            routing.put("domainStrategy", "IPIfNonMatch");

            // Balancers
            JSONArray balancers = new JSONArray();
            JSONObject balancer = new JSONObject();
            balancer.put("tag", "proxy-balancer");
            JSONArray selector = new JSONArray();
            selector.put("proxy-");
            balancer.put("selector", selector);
            JSONObject strategy = new JSONObject();
            strategy.put("type", "leastPing");
            balancer.put("strategy", strategy);
            balancers.put(balancer);
            routing.put("balancers", balancers);

            // Rules
            JSONArray rules = new JSONArray();

            // CRITICAL: Route XRay's internal DNS module queries direct.
            // Without "tag" in DNS config + this rule, DNS queries bypass routing
            // and go to the first outbound (proxy), creating a chicken-and-egg loop.
            JSONObject directDnsTag = new JSONObject();
            directDnsTag.put("type", "field");
            directDnsTag.put("inboundTag", new JSONArray().put("dns-internal"));
            directDnsTag.put("outboundTag", "direct");
            rules.put(directDnsTag);

            // Route client app DNS (from socks-in) through balancer
            // so censored domains are resolved correctly on the remote server
            JSONObject proxyDns = new JSONObject();
            proxyDns.put("type", "field");
            proxyDns.put("inboundTag", new JSONArray().put("socks-in"));
            proxyDns.put("port", "53");
            proxyDns.put("balancerTag", "proxy-balancer");
            rules.put(proxyDns);

            // Catch any remaining port-53 traffic and send direct
            JSONObject directDns = new JSONObject();
            directDns.put("type", "field");
            directDns.put("port", "53");
            directDns.put("outboundTag", "direct");
            rules.put(directDns);

            // Private IPs and DNS server IPs go direct
            JSONObject directPrivate = new JSONObject();
            directPrivate.put("type", "field");
            directPrivate.put("ip", new JSONArray()
                .put("192.168.0.0/16").put("172.16.0.0/12")
                .put("10.0.0.0/8").put("127.0.0.0/8")
                .put("178.22.122.100").put("78.157.42.100")
                .put("1.1.1.1").put("8.8.8.8"));
            directPrivate.put("outboundTag", "direct");
            rules.put(directPrivate);

            // All other traffic goes through balancer
            JSONObject balancerRule = new JSONObject();
            balancerRule.put("type", "field");
            balancerRule.put("network", "tcp,udp");
            balancerRule.put("balancerTag", "proxy-balancer");
            rules.put(balancerRule);

            routing.put("rules", rules);
            config.put("routing", routing);

            // DNS - include Iranian DNS servers for resolving proxy domains on censored networks
            JSONObject dns = new JSONObject();
            JSONArray servers = new JSONArray();
            servers.put("178.22.122.100"); // Shecan (Iranian DNS, fast)
            servers.put("78.157.42.100");  // Electro (Iranian DNS)
            servers.put("1.1.1.1");        // Cloudflare
            servers.put("8.8.8.8");        // Google
            dns.put("servers", servers);
            dns.put("disableCache", false);
            dns.put("queryStrategy", "UseIPv4");
            dns.put("tag", "dns-internal");  // CRITICAL: enables routing for DNS module queries
            config.put("dns", dns);

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
