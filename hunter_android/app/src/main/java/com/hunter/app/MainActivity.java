package com.hunter.app;

import android.app.Activity;
import android.content.Context;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class MainActivity extends Activity {

    private static final String TAG = "HunterMainActivity";

    static {
        System.loadLibrary("hunter");
    }

    private HunterView view;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_main);

        // Extract bundled engine binaries from APK assets to filesDir.
        // The engines are stored as uncompressed files in assets/engines/
        // and extracted here so the native code can execute them.
        File engineDir = extractEngines();

        // Tell the native layer where the engines are BEFORE initializing.
        if (engineDir != null) {
            nativeSetEngineDir(engineDir.getAbsolutePath());
        }

        // Give the native layer access to APK assets so it can read the
        // embedded config bundle (assets/configs.zst) at startup.
        nativeSetAssetManager(getAssets());

        view = new HunterView(this);
        LinearLayout layout = findViewById(R.id.main_layout);
        layout.addView(view);

        nativeInit();
    }

    /**
     * Extract engine binaries (xray, sing-box) from APK assets to
     * getFilesDir()/engines/. Skips extraction if already present and
     * the size matches (fast re-launch).
     *
     * Assets are stored uncompressed in the APK (no compression flag)
     * so they can be directly mmap'd/copied without decompression.
     *
     * @return The engines directory, or null on failure.
     */
    private File extractEngines() {
        File enginesDir = new File(getFilesDir(), "engines");
        if (!enginesDir.exists()) {
            enginesDir.mkdirs();
        }

        String[] engineNames = {"xray", "sing-box"};
        AssetManager am = getAssets();

        for (String name : engineNames) {
            File target = new File(enginesDir, name);
            String assetPath = "engines/" + name;

            try {
                // Check if already extracted (size match).
                InputStream check = am.open(assetPath);
                long assetSize = check.available();
                check.close();

                if (target.exists() && target.length() == assetSize) {
                    Log.i(TAG, "Engine " + name + " already extracted (" + assetSize + " bytes)");
                    // Ensure executable permission.
                    target.setExecutable(true, true);
                    continue;
                }

                // Extract from assets.
                Log.i(TAG, "Extracting engine " + name + " from APK assets (" + assetSize + " bytes)");
                InputStream is = am.open(assetPath);
                FileOutputStream os = new FileOutputStream(target);
                byte[] buf = new byte[8192];
                int len;
                while ((len = is.read(buf)) > 0) {
                    os.write(buf, 0, len);
                }
                os.close();
                is.close();

                // Set executable permission (owner only).
                target.setExecutable(true, true);
                Log.i(TAG, "Engine " + name + " extracted to " + target.getAbsolutePath());

            } catch (IOException e) {
                Log.e(TAG, "Failed to extract engine " + name + ": " + e.getMessage());
            }
        }

        // Write a manifest for transparency (same as desktop single-file mode).
        writeManifest(enginesDir);

        return enginesDir;
    }

    /**
     * Write a MANIFEST.txt in the engines directory for AV transparency.
     */
    private void writeManifest(File dir) {
        File manifest = new File(dir, "MANIFEST.txt");
        StringBuilder sb = new StringBuilder();
        sb.append("================================================================\n");
        sb.append("  Hunter — Embedded Engine Extraction Manifest (Android)\n");
        sb.append("================================================================\n\n");
        sb.append("This directory contains proxy engine binaries extracted from\n");
        sb.append("the Hunter APK assets. They are NOT malware.\n\n");
        sb.append("Hunter is an open-source anti-censorship proxy tool:\n");
        sb.append("  https://github.com/bahmany/censorship_hunter\n\n");
        sb.append("Engines:\n");
        sb.append("  xray     — https://github.com/XTLS/Xray-core/releases\n");
        sb.append("  sing-box — https://github.com/SagerNet/sing-box/releases\n\n");
        sb.append("To verify: sha256sum <engine_name>\n");
        sb.append("================================================================\n");

        try {
            FileOutputStream fos = new FileOutputStream(manifest);
            fos.write(sb.toString().getBytes());
            fos.close();
        } catch (IOException e) {
            Log.e(TAG, "Failed to write manifest: " + e.getMessage());
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        nativeShutdown();
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
    }

    public void sendTouchEvent(int action, float x, float y) {
        nativeTouchEvent(action, x, y);
    }

    public void sendKeyEvent(int action, int keyCode) {
        nativeKeyEvent(action, keyCode);
    }

    private static native void nativeSetEngineDir(String dir);
    private static native void nativeSetAssetManager(AssetManager assetManager);
    private static native void nativeInit();
    private static native void nativeShutdown();
    private static native void nativePause();
    private static native void nativeResume();
    private static native void nativeTouchEvent(int action, float x, float y);
    private static native void nativeKeyEvent(int action, int keyCode);
}
