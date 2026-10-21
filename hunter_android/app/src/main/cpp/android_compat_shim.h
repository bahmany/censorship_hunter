// Force-included ONLY for hunter_cpp/src/gui/app.cpp on Android.
// Works around desktop-only code in the shared GUI (hunter_cpp is owned by another stage):
//  * setWindowIcon() uses GLFW types that do not exist on Android.
//  * imgui version pinned by the submodule renamed AllowItemOverlap -> AllowOverlap.
// Remove once hunter_cpp guards these with #ifndef __ANDROID__ / the flag fix lands.
#pragma once
#ifdef __ANDROID__
#include "imgui.h"   // for IMGUI_VERSION_NUM + GLFW-independent decls
struct GLFWwindow;
struct GLFWimage { int width; int height; unsigned char* pixels; };
inline void glfwSetWindowIcon(GLFWwindow*, int, const GLFWimage*) {}
// Only needed on imgui < 1.89.7 (where AllowItemOverlap is the sole name).
// On >= 1.89.7 the enum carries a backward-compat alias and the macro would
// redefine the enumerator inside imgui.h itself.
#if IMGUI_VERSION_NUM < 18970
#define ImGuiSelectableFlags_AllowItemOverlap ImGuiSelectableFlags_AllowOverlap
#endif
#endif
