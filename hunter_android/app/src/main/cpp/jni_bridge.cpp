// JNI bridge - connects Java MainActivity/HunterView to native C++ code
#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <GLES3/gl3.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android_custom.h"
#include "android_app.h"
#include "core/engine_embed.h"
#include "core/config_embed.h"
#include <string>

#define TAG "HunterJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Android-only entry point implemented in config_embed_android.cpp.
namespace hunter {
namespace embed {
void setAndroidAssetManager(AAssetManager* am);
}
}  // namespace hunter

// Forward declarations for functions defined in android_app.cpp
extern "C" bool androidAppInit();
extern "C" void androidAppShutdown();
extern "C" void androidUiShutdown();
extern "C" void androidRenderFrame();

static bool g_gl_inited = false;

extern "C" {

// ─── MainActivity native methods ───

JNIEXPORT void JNICALL
Java_com_hunter_app_MainActivity_nativeSetAssetManager(JNIEnv* env, jclass cls, jobject assetManager) {
    AAssetManager* am = AAssetManager_fromJava(env, assetManager);
    LOGI("nativeSetAssetManager: %p", (void*)am);
    hunter::embed::setAndroidAssetManager(am);
}

JNIEXPORT void JNICALL
Java_com_hunter_app_MainActivity_nativeInit(JNIEnv* env, jclass cls) {
    LOGI("nativeInit");
    androidAppInit();  // idempotent; no-op if the service already started the core
}

JNIEXPORT void JNICALL
Java_com_hunter_app_MainActivity_nativeShutdown(JNIEnv* env, jclass cls) {
    // UI teardown only; the core (orchestrator + engines) is owned by HunterVpnService /
    // HunterNative and is stopped via HunterNative.stopCore().
    LOGI("nativeShutdown (UI)");
    if (g_gl_inited) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        g_gl_inited = false;
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

JNIEXPORT void JNICALL
Java_com_hunter_app_MainActivity_nativePause(JNIEnv* env, jclass cls) {
    LOGI("nativePause");
}

JNIEXPORT void JNICALL
Java_com_hunter_app_MainActivity_nativeResume(JNIEnv* env, jclass cls) {
    LOGI("nativeResume");
}

JNIEXPORT void JNICALL
Java_com_hunter_app_MainActivity_nativeTouchEvent(JNIEnv* env, jclass cls,
                                                    jint action, jfloat x, jfloat y) {
    ImGui_ImplAndroid_HandleTouch(action, x, y);
}

JNIEXPORT void JNICALL
Java_com_hunter_app_MainActivity_nativeKeyEvent(JNIEnv* env, jclass cls,
                                                  jint action, jint keyCode) {
    // Keyboard input not critical for touch UI
}

// ─── HunterView.Renderer native methods ───

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterView_00024Renderer_nativeOnSurfaceCreated(JNIEnv* env, jobject thiz) {
    LOGI("nativeOnSurfaceCreated");

    // Initialize ImGui if not already done
    if (!ImGui::GetCurrentContext()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        // Larger font for mobile
        ImFontConfig font_cfg;
        font_cfg.SizePixels = 32.0f;
        io.Fonts->AddFontDefault(&font_cfg);
    }

    // Surface re-created (EGL context preserved/lost): drop old GL objects first to avoid leaks.
    if (g_gl_inited) ImGui_ImplOpenGL3_Shutdown();
    else ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_gl_inited = true;
}

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterView_00024Renderer_nativeOnSurfaceChanged(JNIEnv* env, jobject thiz,
                                                                      jint width, jint height) {
    LOGI("nativeOnSurfaceChanged: %dx%d", width, height);
    ImGui_ImplAndroid_SetDisplaySize(width, height);
}

JNIEXPORT void JNICALL
Java_com_hunter_app_HunterView_00024Renderer_nativeOnDrawFrame(JNIEnv* env, jobject thiz) {
    // Ensure core is up (idempotent; fails harmlessly until the data dir is set).
    androidAppInit();

    // New frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    // Render the hunter UI (same as desktop)
    androidRenderFrame();

    ImGui::Render();

    // Get display size
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // extern "C"
