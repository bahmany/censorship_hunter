package com.hunter.app;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Manages the embedded XRay binary bundled in APK assets.
 * On first run (or version change), extracts the correct binary for the device's
 * CPU architecture from assets/xray/{abi}.zip along with geoip.dat and geosite.dat.
 * No network download needed — everything is inside the APK.
 */
public class XRayManager {
    private static final String TAG = "XRayManager";
    private static final String XRAY_VERSION = "25.1.30";
    private static final String ASSETS_DIR = "xray";

    private static final String XRAY_DIR = "xray_bin";
    private static final String XRAY_BINARY = "xray";
    private static final String VERSION_FILE = "xray_version.txt";

    private final Context context;
    private final File xrayDir;
    private final File xrayBinary;
    private final ExecutorService executor;
    private volatile boolean isInstalling = false;

    public interface InstallCallback {
        void onProgress(String message, int percent);
        void onComplete(String binaryPath);
        void onError(String error);
    }

    // Keep old interface name for compatibility
    public interface DownloadCallback extends InstallCallback {}

    public XRayManager(Context context) {
        this.context = context.getApplicationContext();
        this.xrayDir = new File(context.getFilesDir(), XRAY_DIR);
        this.xrayBinary = new File(xrayDir, XRAY_BINARY);
        this.executor = new ThreadPoolExecutor(1, 1, 30, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(5));

        if (!xrayDir.exists()) xrayDir.mkdirs();
    }

    /**
     * Check if xray binary is ready to use.
     */
    public boolean isReady() {
        return xrayBinary.exists() && xrayBinary.canExecute() && xrayBinary.length() > 1000
            && new File(xrayDir, "geoip.dat").exists()
            && new File(xrayDir, "geosite.dat").exists();
    }

    /**
     * Get path to xray binary.
     */
    public String getBinaryPath() {
        if (isReady()) return xrayBinary.getAbsolutePath();
        return null;
    }

    /**
     * Get the correct asset ZIP filename for this device's CPU architecture.
     * Tries all supported ABIs in order of preference.
     */
    private String getAssetZipName() {
        String[] abis = Build.SUPPORTED_ABIS;
        for (String abi : abis) {
            switch (abi) {
                case "arm64-v8a":
                case "armeabi-v7a":
                case "x86_64":
                case "x86":
                    return abi + ".zip";
            }
        }
        // Default fallback
        return "arm64-v8a.zip";
    }

    /**
     * Check if binary needs installation or update (wrong version or missing).
     */
    public boolean needsUpdate() {
        if (!isReady()) return true;
        try {
            File vf = new File(xrayDir, VERSION_FILE);
            if (!vf.exists()) return true;
            FileInputStream fis = new FileInputStream(vf);
            byte[] buf = new byte[64];
            int len = fis.read(buf);
            fis.close();
            String installed = new String(buf, 0, len).trim();
            return !XRAY_VERSION.equals(installed);
        } catch (Exception e) {
            return true;
        }
    }

    /**
     * Extract and install xray binary from APK assets. No network needed.
     * Safe to call multiple times — skips if already installed and up-to-date.
     */
    public void ensureInstalled(InstallCallback callback) {
        if (isReady() && !needsUpdate()) {
            callback.onComplete(xrayBinary.getAbsolutePath());
            return;
        }

        if (isInstalling) {
            callback.onError("Installation already in progress");
            return;
        }

        isInstalling = true;
        executor.execute(() -> {
            try {
                String zipName = getAssetZipName();
                String detectedAbi = zipName.replace(".zip", "");
                Log.i(TAG, "Installing XRay for ABI: " + detectedAbi + " from assets/" + ASSETS_DIR + "/" + zipName);
                callback.onProgress("Installing V2Ray engine (" + detectedAbi + ")...", 0);

                AssetManager assets = context.getAssets();

                // Step 1: Extract xray binary from ZIP asset
                callback.onProgress("Extracting xray binary...", 10);
                InputStream zipStream = assets.open(ASSETS_DIR + "/" + zipName);
                extractXrayFromZip(zipStream);
                zipStream.close();

                // Make executable
                xrayBinary.setExecutable(true, false);
                xrayBinary.setReadable(true, false);
                Log.i(TAG, "Extracted xray binary: " + xrayBinary.length() + " bytes");

                // Step 2: Copy geoip.dat from assets
                callback.onProgress("Installing geoip.dat...", 40);
                copyAssetToFile(assets, ASSETS_DIR + "/geoip.dat", new File(xrayDir, "geoip.dat"));

                // Step 3: Copy geosite.dat from assets
                callback.onProgress("Installing geosite.dat...", 70);
                copyAssetToFile(assets, ASSETS_DIR + "/geosite.dat", new File(xrayDir, "geosite.dat"));

                // Step 4: Save version marker
                callback.onProgress("Finalizing...", 90);
                FileOutputStream vos = new FileOutputStream(new File(xrayDir, VERSION_FILE));
                vos.write(XRAY_VERSION.getBytes());
                vos.close();

                callback.onProgress("V2Ray engine ready", 100);
                Log.i(TAG, "XRay v" + XRAY_VERSION + " installed for " + detectedAbi
                    + " at: " + xrayBinary.getAbsolutePath());
                callback.onComplete(xrayBinary.getAbsolutePath());

            } catch (Exception e) {
                Log.e(TAG, "Failed to install XRay from assets", e);
                callback.onError("Install failed: " + e.getMessage());
            } finally {
                isInstalling = false;
            }
        });
    }

    /**
     * Extract the xray executable from a ZIP input stream.
     */
    private void extractXrayFromZip(InputStream zipStream) throws Exception {
        ZipInputStream zis = new ZipInputStream(zipStream);
        ZipEntry entry;
        boolean found = false;

        while ((entry = zis.getNextEntry()) != null) {
            String name = entry.getName().toLowerCase();
            if (!entry.isDirectory() && (name.equals("xray") || name.equals("xray.exe")
                || name.endsWith("/xray") || name.endsWith("/xray.exe"))) {

                FileOutputStream fos = new FileOutputStream(xrayBinary);
                byte[] buffer = new byte[8192];
                int len;
                while ((len = zis.read(buffer)) > 0) {
                    fos.write(buffer, 0, len);
                }
                fos.close();
                found = true;
                break;
            }
            zis.closeEntry();
        }
        zis.close();

        if (!found) {
            throw new Exception("xray binary not found in ZIP asset");
        }
    }

    /**
     * Copy a file from assets to the filesystem.
     */
    private void copyAssetToFile(AssetManager assets, String assetPath, File outFile) throws Exception {
        InputStream in = assets.open(assetPath);
        FileOutputStream out = new FileOutputStream(outFile);
        byte[] buffer = new byte[8192];
        int len;
        while ((len = in.read(buffer)) > 0) {
            out.write(buffer, 0, len);
        }
        out.close();
        in.close();
        Log.i(TAG, "Copied " + assetPath + " -> " + outFile.getAbsolutePath()
            + " (" + outFile.length() + " bytes)");
    }

    /**
     * Start an xray process with the given config file.
     * Sets XRAY_LOCATION_ASSET to the xrayDir so geoip/geosite are found.
     * Returns the Process, or null on failure.
     */
    public Process startProcess(File configFile) {
        if (!isReady()) {
            Log.e(TAG, "XRay not ready");
            return null;
        }

        try {
            ProcessBuilder pb = new ProcessBuilder(
                xrayBinary.getAbsolutePath(), "run", "-c", configFile.getAbsolutePath());
            pb.directory(xrayDir);
            // Tell xray where to find geoip.dat and geosite.dat
            pb.environment().put("XRAY_LOCATION_ASSET", xrayDir.getAbsolutePath());
            pb.redirectErrorStream(true);
            Process process = pb.start();

            // Wait briefly to check if it started
            Thread.sleep(800);
            if (process.isAlive()) {
                return process;
            } else {
                // Read error output
                byte[] errBuf = new byte[1024];
                int read = process.getInputStream().read(errBuf);
                if (read > 0) {
                    Log.e(TAG, "XRay start error: " + new String(errBuf, 0, read));
                }
                return null;
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to start XRay process", e);
            return null;
        }
    }

    /**
     * Write a V2Ray JSON config to a temp file and return it.
     */
    public File writeConfig(String jsonConfig, int port) {
        try {
            File configFile = new File(context.getCacheDir(), "v2ray_" + port + ".json");
            FileOutputStream fos = new FileOutputStream(configFile);
            fos.write(jsonConfig.getBytes("UTF-8"));
            fos.close();
            return configFile;
        } catch (Exception e) {
            Log.e(TAG, "Failed to write config", e);
            return null;
        }
    }

    public boolean isDownloading() {
        return isInstalling;
    }

    public String getVersion() {
        return XRAY_VERSION;
    }

    public void shutdown() {
        executor.shutdown();
    }
}
