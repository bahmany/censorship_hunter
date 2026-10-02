// Force-included ONLY for hunter_cpp/src/gui/app.cpp on Android.
// Works around desktop-only code in the shared GUI (hunter_cpp is owned by another stage):
//  * setWindowIcon() uses GLFW types that do not exist on Android.
//  * imgui version pinned by the submodule renamed AllowItemOverlap -> AllowOverlap.
// Remove once hunter_cpp guards these with #ifndef __ANDROID__ / the flag fix lands.
#pragma once
#ifdef __ANDROID__
struct GLFWwindow;
struct GLFWimage { int width; int height; unsigned char* pixels; };
inline void glfwSetWindowIcon(GLFWwindow*, int, const GLFWimage*) {}
#define ImGuiSelectableFlags_AllowItemOverlap ImGuiSelectableFlags_AllowOverlap
#endif
