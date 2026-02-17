package com.hunter.app.utils;

import com.hunter.app.model.V2rayConfigModel;

/**
 * Global configuration holder
 * Based on v2rayNG configuration management
 */
public class V2rayConfigs {
    public static V2rayConfigModel currentConfig = new V2rayConfigModel();
    public static V2rayConstants.CONNECTION_STATES connectionState = V2rayConstants.CONNECTION_STATES.DISCONNECTED;
    public static V2rayConstants.SERVICE_MODES serviceMode = V2rayConstants.SERVICE_MODES.VPN_MODE;
}
