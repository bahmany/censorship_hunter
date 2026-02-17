package com.hunter.app.utils;

/**
 * Constants for V2Ray service and configuration
 * Based on v2rayNG constants
 */
public class V2rayConstants {
    
    // Connection states
    public enum CONNECTION_STATES {
        DISCONNECTED,
        CONNECTING,
        CONNECTED
    }
    
    // Core states
    public enum CORE_STATES {
        IDLE,
        RUNNING,
        STOPPED
    }
    
    // Service modes
    public enum SERVICE_MODES {
        VPN_MODE,
        PROXY_MODE
    }
    
    // Service commands
    public enum SERVICE_COMMANDS {
        START_SERVICE,
        STOP_SERVICE,
        MEASURE_DELAY
    }
    
    // Broadcast intents
    public static final String V2RAY_SERVICE_COMMAND_INTENT = "com.hunter.app.V2RAY_SERVICE_COMMAND";
    public static final String V2RAY_SERVICE_COMMAND_EXTRA = "V2RAY_SERVICE_COMMAND";
    public static final String V2RAY_SERVICE_CONFIG_EXTRA = "V2RAY_SERVICE_CONFIG";
    public static final String V2RAY_SERVICE_STATICS_BROADCAST_INTENT = "com.hunter.app.V2RAY_SERVICE_STATICS";
    public static final String V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_INTENT = "com.hunter.app.V2RAY_SERVICE_CURRENT_CONFIG_DELAY";
    public static final String V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_EXTRA = "V2RAY_SERVICE_CURRENT_CONFIG_DELAY";
    public static final String SERVICE_CONNECTION_STATE_BROADCAST_EXTRA = "SERVICE_CONNECTION_STATE";
    public static final String SERVICE_TYPE_BROADCAST_EXTRA = "SERVICE_TYPE";
    
    // Traffic units
    public static final long KILO_BYTE = 1024L;
    public static final long MEGA_BYTE = 1024L * 1024L;
    public static final long GIGA_BYTE = 1024L * 1024L * 1024L;
    
    // Default configuration
    public static final String DEFAULT_FULL_JSON_CONFIG = "{\n" +
            "  \"log\": {\n" +
            "    \"loglevel\": \"warning\"\n" +
            "  },\n" +
            "  \"inbounds\": [\n" +
            "    {\n" +
            "      \"listen\": \"127.0.0.1\",\n" +
            "      \"port\": 10808,\n" +
            "      \"protocol\": \"socks\",\n" +
            "      \"settings\": {\n" +
            "        \"auth\": \"noauth\",\n" +
            "        \"udp\": true,\n" +
            "        \"ip\": \"127.0.0.1\",\n" +
            "        \"clients\": []\n" +
            "      },\n" +
            "      \"sniffing\": {\n" +
            "        \"enabled\": true,\n" +
            "        \"destOverride\": [\"http\", \"tls\"]\n" +
            "      }\n" +
            "    }\n" +
            "  ],\n" +
            "  \"outbounds\": [\n" +
            "    {\n" +
            "      \"protocol\": \"vmess\",\n" +
            "      \"settings\": {\n" +
            "        \"vnext\": [\n" +
            "          {\n" +
            "            \"address\": \"your-server.com\",\n" +
            "            \"port\": 443,\n" +
            "            \"users\": [\n" +
            "              {\n" +
            "                \"id\": \"your-uuid\",\n" +
            "                \"level\": 0,\n" +
            "                \"alterId\": 0\n" +
            "              }\n" +
            "            ]\n" +
            "          }\n" +
            "        ]\n" +
            "      },\n" +
            "      \"streamSettings\": {\n" +
            "        \"network\": \"ws\"\n" +
            "      }\n" +
            "    }\n" +
            "  ]\n" +
            "}";
    
    public static final String DEFAULT_OUT_BOUND_PLACE_IN_FULL_JSON_CONFIG = "{\n" +
            "      \"protocol\": \"vmess\",\n" +
            "      \"settings\": {\n" +
            "        \"vnext\": [\n" +
            "          {\n" +
            "            \"address\": \"your-server.com\",\n" +
            "            \"port\": 443,\n" +
            "            \"users\": [\n" +
            "              {\n" +
            "                \"id\": \"your-uuid\",\n" +
            "                \"level\": 0,\n" +
            "                \"alterId\": 0\n" +
            "              }\n" +
            "            ]\n" +
            "          }\n" +
            "        ]\n" +
            "      },\n" +
            "      \"streamSettings\": {\n" +
            "        \"network\": \"ws\"\n" +
            "      }\n" +
            "    }";
}
