package com.hunter.app.service;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.os.Build;

import androidx.core.app.NotificationCompat;

import com.hunter.app.R;

/**
 * Notification service for VPN connection
 * Based on v2rayNG notification service
 */
public class NotificationService {
    private static final String TAG = "NotificationService";
    private static final String CHANNEL_ID = "vpn_service_channel";
    
    private final Context context;
    private final NotificationManager notificationManager;
    
    public TrafficListener trafficListener;

    public NotificationService(Context context) {
        this.context = context;
        this.notificationManager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        createNotificationChannel();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "VPN Service",
                NotificationManager.IMPORTANCE_LOW
            );
            channel.setDescription("VPN connection status");
            notificationManager.createNotificationChannel(channel);
        }
    }

    public void setConnectedNotification(String remark, int icon) {
        Notification notification = new NotificationCompat.Builder(context, CHANNEL_ID)
            .setContentTitle("Hunter VPN Connected")
            .setContentText("Connected to: " + remark)
            .setSmallIcon(icon)
            .setOngoing(true)
            .build();
        
        // Start foreground service
        if (context instanceof android.app.Service) {
            ((android.app.Service) context).startForeground(1, notification);
        }
    }

    public void dismissNotification() {
        notificationManager.cancel(1);
    }

    public interface TrafficListener {
        void onTrafficUpdate(long uploadSpeed, long downloadSpeed);
    }
}
