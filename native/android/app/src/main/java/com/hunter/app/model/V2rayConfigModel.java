package com.hunter.app.model;

import java.io.Serializable;
import java.util.ArrayList;

/**
 * V2Ray configuration model
 * Based on v2rayNG configuration structure
 */
public class V2rayConfigModel implements Serializable {
    private static final long serialVersionUID = 1L;
    
    // Basic configuration
    public String remark = "";
    public String fullJsonConfig = "";
    public String currentServerAddress = "";
    public int currentServerPort = 443;
    
    // Network settings
    public int localSocksPort = 10808;
    public int localHttpPort = 10809;
    
    // Application settings
    public ArrayList<String> blockedApplications;
    
    // Statistics settings
    public boolean enableTrafficStatics = false;
    public boolean enableTrafficStaticsOnNotification = false;
    
    // DNS settings
    public boolean enableLocalTunneledDNS = false;
    public int localDNSPort = 5353;
    
    // Application info
    public int applicationIcon = android.R.drawable.ic_dialog_info;
    public String applicationName = "Hunter VPN";
    
    public V2rayConfigModel() {
        blockedApplications = new ArrayList<>();
    }
    
    public V2rayConfigModel(String remark, String config) {
        this();
        this.remark = remark;
        this.fullJsonConfig = config;
    }
}
