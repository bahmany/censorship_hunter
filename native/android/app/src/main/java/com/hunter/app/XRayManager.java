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
    private final File packagedXrayBinary;
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
        String nativeLibDir = context.getApplicationInfo().nativeLibraryDir;
        this.packagedXrayBinary = new File(nativeLibDir, "libxray.so");
        this.executor = new ThreadPoolExecutor(1, 1, 30, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(5));

        if (!xrayDir.exists()) xrayDir.mkdirs();

        // Diagnostic logging for binary selection
        Log.i(TAG, "nativeLibraryDir: " + nativeLibDir);
        Log.i(TAG, "packagedXrayBinary: " + packagedXrayBinary.getAbsolutePath()
            + " exists=" + packagedXrayBinary.exists()
            + " canExec=" + packagedXrayBinary.canExecute()
            + " size=" + packagedXrayBinary.length());
        Log.i(TAG, "extractedXrayBinary: " + xrayBinary.getAbsolutePath()
            + " exists=" + xrayBinary.exists()
            + " canExec=" + xrayBinary.canExecute()
            + " size=" + xrayBinary.length());
        // List files in nativeLibraryDir to see what's actually there
        try {
            File nativeDir = new File(nativeLibDir);
            String[] files = nativeDir.list();
            if (files != null) {
                Log.i(TAG, "nativeLibraryDir contents (" + files.length + " files): " + java.util.Arrays.toString(files));
            } else {
                Log.w(TAG, "nativeLibraryDir.list() returned null");
            }
        } catch (Exception e) {
            Log.w(TAG, "Failed to list nativeLibraryDir", e);
        }
    }

    /**
     * Check if xray binary is ready to use.
     */
    public boolean isReady() {
        File exec = getExecBinaryFile();
        boolean execOk = exec != null && exec.exists() && exec.length() > 1000 && exec.canExecute();
        boolean geoOk = new File(xrayDir, "geoip.dat").exists()
            && new File(xrayDir, "geosite.dat").exists();
        if (!execOk || !geoOk) {
            Log.d(TAG, "isReady: execOk=" + execOk + " geoOk=" + geoOk
                + " exec=" + (exec != null ? exec.getAbsolutePath() : "null"));
        }
        return execOk && geoOk;
    }

    /**
     * Get path to xray binary.
     */
    public String getBinaryPath() {
        if (!isReady()) return null;
        File exec = getExecBinaryFile();
        if (exec == null) return null;
        return exec.getAbsolutePath();
    }

    private File getExecBinaryFile() {
        // Prefer packaged binary from nativeLibraryDir (always has exec permission on Android)
        if (packagedXrayBinary.exists() && packagedXrayBinary.length() > 1000 && packagedXrayBinary.canExecute()) {
            Log.d(TAG, "Using packaged libxray.so from nativeLibraryDir");
            return packagedXrayBinary;
        }
        // Fallback to extracted binary in app files dir
        if (xrayBinary.exists() && xrayBinary.length() > 1000 && xrayBinary.canExecute()) {
            Log.d(TAG, "Using extracted xray from files dir");
            return xrayBinary;
        }
        Log.w(TAG, "No xray binary found! packaged=" + packagedXrayBinary.getAbsolutePath()
            + " exists=" + packagedXrayBinary.exists()
            + ", extracted=" + xrayBinary.getAbsolutePath()
            + " exists=" + xrayBinary.exists());
        return xrayBinary; // return anyway, let caller handle the error
    }

    private File getFallbackExecBinaryFile(File current) {
        if (current == null) return null;
        if (current.equals(packagedXrayBinary)) {
            if (xrayBinary.exists() && xrayBinary.length() > 1000) {
                return xrayBinary;
            }
            return null;
        }
        if (current.equals(xrayBinary)) {
            if (packagedXrayBinary.exists() && packagedXrayBinary.length() > 1000) {
                return packagedXrayBinary;
            }
            return null;
        }
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
            callback.onComplete(getBinaryPath());
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

                if (!(packagedXrayBinary.exists() && packagedXrayBinary.length() > 1000 && packagedXrayBinary.canExecute())) {
                    // Step 1: Extract xray binary from ZIP asset
                    callback.onProgress("Extracting xray binary...", 10);
                    InputStream zipStream = assets.open(ASSETS_DIR + "/" + zipName);
                    extractXrayFromZip(zipStream);
                    zipStream.close();

                    // Make executable (may still be blocked from exec on some devices; packagedXrayBinary is preferred)
                    xrayBinary.setExecutable(true, false);
                    xrayBinary.setReadable(true, false);
                    xrayBinary.setWritable(true, false);
                    Log.i(TAG, "Extracted xray binary: " + xrayBinary.length() + " bytes");
                }

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
                String binPath = getBinaryPath();
                Log.i(TAG, "XRay v" + XRAY_VERSION + " ready for " + detectedAbi + " at: " + binPath);
                callback.onComplete(binPath);

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
            File exec = getExecBinaryFile();
            if (exec == null) {
                Log.e(TAG, "XRay exec binary not available");
                return null;
            }
            
            Log.i(TAG, "Starting XRay using: " + exec.getAbsolutePath());
            Log.i(TAG, "XRay config: " + configFile.getAbsolutePath());
            Log.i(TAG, "XRay working dir: " + xrayDir.getAbsolutePath());
            Log.i(TAG, "XRAY_LOCATION_ASSET: " + xrayDir.getAbsolutePath());
            
            ProcessBuilder pb = new ProcessBuilder(
                exec.getAbsolutePath(), "run", "-c", configFile.getAbsolutePath());
            pb.directory(xrayDir);
            // Tell xray where to find geoip.dat and geosite.dat
            pb.environment().put("XRAY_LOCATION_ASSET", xrayDir.getAbsolutePath());
            pb.redirectErrorStream(true);
            Process process = pb.start();

            // Start background thread to continuously read and log XRay output
            final String processTag = "XRay-" + configFile.getName();
            Thread outputReader = new Thread(() -> {
                try {
                    java.io.BufferedReader reader = new java.io.BufferedReader(
                        new java.io.InputStreamReader(process.getInputStream()));
                    String line;
                    while ((line = reader.readLine()) != null) {
                        Log.i(processTag, line);
                    }
                } catch (java.io.InterruptedIOException e) {
                    Log.i(processTag, "Output reader stopped (process terminated)");
                } catch (Exception e) {
                    Log.w(processTag, "Output reader stopped", e);
                }
            }, "XRayOutputReader");
            outputReader.setDaemon(true);
            outputReader.start();

            // Wait briefly to check if it started
            Thread.sleep(800);
            if (process.isAlive()) {
                Log.i(TAG, "XRay process started successfully (PID would be available via reflection)");
                return process;
            } else {
                // Log exit code and any remaining output for debugging
                try {
                    int exitCode = process.exitValue();
                    Log.e(TAG, "XRay process died immediately after start, exit code: " + exitCode);
                    
                    // Read any remaining error output
                    java.io.BufferedReader errorReader = new java.io.BufferedReader(
                        new java.io.InputStreamReader(process.getInputStream()));
                    StringBuilder errorOutput = new StringBuilder();
                    String line;
                    while ((line = errorReader.readLine()) != null) {
                        errorOutput.append(line).append("\n");
                    }
                    if (errorOutput.length() > 0) {
                        Log.e(TAG, "XRay error output: " + errorOutput.toString());
                    }
                    
                    // Log the config that caused the issue (avoid logging full config to prevent OOM)
                    Log.e(TAG, "Config that caused failure: " + configFile.getAbsolutePath() + " (" + configFile.length() + " bytes)");
                    try {
                        java.io.BufferedReader configReader = new java.io.BufferedReader(new java.io.FileReader(configFile));
                        StringBuilder configPrefix = new StringBuilder();
                        String configLine;
                        int lines = 0;
                        int chars = 0;
                        while ((configLine = configReader.readLine()) != null) {
                            configPrefix.append(configLine).append("\n");
                            lines++;
                            chars += configLine.length();
                            if (lines >= 40 || chars >= 4096) {
                                break;
                            }
                        }
                        configReader.close();
                        if (configPrefix.length() > 0) {
                            Log.e(TAG, "Config prefix (first ~4KB):\n" + configPrefix);
                        }
                    } catch (Exception ignored) {}
                    
                } catch (Exception e) {
                    Log.e(TAG, "Failed to get XRay exit info", e);
                }
                return null;
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to start XRay process", e);
            try {
                File exec = getExecBinaryFile();
                File fallback = getFallbackExecBinaryFile(exec);
                if (fallback != null) {
                    Log.w(TAG, "Retrying XRay using fallback: " + fallback.getAbsolutePath());
                    ProcessBuilder pb = new ProcessBuilder(
                        fallback.getAbsolutePath(), "run", "-c", configFile.getAbsolutePath());
                    pb.directory(xrayDir);
                    pb.environment().put("XRAY_LOCATION_ASSET", xrayDir.getAbsolutePath());
                    pb.redirectErrorStream(true);
                    Process process = pb.start();
                    Thread.sleep(800);
                    if (process.isAlive()) {
                        return process;
                    }
                }
            } catch (Exception ignored) {}
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
