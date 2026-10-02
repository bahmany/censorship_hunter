#pragma once

//
// gl_fallback_embed.h — Software-OpenGL fallback for Windows machines with
// no working hardware OpenGL driver.
//
// Some real Windows machines (VMs with a 2D-only display adapter, some RDP
// sessions, very old GPUs) fail glfwCreateWindow() outright with a WGL error
// ("The driver does not appear to support OpenGL"). This module extracts a
// bundled Mesa opengl32.dll + libgallium_wgl.dll next to the running exe —
// implicit DLL imports resolve from the application directory before
// System32, so this transparently swaps in software rendering — and the
// caller relaunches the process once so the new import resolves from the
// start.
//
// Windows-only; on other platforms hasEmbeddedGlFallback() is always false.
//

#include <string>

namespace hunter {
namespace embed {

/// True if this Windows binary carries the embedded GL fallback payload.
bool hasEmbeddedGlFallback();

/// Extracts opengl32.dll + libgallium_wgl.dll into the same directory as the
/// running executable, if they are not already present there. Returns true
/// if both files exist there after the call (whether freshly extracted or
/// already present from an earlier run).
bool ensureGlFallbackExtracted();

/// True if the fallback DLLs are already sitting next to the exe — used to
/// decide whether a previous relaunch already happened, so a second failure
/// gives up instead of looping.
bool glFallbackAlreadyPresent();

} // namespace embed
} // namespace hunter
