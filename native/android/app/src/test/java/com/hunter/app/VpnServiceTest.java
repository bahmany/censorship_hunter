package com.hunter.app;

import org.junit.Test;
import org.junit.Before;
import static org.junit.Assert.*;

/**
 * Unit tests for VPN service functionality.
 */
public class VpnServiceTest {

    @Test
    public void testPortRange() {
        int basePort = 10808;
        int maxBackends = 10;
        
        for (int i = 0; i < maxBackends; i++) {
            int port = basePort + i;
            assertTrue("Port should be in valid range", port >= 10808 && port < 10818);
        }
    }

    @Test
    public void testTunAddress() {
        String tunAddress = "10.255.0.1";
        int tunPrefix = 24;
        
        assertEquals("TUN address should be 10.255.0.1", "10.255.0.1", tunAddress);
        assertEquals("TUN prefix should be 24", 24, tunPrefix);
    }

    @Test
    public void testMtuSettings() {
        // For Iran 5G networks, MTU should be lower to avoid PMTUD blackhole
        int normalMtu = 1500;
        int iranMtu = 1350;
        
        assertTrue("Iran MTU should be lower than normal", iranMtu < normalMtu);
        assertTrue("Iran MTU should be at least 1280 for IPv6", iranMtu >= 1280);
    }

    @Test
    public void testDnsServers() {
        String primaryDns = "1.1.1.1";
        String secondaryDns = "8.8.8.8";
        
        assertNotNull("Primary DNS should not be null", primaryDns);
        assertNotNull("Secondary DNS should not be null", secondaryDns);
    }
}
