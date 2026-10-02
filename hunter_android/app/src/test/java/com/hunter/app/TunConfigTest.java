package com.hunter.app;

import static org.junit.Assert.*;
import org.junit.Test;

public class TunConfigTest {
    @Test public void yamlContainsPortAndLoopback() {
        String y = TunConfig.hevYaml(3111);
        assertTrue(y.contains("port: 3111"));
        assertTrue(y.contains("address: 127.0.0.1"));
        assertTrue(y.contains("mapdns:"));
    }
    @Test(expected = IllegalArgumentException.class) public void rejectsBadPort() { TunConfig.hevYaml(0); }
    @Test public void failoverThreshold() {
        assertFalse(TunConfig.shouldFailover(2, 3));
        assertTrue(TunConfig.shouldFailover(3, 3));
    }
}
