package com.hunter.app;

import android.content.Intent;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Activity that lets users select specific V2Ray configs from the found/cached list.
 * Selected configs are passed back to MainActivity which starts VPN with them.
 */
public class ConfigSelectActivity extends AppCompatActivity {

    public static final String EXTRA_SELECTED_URIS = "selected_uris";

    private RecyclerView recyclerView;
    private ConfigAdapter adapter;
    private TextView selectedCountText;
    private View btnConnect;
    private List<SelectableConfig> configs = new ArrayList<>();

    static class SelectableConfig {
        String uri;
        String name;
        String protocol;
        int latency;
        int telegramLatency;
        boolean selected;

        SelectableConfig(ConfigManager.ConfigItem item) {
            this.uri = item.uri;
            this.name = item.name != null ? item.name : "Unknown";
            this.protocol = item.protocol != null ? item.protocol : "?";
            this.latency = item.latency;
            this.telegramLatency = item.telegram_latency;
            this.selected = false;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_config_select);

        recyclerView = findViewById(R.id.config_list);
        selectedCountText = findViewById(R.id.selected_count);
        btnConnect = findViewById(R.id.btn_connect);
        View btnBack = findViewById(R.id.btn_back);
        View btnSelectAll = findViewById(R.id.btn_select_all);

        recyclerView.setLayoutManager(new LinearLayoutManager(this));

        // Load configs from ConfigManager
        loadConfigs();

        adapter = new ConfigAdapter();
        recyclerView.setAdapter(adapter);

        btnBack.setOnClickListener(v -> finish());

        btnSelectAll.setOnClickListener(v -> {
            boolean allSelected = true;
            for (SelectableConfig c : configs) {
                if (!c.selected) { allSelected = false; break; }
            }
            // Toggle: if all selected, deselect all; otherwise select all
            for (SelectableConfig c : configs) {
                c.selected = !allSelected;
            }
            adapter.notifyDataSetChanged();
            updateSelectedCount();
        });

        btnConnect.setOnClickListener(v -> {
            ArrayList<String> selectedUris = new ArrayList<>();
            for (SelectableConfig c : configs) {
                if (c.selected) selectedUris.add(c.uri);
            }
            if (selectedUris.isEmpty()) return;

            Intent result = new Intent();
            result.putStringArrayListExtra(EXTRA_SELECTED_URIS, selectedUris);
            setResult(RESULT_OK, result);
            finish();
        });

        updateSelectedCount();
    }

    private void loadConfigs() {
        configs.clear();
        FreeV2RayApplication app = FreeV2RayApplication.getInstance();
        if (app == null) return;
        ConfigManager cm = app.getConfigManager();
        if (cm == null) return;

        // Load top cached configs (these have been tested and have latency data)
        List<ConfigManager.ConfigItem> topConfigs = cm.getTopConfigs();
        Set<String> seen = new HashSet<>();
        for (ConfigManager.ConfigItem item : topConfigs) {
            if (item.uri == null || seen.contains(item.uri)) continue;
            if (item.latency <= 0) continue; // Skip untested configs
            seen.add(item.uri);
            configs.add(new SelectableConfig(item));
        }

        // Sort: best latency first, Telegram-capable first
        configs.sort((a, b) -> {
            // Telegram-capable configs first
            boolean aTg = a.telegramLatency > 0 && a.telegramLatency < Integer.MAX_VALUE;
            boolean bTg = b.telegramLatency > 0 && b.telegramLatency < Integer.MAX_VALUE;
            if (aTg != bTg) return aTg ? -1 : 1;
            // Then by latency
            return Integer.compare(a.latency, b.latency);
        });

        TextView infoText = findViewById(R.id.info_text);
        if (configs.isEmpty()) {
            infoText.setText(R.string.no_configs_to_select);
        }
    }

    private void updateSelectedCount() {
        int count = 0;
        for (SelectableConfig c : configs) {
            if (c.selected) count++;
        }
        selectedCountText.setText(count + " selected");
        btnConnect.setEnabled(count > 0);
    }

    // ── RecyclerView Adapter ──────────────────────────────────────────

    class ConfigAdapter extends RecyclerView.Adapter<ConfigAdapter.VH> {

        class VH extends RecyclerView.ViewHolder {
            CheckBox checkbox;
            TextView name, protocol, latency, telegram;

            VH(View v) {
                super(v);
                checkbox = v.findViewById(R.id.config_checkbox);
                name = v.findViewById(R.id.config_name);
                protocol = v.findViewById(R.id.config_protocol);
                latency = v.findViewById(R.id.config_latency);
                telegram = v.findViewById(R.id.config_telegram);
            }
        }

        @NonNull
        @Override
        public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View v = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_config_select, parent, false);
            return new VH(v);
        }

        @Override
        public void onBindViewHolder(@NonNull VH h, int position) {
            SelectableConfig c = configs.get(position);

            h.name.setText(c.name);
            h.protocol.setText(c.protocol.toUpperCase());
            h.latency.setText(c.latency > 0 ? c.latency + "ms" : "--");

            if (c.telegramLatency > 0 && c.telegramLatency < Integer.MAX_VALUE) {
                h.telegram.setVisibility(View.VISIBLE);
                h.telegram.setText("TG " + c.telegramLatency + "ms");
            } else {
                h.telegram.setVisibility(View.GONE);
            }

            h.checkbox.setOnCheckedChangeListener(null);
            h.checkbox.setChecked(c.selected);
            h.checkbox.setOnCheckedChangeListener((btn, checked) -> {
                c.selected = checked;
                updateSelectedCount();
            });

            h.itemView.setOnClickListener(v -> {
                c.selected = !c.selected;
                h.checkbox.setChecked(c.selected);
            });
        }

        @Override
        public int getItemCount() {
            return configs.size();
        }
    }
}
