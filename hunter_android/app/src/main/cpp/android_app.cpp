// Android app wrapper - uses the same HunterGuiApp rendering logic
// but with ImGui Android backend + GLES3 instead of GLFW + OpenGL2
#include "android_app.h"
#include "imgui_impl_android_custom.h"
#include "imgui_impl_opengl3.h"
#include "gui/app.h"
#include "core/updater.h"
#include <android/log.h>
#include <GLES3/gl3.h>
#include <memory>

#define TAG "HunterApp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace hunter {
namespace gui {

// We reuse the desktop HunterGuiApp class but call its public methods
// from our Android wrapper. The renderFrame() method is backend-agnostic
// (pure ImGui calls), so it works identically on Android.

static std::unique_ptr<HunterConfig> g_config;
static std::unique_ptr<HunterOrchestrator> g_orchestrator;
static std::unique_ptr<HunterGuiApp> g_gui_app;
static bool g_im_initialized = false;

AndroidApp::AndroidApp(HunterOrchestrator& orch) : orch_(orch) {}

AndroidApp::~AndroidApp() {
    if (g_gui_app) {
        g_gui_app->stopOrchestrator();
    }
}

void AndroidApp::onSurfaceCreated() {
    LOGI("onSurfaceCreated");

    if (!g_im_initialized) {
        // Initialize ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        // Scale font for mobile display
        ImFontConfig font_cfg;
        font_cfg.SizePixels = 32.0f;  // Larger font for touch screens
        io.Fonts->AddFontDefault(&font_cfg);

        // Initialize backends
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");

        g_im_initialized = true;
        LOGI("ImGui initialized with GLES3");
    }
}

void AndroidApp::onSurfaceChanged(int width, int height) {
    LOGI("onSurfaceChanged: %dx%d", width, height);
    width_ = width;
    height_ = height;
    ImGui_ImplAndroid_SetDisplaySize(width, height);
}

void AndroidApp::onDrawFrame() {
    if (!g_im_initialized) return;

    // Start new frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    // Render the same UI as desktop
    renderFrame();

    // Render ImGui draw data
    ImGui::Render();
    glViewport(0, 0, width_, height_);
    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void AndroidApp::onTouchEvent(int action, float x, float y) {
    ImGui_ImplAndroid_HandleTouch(action, x, y);
}

void AndroidApp::onPause() {
    LOGI("onPause");
}

void AndroidApp::onResume() {
    LOGI("onResume");
}

void AndroidApp::renderFrame() {
    if (g_gui_app) {
        g_gui_app->pollProxyServers();
        g_gui_app->renderFrame();
    }
}

// ─── Global initialization functions called from JNI ───

bool androidAppInitImpl() {
    if (g_orchestrator) return true;  // already initialized

    LOGI("Initializing Hunter app...");

    // Create config and orchestrator
    g_config = std::make_unique<HunterConfig>();
    g_config->loadFromFile("runtime/hunter_config.json");

    g_orchestrator = std::make_unique<HunterOrchestrator>(*g_config);
    g_gui_app = std::make_unique<HunterGuiApp>(*g_orchestrator);

    // Start the orchestrator on background thread
    g_gui_app->startOrchestrator();

    // Start auto-update checker (background thread, fully automatic)
    hunter::core::SelfUpdateManager::instance().startAutoCheck(21600, 15);

    LOGI("Hunter app initialized");
    return true;
}

void androidAppShutdownImpl() {
    LOGI("Shutting down Hunter app...");

    hunter::core::SelfUpdateManager::instance().stopAutoCheck();

    if (g_gui_app) {
        g_gui_app->stopOrchestrator();
        g_gui_app.reset();
    }
    g_orchestrator.reset();
    g_config.reset();

    if (g_im_initialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
        g_im_initialized = false;
    }

    LOGI("Hunter app shut down");
}

void androidRenderFrameImpl() {
    if (g_gui_app) {
        g_gui_app->pollProxyServers();
        g_gui_app->renderFrame();
    }
}

} // namespace gui
} // namespace hunter

// ─── extern "C" functions for JNI bridge (outside namespace) ───
extern "C" bool androidAppInit() {
    return hunter::gui::androidAppInitImpl();
}

extern "C" void androidAppShutdown() {
    hunter::gui::androidAppShutdownImpl();
}

extern "C" void androidRenderFrame() {
    hunter::gui::androidRenderFrameImpl();
}
