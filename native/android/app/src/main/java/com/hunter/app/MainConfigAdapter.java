package com.hunter.app;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.hunter.app.ConfigManager;

import java.util.List;

public class MainConfigAdapter extends RecyclerView.Adapter<MainConfigAdapter.ViewHolder> {

    private List<ConfigManager.ConfigItem> configs;

    public MainConfigAdapter(List<ConfigManager.ConfigItem> configs) {
        this.configs = configs;
    }

    @NonNull
    @Override
    public ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_main_config, parent, false);
        return new ViewHolder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull ViewHolder holder, int position) {
        ConfigManager.ConfigItem item = configs.get(position);
        holder.nameText.setText(item.name != null ? item.name : "Unknown");
        holder.protocolText.setText(item.protocol != null ? item.protocol : "Unknown");
        holder.latencyText.setText(item.latency >= 0 ? item.latency + "ms" : "--");
    }

    @Override
    public int getItemCount() {
        return configs.size();
    }

    public void updateConfigs(List<ConfigManager.ConfigItem> newConfigs) {
        this.configs = newConfigs;
        notifyDataSetChanged();
    }

    static class ViewHolder extends RecyclerView.ViewHolder {
        TextView nameText;
        TextView protocolText;
        TextView latencyText;

        ViewHolder(View itemView) {
            super(itemView);
            nameText = itemView.findViewById(R.id.config_name);
            protocolText = itemView.findViewById(R.id.config_protocol);
            latencyText = itemView.findViewById(R.id.config_latency);
        }
    }
}
