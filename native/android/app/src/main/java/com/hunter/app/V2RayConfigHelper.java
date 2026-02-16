package com.hunter.app;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.HashMap;
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
            if (uri.startsWith("vmess://")) {
                return parseVmessUri(uri);
            } else if (uri.startsWith("vless://")) {
                return parseVlessUri(uri);
            } else if (uri.startsWith("trojan://")) {
                return parseTrojanUri(uri);
            } else if (uri.startsWith("ss://")) {
                return parseShadowsocksUri(uri);
            } else if (uri.startsWith("hy2://") || uri.startsWith("hysteria2://")) {
                // Hysteria2 not supported by XRay core, skip
                Log.w(TAG, "Hysteria2 protocol not supported by XRay");
                return null;
            }
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

            // Routing - simplified to reduce memory usage
            JSONObject routing = new JSONObject();
            routing.put("domainStrategy", "IPIfNonMatch");
            JSONArray rules = new JSONArray();

            // Only route private IPs directly to avoid loops
            JSONObject directPrivate = new JSONObject();
            directPrivate.put("type", "field");
            directPrivate.put("ip", new JSONArray().put("192.168.0.0/16").put("172.16.0.0/12").put("10.0.0.0/8").put("127.0.0.0/8"));
            directPrivate.put("outboundTag", "direct");
            rules.put(directPrivate);

            routing.put("rules", rules);
            config.put("routing", routing);

            // DNS
            JSONObject dns = new JSONObject();
            JSONArray servers = new JSONArray();
            servers.put("1.1.1.1");
            servers.put("8.8.8.8");
            dns.put("servers", servers);
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
        server.put("address", vmess.getString("add"));
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
        streamSettings.put("network", vmess.optString("net", "tcp"));

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
        String net = vmess.optString("net", "tcp");
        if ("ws".equals(net)) {
            JSONObject wsSettings = new JSONObject();
            wsSettings.put("path", vmess.optString("path", "/"));
            wsSettings.put("host", vmess.optString("host", vmess.getString("add")));
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
            user.put("flow", flow);
        }

        users.put(user);
        server.put("users", users);
        vnext.put(server);
        settings.put("vnext", vnext);
        outbound.put("settings", settings);

        JSONObject streamSettings = new JSONObject();
        String network = params.getOrDefault("type", "tcp");
        streamSettings.put("network", network);

        String security = params.getOrDefault("security", "none");
        streamSettings.put("security", security);

        if ("reality".equals(security)) {
            JSONObject realitySettings = new JSONObject();
            realitySettings.put("serverName", params.getOrDefault("sni", host));
            realitySettings.put("fingerprint", params.getOrDefault("fp", "chrome"));
            realitySettings.put("publicKey", params.getOrDefault("pbk", ""));
            realitySettings.put("shortId", params.getOrDefault("sid", ""));
            realitySettings.put("spiderX", params.getOrDefault("spx", ""));
            streamSettings.put("realitySettings", realitySettings);
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
            wsSettings.put("host", params.getOrDefault("host", host));
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
            String headerType = params.getOrDefault("headerType", "none");
            if (!"none".equals(headerType)) {
                JSONObject tcpSettings = new JSONObject();
                JSONObject header = new JSONObject();
                header.put("type", headerType);
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
        String network = params.getOrDefault("type", "tcp");
        streamSettings.put("network", network);
        streamSettings.put("security", "tls");

        JSONObject tlsSettings = new JSONObject();
        tlsSettings.put("serverName", params.getOrDefault("sni", host));
        tlsSettings.put("fingerprint", "chrome");
        tlsSettings.put("allowInsecure", false);
        streamSettings.put("tlsSettings", tlsSettings);

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

    private static Map<String, String> parseQueryParams(String queryString) {
        Map<String, String> params = new HashMap<>();
        if (queryString == null || queryString.isEmpty()) return params;

        for (String param : queryString.split("&")) {
            String[] kv = param.split("=", 2);
            if (kv.length == 2) {
                try {
                    params.put(kv[0], java.net.URLDecoder.decode(kv[1], "UTF-8"));
                } catch (Exception e) {
                    params.put(kv[0], kv[1]);
                }
            }
        }
        return params;
    }
}
