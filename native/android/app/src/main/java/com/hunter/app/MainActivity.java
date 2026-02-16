package com.hunter.app;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
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

import com.google.android.material.button.MaterialButton;

import java.util.Collections;
import java.util.List;

/**
 * Main activity for Iranian Free V2Ray.
 * Pure Java implementation - no native dependencies.
 */
public class MainActivity extends AppCompatActivity {

    private MaterialButton connectButton;
    private MaterialButton refreshButton, shareButton;
    private ProgressBar progressBar;
    private TextView statusText, statusDetail, configCount;
    private ImageView statusIcon;
    private ImageButton settingsButton;

    private ConfigManager configManager;
    private boolean isConnected = false;
    private boolean isConnecting = false;

    private final ActivityResultLauncher<Intent> vpnPermissionLauncher =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK) {
                    startVpnService();
                } else {
                    setDisconnectedState();
                    statusDetail.setText("VPN permission required");
                }
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main_v2);

        initViews();
        setupListeners();

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

    private void startMonitorService() {
        try {
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
        statusText = findViewById(R.id.status_text);
        statusDetail = findViewById(R.id.status_detail);
        configCount = findViewById(R.id.config_count);
        statusIcon = findViewById(R.id.status_icon);
        settingsButton = findViewById(R.id.btn_settings);
        refreshButton = findViewById(R.id.btn_refresh);
        shareButton = findViewById(R.id.btn_share);
    }

    private void setupListeners() {
        connectButton.setOnClickListener(v -> toggleVpn());
        
        settingsButton.setOnClickListener(v -> 
            startActivity(new Intent(this, SettingsActivity.class)));
        
        refreshButton.setOnClickListener(v -> refreshConfigs());
        
        shareButton.setOnClickListener(v -> 
            startActivity(new Intent(this, ConfigShareActivity.class)));
    }

    private void refreshConfigs() {
        refreshButton.setEnabled(false);
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

        // Check XRay engine readiness (binary is bundled in APK, just needs extraction on first run)
        XRayManager xrayManager = FreeV2RayApplication.getInstance().getXRayManager();
        if (xrayManager == null || !xrayManager.isReady()) {
            setConnectingState();
            statusDetail.setText("در حال آماده‌سازی موتور V2Ray...");
            if (xrayManager != null) {
                xrayManager.ensureInstalled(new XRayManager.InstallCallback() {
                    @Override
                    public void onProgress(String message, int percent) {
                        runOnUiThread(() -> statusDetail.setText(message));
                    }
                    @Override
                    public void onComplete(String binaryPath) {
                        runOnUiThread(() -> {
                            statusDetail.setText("موتور آماده، در حال اتصال...");
                            proceedWithVpnConnect();
                        });
                    }
                    @Override
                    public void onError(String error) {
                        runOnUiThread(() -> {
                            setDisconnectedState();
                            statusDetail.setText("خطا در نصب موتور: " + error);
                            Toast.makeText(MainActivity.this,
                                "خطا در نصب موتور V2Ray", Toast.LENGTH_LONG).show();
                        });
                    }
                });
            } else {
                setDisconnectedState();
                statusDetail.setText("موتور V2Ray در دسترس نیست");
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
        Intent intent = new Intent(this, com.hunter.app.VpnService.class);
        intent.setAction(com.hunter.app.VpnService.ACTION_START);
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }

        // Check connection state after delay (xray needs time to start)
        connectButton.postDelayed(() -> {
            if (com.hunter.app.VpnService.isActive()) {
                setConnectedState();
            } else {
                setDisconnectedState();
                statusDetail.setText("Connection failed - try again");
            }
        }, 5000);
    }

    private void disconnectVpn() {
        Intent intent = new Intent(this, com.hunter.app.VpnService.class);
        intent.setAction(com.hunter.app.VpnService.ACTION_STOP);
        startService(intent);
        setDisconnectedState();
    }

    private void setConnectingState() {
        isConnecting = true;
        isConnected = false;
        connectButton.setVisibility(View.INVISIBLE);
        progressBar.setVisibility(View.VISIBLE);
        statusText.setText("Connecting...");
        statusText.setTextColor(getColor(R.color.status_connecting));
        statusDetail.setText("Establishing connection...");
        statusIcon.setColorFilter(getColor(R.color.status_connecting));
    }

    private void setConnectedState() {
        isConnecting = false;
        isConnected = true;
        connectButton.setVisibility(View.VISIBLE);
        progressBar.setVisibility(View.GONE);
        connectButton.setText("Disconnect");
        connectButton.setBackgroundTintList(getColorStateList(R.color.accent_red));
        statusText.setText("Connected");
        statusText.setTextColor(getColor(R.color.status_connected));
        statusDetail.setText("VPN is active");
        statusIcon.setImageResource(R.drawable.ic_vpn_on);
        statusIcon.clearColorFilter();
    }

    private void setDisconnectedState() {
        isConnecting = false;
        isConnected = false;
        connectButton.setVisibility(View.VISIBLE);
        progressBar.setVisibility(View.GONE);
        connectButton.setText("Connect");
        connectButton.setBackgroundTintList(getColorStateList(R.color.accent_green));
        statusText.setText("Disconnected");
        statusText.setTextColor(getColor(R.color.text_primary));
        statusDetail.setText("Tap to connect");
        statusIcon.setImageResource(R.drawable.ic_vpn_off);
        statusIcon.setColorFilter(getColor(R.color.accent_blue));
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
        if (com.hunter.app.VpnService.isActive()) {
            setConnectedState();
        } else {
            setDisconnectedState();
        }
        updateConfigStats();
    }
}
 
