package com.hunter.app;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.widget.Toast;
import androidx.recyclerview.widget.RecyclerView;
import java.util.List;

public class ConfigAdapter extends RecyclerView.Adapter<ConfigAdapter.ViewHolder> {

    private List<ConfigItem> configs;

    public ConfigAdapter(List<ConfigItem> configs) {
        this.configs = configs;
    }

    @Override
    public ViewHolder onCreateViewHolder(ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_config, parent, false);
        return new ViewHolder(view);
    }

    @Override
    public void onBindViewHolder(ViewHolder holder, int position) {
        ConfigItem item = configs.get(position);
        holder.ps.setText(item.getPs());
        
        String protocol = item.getProtocol();
        holder.protocol.setText(protocol != null ? protocol : "Unknown");
        
        int latency = item.getLatency();
        if (latency > 0) {
            holder.latency.setText(latency + " ms");
        } else {
            holder.latency.setText("--");
        }

        holder.itemView.setOnClickListener(v -> {
            String uri = item.getUri();
            if (uri == null || uri.isEmpty()) return;
            ClipboardManager clipboard = (ClipboardManager) v.getContext().getSystemService(Context.CLIPBOARD_SERVICE);
            if (clipboard != null) {
                clipboard.setPrimaryClip(ClipData.newPlainText("v2ray_config", uri));
                Toast.makeText(v.getContext(), "Config copied to clipboard", Toast.LENGTH_SHORT).show();
            }
        });
    }

    @Override
    public int getItemCount() {
        return configs.size();
    }

    public static class ViewHolder extends RecyclerView.ViewHolder {
        TextView ps, latency, protocol;

        public ViewHolder(View itemView) {
            super(itemView);
            ps = itemView.findViewById(R.id.config_ps);
            latency = itemView.findViewById(R.id.config_latency);
            protocol = itemView.findViewById(R.id.config_protocol);
        }
    }
}
