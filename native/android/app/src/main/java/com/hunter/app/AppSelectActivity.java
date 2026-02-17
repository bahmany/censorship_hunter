package com.hunter.app;

import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.view.MenuItem;
import android.view.View;
import android.widget.CheckBox;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.appbar.MaterialToolbar;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Activity to select apps for per-app VPN (split tunneling).
 */
public class AppSelectActivity extends AppCompatActivity {

    private RecyclerView recyclerView;
    private ProgressBar progressBar;
    private SharedPreferences prefs;
    private Set<String> selectedApps;
    private AppListAdapter adapter;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_app_select);

        prefs = getSharedPreferences("vpn_settings", MODE_PRIVATE);
        selectedApps = new HashSet<>(prefs.getStringSet("selected_apps", new HashSet<>()));

        MaterialToolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);
        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
            getSupportActionBar().setTitle("Select Apps");
        }

        recyclerView = findViewById(R.id.apps_recycler);
        progressBar = findViewById(R.id.progress_bar);
        recyclerView.setLayoutManager(new LinearLayoutManager(this));

        loadApps();
    }

    private void loadApps() {
        progressBar.setVisibility(View.VISIBLE);
        executor.execute(() -> {
            List<AppInfo> apps = new ArrayList<>();
            PackageManager pm = getPackageManager();
            List<ApplicationInfo> packages = pm.getInstalledApplications(PackageManager.GET_META_DATA);

            for (ApplicationInfo appInfo : packages) {
                // Skip system apps without launcher
                if ((appInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0 && 
                    pm.getLaunchIntentForPackage(appInfo.packageName) == null) {
                    continue;
                }
                // Skip self
                if (appInfo.packageName.equals(getPackageName())) {
                    continue;
                }

                String name = pm.getApplicationLabel(appInfo).toString();
                apps.add(new AppInfo(appInfo.packageName, name, appInfo));
            }

            // Sort by name
            apps.sort((a, b) -> a.name.compareToIgnoreCase(b.name));

            runOnUiThread(() -> {
                progressBar.setVisibility(View.GONE);
                adapter = new AppListAdapter(apps);
                recyclerView.setAdapter(adapter);
            });
        });
    }

    private void saveSelectedApps() {
        prefs.edit().putStringSet("selected_apps", selectedApps).apply();
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            saveSelectedApps();
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    @Override
    public void onBackPressed() {
        saveSelectedApps();
        super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        executor.shutdownNow();
        super.onDestroy();
    }

    static class AppInfo {
        String packageName;
        String name;
        ApplicationInfo appInfo;

        AppInfo(String packageName, String name, ApplicationInfo appInfo) {
            this.packageName = packageName;
            this.name = name;
            this.appInfo = appInfo;
        }
    }

    class AppListAdapter extends RecyclerView.Adapter<AppListAdapter.ViewHolder> {
        private final List<AppInfo> apps;

        AppListAdapter(List<AppInfo> apps) {
            this.apps = apps;
        }

        @Override
        public ViewHolder onCreateViewHolder(android.view.ViewGroup parent, int viewType) {
            View view = getLayoutInflater().inflate(R.layout.item_app, parent, false);
            return new ViewHolder(view);
        }

        @Override
        public void onBindViewHolder(ViewHolder holder, int position) {
            AppInfo app = apps.get(position);
            holder.nameText.setText(app.name);
            holder.packageText.setText(app.packageName);
            holder.icon.setImageDrawable(getPackageManager().getApplicationIcon(app.appInfo));
            holder.checkBox.setChecked(selectedApps.contains(app.packageName));

            holder.itemView.setOnClickListener(v -> {
                boolean isChecked = !holder.checkBox.isChecked();
                holder.checkBox.setChecked(isChecked);
                if (isChecked) {
                    selectedApps.add(app.packageName);
                } else {
                    selectedApps.remove(app.packageName);
                }
            });

            holder.checkBox.setOnCheckedChangeListener((buttonView, isChecked) -> {
                if (isChecked) {
                    selectedApps.add(app.packageName);
                } else {
                    selectedApps.remove(app.packageName);
                }
            });
        }

        @Override
        public int getItemCount() {
            return apps.size();
        }

        class ViewHolder extends RecyclerView.ViewHolder {
            ImageView icon;
            TextView nameText, packageText;
            CheckBox checkBox;

            ViewHolder(View itemView) {
                super(itemView);
                icon = itemView.findViewById(R.id.app_icon);
                nameText = itemView.findViewById(R.id.app_name);
                packageText = itemView.findViewById(R.id.app_package);
                checkBox = itemView.findViewById(R.id.app_checkbox);
            }
        }
    }
}
