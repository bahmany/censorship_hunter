package com.hunter.app;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.MenuItem;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.switchmaterial.SwitchMaterial;

/**
 * Settings activity for VPN configuration.
 * Supports per-app VPN (split tunneling) and other options.
 */
public class SettingsActivity extends AppCompatActivity {

    private SharedPreferences prefs;
    private SwitchMaterial perAppSwitch, whitelistSwitch, autoStartSwitch, autoConnectSwitch;
    private MaterialCardView appSelectCard;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);

        prefs = getSharedPreferences("vpn_settings", MODE_PRIVATE);

        MaterialToolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);
        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
            getSupportActionBar().setTitle("تنظیمات");
        }

        initViews();
        loadSettings();
        setupListeners();
    }

    private void initViews() {
        perAppSwitch = findViewById(R.id.per_app_switch);
        whitelistSwitch = findViewById(R.id.whitelist_switch);
        autoStartSwitch = findViewById(R.id.auto_start_switch);
        autoConnectSwitch = findViewById(R.id.auto_connect_switch);
        appSelectCard = findViewById(R.id.app_select_card);
    }

    private void loadSettings() {
        perAppSwitch.setChecked(prefs.getBoolean("per_app_enabled", false));
        whitelistSwitch.setChecked(prefs.getBoolean("per_app_whitelist", true));
        autoStartSwitch.setChecked(prefs.getBoolean("auto_start", false));
        autoConnectSwitch.setChecked(prefs.getBoolean("auto_connect", false));

        updateAppSelectVisibility();
    }

    private void setupListeners() {
        perAppSwitch.setOnCheckedChangeListener((buttonView, isChecked) -> {
            prefs.edit().putBoolean("per_app_enabled", isChecked).apply();
            updateAppSelectVisibility();
        });

        whitelistSwitch.setOnCheckedChangeListener((buttonView, isChecked) -> {
            prefs.edit().putBoolean("per_app_whitelist", isChecked).apply();
            String mode = isChecked ? "فقط برنامه‌های انتخاب شده از VPN استفاده می‌کنند" 
                                   : "همه برنامه‌ها به جز انتخاب شده‌ها از VPN استفاده می‌کنند";
            Toast.makeText(this, mode, Toast.LENGTH_SHORT).show();
        });

        autoStartSwitch.setOnCheckedChangeListener((buttonView, isChecked) -> 
            prefs.edit().putBoolean("auto_start", isChecked).apply());

        autoConnectSwitch.setOnCheckedChangeListener((buttonView, isChecked) -> 
            prefs.edit().putBoolean("auto_connect", isChecked).apply());

        appSelectCard.setOnClickListener(v -> {
            if (!perAppSwitch.isChecked()) {
                Toast.makeText(this, "ابتدا حالت انتخاب برنامه را فعال کنید", Toast.LENGTH_SHORT).show();
                return;
            }
            startActivity(new Intent(this, AppSelectActivity.class));
        });
    }

    private void updateAppSelectVisibility() {
        appSelectCard.setAlpha(perAppSwitch.isChecked() ? 1.0f : 0.5f);
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }
}
