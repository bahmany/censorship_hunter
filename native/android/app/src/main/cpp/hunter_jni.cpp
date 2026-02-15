#include <jni.h>
#include <string>
#include <memory>
#include <android/log.h>

#include "core/config.h"
#include "core/utils.h"
#include "orchestrator.h"

#define LOG_TAG "HunterJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Global instances (owned by native layer)
std::unique_ptr<hunter::HunterConfig> g_config;
std::unique_ptr<hunter::HunterOrchestrator> g_orchestrator;

// Cached JVM reference for callbacks from native threads
JavaVM* g_jvm = nullptr;

// Global refs to Java callback objects
jobject g_callback_obj = nullptr;
jclass g_callback_class = nullptr;

JNIEnv* get_env() {
    JNIEnv* env = nullptr;
    if (g_jvm) {
        int status = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            g_jvm->AttachCurrentThread(&env, nullptr);
        }
    }
    return env;
}

std::string jstring_to_string(JNIEnv* env, jstring jstr) {
    if (!jstr) return "";
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return result;
}

jstring string_to_jstring(JNIEnv* env, const std::string& str) {
    return env->NewStringUTF(str.c_str());
}

// HTTP callback: calls Java HunterNative.httpFetch(url, userAgent, timeout, proxy)
std::string jni_http_fetch(const std::string& url, const std::string& user_agent,
                           int timeout, const std::string& proxy) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return "";

    jmethodID method = env->GetMethodID(g_callback_class, "httpFetch",
        "(Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;)Ljava/lang/String;");
    if (!method) return "";

    jstring j_url = string_to_jstring(env, url);
    jstring j_ua = string_to_jstring(env, user_agent);
    jstring j_proxy = string_to_jstring(env, proxy);

    auto result = (jstring)env->CallObjectMethod(g_callback_obj, method,
                                                  j_url, j_ua, timeout, j_proxy);

    env->DeleteLocalRef(j_url);
    env->DeleteLocalRef(j_ua);
    env->DeleteLocalRef(j_proxy);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return "";
    }

    std::string str = jstring_to_string(env, result);
    if (result) env->DeleteLocalRef(result);
    return str;
}

// Start proxy callback: calls Java HunterNative.startProxy(configJson, socksPort)
int jni_start_proxy(const std::string& config_json, int socks_port) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return -1;

    jmethodID method = env->GetMethodID(g_callback_class, "startProxy",
        "(Ljava/lang/String;I)I");
    if (!method) return -1;

    jstring j_config = string_to_jstring(env, config_json);
    int handle = env->CallIntMethod(g_callback_obj, method, j_config, socks_port);

    env->DeleteLocalRef(j_config);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return -1;
    }
    return handle;
}

// Stop proxy callback
void jni_stop_proxy(int handle_id) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return;

    jmethodID method = env->GetMethodID(g_callback_class, "stopProxy", "(I)V");
    if (!method) return;

    env->CallVoidMethod(g_callback_obj, method, handle_id);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

// Test URL callback: calls Java HunterNative.testUrl(url, socksPort, timeout)
// Returns {statusCode, latencyMs}
std::pair<int, double> jni_test_url(const std::string& url, int socks_port, int timeout) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return {0, 0.0};

    jmethodID method = env->GetMethodID(g_callback_class, "testUrl",
        "(Ljava/lang/String;II)[D");
    if (!method) return {0, 0.0};

    jstring j_url = string_to_jstring(env, url);
    auto result = (jdoubleArray)env->CallObjectMethod(g_callback_obj, method,
                                                       j_url, socks_port, timeout);
    env->DeleteLocalRef(j_url);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return {0, 0.0};
    }

    if (!result) return {0, 0.0};

    jdouble* elements = env->GetDoubleArrayElements(result, nullptr);
    int status_code = static_cast<int>(elements[0]);
    double latency = elements[1];
    env->ReleaseDoubleArrayElements(result, elements, 0);
    env->DeleteLocalRef(result);

    return {status_code, latency};
}

// Telegram fetch callback
std::vector<std::string> jni_telegram_fetch(const std::string& channel, int limit) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return {};

    jmethodID method = env->GetMethodID(g_callback_class, "telegramFetch",
        "(Ljava/lang/String;I)[Ljava/lang/String;");
    if (!method) return {};

    jstring j_channel = string_to_jstring(env, channel);
    auto result = (jobjectArray)env->CallObjectMethod(g_callback_obj, method, j_channel, limit);
    env->DeleteLocalRef(j_channel);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return {};
    }

    if (!result) return {};

    std::vector<std::string> messages;
    int len = env->GetArrayLength(result);
    for (int i = 0; i < len; ++i) {
        auto jstr = (jstring)env->GetObjectArrayElement(result, i);
        if (jstr) {
            messages.push_back(jstring_to_string(env, jstr));
            env->DeleteLocalRef(jstr);
        }
    }
    env->DeleteLocalRef(result);
    return messages;
}

// Telegram send callback
bool jni_telegram_send(const std::string& text) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return false;

    jmethodID method = env->GetMethodID(g_callback_class, "telegramSend",
        "(Ljava/lang/String;)Z");
    if (!method) return false;

    jstring j_text = string_to_jstring(env, text);
    bool ok = env->CallBooleanMethod(g_callback_obj, method, j_text);
    env->DeleteLocalRef(j_text);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return ok;
}

// Telegram send file callback
bool jni_telegram_send_file(const std::string& filename, const std::string& content,
                            const std::string& caption) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return false;

    jmethodID method = env->GetMethodID(g_callback_class, "telegramSendFile",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
    if (!method) return false;

    jstring j_fn = string_to_jstring(env, filename);
    jstring j_ct = string_to_jstring(env, content);
    jstring j_cap = string_to_jstring(env, caption);
    bool ok = env->CallBooleanMethod(g_callback_obj, method, j_fn, j_ct, j_cap);
    env->DeleteLocalRef(j_fn);
    env->DeleteLocalRef(j_ct);
    env->DeleteLocalRef(j_cap);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return ok;
}

// Progress callback
void jni_progress(const std::string& phase, int current, int total) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return;

    jmethodID method = env->GetMethodID(g_callback_class, "onProgress",
        "(Ljava/lang/String;II)V");
    if (!method) return;

    jstring j_phase = string_to_jstring(env, phase);
    env->CallVoidMethod(g_callback_obj, method, j_phase, current, total);
    env->DeleteLocalRef(j_phase);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

// Status callback
void jni_status(const std::string& status_json) {
    JNIEnv* env = get_env();
    if (!env || !g_callback_obj || !g_callback_class) return;

    jmethodID method = env->GetMethodID(g_callback_class, "onStatusUpdate",
        "(Ljava/lang/String;)V");
    if (!method) return;

    jstring j_status = string_to_jstring(env, status_json);
    env->CallVoidMethod(g_callback_obj, method, j_status);
    env->DeleteLocalRef(j_status);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

} // anonymous namespace

// ---------- JNI exported functions ----------

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("Hunter native library loaded");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    JNIEnv* env = get_env();
    if (env) {
        if (g_callback_obj) {
            env->DeleteGlobalRef(g_callback_obj);
            g_callback_obj = nullptr;
        }
        if (g_callback_class) {
            env->DeleteGlobalRef(g_callback_class);
            g_callback_class = nullptr;
        }
    }
    g_jvm = nullptr;
}

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterNative_nativeInit(JNIEnv* env, jobject thiz,
                                             jstring filesDir, jstring secretsFile,
                                             jobject callback) {
    std::string files_dir = jstring_to_string(env, filesDir);
    std::string secrets = jstring_to_string(env, secretsFile);

    LOGI("Initializing Hunter native: filesDir=%s", files_dir.c_str());

    // Store callback reference
    if (g_callback_obj) {
        env->DeleteGlobalRef(g_callback_obj);
    }
    if (g_callback_class) {
        env->DeleteGlobalRef(g_callback_class);
    }
    g_callback_obj = nullptr;
    g_callback_class = nullptr;
    if (callback) {
        g_callback_obj = env->NewGlobalRef(callback);
        jclass local_cb_class = env->GetObjectClass(callback);
        if (local_cb_class) {
            g_callback_class = (jclass)env->NewGlobalRef(local_cb_class);
            env->DeleteLocalRef(local_cb_class);
        }
    }

    // Create config
    g_config = std::make_unique<hunter::HunterConfig>(secrets);
    g_config->set_files_dir(files_dir);

    // Ensure runtime directory
    hunter::ensure_directory(files_dir + "/runtime");

    // Create orchestrator
    g_orchestrator = std::make_unique<hunter::HunterOrchestrator>(*g_config);

    // Push Telegram reporting credentials into the Java callback implementation (if supported).
    // This is required for telegramSend/telegramSendFile to actually send via Bot API.
    if (callback && g_callback_class) {
        const std::string bot_token = g_config->get_string("bot_token");
        const std::string configured_chat_id = g_config->get_string("chat_id");
        const long long report_channel = g_config->get_int("report_channel", 0);
        const std::string fallback_chat_id = (report_channel != 0) ? std::to_string(report_channel) : std::string();
        const std::string chat_id = !configured_chat_id.empty() ? configured_chat_id : fallback_chat_id;
        const std::string xray_path = g_config->get_string("xray_path");

        jmethodID set_bot_token = env->GetMethodID(g_callback_class, "setBotToken", "(Ljava/lang/String;)V");
        jmethodID set_chat_id = env->GetMethodID(g_callback_class, "setChatId", "(Ljava/lang/String;)V");
        jmethodID set_xray_path = env->GetMethodID(g_callback_class, "setXrayBinaryPath", "(Ljava/lang/String;)V");

        if (set_bot_token) {
            jstring j_token = string_to_jstring(env, bot_token);
            env->CallVoidMethod(callback, set_bot_token, j_token);
            env->DeleteLocalRef(j_token);
        }
        if (set_chat_id) {
            jstring j_chat = string_to_jstring(env, chat_id);
            env->CallVoidMethod(callback, set_chat_id, j_chat);
            env->DeleteLocalRef(j_chat);
        }
        if (set_xray_path) {
            jstring j_xray = string_to_jstring(env, xray_path);
            env->CallVoidMethod(callback, set_xray_path, j_xray);
            env->DeleteLocalRef(j_xray);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    // Wire up all JNI callbacks
    g_orchestrator->set_http_callback(jni_http_fetch);
    g_orchestrator->set_start_proxy_callback(jni_start_proxy);
    g_orchestrator->set_stop_proxy_callback(jni_stop_proxy);
    g_orchestrator->set_test_url_callback(jni_test_url);
    g_orchestrator->set_telegram_fetch_callback(jni_telegram_fetch);
    g_orchestrator->set_telegram_send_callback(jni_telegram_send);
    g_orchestrator->set_telegram_send_file_callback(jni_telegram_send_file);
    g_orchestrator->set_progress_callback(jni_progress);
    g_orchestrator->set_status_callback(jni_status);

    LOGI("Hunter native initialized");
}

JNIEXPORT jstring JNICALL
Java_com_hunter_app_HunterNative_nativeValidateConfig(JNIEnv* env, jobject thiz) {
    if (!g_config) {
        return string_to_jstring(env, "[]");
    }
    try {
        auto errors = g_config->validate();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : errors) {
            arr.push_back(e);
        }
        return string_to_jstring(env, arr.dump());
    } catch (...) {
        return string_to_jstring(env, "[]");
    }
}

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterNative_nativeStart(JNIEnv* env, jobject thiz) {
    if (g_orchestrator) {
        g_orchestrator->start();
    }
}

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterNative_nativeStop(JNIEnv* env, jobject thiz) {
    if (g_orchestrator) {
        g_orchestrator->stop();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_hunter_app_HunterNative_nativeIsRunning(JNIEnv* env, jobject thiz) {
    return g_orchestrator ? g_orchestrator->is_running() : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_hunter_app_HunterNative_nativeGetStatus(JNIEnv* env, jobject thiz) {
    if (!g_orchestrator) {
        return string_to_jstring(env, "{}");
    }
    return string_to_jstring(env, g_orchestrator->get_status().dump());
}

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterNative_nativeRunCycle(JNIEnv* env, jobject thiz) {
    if (g_orchestrator) {
        g_orchestrator->run_cycle();
    }
}

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterNative_nativeSetConfig(JNIEnv* env, jobject thiz,
                                                   jstring key, jstring value) {
    if (!g_config) return;
    std::string k = jstring_to_string(env, key);
    std::string v = jstring_to_string(env, value);
    g_config->set_env(k, v);
    LOGI("Config set: %s", k.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_hunter_app_HunterNative_nativeGetConfig(JNIEnv* env, jobject thiz, jstring key) {
    if (!g_config) {
        return string_to_jstring(env, "");
    }
    std::string k = jstring_to_string(env, key);
    return string_to_jstring(env, g_config->get_string(k));
}

JNIEXPORT jstring JNICALL
Java_com_hunter_app_HunterNative_nativeGetConfigs(JNIEnv* env, jobject thiz) {
    // Get configs
    if (g_orchestrator) {
        std::string configs = g_orchestrator->get_cached_configs();
        return string_to_jstring(env, configs);
    } else {
        return string_to_jstring(env, "[]");
    }
}

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterNative_nativeDestroy(JNIEnv* env, jobject thiz) {
    if (g_orchestrator) {
        g_orchestrator->stop();
        g_orchestrator.reset();
    }
    g_config.reset();

    if (g_callback_obj) {
        env->DeleteGlobalRef(g_callback_obj);
        g_callback_obj = nullptr;
    }
    if (g_callback_class) {
        env->DeleteGlobalRef(g_callback_class);
        g_callback_class = nullptr;
    }

    LOGI("Hunter native destroyed");
}

} // extern "C"
