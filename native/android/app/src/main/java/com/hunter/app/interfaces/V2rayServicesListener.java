package com.hunter.app.interfaces;

import android.app.Service;

/**
 * Interface for V2Ray service communication
 * Based on v2rayNG service listener pattern
 */
public interface V2rayServicesListener {
    
    /**
     * Called when core setup is complete and service should start
     */
    void startService();
    
    /**
     * Called when service should stop
     */
    void stopService();
    
    /**
     * Protect socket from VPN
     */
    boolean onProtect(int socket);
    
    /**
     * Get the service instance
     */
    Service getService();
}
