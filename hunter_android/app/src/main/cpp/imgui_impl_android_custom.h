// Minimal ImGui platform backend for Android (touch input via JNI)
#pragma once
#include "imgui.h"

// Initialize the Android platform backend
bool ImGui_ImplAndroid_Init();

// Shutdown
void ImGui_ImplAndroid_Shutdown();

// Call at the start of each frame
void ImGui_ImplAndroid_NewFrame();

// Touch event handling (called from JNI)
// action: 0=DOWN, 1=UP, 2=MOVE, 3=CANCEL
void ImGui_ImplAndroid_HandleTouch(int action, float x, float y);

// Display size update (called from JNI when surface changes)
void ImGui_ImplAndroid_SetDisplaySize(int width, int height);
