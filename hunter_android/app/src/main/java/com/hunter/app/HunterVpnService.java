package com.hunter.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.VpnService;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * Foreground VpnService that owns the engine lifecycle:
 * core (orchestrator) -> local SOCKS5 (xray from nativeLibraryDir) -> TUN -> hev-socks5-tunnel.
 */
public class HunterVpnService extends VpnService {
    private static final String TAG = "HunterVpn";
    public static final String ACTION_START = "com.hunter.app.START";
    public static final String ACTION_STOP = "com.hunter.app.STOP";
    static final String PREFS = "hunter_vpn";
    static final String KEY_WANTED = "vpn_wanted";
    private static final String CHANNEL = "hunter_vpn";
    private static final int NOTIF_ID = 1001;
    private static final int PROBE_INTERVAL_S = 30;
    private static final int FAIL_THRESHOLD = 3;

    private static volatile boolean running = false;
    public static boolean isRunning() { return running; }

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final ScheduledExecutorService timer = Executors.newSingleThreadScheduledExecutor();
    private ScheduledFuture<?> probeTask;
    private ParcelFileDescriptor tun;
    private ConnectivityManager cm;
    private ConnectivityManager.NetworkCallback netCb;
    private final List<Network> networks = new ArrayList<>();
    private volatile Network currentUnderlying;
    private volatile boolean stopping = false;
    private int failures = 0;
    private volatile long lastNetChangeMs = 0;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        if (ACTION_STOP.equals(action)) {
            prefs().edit().putBoolean(KEY_WANTED, false).apply();
            shutdown();
            return START_NOT_STICKY;
        }
        // START, or null intent = sticky restart after process death.
        if (intent == null && !prefs().getBoolean(KEY_WANTED, false)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        // Must call startForeground quickly (5 s limit) even if we end up bailing out.
        startForegroundCompat(buildNotification(getString(R.string.vpn_connecting)));
        if (VpnService.prepare(this) != null) {   // permission revoked / never granted
            Log.w(TAG, "VPN permission missing; stopping");
            prefs().edit().putBoolean(KEY_WANTED, false).apply();
            shutdown();
            return START_NOT_STICKY;
        }
        prefs().edit().putBoolean(KEY_WANTED, true).apply();
        if (!running) {
            running = true;
            stopping = false;
            worker.execute(this::connect);
        }
        return START_STICKY;
    }

    // ---- connect pipeline (worker thread) ----
    private void connect() {
        try {
            if (!HunterNative.init(this)) { fail("core init failed"); return; }
            registerNetworkCallback();
            int port = 0;
            // The orchestrator needs time to find a working config on first run.
            for (int i = 0; i < 100 && !stopping && port == 0; i++) {
                port = HunterNative.startSocks(0);
                if (port == 0) { update(getString(R.string.vpn_searching)); sleep(3000); }
            }
            if (stopping) return;
            if (port == 0) { fail("no working config found"); return; }
            if (!establishTun(port)) { fail("TUN establish failed"); return; }
            update(getString(R.string.vpn_connected));
            probeTask = timer.scheduleWithFixedDelay(this::probe, PROBE_INTERVAL_S, PROBE_INTERVAL_S, TimeUnit.SECONDS);
        } catch (Throwable t) {
            Log.e(TAG, "connect failed", t);
            fail(String.valueOf(t));
        }
    }

    private boolean establishTun(int socksPort) throws Exception {
        Builder b = new Builder()
                .setSession("Hunter")
                .setMtu(TunConfig.MTU)
                .addAddress(TunConfig.TUN_V4, 32)
                .addAddress(TunConfig.TUN_V6, 128)
                .addRoute("0.0.0.0", 0)
                .addRoute("::", 0)
                .addDnsServer(TunConfig.MAPDNS_ADDR)
                .setBlocking(false);
        if (Build.VERSION.SDK_INT >= 29) b.setMetered(false);
        try {
            b.addDisallowedApplication(getPackageName());   // our engines/probes must bypass the TUN
        } catch (Exception e) {
            Log.e(TAG, "cannot exclude own app", e);
            return false;                                   // refusing: would create a routing loop
        }
        Network under = pickUnderlying();
        if (under != null) b.setUnderlyingNetworks(new Network[]{under});
        File cfg = new File(getFilesDir(), "runtime/hev.yml");
        try (FileOutputStream os = new FileOutputStream(cfg)) {
            os.write(TunConfig.hevYaml(socksPort).getBytes("UTF-8"));
        }
        tun = b.establish();
        if (tun == null) return false;
        TProxyService.TProxyStartService(cfg.getAbsolutePath(), tun.getFd());
        return true;
    }

    private void probe() {
        if (stopping) return;
        int port = HunterNative.socksPort();
        boolean ok = port > 0 && HealthProbe.viaSocks(port, 8000);
        failures = ok ? 0 : failures + 1;
        if (TunConfig.shouldFailover(failures, FAIL_THRESHOLD)) {
            failures = 0;
            reconnectSocks(1);
        }
    }

    /** Restart the engine on the next candidate; hev reconnects to the new port by restarting. */
    private synchronized void reconnectSocks(int skip) {
        worker.execute(() -> {
            if (stopping) return;
            update(getString(R.string.vpn_reconnecting));
            TProxyService.TProxyStopService();
            int port = HunterNative.startSocks(skip);
            if (port == 0 || tun == null) { update(getString(R.string.vpn_searching)); return; }
            try {
                File cfg = new File(getFilesDir(), "runtime/hev.yml");
                try (FileOutputStream os = new FileOutputStream(cfg)) {
                    os.write(TunConfig.hevYaml(port).getBytes("UTF-8"));
                }
                TProxyService.TProxyStartService(cfg.getAbsolutePath(), tun.getFd());
                update(getString(R.string.vpn_connected));
            } catch (Exception e) {
                Log.e(TAG, "reconnect failed", e);
            }
        });
    }

    // ---- network handling ----
    private void registerNetworkCallback() {
        cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        NetworkRequest req = new NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)
                .build();
        netCb = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) {
                synchronized (networks) { networks.remove(n); networks.add(0, n); }
                onUnderlyingChanged();
            }
            @Override public void onLost(Network n) {
                synchronized (networks) { networks.remove(n); }
                onUnderlyingChanged();
            }
            @Override public void onCapabilitiesChanged(Network n, NetworkCapabilities c) {
                if (n.equals(currentUnderlying)) return;
                if (c.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)) {
                    synchronized (networks) { networks.remove(n); networks.add(0, n); }
                    onUnderlyingChanged();
                }
            }
        };
        cm.registerNetworkCallback(req, netCb);
    }

    private Network pickUnderlying() {
        synchronized (networks) { return networks.isEmpty() ? null : networks.get(0); }
    }

    private void onUnderlyingChanged() {
        Network n = pickUnderlying();
        if (n == null ? currentUnderlying == null : n.equals(currentUnderlying)) return;
        currentUnderlying = n;
        setUnderlyingNetworks(n == null ? null : new Network[]{n});
        long now = System.currentTimeMillis();
        if (tun != null && n != null && now - lastNetChangeMs > 3000) {   // debounce flaps
            lastNetChangeMs = now;
            Log.i(TAG, "underlying network changed -> re-check");
            timer.schedule(() -> {
                if (stopping) return;
                int port = HunterNative.socksPort();
                if (port == 0 || !HealthProbe.viaSocks(port, 6000)) reconnectSocks(0);
            }, 2, TimeUnit.SECONDS);
        }
    }

    // ---- notification / lifecycle ----
    private SharedPreferences prefs() { return getSharedPreferences(PREFS, MODE_PRIVATE); }

    private Notification buildNotification(String text) {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26 && nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Hunter VPN", NotificationManager.IMPORTANCE_LOW));
        }
        int piFlags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), piFlags);
        PendingIntent stop = PendingIntent.getService(this, 1,
                new Intent(this, HunterVpnService.class).setAction(ACTION_STOP), piFlags);
        Notification.Builder nb = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        return nb.setContentTitle("Hunter VPN").setContentText(text)
                .setSmallIcon(R.mipmap.ic_launcher).setOngoing(true).setContentIntent(open)
                .addAction(new Notification.Action.Builder(null, getString(R.string.vpn_disconnect), stop).build())
                .build();
    }

    private void startForegroundCompat(Notification n) {
        if (Build.VERSION.SDK_INT >= 29) {
            // VPN apps use the systemExempted FGS type on Android 14+.
            int type = Build.VERSION.SDK_INT >= 34 ? ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED : 0;
            startForeground(NOTIF_ID, n, type);
        } else {
            startForeground(NOTIF_ID, n);
        }
    }

    private void update(String text) {
        ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).notify(NOTIF_ID, buildNotification(text));
    }

    private void fail(String why) {
        Log.e(TAG, "VPN failed: " + why);
        prefs().edit().putBoolean(KEY_WANTED, false).apply();
        shutdown();
    }

    private static void sleep(long ms) { try { Thread.sleep(ms); } catch (InterruptedException ignored) {} }

    /** Tear down everything this service owns: tunnel, engines, core, callbacks. Idempotent. */
    private synchronized void shutdown() {
        stopping = true;
        if (probeTask != null) probeTask.cancel(false);
        if (cm != null && netCb != null) {
            try { cm.unregisterNetworkCallback(netCb); } catch (Exception ignored) {}
            netCb = null;
        }
        try { TProxyService.TProxyStopService(); } catch (Throwable ignored) {}
        if (tun != null) { try { tun.close(); } catch (Exception ignored) {} tun = null; }
        try { HunterNative.stopCore(); } catch (Throwable t) { Log.e(TAG, "stopCore", t); }
        running = false;
        stopForeground(true);
        stopSelf();
    }

    @Override public void onRevoke() {          // user turned VPN off / another VPN took over
        prefs().edit().putBoolean(KEY_WANTED, false).apply();
        shutdown();
        super.onRevoke();
    }

    @Override public void onDestroy() {
        if (running) shutdown();
        worker.shutdownNow();
        timer.shutdownNow();
        super.onDestroy();
    }
}
