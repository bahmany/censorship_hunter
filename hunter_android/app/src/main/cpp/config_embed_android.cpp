// config_embed_android.cpp — Android implementation of core/config_embed.h.
//
// On Android the config bundle ships as APK asset assets/configs.zst
// (zstd-compressed, produced by build_apk.sh) instead of the desktop
// .incbin mechanism. This file implements the same hunter::embed contract
// the orchestrator already consumes (PHASE 1b: Load embedded configs):
// open the asset via AAssetManager, decompress with the vendored zstd
// decoder (zstddeclib.c, linked into hunter_core), parse one URI per line,
// and expose them as a vector of strings.
#include "core/config_embed.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include "zstd.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#define TAG "ConfigEmbed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace hunter {
namespace embed {

namespace {

// Set from Java (MainActivity.onCreate) via nativeSetAssetManager() before
// nativeInit() runs, so it is always available when the orchestrator starts.
AAssetManager* g_asset_manager = nullptr;

std::once_flag g_once;
std::vector<std::string> g_configs;

// Mirror of the desktop parser in src/core/config_embed.cpp: one URI per
// line, skip blank/comment lines, keep only lines containing "://".
std::vector<std::string> parseBundle(const std::string& text) {
    std::vector<std::string> out;
    out.reserve(65536);
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        size_t b = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(a, b - a + 1);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (trimmed.find("://") == std::string::npos) continue;
        out.push_back(trimmed);
    }
    return out;
}

void decompressAsset() {
    if (!g_asset_manager) {
        LOGE("asset manager not set — no embedded configs");
        return;
    }

    AAsset* asset =
        AAssetManager_open(g_asset_manager, "configs.zst", AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("configs.zst asset not found in APK");
        return;
    }

    const void* zst_buf = AAsset_getBuffer(asset);
    size_t zst_len = (size_t)AAsset_getLength(asset);
    if (!zst_buf || zst_len == 0) {
        LOGE("configs.zst asset is empty or unreadable");
        AAsset_close(asset);
        return;
    }
    LOGI("Decompressing embedded config bundle (%zu compressed bytes)", zst_len);

    // Streaming decompression — handles unknown frame content sizes.
    std::vector<uint8_t> out;
    out.reserve(64 * 1024 * 1024);

    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) {
        LOGE("zstd: createDStream failed");
        AAsset_close(asset);
        return;
    }
    ZSTD_initDStream(ds);

    ZSTD_inBuffer in{zst_buf, zst_len, 0};
    std::vector<uint8_t> chunk(1 << 20);  // 1 MiB output chunk
    bool ok = true;
    while (in.pos < in.size) {
        ZSTD_outBuffer ob{chunk.data(), chunk.size(), 0};
        size_t ret = ZSTD_decompressStream(ds, &ob, &in);
        if (ZSTD_isError(ret)) {
            LOGE("zstd: decompress failed: %s", ZSTD_getErrorName(ret));
            ok = false;
            break;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + ob.pos);
        if (ret == 0) break;  // frame complete
    }
    ZSTD_freeDStream(ds);
    AAsset_close(asset);

    if (!ok) return;

    std::string text(reinterpret_cast<const char*>(out.data()), out.size());
    g_configs = parseBundle(text);
    LOGI("Loaded %zu embedded configs", g_configs.size());
}

}  // namespace

void setAndroidAssetManager(AAssetManager* am) { g_asset_manager = am; }

bool hasEmbeddedConfigs() {
    if (!g_asset_manager) return false;
    AAsset* asset =
        AAssetManager_open(g_asset_manager, "configs.zst", AASSET_MODE_BUFFER);
    if (!asset) return false;
    AAsset_close(asset);
    return true;
}

const std::vector<std::string>& configs() {
    std::call_once(g_once, [] { decompressAsset(); });
    return g_configs;
}

size_t configCount() { return configs().size(); }

}  // namespace embed
}  // namespace hunter
