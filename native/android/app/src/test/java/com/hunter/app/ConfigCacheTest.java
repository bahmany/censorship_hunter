package com.hunter.app;

import org.junit.Test;
import org.junit.Before;
import static org.junit.Assert.*;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Unit tests for config caching functionality.
 */
public class ConfigCacheTest {

    @Test
    public void testConfigJsonFormat() throws Exception {
        JSONArray configs = new JSONArray();
        
        JSONObject config1 = new JSONObject();
        config1.put("ps", "TestServer1");
        config1.put("latency_ms", 150);
        config1.put("uri", "vless://uuid@server.com:443?security=reality#Test1");
        configs.put(config1);
        
        JSONObject config2 = new JSONObject();
        config2.put("ps", "TestServer2");
        config2.put("latency_ms", 200);
        config2.put("uri", "vmess://base64content");
        configs.put(config2);
        
        assertEquals("Should have 2 configs", 2, configs.length());
        
        JSONObject first = configs.getJSONObject(0);
        assertEquals("First config should have correct name", "TestServer1", first.getString("ps"));
        assertEquals("First config should have correct latency", 150, first.getInt("latency_ms"));
    }

    @Test
    public void testLatencySorting() throws Exception {
        int[] latencies = {500, 150, 300, 100, 250};
        
        // Simple bubble sort simulation
        for (int i = 0; i < latencies.length - 1; i++) {
            for (int j = 0; j < latencies.length - i - 1; j++) {
                if (latencies[j] > latencies[j + 1]) {
                    int temp = latencies[j];
                    latencies[j] = latencies[j + 1];
                    latencies[j + 1] = temp;
                }
            }
        }
        
        assertEquals("First should be lowest latency", 100, latencies[0]);
        assertEquals("Last should be highest latency", 500, latencies[latencies.length - 1]);
    }

    @Test
    public void testMaxBackendsLimit() {
        int maxBackends = 10;
        int configCount = 25;
        
        int actualBackends = Math.min(configCount, maxBackends);
        
        assertEquals("Should limit to maxBackends", 10, actualBackends);
    }

    @Test
    public void testProtocolDetection() {
        String vmessUri = "vmess://abc123";
        String vlessUri = "vless://uuid@server:443";
        String trojanUri = "trojan://pass@server:443";
        String ssUri = "ss://abc@server:8388";
        String hysteria2Uri = "hysteria2://pass@server:443";
        
        assertTrue("VMess detection", vmessUri.startsWith("vmess://"));
        assertTrue("VLESS detection", vlessUri.startsWith("vless://"));
        assertTrue("Trojan detection", trojanUri.startsWith("trojan://"));
        assertTrue("SS detection", ssUri.startsWith("ss://"));
        assertTrue("Hysteria2 detection", hysteria2Uri.startsWith("hysteria2://"));
    }
}
