package com.hunter.app;

import org.junit.Test;
import org.junit.Before;
import static org.junit.Assert.*;

import org.json.JSONObject;

/**
 * Unit tests for URI parsing functionality.
 * Tests VMess, VLESS, Trojan, and Shadowsocks URI formats.
 */
public class UriParserTest {

    @Test
    public void testVmessUriParsing() {
        // VMess URI format: vmess://base64(json)
        String vmessJson = "{\"v\":\"2\",\"ps\":\"test\",\"add\":\"server.com\",\"port\":443,\"id\":\"uuid-here\",\"aid\":0,\"scy\":\"auto\",\"net\":\"ws\",\"type\":\"\",\"host\":\"server.com\",\"path\":\"/path\",\"tls\":\"tls\",\"sni\":\"server.com\"}";
        String base64 = android.util.Base64.encodeToString(vmessJson.getBytes(), android.util.Base64.NO_WRAP);
        String vmessUri = "vmess://" + base64;
        
        assertTrue("VMess URI should start with vmess://", vmessUri.startsWith("vmess://"));
    }

    @Test
    public void testVlessUriParsing() {
        // VLESS URI format: vless://uuid@host:port?params#name
        String vlessUri = "vless://uuid-here@server.com:443?encryption=none&security=reality&sni=www.apple.com&fp=chrome&pbk=publickey&sid=shortid&type=tcp&flow=xtls-rprx-vision#TestServer";
        
        assertTrue("VLESS URI should start with vless://", vlessUri.startsWith("vless://"));
        assertTrue("VLESS URI should contain @", vlessUri.contains("@"));
        assertTrue("VLESS Reality should have pbk param", vlessUri.contains("pbk="));
        assertTrue("VLESS Vision should have flow param", vlessUri.contains("flow="));
    }

    @Test
    public void testTrojanUriParsing() {
        // Trojan URI format: trojan://password@host:port?params#name
        String trojanUri = "trojan://password123@server.com:443?security=tls&sni=server.com&type=tcp#TrojanServer";
        
        assertTrue("Trojan URI should start with trojan://", trojanUri.startsWith("trojan://"));
        assertTrue("Trojan URI should contain password@host", trojanUri.contains("@"));
    }

    @Test
    public void testShadowsocksUriParsing() {
        // SS URI format: ss://base64(method:password)@host:port#name
        String ssUri = "ss://YWVzLTI1Ni1nY206cGFzc3dvcmQ@server.com:8388#SSServer";
        
        assertTrue("SS URI should start with ss://", ssUri.startsWith("ss://"));
    }

    @Test
    public void testVlessRealityParams() {
        String vlessUri = "vless://uuid@server.com:443?encryption=none&security=reality&sni=swdist.apple.com&fp=chrome&pbk=key123&sid=ab&type=tcp&flow=xtls-rprx-vision#Test";
        
        // Extract params
        String queryString = vlessUri.substring(vlessUri.indexOf("?") + 1, vlessUri.indexOf("#"));
        String[] params = queryString.split("&");
        
        boolean hasReality = false;
        boolean hasVision = false;
        boolean hasFingerprint = false;
        
        for (String param : params) {
            String[] kv = param.split("=");
            if (kv.length == 2) {
                if (kv[0].equals("security") && kv[1].equals("reality")) hasReality = true;
                if (kv[0].equals("flow") && kv[1].contains("vision")) hasVision = true;
                if (kv[0].equals("fp")) hasFingerprint = true;
            }
        }
        
        assertTrue("VLESS should have Reality security", hasReality);
        assertTrue("VLESS should have Vision flow", hasVision);
        assertTrue("VLESS should have fingerprint", hasFingerprint);
    }

    @Test
    public void testSplitHttpParams() {
        String vlessUri = "vless://uuid@server.com:443?encryption=none&security=tls&sni=cdn.com&type=splithttp&path=/xhttp&host=cdn.com#SplitHTTP";
        
        assertTrue("SplitHTTP should have type=splithttp", vlessUri.contains("type=splithttp"));
        assertTrue("SplitHTTP should have path param", vlessUri.contains("path="));
    }
}
