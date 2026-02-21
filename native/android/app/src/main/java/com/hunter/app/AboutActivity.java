package com.hunter.app;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.ImageButton;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.button.MaterialButton;

/**
 * About page — community, contact, and donation info.
 */
public class AboutActivity extends AppCompatActivity {

    private static final String TELEGRAM_GROUP = "https://t.me/free_v2ray_iranian";
    private static final String ADDR_BTC  = "bc1qe9l9rxs54vglu6velwdssygngy99njr8s95wss";
    private static final String ADDR_USDT = "0x37e14feC86acAA16F1b92d3f0Ee08d11c851e963";
    private static final String ADDR_SOL  = "Ek73rGNUz4fvy7QAJpsUVfHxeJJV5HTWVcG1N1fPf7wA";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_about);

        // Back button
        findViewById(R.id.btn_back).setOnClickListener(v -> finish());

        // Version
        TextView appVersion = findViewById(R.id.app_version);
        try {
            String ver = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
            appVersion.setText("Version " + ver);
        } catch (Exception e) {
            appVersion.setText("Version 1.0.0");
        }

        // Telegram group button
        MaterialButton btnTelegram = findViewById(R.id.btn_telegram);
        btnTelegram.setOnClickListener(v -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(TELEGRAM_GROUP)));
            } catch (Exception e) {
                copyToClipboard("Telegram", TELEGRAM_GROUP);
            }
        });

        // Tap-to-copy for each crypto address row
        findViewById(R.id.row_btc).setOnClickListener(v -> copyToClipboard("Bitcoin", ADDR_BTC));
        findViewById(R.id.row_usdt).setOnClickListener(v -> copyToClipboard("USDT", ADDR_USDT));
        findViewById(R.id.row_sol).setOnClickListener(v -> copyToClipboard("Solana", ADDR_SOL));
    }

    private void copyToClipboard(String label, String text) {
        ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (clipboard != null) {
            clipboard.setPrimaryClip(ClipData.newPlainText(label, text));
        }
        Toast.makeText(this, label + " address copied!", Toast.LENGTH_SHORT).show();
    }
}
