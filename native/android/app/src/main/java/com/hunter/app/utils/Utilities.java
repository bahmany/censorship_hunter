package com.hunter.app.utils;

import android.content.Context;
import android.provider.Settings;
import android.util.Base64;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/**
 * Utility functions for V2Ray configuration and management
 * Based on v2rayNG utilities
 */
public class Utilities {
    private static final String TAG = "Utilities";

    /**
     * Generate device ID for XUDP base key
     */
    public static String getDeviceIdForXUDPBaseKey() {
        String androidId = Settings.Secure.ANDROID_ID;
        byte[] androidIdBytes = androidId.getBytes(StandardCharsets.UTF_8);
        return Base64.encodeToString(Arrays.copyOf(androidIdBytes, 32), Base64.NO_PADDING | Base64.URL_SAFE);
    }

    /**
     * Copy files from input stream to file
     */
    public static void CopyFiles(InputStream src, File dst) throws IOException {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            try (OutputStream out = Files.newOutputStream(dst.toPath())) {
                byte[] buf = new byte[1024];
                int len;
                while ((len = src.read(buf)) > 0) {
                    out.write(buf, 0, len);
                }
            }
        } else {
            try (OutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[1024];
                int len;
                while ((len = src.read(buf)) > 0) {
                    out.write(buf, 0, len);
                }
            }
        }
    }

    /**
     * Get user assets path for geo files
     */
    public static String getUserAssetsPath(Context context) {
        File extDir = context.getExternalFilesDir("assets");
        if (extDir == null) {
            return "";
        }
        if (!extDir.exists()) {
            return context.getDir("assets", 0).getAbsolutePath();
        } else {
            return extDir.getAbsolutePath();
        }
    }

    /**
     * Normalize IPv6 address format
     */
    public static String normalizeIpv6(String address) {
        if (isIpv6Address(address) && !address.contains("[") && !address.contains("]")) {
            return String.format("[%s]", address);
        } else {
            return address;
        }
    }

    /**
     * Check if address is IPv6
     */
    public static boolean isIpv6Address(String address) {
        String[] tmp = address.split(":");
        return tmp.length > 2;
    }

    /**
     * Normalize V2Ray full configuration
     */
    public static String normalizeV2rayFullConfig(String config) {
        // Since libv2ray is not available, return config as-is
        // This functionality can be added later when libv2ray is integrated
        return config;
    }

    /**
     * Parse traffic statistics
     */
    public static String parseTraffic(final double bytes, final boolean inBits, final boolean isMomentary) {
        double value = inBits ? bytes * 8 : bytes;
        if (value < V2rayConstants.KILO_BYTE) {
            return String.format(java.util.Locale.getDefault(), "%.1f " + (inBits ? "b" : "B") + (isMomentary ? "/s" : ""), value);
        } else if (value < V2rayConstants.MEGA_BYTE) {
            return String.format(java.util.Locale.getDefault(), "%.1f K" + (inBits ? "b" : "B") + (isMomentary ? "/s" : ""), value / V2rayConstants.KILO_BYTE);
        } else if (value < V2rayConstants.GIGA_BYTE) {
            return String.format(java.util.Locale.getDefault(), "%.1f M" + (inBits ? "b" : "B") + (isMomentary ? "/s" : ""), value / V2rayConstants.MEGA_BYTE);
        } else {
            return String.format(java.util.Locale.getDefault(), "%.2f G" + (inBits ? "b" : "B") + (isMomentary ? "/s" : ""), value / V2rayConstants.GIGA_BYTE);
        }
    }

    /**
     * Convert integer to two-digit string
     */
    public static String convertIntToTwoDigit(int value) {
        if (value < 10) return "0" + value;
        else return String.valueOf(value);
    }
}
