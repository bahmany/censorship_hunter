package com.hunter.app.service;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import com.hunter.app.interfaces.StateListener;
import com.hunter.app.utils.V2rayConstants;

/**
 * Broadcast service for statistics and state management
 * Based on v2rayNG statistics broadcast service
 */
public class StaticsBroadCastService {
    private static final String TAG = "StaticsBroadCastService";
    
    private final Context context;
    private final StateListener stateListener;
    private Thread broadcastThread;
    private volatile boolean isRunning = false;
    
    public TrafficListener trafficListener;
    public boolean isTrafficStaticsEnabled = false;

    public StaticsBroadCastService(Context context, StateListener stateListener) {
        this.context = context;
        this.stateListener = stateListener;
    }

    public void start() {
        if (isRunning) return;
        
        isRunning = true;
        broadcastThread = new Thread(() -> {
            while (isRunning) {
                try {
                    if (isTrafficStaticsEnabled && trafficListener != null) {
                        long uploadSpeed = stateListener.getUploadSpeed();
                        long downloadSpeed = stateListener.getDownloadSpeed();
                        trafficListener.onTrafficUpdate(uploadSpeed, downloadSpeed);
                    }
                    Thread.sleep(1000); // Update every second
                } catch (InterruptedException e) {
                    isRunning = false;
                    break;
                } catch (Exception e) {
                    Log.e(TAG, "Error in broadcast loop", e);
                }
            }
        }, "StatsBroadcastThread");
        broadcastThread.start();
    }

    public void stop() {
        isRunning = false;
        if (broadcastThread != null) {
            broadcastThread.interrupt();
            broadcastThread = null;
        }
    }

    public void sendDisconnectedBroadCast(Context context) {
        Intent intent = new Intent(V2rayConstants.V2RAY_SERVICE_STATICS_BROADCAST_INTENT);
        intent.setPackage(context.getPackageName());
        intent.putExtra(V2rayConstants.SERVICE_CONNECTION_STATE_BROADCAST_EXTRA, V2rayConstants.CONNECTION_STATES.DISCONNECTED);
        intent.putExtra(V2rayConstants.SERVICE_TYPE_BROADCAST_EXTRA, "EnhancedVpnService");
        context.sendBroadcast(intent);
    }

    public interface TrafficListener {
        void onTrafficUpdate(long uploadSpeed, long downloadSpeed);
    }
}
