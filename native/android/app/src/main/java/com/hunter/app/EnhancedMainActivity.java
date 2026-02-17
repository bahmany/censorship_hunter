package com.hunter.app;

import android.Manifest;
import android.app.Activity;
import android.app.NotificationManager;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.widget.ImageButton;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import com.google.android.material.button.MaterialButton;
import com.hunter.app.controller.V2rayController;
import com.hunter.app.utils.V2rayConstants;

import java.util.ArrayList;

/**
 * Enhanced Main Activity using V2rayController
 * Based on v2rayNG architecture for better reliability
 */
public class EnhancedMainActivity extends AppCompatActivity {

    private MaterialButton connectButton;
    private MaterialButton refreshButton, shareButton, switchButton;
    private ProgressBar progressBar;
    private ProgressBar v2rayProgressBar;
    private TextView statusText, statusDetail, configCount;
    private TextView progressLabel, progressDetail;
    private ImageButton settingsButton, aboutButton;

    private static final int REQUEST_CODE_PERMISSIONS = 100;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main_v2);

        initViews();
        setupClickListeners();
        
        // Initialize V2rayController
        V2rayController.init(this, R.mipmap.ic_launcher, "Hunter VPN");
        
        // Request permissions if needed
        requestPermissionsIfNeeded();
        
        // Update UI based on current state
        updateUI();
    }

    private void initViews() {
        connectButton = findViewById(R.id.connect_button);
        refreshButton = findViewById(R.id.btn_refresh);
        shareButton = findViewById(R.id.btn_share);
        switchButton = findViewById(R.id.btn_switch);
        progressBar = findViewById(R.id.progress_bar);
        v2rayProgressBar = findViewById(R.id.v2ray_progress_bar);
        statusText = findViewById(R.id.status_text);
        statusDetail = findViewById(R.id.status_detail);
        configCount = findViewById(R.id.config_count);
        progressLabel = findViewById(R.id.progress_label);
        progressDetail = findViewById(R.id.progress_detail);
        settingsButton = findViewById(R.id.btn_settings);
        aboutButton = findViewById(R.id.btn_about);
    }

    private void setupClickListeners() {
        connectButton.setOnClickListener(v -> toggleConnection());
        refreshButton.setOnClickListener(v -> refreshConfigs());
        shareButton.setOnClickListener(v -> shareConfigs());
        switchButton.setOnClickListener(v -> switchMode());
        settingsButton.setOnClickListener(v -> openSettings());
        aboutButton.setOnClickListener(v -> openAbout());
    }

    private void toggleConnection() {
        V2rayConstants.CONNECTION_STATES currentState = V2rayController.getConnectionState();
        
        if (currentState == V2rayConstants.CONNECTION_STATES.CONNECTED) {
            // Disconnect
            V2rayController.stopV2ray(this);
        } else {
            // Connect - use a test config for now
            String testConfig = "{\n" +
                    "  \"inbounds\": [\n" +
                    "    {\n" +
                    "      \"listen\": \"127.0.0.1\",\n" +
                    "      \"port\": 10808,\n" +
                    "      \"protocol\": \"socks\",\n" +
                    "      \"settings\": {\n" +
                    "        \"auth\": \"noauth\",\n" +
                    "        \"udp\": true\n" +
                    "      }\n" +
                    "    }\n" +
                    "  ],\n" +
                    "  \"outbounds\": [\n" +
                    "    {\n" +
                    "      \"protocol\": \"vmess\",\n" +
                    "      \"settings\": {\n" +
                    "        \"vnext\": [\n" +
                    "          {\n" +
                    "            \"address\": \"your-server.com\",\n" +
                    "            \"port\": 443,\n" +
                    "            \"users\": [\n" +
                    "              {\n" +
                    "                \"id\": \"your-uuid\",\n" +
                    "                \"level\": 0,\n" +
                    "                \"alterId\": 0\n" +
                    "              }\n" +
                    "            ]\n" +
                    "          }\n" +
                    "        ]\n" +
                    "      },\n" +
                    "      \"streamSettings\": {\n" +
                    "        \"network\": \"ws\"\n" +
                    "      }\n" +
                    "    }\n" +
                    "  ]\n" +
                    "}";
            
            V2rayController.startV2ray(this, "Test Server", testConfig, new ArrayList<>());
        }
    }

    private void refreshConfigs() {
        // TODO: Implement config refresh logic
        Toast.makeText(this, "Refreshing configs...", Toast.LENGTH_SHORT).show();
    }

    private void shareConfigs() {
        // TODO: Implement config sharing logic
        Toast.makeText(this, "Share configs", Toast.LENGTH_SHORT).show();
    }

    private void switchMode() {
        // TODO: Implement mode switching logic
        Toast.makeText(this, "Switch mode", Toast.LENGTH_SHORT).show();
    }

    private void openSettings() {
        Intent intent = new Intent(this, SettingsActivity.class);
        startActivity(intent);
    }

    private void openAbout() {
        // TODO: Implement about activity
        Toast.makeText(this, "About", Toast.LENGTH_SHORT).show();
    }

    private void requestPermissionsIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQUEST_CODE_PERMISSIONS);
            }
        }
    }

    private void updateUI() {
        V2rayConstants.CONNECTION_STATES state = V2rayController.getConnectionState();
        
        switch (state) {
            case DISCONNECTED:
                statusText.setText("Disconnected");
                connectButton.setText("Connect");
                progressBar.setVisibility(View.GONE);
                break;
            case CONNECTING:
                statusText.setText("Connecting...");
                connectButton.setText("Connecting");
                progressBar.setVisibility(View.VISIBLE);
                break;
            case CONNECTED:
                statusText.setText("Connected");
                connectButton.setText("Disconnect");
                progressBar.setVisibility(View.GONE);
                break;
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateUI();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // Clean up if needed
    }
}
