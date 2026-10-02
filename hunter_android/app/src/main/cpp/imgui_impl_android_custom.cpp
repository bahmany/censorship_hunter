// Minimal ImGui platform backend for Android (touch input via JNI)
#include "imgui_impl_android_custom.h"
#include <imgui.h>
#include <android/log.h>
#include <cmath>

#define TAG "HunterImGui"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static struct {
    float display_width = 0;
    float display_height = 0;
    float touch_x = 0;
    float touch_y = 0;
    bool mouse_down = false;
    bool initialized = false;
} g_android;

bool ImGui_ImplAndroid_Init() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "imgui_impl_android_custom";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

    // Enable touch input
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;

    g_android.initialized = true;
    LOGI("ImGui Android platform backend initialized");
    return true;
}

void ImGui_ImplAndroid_Shutdown() {
    g_android.initialized = false;
}

void ImGui_ImplAndroid_NewFrame() {
    if (!g_android.initialized) return;

    ImGuiIO& io = ImGui::GetIO();

    // Set display size
    if (g_android.display_width > 0 && g_android.display_height > 0) {
        io.DisplaySize = ImVec2(g_android.display_width, g_android.display_height);
    } else {
        io.DisplaySize = ImVec2(1280, 800); // fallback
    }

    // Set display framebuffer scale (1.0 for now, could use density)
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // Update mouse position from touch
    if (g_android.mouse_down) {
        io.AddMousePosEvent(g_android.touch_x, g_android.touch_y);
    } else {
        io.AddMousePosEvent(-1.0f, -1.0f); // clear mouse position
    }

    // Update mouse button state (button 0 = left)
    io.AddMouseButtonEvent(0, g_android.mouse_down);
}

void ImGui_ImplAndroid_HandleTouch(int action, float x, float y) {
    g_android.touch_x = x;
    g_android.touch_y = y;

    switch (action) {
        case 0: // ACTION_DOWN
        case 5: // ACTION_POINTER_DOWN
            g_android.mouse_down = true;
            break;
        case 1: // ACTION_UP
        case 6: // ACTION_POINTER_UP
            g_android.mouse_down = false;
            break;
        case 2: // ACTION_MOVE
            // mouse_down stays as-is
            break;
        case 3: // ACTION_CANCEL
            g_android.mouse_down = false;
            break;
    }
}

void ImGui_ImplAndroid_SetDisplaySize(int width, int height) {
    g_android.display_width = (float)width;
    g_android.display_height = (float)height;
    LOGI("Display size set to %dx%d", width, height);
}
