package com.hunter.app;

public class ConfigItem {
    private String ps;
    private int latency;
    private String uri;
    private String protocol;

    public ConfigItem(String ps, int latency, String uri, String protocol) {
        this.ps = ps;
        this.latency = latency;
        this.uri = uri;
        this.protocol = protocol;
    }

    public String getPs() {
        return ps;
    }

    public int getLatency() {
        return latency;
    }

    public String getUri() {
        return uri;
    }

    public String getProtocol() {
        return protocol;
    }
}
