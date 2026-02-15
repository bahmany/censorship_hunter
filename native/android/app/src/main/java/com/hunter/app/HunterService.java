package com.hunter.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import androidx.core.app.NotificationCompat;

/**
 * Foreground service that keeps the Hunter engine running in the background.
 * Required for Android 8+ to maintain long-running background operations.
 */
public class HunterService extends Service {
    private static final String TAG = "HunterService";
    private static final String CHANNEL_ID = "hunter_service_channel";
    private static final int NOTIFICATION_ID = 1001;

    private final IBinder binder = new LocalBinder();
    private HunterNative hunterNative;
    private HunterCallbackImpl callbackImpl;
    private boolean isRunning = false;

    public class LocalBinder extends Binder {
        public HunterService getService() {
            return HunterService.this;
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        hunterNative = new HunterNative();
        callbackImpl = new HunterCallbackImpl(getApplicationContext());
        Log.i(TAG, "HunterService created");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, buildNotification("Hunter initializing..."), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTIFICATION_ID, buildNotification("Hunter initializing..."));
        }

        if (!isRunning) {
            initAndStart();
        }

        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override
    public void onDestroy() {
        stopHunter();
        super.onDestroy();
        Log.i(TAG, "HunterService destroyed");
    }

    private void initAndStart() {
        try {
            String filesDir = getFilesDir().getAbsolutePath();
            String secretsFile = filesDir + "/hunter_secrets.env";

            // Configure callbacks
            callbackImpl.setProgressListener(new HunterCallbackImpl.ProgressListener() {
                @Override
                public void onProgress(String phase, int current, int total) {
                    String text = "Phase: " + phase;
                    if (total > 0) {
                        text += " (" + current + "/" + total + ")";
                    }
                    updateNotification(text);
                    broadcastProgress(phase, current, total);
                }

                @Override
                public void onStatusUpdate(String statusJson) {
                    broadcastStatus(statusJson);
                }
            });

            // Read config from shared preferences or env file
            android.content.SharedPreferences prefs =
                    getSharedPreferences("hunter_config", MODE_PRIVATE);
            String botToken = prefs.getString("bot_token", "");
            String chatId = prefs.getString("chat_id", "");
            String xrayPath = prefs.getString("xray_path",
                    getApplicationInfo().nativeLibraryDir + "/libxray.so");

            callbackImpl.setBotToken(botToken);
            callbackImpl.setChatId(chatId);
            callbackImpl.setXrayBinaryPath(xrayPath);

            // Initialize native layer
            hunterNative.nativeInit(filesDir, secretsFile, callbackImpl);

            // Apply saved config overrides
            applyConfigOverrides(prefs);

            // Validate config
            String errors = hunterNative.nativeValidateConfig();
            if (!"[]".equals(errors)) {
                Log.w(TAG, "Config validation warnings: " + errors);
            }

            // Start
            hunterNative.nativeStart();
            isRunning = true;

            updateNotification("Hunter running - autonomous mode");
            Log.i(TAG, "Hunter started successfully");

        } catch (Exception e) {
            Log.e(TAG, "Failed to start Hunter: " + e.getMessage(), e);
            updateNotification("Hunter failed to start");
        }
    }

    private void applyConfigOverrides(android.content.SharedPreferences prefs) {
        String[][] overrides = {
                {"HUNTER_API_ID", prefs.getString("api_id", "")},
                {"HUNTER_API_HASH", prefs.getString("api_hash", "")},
                {"HUNTER_PHONE", prefs.getString("phone", "")},
                {"TOKEN", prefs.getString("bot_token", "")},
                {"CHAT_ID", prefs.getString("chat_id", "")},
                {"HUNTER_WORKERS", prefs.getString("max_workers", "30")},
                {"HUNTER_TEST_TIMEOUT", prefs.getString("timeout", "10")},
                {"HUNTER_SLEEP", prefs.getString("sleep_seconds", "300")},
                {"IRAN_FRAGMENT_ENABLED", prefs.getString("iran_fragment", "false")},
        };

        for (String[] entry : overrides) {
            if (entry[1] != null && !entry[1].isEmpty()) {
                hunterNative.nativeSetConfig(entry[0], entry[1]);
            }
        }
    }

    public void stopHunter() {
        if (isRunning) {
            try {
                hunterNative.nativeStop();
                hunterNative.nativeDestroy();
            } catch (Exception e) {
                Log.e(TAG, "Error stopping Hunter: " + e.getMessage());
            }
            callbackImpl.destroyAllProxies();
            isRunning = false;
        }
        stopForeground(Service.STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    public boolean isHunterRunning() {
        try {
            return hunterNative.nativeIsRunning();
        } catch (Exception e) {
            return false;
        }
    }

    public String getStatus() {
        try {
            return hunterNative.nativeGetStatus();
        } catch (Exception e) {
            return "{}";
        }
    }

    public void triggerCycle() {
        if (isRunning) {
            new Thread(() -> {
                try {
                    hunterNative.nativeRunCycle();
                } catch (Exception e) {
                    Log.e(TAG, "Manual cycle failed: " + e.getMessage());
                }
            }).start();
        }
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "Hunter Service",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Hunter proxy hunting service");
            channel.setShowBadge(false);

            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) {
                nm.createNotificationChannel(channel);
            }
        }
    }

    private Notification buildNotification(String text) {
        Intent intent = new Intent(this, MainActivity.class);
        intent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(this, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Hunter")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_menu_compass)
                .setContentIntent(pi)
                .setOngoing(true)
                .setSilent(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .build();
    }

    private void updateNotification(String text) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.notify(NOTIFICATION_ID, buildNotification(text));
        }
    }

    private void broadcastProgress(String phase, int current, int total) {
        Intent intent = new Intent("com.hunter.app.PROGRESS");
        intent.putExtra("phase", phase);
        intent.putExtra("current", current);
        intent.putExtra("total", total);
        sendBroadcast(intent);
    }

    private void broadcastStatus(String statusJson) {
        Intent intent = new Intent("com.hunter.app.STATUS");
        intent.putExtra("status", statusJson);
        sendBroadcast(intent);
    }
}
