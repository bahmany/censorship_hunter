package com.hunter.app.interfaces;

import com.hunter.app.utils.V2rayConstants;

/**
 * Interface for state monitoring
 * Based on v2rayNG state listener pattern
 */
public interface StateListener {
    
    /**
     * Get current connection state
     */
    V2rayConstants.CONNECTION_STATES getConnectionState();
    
    /**
     * Get current core state
     */
    V2rayConstants.CORE_STATES getCoreState();
    
    /**
     * Get current download speed
     */
    long getDownloadSpeed();
    
    /**
     * Get current upload speed
     */
    long getUploadSpeed();
}
