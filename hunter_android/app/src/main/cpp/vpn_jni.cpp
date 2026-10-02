// JNI for HunterNative: core lifecycle + local SOCKS selection used by HunterVpnService.
// Engines run from nativeLibraryDir (lib*.so); all data paths are absolute under filesDir.
#include <jni.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "android_app.h"
#include "core/engine_embed.h"
#include "proxy/proxy_server_manager.h"

#define TAG "HunterVpnJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" bool androidAppInit();
extern "C" void androidAppShutdown();
namespace hunter { namespace embed { void setAndroidAssetManager(AAssetManager* am); } }

namespace {
std::mutex g_mu;
std::string g_lib_dir;
std::string g_cur_uri;
int g_cur_port = 0;
size_t g_rotation = 0;

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

void rmrf(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) { unlink(path.c_str()); return; }
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        rmrf(path + "/" + n);
    }
    closedir(d);
    rmdir(path.c_str());
}

// Engines are children of this process; after a crash/LMK kill they can be orphaned and keep
// ports bound. Kill any process whose argv[0] lives in our nativeLibraryDir.
void killOrphanEngines() {
    if (g_lib_dir.empty()) return;
    DIR* d = opendir("/proc");
    if (!d) return;
    const pid_t self = getpid();
    while (dirent* e = readdir(d)) {
        int pid = atoi(e->d_name);
        if (pid <= 1 || pid == self) continue;
        std::ifstream f(std::string("/proc/") + e->d_name + "/cmdline", std::ios::binary);
        std::string argv0;
        std::getline(f, argv0, '\0');
        if (argv0.rfind(g_lib_dir + "/", 0) == 0) {
            LOGI("killing orphan engine pid %d (%s)", pid, argv0.c_str());
            kill(pid, SIGKILL);
        }
    }
    closedir(d);
}

std::vector<std::string> candidateUris() {
    std::vector<std::string> out;
    auto* orch = hunter::gui::androidOrchestrator();
    if (orch) {
        for (auto& p : orch->lastGoodConfigs()) out.push_back(p.first);
    }
    if (out.empty()) {  // fall back to persisted gold tier
        std::ifstream f(hunter::gui::androidDataDir() + "/runtime/gold.txt");
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.find("://") != std::string::npos) out.push_back(line);
        }
    }
    return out;
}
}  // namespace

extern "C" {

// (String filesDir, String nativeLibDir, AssetManager am) -> boolean
JNIEXPORT jboolean JNICALL
Java_com_hunter_app_HunterNative_nativeInit(JNIEnv* env, jclass, jstring filesDir, jstring libDir,
                                            jobject am) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string files = jstr(env, filesDir);
    g_lib_dir = jstr(env, libDir);
    if (files.empty() || files[0] != '/' || g_lib_dir.empty()) return JNI_FALSE;

    hunter::gui::androidSetDataDir(files);
    mkdir((files + "/runtime").c_str(), 0700);
    mkdir((files + "/runtime/xray_tmp").c_str(), 0700);
    mkdir((files + "/bin").c_str(), 0700);
    // The shared core still has some CWD-relative paths ("runtime/...", "bin/..."); anchor the
    // CWD at filesDir so they resolve to the app's private dir instead of read-only "/".
    if (chdir(files.c_str()) != 0) LOGE("chdir(%s) failed", files.c_str());

    // Engines: exec from nativeLibraryDir only. Remove any legacy copies in filesDir.
    rmrf(files + "/engines");
    setenv("HUNTER_XRAY_PATH", (g_lib_dir + "/libxray.so").c_str(), 1);
    setenv("HUNTER_SINGBOX_PATH", (g_lib_dir + "/libsingbox.so").c_str(), 1);
    hunter::embed::setExtractionBaseDir(files + "/engines_unused");

    if (am) hunter::embed::setAndroidAssetManager(AAssetManager_fromJava(env, am));
    killOrphanEngines();
    return androidAppInit() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_hunter_app_HunterNative_nativeStopCore(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cur_port = 0;
    g_cur_uri.clear();
    androidAppShutdown();   // stops orchestrator + every engine child
    killOrphanEngines();
}

JNIEXPORT jboolean JNICALL Java_com_hunter_app_HunterNative_nativeIsCoreRunning(JNIEnv*, jclass) {
    return hunter::gui::androidCoreRunning() ? JNI_TRUE : JNI_FALSE;
}

// Start (or restart) a local SOCKS5 proxy on the best available config.
// `skip` > 0 rotates to later candidates (failover). Returns port or 0.
JNIEXPORT jint JNICALL Java_com_hunter_app_HunterNative_nativeStartSocks(JNIEnv*, jclass, jint skip) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* orch = hunter::gui::androidOrchestrator();
    if (!orch) return 0;
    auto& psm = orch->proxyServerManager();
    auto uris = candidateUris();
    if (uris.empty()) return 0;
    if (g_cur_port) { psm.stopAll(); g_cur_port = 0; g_cur_uri.clear(); }
    g_rotation = (skip > 0) ? g_rotation + (size_t)skip : 0;
    const size_t n = uris.size();
    for (size_t i = 0; i < n && i < 8; ++i) {
        const std::string& uri = uris[(g_rotation + i) % n];
        int port = psm.startProxy(uri);
        if (port > 0) {
            g_cur_uri = uri; g_cur_port = port;
            LOGI("socks up on 127.0.0.1:%d", port);
            return port;
        }
    }
    return 0;
}

JNIEXPORT void JNICALL Java_com_hunter_app_HunterNative_nativeStopSocks(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* orch = hunter::gui::androidOrchestrator();
    if (orch) orch->proxyServerManager().stopAll();
    g_cur_port = 0; g_cur_uri.clear();
}

JNIEXPORT jint JNICALL Java_com_hunter_app_HunterNative_nativeSocksPort(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cur_port;
}

}  // extern "C"
