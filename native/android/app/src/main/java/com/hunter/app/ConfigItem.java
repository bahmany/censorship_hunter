package com.hunter.app;

public class ConfigItem {
    public String name;
    public int latency;
    public String uri;
    public String protocol;
    public boolean googleAccessible;

    public ConfigItem(String name, int latency, String uri, String protocol) {
        this.name = name;
        this.latency = latency;
        this.uri = uri;
        this.protocol = protocol;
        this.googleAccessible = false;
    }
}
