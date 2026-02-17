package com.hunter.app.controller;

import android.Manifest;
import android.app.Activity;
import androidx.activity.result.ActivityResultLauncher;
import android.app.RecoverableSecurityException;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.VpnService;
import android.os.Build;
import android.util.Log;
import android.widget.Toast;

import androidx.activity.result.ActivityResult;
import androidx.activity.result.ActivityResultCallback;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.core.content.PermissionChecker;

import com.hunter.app.R;
import com.hunter.app.model.V2rayConfigModel;
import com.hunter.app.service.EnhancedVpnService;
import com.hunter.app.utils.V2rayConstants;
import com.hunter.app.utils.V2rayConfigs;

import java.util.ArrayList;
import java.util.Objects;

/**
 * Central V2Ray controller with simple API
 * Based on v2rayNG V2rayController for easy integration
 */
public class V2rayController {
    private static final String TAG = "V2rayController";
    
    private static ActivityResultLauncher<Intent> activityResultLauncher;
    
    static final BroadcastReceiver stateUpdaterBroadcastReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            try {
                V2rayConfigs.connectionState = (V2rayConstants.CONNECTION_STATES) 
                    Objects.requireNonNull(intent.getExtras()).getSerializable(V2rayConstants.SERVICE_CONNECTION_STATE_BROADCAST_EXTRA);
                if (Objects.equals(intent.getExtras().getString(V2rayConstants.SERVICE_TYPE_BROADCAST_EXTRA), EnhancedVpnService.class.getSimpleName())) {
                    V2rayConfigs.serviceMode = V2rayConstants.SERVICE_MODES.VPN_MODE;
                } else {
                    V2rayConfigs.serviceMode = V2rayConstants.SERVICE_MODES.PROXY_MODE;
                }
            } catch (Exception ignore) {}
        }
    };

    public static void init(final AppCompatActivity activity, final int app_icon, final String app_name) {
        V2rayConfigs.currentConfig.applicationIcon = app_icon;
        V2rayConfigs.currentConfig.applicationName = app_name;
        registerReceivers(activity);
        
        activityResultLauncher = activity.registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(),
            new ActivityResultCallback<ActivityResult>() {
                @Override
                public void onActivityResult(ActivityResult result) {
                    if (result.getResultCode() == Activity.RESULT_OK) {
                        startTunnel(activity);
                    } else {
                        Toast.makeText(activity, "Permission not granted.", Toast.LENGTH_LONG).show();
                    }
                }
            }
        );
    }

    public static void registerReceivers(final Activity activity) {
        try {
            activity.unregisterReceiver(stateUpdaterBroadcastReceiver);
        } catch (Exception ignore) {}
        
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            activity.registerReceiver(stateUpdaterBroadcastReceiver, 
                new IntentFilter(V2rayConstants.V2RAY_SERVICE_STATICS_BROADCAST_INTENT), 
                Context.RECEIVER_EXPORTED);
        } else {
            activity.registerReceiver(stateUpdaterBroadcastReceiver, 
                new IntentFilter(V2rayConstants.V2RAY_SERVICE_STATICS_BROADCAST_INTENT));
        }
    }

    public static V2rayConstants.CONNECTION_STATES getConnectionState() {
        return V2rayConfigs.connectionState;
    }

    public static boolean isPreparedForConnection(final Context context) {
        if (Build.VERSION.SDK_INT >= 33) {
            if (ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PermissionChecker.PERMISSION_GRANTED) {
                return false;
            }
        }
        Intent vpnServicePrepareIntent = VpnService.prepare(context);
        return vpnServicePrepareIntent == null;
    }

    private static void prepareForConnection(final Activity activity) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(activity, Manifest.permission.POST_NOTIFICATIONS) != PermissionChecker.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(activity, new String[]{Manifest.permission.POST_NOTIFICATIONS}, 101);
                return;
            }
        }
        Intent vpnServicePrepareIntent = VpnService.prepare(activity);
        if (vpnServicePrepareIntent != null) {
            activityResultLauncher.launch(vpnServicePrepareIntent);
        }
    }

    public static void startV2ray(final Activity activity, final String remark, final String config, final ArrayList<String> blocked_apps) {
        if (!refillV2rayConfig(remark, config, blocked_apps)) {
            return;
        }
        if (!isPreparedForConnection(activity)) {
            prepareForConnection(activity);
        } else {
            startTunnel(activity);
        }
    }

    public static void stopV2ray(final Context context) {
        Intent stop_intent = new Intent(V2rayConstants.V2RAY_SERVICE_COMMAND_INTENT);
        stop_intent.setPackage(context.getPackageName());
        stop_intent.putExtra(V2rayConstants.V2RAY_SERVICE_COMMAND_EXTRA, V2rayConstants.SERVICE_COMMANDS.STOP_SERVICE);
        context.sendBroadcast(stop_intent);
    }

    public static void getConnectedV2rayServerDelay(final Context context, final LatencyDelayListener latencyDelayCallback) {
        if (getConnectionState() != V2rayConstants.CONNECTION_STATES.CONNECTED) {
            latencyDelayCallback.OnResultReady(-1);
            return;
        }
        
        BroadcastReceiver connectionLatencyBroadcastReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                try {
                    int delay = Objects.requireNonNull(intent.getExtras()).getInt(V2rayConstants.V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_EXTRA);
                    latencyDelayCallback.OnResultReady(delay);
                } catch (Exception ignore) {
                    latencyDelayCallback.OnResultReady(-1);
                }
                context.unregisterReceiver(this);
            }
        };
        
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(connectionLatencyBroadcastReceiver, 
                new IntentFilter(V2rayConstants.V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_INTENT), 
                Context.RECEIVER_EXPORTED);
        } else {
            context.registerReceiver(connectionLatencyBroadcastReceiver, 
                new IntentFilter(V2rayConstants.V2RAY_SERVICE_CURRENT_CONFIG_DELAY_BROADCAST_INTENT));
        }
        
        Intent get_delay_intent = new Intent(V2rayConstants.V2RAY_SERVICE_COMMAND_INTENT);
        get_delay_intent.setPackage(context.getPackageName());
        get_delay_intent.putExtra(V2rayConstants.V2RAY_SERVICE_COMMAND_EXTRA, V2rayConstants.SERVICE_COMMANDS.MEASURE_DELAY);
        context.sendBroadcast(get_delay_intent);
    }

    public static long getV2rayServerDelay(final String config) {
        return com.hunter.app.core.V2rayCoreExecutor.getConfigDelay(config);
    }

    public static String getCoreVersion() {
        try {
            // Return the known XRay version from XRayManager
            return "25.1.30";
        } catch (Exception e) {
            Log.e(TAG, "Failed to get core version", e);
            return "Unknown";
        }
    }

    private static void startTunnel(final Context context) {
        Intent start_intent = new Intent(context, EnhancedVpnService.class);
        start_intent.setPackage(context.getPackageName());
        start_intent.putExtra(V2rayConstants.V2RAY_SERVICE_COMMAND_EXTRA, V2rayConstants.SERVICE_COMMANDS.START_SERVICE);
        start_intent.putExtra(V2rayConstants.V2RAY_SERVICE_CONFIG_EXTRA, V2rayConfigs.currentConfig);
        
        if (Build.VERSION.SDK_INT > Build.VERSION_CODES.N_MR1) {
            context.startForegroundService(start_intent);
        } else {
            context.startService(start_intent);
        }
    }

    private static boolean refillV2rayConfig(String remark, String config, final ArrayList<String> blockedApplications) {
        V2rayConfigs.currentConfig.remark = remark;
        V2rayConfigs.currentConfig.blockedApplications = blockedApplications;
        
        try {
            // Parse and validate configuration
            org.json.JSONObject config_json = new org.json.JSONObject(config);
            
            // Extract server info
            try {
                org.json.JSONArray outbounds = config_json.getJSONArray("outbounds");
                if (outbounds.length() > 0) {
                    org.json.JSONObject outbound = outbounds.getJSONObject(0);
                    org.json.JSONObject settings = outbound.getJSONObject("settings");
                    
                    if (settings.has("vnext")) {
                        org.json.JSONArray vnext = settings.getJSONArray("vnext");
                        if (vnext.length() > 0) {
                            org.json.JSONObject server = vnext.getJSONObject(0);
                            V2rayConfigs.currentConfig.currentServerAddress = server.getString("address");
                            V2rayConfigs.currentConfig.currentServerPort = server.getInt("port");
                        }
                    } else if (settings.has("servers")) {
                        org.json.JSONArray servers = settings.getJSONArray("servers");
                        if (servers.length() > 0) {
                            org.json.JSONObject server = servers.getJSONObject(0);
                            V2rayConfigs.currentConfig.currentServerAddress = server.getString("address");
                            V2rayConfigs.currentConfig.currentServerPort = server.getInt("port");
                        }
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "Failed to extract server info", e);
            }
            
            V2rayConfigs.currentConfig.fullJsonConfig = config_json.toString();
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to parse config", e);
            return false;
        }
    }

    public interface LatencyDelayListener {
        void OnResultReady(int delay);
    }
}
