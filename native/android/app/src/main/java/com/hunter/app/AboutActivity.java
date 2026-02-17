package com.hunter.app;

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
 * About page for Iranian Free V2Ray
 * Displays app information, developers, and donation information
 */
public class AboutActivity extends AppCompatActivity {

    private ImageButton backButton;
    private TextView appVersion, appDescription;
    private MaterialButton donateButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_about);

        initViews();
        setupListeners();
        loadAppInfo();
    }

    private void initViews() {
        backButton = findViewById(R.id.btn_back);
        appVersion = findViewById(R.id.app_version);
        appDescription = findViewById(R.id.app_description);
        donateButton = findViewById(R.id.btn_donate);
    }

    private void setupListeners() {
        backButton.setOnClickListener(v -> finish());

        donateButton.setOnClickListener(v -> {
            // Copy address or open crypto wallet
            String cryptoAddress = "bc1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"; // Replace with actual address
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setData(Uri.parse("bitcoin:" + cryptoAddress));
            try {
                startActivity(intent);
            } catch (Exception e) {
                // Alternative - copy address
                android.content.ClipboardManager clipboard =
                    (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                android.content.ClipData clip = android.content.ClipData.newPlainText("Bitcoin Address", cryptoAddress);
                clipboard.setPrimaryClip(clip);
                android.widget.Toast.makeText(this, "Bitcoin address copied", android.widget.Toast.LENGTH_SHORT).show();
            }
        });
    }

    private void loadAppInfo() {
        try {
            String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
            appVersion.setText("Version " + version);
        } catch (Exception e) {
            appVersion.setText("Version 1.0.0");
        }

        appDescription.setText("Iranian Free V2Ray is a completely free VPN application designed to help Iranian users bypass internet restrictions and access information freely.\n\n" +
                              "This application is developed by independent developers and distributed completely free. " +
                              "We believe that unlimited access to information should be available to everyone.\n\n" +
                              "If you find this application useful and would like to support its development and maintenance, " +
                              "you can make voluntary donations through cryptocurrency.");
    }
}
