package com.hunter.app;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.VpnService;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.LinearLayout;

public class MainActivity extends Activity {

    private static final String TAG = "HunterMainActivity";
    private static final int REQ_VPN = 100;
    private static final int REQ_NOTIF = 101;

    static {
        System.loadLibrary("hunter");
    }

    private HunterView view;
    private Button vpnButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        LinearLayout layout = findViewById(R.id.main_layout);

        vpnButton = new Button(this);
        vpnButton.setOnClickListener(v -> onVpnButton());
        layout.addView(vpnButton, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        view = new HunterView(this);
        layout.addView(view, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        // Core init (absolute filesDir, engines from nativeLibraryDir) off the UI thread.
        // Idempotent: a no-op when HunterVpnService already started the core.
        new Thread(() -> {
            if (!HunterNative.init(this)) Log.e(TAG, "core init failed");
        }, "hunter-init").start();
        refreshButton();
    }

    private void refreshButton() {
        vpnButton.setText(HunterVpnService.isRunning() ? R.string.vpn_disconnect : R.string.vpn_connect);
    }

    private void onVpnButton() {
        if (HunterVpnService.isRunning()) {
            startService(new Intent(this, HunterVpnService.class).setAction(HunterVpnService.ACTION_STOP));
            vpnButton.postDelayed(this::refreshButton, 500);
            return;
        }
        // 1) notification permission (Android 13+) so the foreground notification is visible.
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIF);
            return;
        }
        requestVpnPermissionAndStart();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        // Proceed even if denied: the VPN still works, only the notification is hidden.
        if (code == REQ_NOTIF) requestVpnPermissionAndStart();
    }

    private void requestVpnPermissionAndStart() {
        Intent prepare = VpnService.prepare(this);   // null if already granted
        if (prepare != null) {
            startActivityForResult(prepare, REQ_VPN);
        } else {
            startVpn();
        }
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req == REQ_VPN && result == RESULT_OK) startVpn();
        else if (req == REQ_VPN) Log.w(TAG, "VPN permission denied");
    }

    private void startVpn() {
        Intent i = new Intent(this, HunterVpnService.class).setAction(HunterVpnService.ACTION_START);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(i); else startService(i);
        vpnButton.postDelayed(this::refreshButton, 500);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        nativeShutdown();                       // UI (ImGui) teardown only
        // The service owns the engines while the VPN is up; otherwise stop the core with the UI.
        if (!HunterVpnService.isRunning() && isFinishing()) {
            HunterNative.stopCore();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        nativePause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        nativeResume();
        refreshButton();
    }

    public void sendTouchEvent(int action, float x, float y) {
        nativeTouchEvent(action, x, y);
    }

    public void sendKeyEvent(int action, int keyCode) {
        nativeKeyEvent(action, keyCode);
    }

    private static native void nativeShutdown();
    private static native void nativePause();
    private static native void nativeResume();
    private static native void nativeTouchEvent(int action, float x, float y);
    private static native void nativeKeyEvent(int action, int keyCode);
}
