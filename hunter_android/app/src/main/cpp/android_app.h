// Android app wrapper - replaces GLFW-based app for Android
// Provides the same renderFrame() logic but uses ImGui Android backend + GLES3
#pragma once

#include "core/config.h"
#include "orchestrator/orchestrator.h"
#include "gui/app.h"

namespace hunter {
namespace gui {

// Android-specific app class that wraps the same UI logic
class AndroidApp {
public:
    AndroidApp(HunterOrchestrator& orch);
    ~AndroidApp();

    // Called from JNI when GL surface is created
    void onSurfaceCreated();

    // Called from JNI when GL surface changes size
    void onSurfaceChanged(int width, int height);

    // Called from JNI each frame to render
    void onDrawFrame();

    // Touch event from JNI
    void onTouchEvent(int action, float x, float y);

    // Pause/resume
    void onPause();
    void onResume();

private:
    void renderFrame();

    HunterOrchestrator& orch_;
    bool initialized_ = false;
    int width_ = 1280;
    int height_ = 800;
};

// Core accessors (implemented in android_app.cpp; used by vpn_jni.cpp)
void androidSetDataDir(const std::string& dir);
const std::string& androidDataDir();
HunterOrchestrator* androidOrchestrator();
bool androidCoreRunning();

} // namespace gui
} // namespace hunter
