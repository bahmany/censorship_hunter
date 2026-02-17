package com.hunter.app.interfaces;

import com.hunter.app.utils.V2rayConstants;

/**
 * Interface for tun2socks communication
 * Based on v2rayNG tun2socks listener pattern
 */
public interface Tun2SocksListener {
    
    /**
     * Called when tun2socks has a message or state change
     */
    void OnTun2SocksHasMessage(V2rayConstants.CORE_STATES state, String message);
}
