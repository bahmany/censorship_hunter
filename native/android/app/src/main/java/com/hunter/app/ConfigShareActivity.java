package com.hunter.app;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.util.Base64;
import android.view.MenuItem;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.button.MaterialButton;

import java.util.ArrayList;
import java.util.List;

/**
 * Share working V2Ray configs via text, subscription link, or file.
 * Uses tiered cache: top 100 tested configs for quick sharing.
 */
public class ConfigShareActivity extends AppCompatActivity {

    private RecyclerView recyclerView;
    private TextView emptyText;
    private MaterialButton shareAllButton;
    private MaterialButton copySubButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_config_share);

        MaterialToolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);
        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
            getSupportActionBar().setTitle("Share Configs");
        }

        recyclerView = findViewById(R.id.recycler_view);
        emptyText = findViewById(R.id.empty_text);
        shareAllButton = findViewById(R.id.btn_share_all);
        copySubButton = findViewById(R.id.btn_copy_subscription);

        recyclerView.setLayoutManager(new LinearLayoutManager(this));

        loadWorkingConfigs();
        setupListeners();
    }

    private void loadWorkingConfigs() {
        ConfigManager configManager = FreeV2RayApplication.getInstance().getConfigManager();

        // Prefer top tested configs, fallback to all configs
        List<ConfigManager.ConfigItem> topConfigs = configManager.getTopConfigs();
        List<ConfigManager.ConfigItem> displayConfigs;

        if (!topConfigs.isEmpty()) {
            displayConfigs = topConfigs;
        } else {
            // Fallback: get working configs from all
            displayConfigs = new ArrayList<>();
            for (ConfigManager.ConfigItem c : configManager.getConfigs()) {
                if (c.latency > 0 && c.latency < 10000) {
                    displayConfigs.add(c);
                    if (displayConfigs.size() >= 100) break;
                }
            }
        }

        if (displayConfigs.isEmpty()) {
            emptyText.setVisibility(android.view.View.VISIBLE);
            recyclerView.setVisibility(android.view.View.GONE);
            shareAllButton.setEnabled(false);
            copySubButton.setEnabled(false);
        } else {
            emptyText.setVisibility(android.view.View.GONE);
            recyclerView.setVisibility(android.view.View.VISIBLE);
            shareAllButton.setEnabled(true);
            copySubButton.setEnabled(true);

            List<ConfigItem> items = new ArrayList<>();
            for (ConfigManager.ConfigItem item : displayConfigs) {
                items.add(new ConfigItem(item.name, item.latency, item.uri, item.protocol));
            }
            ConfigAdapter adapter = new ConfigAdapter(items);
            recyclerView.setAdapter(adapter);
        }
    }

    private void setupListeners() {
        shareAllButton.setOnClickListener(v -> shareWorkingConfigs());
        copySubButton.setOnClickListener(v -> copySubscriptionLink());
    }

    /**
     * Build a shareable text from top/working configs (max 100).
     */
    private String buildShareText(int maxConfigs) {
        ConfigManager configManager = FreeV2RayApplication.getInstance().getConfigManager();
        List<ConfigManager.ConfigItem> topConfigs = configManager.getTopConfigs();

        StringBuilder sb = new StringBuilder();
        int count = 0;

        // Use top configs first
        for (ConfigManager.ConfigItem c : topConfigs) {
            sb.append(c.uri).append('\n');
            count++;
            if (count >= maxConfigs) break;
        }

        // If not enough, add from all configs
        if (count < maxConfigs) {
            for (ConfigManager.ConfigItem c : configManager.getConfigs()) {
                if (c.latency > 0 && c.latency < 10000) {
                    sb.append(c.uri).append('\n');
                    count++;
                    if (count >= maxConfigs) break;
                }
            }
        }

        return count > 0 ? sb.toString() : null;
    }

    private void shareWorkingConfigs() {
        String text = buildShareText(100);
        if (text == null) {
            Toast.makeText(this, "No working configs to share", Toast.LENGTH_SHORT).show();
            return;
        }

        int count = text.split("\n").length;
        Intent shareIntent = new Intent(Intent.ACTION_SEND);
        shareIntent.setType("text/plain");
        shareIntent.putExtra(Intent.EXTRA_SUBJECT, "Iranian Free V2Ray Configs");
        shareIntent.putExtra(Intent.EXTRA_TEXT, text);
        startActivity(Intent.createChooser(shareIntent, "Share " + count + " configs"));
    }

    private void copySubscriptionLink() {
        String text = buildShareText(100);
        if (text == null) {
            Toast.makeText(this, "No working configs available", Toast.LENGTH_SHORT).show();
            return;
        }

        String base64 = Base64.encodeToString(text.getBytes(), Base64.NO_WRAP);

        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard != null) {
            clipboard.setPrimaryClip(ClipData.newPlainText("V2Ray Sub", base64));
            int count = text.split("\n").length;
            Toast.makeText(this,
                "Subscription copied (" + count + " configs)",
                Toast.LENGTH_LONG).show();
        }
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
