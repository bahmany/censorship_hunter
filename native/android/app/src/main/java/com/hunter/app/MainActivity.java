package com.hunter.app;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.VpnService;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.localbroadcastmanager.content.LocalBroadcastManager;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.net.TrafficStats;

import android.animation.ObjectAnimator;
import android.animation.AnimatorSet;
import android.content.SharedPreferences;
import android.view.animation.AccelerateDecelerateInterpolator;

import androidx.appcompat.widget.SwitchCompat;

import java.util.Collections;
import java.util.List;

/**
 * Main activity for Iranian Free V2Ray.
 * Pure Java implementation - no native dependencies.
 */
public class MainActivity extends AppCompatActivity {

    private ImageView connectButton;
    private View refreshButton, shareButton, switchButton, exitButton;
    private ProgressBar progressBar;
    private ProgressBar v2rayProgressBar;
    private TextView statusText, statusDetail, configCount;
    private TextView progressLabel, progressDetail;
    private ImageView statusIcon;
    private ImageButton settingsButton, aboutButton;
    private View progressCard;
    private View speedTrafficCard;
    private TextView downloadSpeed, uploadSpeed, totalTraffic, sessionTime;
    private TextView patienceMessage;
    private TextView poolSizeText;
    private SwitchCompat continuousScanSwitch;

    // Power button visuals
    private ImageView powerGlow, powerRing;

    private ConfigManager configManager;
    private boolean isConnected = false;
    private boolean isConnecting = false;
    private BroadcastReceiver progressReceiver;
    private BroadcastReceiver vpnStatusReceiver;
    private BroadcastReceiver configPoolReceiver;

    // Speed and traffic monitoring
    private Handler speedUpdateHandler;
    private Runnable speedUpdateRunnable;
    private long sessionStartTime = 0;
    private long lastRxBytes = 0;
    private long lastTxBytes = 0;
    private long totalRxBytes = 0;
    private long totalTxBytes = 0;

    // Speed averaging for smoother display
    private final int SPEED_HISTORY_SIZE = 5;
    private final java.util.Queue<Long> rxSpeedHistory = new java.util.LinkedList<>();
    private final java.util.Queue<Long> txSpeedHistory = new java.util.LinkedList<>();

    private final ActivityResultLauncher<Intent> vpnPermissionLauncher =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK) {
                    startVpnService();
                } else {
                    setDisconnectedState();
                    statusDetail.setText("VPN permission required");
                }
            });

    private final ActivityResultLauncher<Intent> configSelectLauncher =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                    java.util.ArrayList<String> uris = result.getData()
                        .getStringArrayListExtra(ConfigSelectActivity.EXTRA_SELECTED_URIS);
                    if (uris != null && !uris.isEmpty()) {
                        connectWithSelectedConfigs(uris);
                    }
                }
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main_v2);

        initViews();
        setupListeners();

        // Initialize progress receiver
        progressReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                if ("com.hunter.app.V2RAY_PROGRESS".equals(intent.getAction())) {
                    String label = intent.getStringExtra("label");
                    String detail = intent.getStringExtra("detail");
                    int progress = intent.getIntExtra("progress", 0);
                    int max = intent.getIntExtra("max", 100);
                    boolean show = intent.getBooleanExtra("show", true);

                    if (show) {
                        showV2RayProgress(label, detail, progress, max);
                    } else {
                        hideV2RayProgress();
                    }
                }
            }
        };
        LocalBroadcastManager.getInstance(this).registerReceiver(progressReceiver, 
            new IntentFilter("com.hunter.app.V2RAY_PROGRESS"));

        // Config pool receiver - updates pool size display
        configPoolReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                if (com.hunter.app.VpnService.BROADCAST_CONFIG_POOL.equals(intent.getAction())) {
                    int poolSize = intent.getIntExtra("pool_size", 0);
                    runOnUiThread(() -> updatePoolSizeDisplay(poolSize));
                }
            }
        };
        LocalBroadcastManager.getInstance(this).registerReceiver(configPoolReceiver,
            new IntentFilter(com.hunter.app.VpnService.BROADCAST_CONFIG_POOL));

        // VPN status receiver - updates UI based on VpnService state changes
        vpnStatusReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                if (com.hunter.app.VpnService.BROADCAST_STATUS.equals(intent.getAction())) {
                    String status = intent.getStringExtra("status");
                    String detail = intent.getStringExtra("detail");
                    boolean connecting = intent.getBooleanExtra("isConnecting", false);
                    boolean connected = intent.getBooleanExtra("isConnected", false);
                    
                    runOnUiThread(() -> {
                        if (connected) {
                            setConnectedState();
                            if (detail != null) statusDetail.setText(detail);
                        } else if (connecting) {
                            setConnectingState();
                            if (status != null) statusText.setText(status);
                            if (detail != null) statusDetail.setText(detail);
                        } else {
                            setDisconnectedState();
                            if (detail != null) statusDetail.setText(detail);
                        }
                    });
                }
            }
        };
        LocalBroadcastManager.getInstance(this).registerReceiver(vpnStatusReceiver,
            new IntentFilter(com.hunter.app.VpnService.BROADCAST_STATUS));

        // Request notification permission for Android 13+
        requestNotificationPermission();

        // Get config manager from application
        FreeV2RayApplication app = FreeV2RayApplication.getInstance();
        if (app != null) {
            configManager = app.getConfigManager();
        }
        if (configManager == null) {
            Toast.makeText(this, "Initialization error", Toast.LENGTH_LONG).show();
            return;
        }

        updateConfigStats();

        // Start background monitor service
        startMonitorService();

        // Auto-refresh if no configs
        if (configManager.getConfigCount() == 0) {
            refreshConfigs();
        }
    }

    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this,
                    new String[]{Manifest.permission.POST_NOTIFICATIONS}, 100);
            }
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 100) {
            startMonitorService();
        }
    }

    private void startMonitorService() {
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                        != PackageManager.PERMISSION_GRANTED) {
                    return;
                }
            }
            Intent monitorIntent = new Intent(this, ConfigMonitorService.class);
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                startForegroundService(monitorIntent);
            } else {
                startService(monitorIntent);
            }
        } catch (Exception e) {
            android.util.Log.w("MainActivity", "Could not start monitor service", e);
        }
    }

    private void initViews() {
        connectButton = findViewById(R.id.connect_button);
        progressBar = findViewById(R.id.progress_bar);
        v2rayProgressBar = findViewById(R.id.v2ray_progress_bar);
        statusText = findViewById(R.id.status_text);
        statusDetail = findViewById(R.id.status_detail);
        configCount = findViewById(R.id.config_count);
        progressLabel = findViewById(R.id.progress_label);
        progressDetail = findViewById(R.id.progress_detail);
        statusIcon = findViewById(R.id.status_icon);
        settingsButton = findViewById(R.id.btn_settings);
        refreshButton = findViewById(R.id.btn_refresh);
        shareButton = findViewById(R.id.btn_share);
        switchButton = findViewById(R.id.btn_switch);
        exitButton = findViewById(R.id.btn_exit);
        aboutButton = findViewById(R.id.btn_about);
        progressCard = findViewById(R.id.progress_card);
        speedTrafficCard = findViewById(R.id.speed_traffic_card);
        downloadSpeed = findViewById(R.id.download_speed);
        uploadSpeed = findViewById(R.id.upload_speed);
        totalTraffic = findViewById(R.id.total_traffic);
        sessionTime = findViewById(R.id.session_time);
        powerGlow = findViewById(R.id.power_glow);
        powerRing = findViewById(R.id.power_ring);
        patienceMessage = findViewById(R.id.patience_message);
        poolSizeText = findViewById(R.id.pool_size_text);
        continuousScanSwitch = findViewById(R.id.continuous_scan_switch);
        
        // Switch button always enabled — opens config selection
        switchButton.setEnabled(true);
        switchButton.setAlpha(1.0f);

        // Select server button
        View selectServerBtn = findViewById(R.id.btn_select_server);
        if (selectServerBtn != null) {
            selectServerBtn.setOnClickListener(v -> switchConfig());
        }

        // DNS and fragment switches are display-only for now (always enabled)
        // They show the user that these features are active
        SwitchCompat dnsSwitch = findViewById(R.id.dns_proxy_switch);
        if (dnsSwitch != null) {
            dnsSwitch.setChecked(true);
            dnsSwitch.setEnabled(false); // Always on — DNS must go through proxy
        }
        SwitchCompat fragmentSwitch = findViewById(R.id.fragment_switch);
        if (fragmentSwitch != null) {
            fragmentSwitch.setChecked(true);
        }

        // Load continuous scan preference
        SharedPreferences vpnPrefs = getSharedPreferences("vpn_settings", MODE_PRIVATE);
        boolean scanEnabled = vpnPrefs.getBoolean(com.hunter.app.VpnService.PREF_CONTINUOUS_SCAN, true);
        if (continuousScanSwitch != null) {
            continuousScanSwitch.setChecked(scanEnabled);
            continuousScanSwitch.setOnCheckedChangeListener((buttonView, isChecked) -> {
                vpnPrefs.edit().putBoolean(com.hunter.app.VpnService.PREF_CONTINUOUS_SCAN, isChecked).apply();
            });
        }
    }

    private void setupListeners() {
        connectButton.setOnClickListener(v -> toggleVpn());
        
        settingsButton.setOnClickListener(v -> 
            startActivity(new Intent(this, SettingsActivity.class)));
        
        aboutButton.setOnClickListener(v -> 
            startActivity(new Intent(this, AboutActivity.class)));
        
        refreshButton.setOnClickListener(v -> refreshConfigs());
        
        switchButton.setOnClickListener(v -> switchConfig());
        
        shareButton.setOnClickListener(v -> 
            startActivity(new Intent(this, ConfigShareActivity.class)));
        
        exitButton.setOnClickListener(v -> exitApp());
    }

    private void switchConfig() {
        // Open config selection activity
        configSelectLauncher.launch(new Intent(this, ConfigSelectActivity.class));
    }

    private void connectWithSelectedConfigs(java.util.ArrayList<String> uris) {
        // Disconnect if currently connected
        if (isConnected || isConnecting) {
            disconnectVpn();
        }

        // Start VPN with selected configs after short delay
        connectButton.postDelayed(() -> {
            Intent vpnIntent = VpnService.prepare(this);
            if (vpnIntent != null) {
                vpnPermissionLauncher.launch(vpnIntent);
                // Store selected URIs for after permission grant
                getSharedPreferences("vpn_settings", MODE_PRIVATE).edit()
                    .putStringSet("custom_uris", new java.util.HashSet<>(uris)).apply();
            } else {
                startVpnServiceWithUris(uris);
            }
        }, isConnected ? 1500 : 100);
    }

    private void startVpnServiceWithUris(java.util.ArrayList<String> uris) {
        Intent intent = new Intent(this, com.hunter.app.VpnService.class);
        intent.setAction(com.hunter.app.VpnService.ACTION_START);
        intent.putStringArrayListExtra("custom_uris", uris);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }
        setConnectingState();
    }

    private void refreshConfigs() {
        refreshButton.setEnabled(false);
        refreshButton.setAlpha(0.4f);
        statusDetail.setText("Fetching configs from GitHub...");
        
        configManager.refreshConfigs(new ConfigManager.RefreshCallback() {
            @Override
            public void onProgress(String message, int current, int total) {
                runOnUiThread(() -> {
                    statusDetail.setText(message + " (" + current + "/" + total + ")");
                });
            }

            @Override
            public void onComplete(int totalConfigs) {
                runOnUiThread(() -> {
                    refreshButton.setEnabled(true);
                    refreshButton.setAlpha(1.0f);
                    statusDetail.setText("Found " + totalConfigs + " configs");
                    updateConfigStats();
                    Toast.makeText(MainActivity.this, 
                        "Found " + totalConfigs + " configs", Toast.LENGTH_SHORT).show();
                });
            }

            @Override
            public void onError(String error) {
                runOnUiThread(() -> {
                    refreshButton.setEnabled(true);
                    refreshButton.setAlpha(1.0f);
                    statusDetail.setText("Error: " + error);
                    Toast.makeText(MainActivity.this, 
                        "Error: " + error, Toast.LENGTH_SHORT).show();
                });
            }
        });
    }

    private void toggleVpn() {
        if (isConnecting) return;

        if (isConnected) {
            disconnectVpn();
        } else {
            connectVpn();
        }
    }

    private void connectVpn() {
        if (configManager.getConfigCount() == 0) {
            Toast.makeText(this, "No configs available. Tap Refresh first.", Toast.LENGTH_SHORT).show();
            return;
        }

        if (Build.VERSION.SDK_INT >= 33) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                requestNotificationPermission();
                Toast.makeText(this, "Please allow notifications to start VPN", Toast.LENGTH_SHORT).show();
                return;
            }
        }

        // Check XRay engine readiness (binary is bundled in APK, just needs extraction on first run)
        XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
        if (xrayManager == null || !xrayManager.isReady()) {
            setConnectingState();
            statusDetail.setText("Preparing V2Ray engine...");
            if (xrayManager != null) {
                xrayManager.ensureInstalled(new XRayManager.InstallCallback() {
                    @Override
                    public void onProgress(String message, int percent) {
                        runOnUiThread(() -> statusDetail.setText(message));
                    }
                    @Override
                    public void onComplete(String binaryPath) {
                        runOnUiThread(() -> {
                            statusDetail.setText("Engine ready, connecting...");
                            proceedWithVpnConnect();
                        });
                    }
                    @Override
                    public void onError(String error) {
                        runOnUiThread(() -> {
                            setDisconnectedState();
                            statusDetail.setText("Engine installation error: " + error);
                            Toast.makeText(MainActivity.this,
                                "V2Ray engine installation error", Toast.LENGTH_LONG).show();
                        });
                    }
                });
            } else {
                setDisconnectedState();
                statusDetail.setText("V2Ray engine not available");
            }
            return;
        }

        setConnectingState();
        proceedWithVpnConnect();
    }

    private void proceedWithVpnConnect() {
        // Check VPN permission
        Intent vpnIntent = VpnService.prepare(this);
        if (vpnIntent != null) {
            vpnPermissionLauncher.launch(vpnIntent);
        } else {
            startVpnService();
        }
    }

    private void startVpnService() {
        try {
            Intent intent = new Intent(this, com.hunter.app.VpnService.class);
            intent.setAction(com.hunter.app.VpnService.ACTION_START);
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                startForegroundService(intent);
            } else {
                startService(intent);
            }
        } catch (Throwable t) {
            android.util.Log.e("MainActivity", "Failed to start VPN service", t);
            setDisconnectedState();
            statusDetail.setText("Failed to start VPN service");
            return;
        }

        waitForVpnActive(System.currentTimeMillis());
    }

    private void waitForVpnActive(long startTimeMs) {
        connectButton.postDelayed(() -> {
            if (!isConnecting) return;
            if (VpnState.isActive()) {
                setConnectedState();
                return;
            }
            if (System.currentTimeMillis() - startTimeMs > 60000) {
                setDisconnectedState();
                statusDetail.setText("Connection timeout - try again");
                return;
            }
            waitForVpnActive(startTimeMs);
        }, 1000);
    }

    private void disconnectVpn() {
        try {
            Intent intent = new Intent(this, com.hunter.app.VpnService.class);
            intent.setAction(com.hunter.app.VpnService.ACTION_STOP);
            startService(intent);
        } catch (Throwable t) {
            android.util.Log.w("MainActivity", "Failed to stop VPN service", t);
        } finally {
            setDisconnectedState();
        }
    }

    private void setConnectingState() {
        isConnecting = true;
        isConnected = false;
        progressBar.setVisibility(View.VISIBLE);
        statusText.setText("Connecting");
        statusText.setTextColor(getColor(R.color.accent_amber));
        statusDetail.setText("Establishing connection...");

        // Show patience message
        if (patienceMessage != null) patienceMessage.setVisibility(View.VISIBLE);

        // Power button: amber glow + connecting ring
        powerRing.setImageResource(R.drawable.bg_power_ring_connecting);
        powerGlow.setImageResource(R.drawable.bg_power_glow_connecting);
        animateGlow(0.6f);
        pulseRing(true);

        // switchButton stays enabled during connecting
    }

    private void setConnectedState() {
        isConnecting = false;
        isConnected = true;
        progressBar.setVisibility(View.GONE);
        statusText.setText("Connected");
        statusText.setTextColor(getColor(R.color.accent_cyan));
        statusDetail.setText("VPN is active");

        // Hide patience message
        if (patienceMessage != null) patienceMessage.setVisibility(View.GONE);

        // Power button: cyan glow + on ring
        powerRing.setImageResource(R.drawable.bg_power_ring_on);
        powerGlow.setImageResource(R.drawable.bg_power_glow_on);
        animateGlow(0.8f);
        pulseRing(false);
        powerRing.setScaleX(1f);
        powerRing.setScaleY(1f);

        startSpeedMonitoring();
    }

    private void setDisconnectedState() {
        isConnecting = false;
        isConnected = false;
        progressBar.setVisibility(View.GONE);
        statusText.setText("Disconnected");
        statusText.setTextColor(getColor(R.color.text_secondary));
        statusDetail.setText("Tap to connect");

        // Hide patience message
        if (patienceMessage != null) patienceMessage.setVisibility(View.GONE);

        // Power button: dim ring, no glow
        powerRing.setImageResource(R.drawable.bg_power_ring_off);
        animateGlow(0f);
        pulseRing(false);
        powerRing.setScaleX(1f);
        powerRing.setScaleY(1f);

        switchButton.setEnabled(false);
        switchButton.setAlpha(0.4f);

        stopSpeedMonitoring();
    }

    private void animateGlow(float targetAlpha) {
        if (powerGlow == null) return;
        powerGlow.animate()
            .alpha(targetAlpha)
            .setDuration(600)
            .setInterpolator(new AccelerateDecelerateInterpolator())
            .start();
    }

    private AnimatorSet pulseAnimator;

    private void pulseRing(boolean start) {
        if (powerRing == null) return;
        if (pulseAnimator != null) {
            pulseAnimator.cancel();
            pulseAnimator = null;
        }
        if (!start) return;

        ObjectAnimator scaleX = ObjectAnimator.ofFloat(powerRing, "scaleX", 1f, 1.06f, 1f);
        ObjectAnimator scaleY = ObjectAnimator.ofFloat(powerRing, "scaleY", 1f, 1.06f, 1f);
        pulseAnimator = new AnimatorSet();
        pulseAnimator.playTogether(scaleX, scaleY);
        pulseAnimator.setDuration(1500);
        pulseAnimator.setInterpolator(new AccelerateDecelerateInterpolator());
        pulseAnimator.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(android.animation.Animator animation) {
                if (isConnecting && pulseAnimator != null) {
                    pulseAnimator.start();
                }
            }
        });
        pulseAnimator.start();
    }

    private void showV2RayProgress(String label, String detail, int progress, int max) {
        runOnUiThread(() -> {
            if (progressCard != null) {
                progressCard.setVisibility(View.VISIBLE);
                if (progressLabel != null) progressLabel.setText(label);
                if (progressDetail != null) progressDetail.setText(detail);
                if (v2rayProgressBar != null) {
                    v2rayProgressBar.setMax(max);
                    v2rayProgressBar.setProgress(progress);
                }
            }
        });
    }

    private void hideV2RayProgress() {
        runOnUiThread(() -> {
            if (progressCard != null) {
                progressCard.setVisibility(View.GONE);
            }
        });
    }

    private void updateV2RayProgress(String label, String detail, int progress) {
        runOnUiThread(() -> {
            if (progressLabel != null) progressLabel.setText(label);
            if (progressDetail != null) progressDetail.setText(detail);
            if (v2rayProgressBar != null) v2rayProgressBar.setProgress(progress);
        });
    }

    private void startSpeedMonitoring() {
        sessionStartTime = System.currentTimeMillis();
        resetTrafficStats();

        speedUpdateHandler = new Handler(Looper.getMainLooper());
        speedUpdateRunnable = new Runnable() {
            @Override
            public void run() {
                updateSpeedAndTraffic();
                speedUpdateHandler.postDelayed(this, 2000); // Update every 2 seconds for smoother display
            }
        };
        speedUpdateHandler.post(speedUpdateRunnable);

        // Show speed/traffic card
        if (speedTrafficCard != null) {
            speedTrafficCard.setVisibility(View.VISIBLE);
        }
    }

    private void stopSpeedMonitoring() {
        if (speedUpdateHandler != null && speedUpdateRunnable != null) {
            speedUpdateHandler.removeCallbacks(speedUpdateRunnable);
        }

        // Hide speed/traffic card
        if (speedTrafficCard != null) {
            speedTrafficCard.setVisibility(View.GONE);
        }
    }

    private void resetTrafficStats() {
        lastRxBytes = getTotalRxBytes();
        lastTxBytes = getTotalTxBytes();
        totalRxBytes = 0;
        totalTxBytes = 0;
    }

    private void updateSpeedAndTraffic() {
        long currentRxBytes = getTotalRxBytes();
        long currentTxBytes = getTotalTxBytes();

        // Calculate speeds (bytes per second)
        long rxSpeed = currentRxBytes - lastRxBytes;
        long txSpeed = currentTxBytes - lastTxBytes;

        // Add to history for averaging
        rxSpeedHistory.add(rxSpeed);
        txSpeedHistory.add(txSpeed);
        if (rxSpeedHistory.size() > SPEED_HISTORY_SIZE) {
            rxSpeedHistory.poll();
        }
        if (txSpeedHistory.size() > SPEED_HISTORY_SIZE) {
            txSpeedHistory.poll();
        }

        // Calculate average speeds
        long avgRxSpeed = rxSpeedHistory.stream().mapToLong(Long::longValue).sum() / rxSpeedHistory.size();
        long avgTxSpeed = txSpeedHistory.stream().mapToLong(Long::longValue).sum() / txSpeedHistory.size();

        // Update totals
        totalRxBytes += rxSpeed;
        totalTxBytes += txSpeed;

        // Update last values
        lastRxBytes = currentRxBytes;
        lastTxBytes = currentTxBytes;

        // Format average speeds
        String downloadSpeedStr = formatSpeed(avgRxSpeed);
        String uploadSpeedStr = formatSpeed(avgTxSpeed);
        String totalTrafficStr = formatTraffic(totalRxBytes + totalTxBytes);

        // Update UI (remove session time for cleaner look)
        runOnUiThread(() -> {
            if (downloadSpeed != null) downloadSpeed.setText(downloadSpeedStr);
            if (uploadSpeed != null) uploadSpeed.setText(uploadSpeedStr);
            if (totalTraffic != null) totalTraffic.setText(totalTrafficStr);
            // Hide session time to make it less timer-focused
            if (sessionTime != null) sessionTime.setVisibility(View.GONE);
        });
    }

    private long getTotalRxBytes() {
        try {
            return TrafficStats.getTotalRxBytes();
        } catch (Exception e) {
            return 0;
        }
    }

    private long getTotalTxBytes() {
        try {
            return TrafficStats.getTotalTxBytes();
        } catch (Exception e) {
            return 0;
        }
    }

    private String formatSpeed(long bytesPerSecond) {
        if (bytesPerSecond < 1024) {
            return bytesPerSecond + " B/s";
        } else if (bytesPerSecond < 1024 * 1024) {
            return String.format("%.1f KB/s", bytesPerSecond / 1024.0);
        } else {
            return String.format("%.1f MB/s", bytesPerSecond / (1024.0 * 1024.0));
        }
    }

    private String formatTraffic(long totalBytes) {
        if (totalBytes < 1024 * 1024) {
            return String.format("%.1f KB", totalBytes / 1024.0);
        } else {
            return String.format("%.1f MB", totalBytes / (1024.0 * 1024.0));
        }
    }

    private String formatSessionTime() {
        if (sessionStartTime == 0) return "00:00:00";

        long elapsed = System.currentTimeMillis() - sessionStartTime;
        long seconds = elapsed / 1000;
        long minutes = seconds / 60;
        long hours = minutes / 60;

        return String.format("%02d:%02d:%02d", hours % 24, minutes % 60, seconds % 60);
    }

    private void updateConfigStats() {
        try {
            int total = configManager.getConfigCount();
            int top = configManager.getTopConfigCount();
            
            if (top > 0) {
                configCount.setText(top + " / " + total);
            } else {
                configCount.setText(String.valueOf(total));
            }
        } catch (Exception e) {
            configCount.setText("0");
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Update VPN state
        if (VpnState.isActive()) {
            setConnectedState();
        } else {
            setDisconnectedState();
        }
        updateConfigStats();
    }

    private void exitApp() {
        // Stop VPN service completely
        try {
            Intent intent = new Intent(this, com.hunter.app.VpnService.class);
            intent.setAction(com.hunter.app.VpnService.ACTION_EXIT);
            startService(intent);
        } catch (Throwable t) {
            android.util.Log.w("MainActivity", "Failed to send exit to VPN service", t);
        }
        
        // Stop monitor service
        try {
            stopService(new Intent(this, ConfigMonitorService.class));
        } catch (Throwable ignored) {}
        
        // Finish activity
        finishAffinity();
    }

    private void updatePoolSizeDisplay(int poolSize) {
        if (poolSizeText == null) return;
        if (poolSize > 1 && isConnected) {
            poolSizeText.setText(String.format(getString(R.string.pool_active), poolSize));
            poolSizeText.setVisibility(View.VISIBLE);
        } else {
            poolSizeText.setVisibility(View.GONE);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (progressReceiver != null) {
            LocalBroadcastManager.getInstance(this).unregisterReceiver(progressReceiver);
        }
        if (vpnStatusReceiver != null) {
            LocalBroadcastManager.getInstance(this).unregisterReceiver(vpnStatusReceiver);
        }
        if (configPoolReceiver != null) {
            LocalBroadcastManager.getInstance(this).unregisterReceiver(configPoolReceiver);
        }
    }
}
 
