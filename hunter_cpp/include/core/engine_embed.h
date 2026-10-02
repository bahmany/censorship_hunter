#pragma once

//
// engine_embed.h — Embedded proxy-engine (xray / sing-box) extraction.
//
// The Hunter single-file build compresses the xray and sing-box binaries
// with zstd and bakes them into the executable at link time (see
// tools/embed_engines.py + the generated engine_embedded.h). At runtime
// this module decompresses them into a per-user cache directory, verifies
// their SHA-256, marks them executable, and hands the resolved paths to
// XRayManager / ProxyServerManager.
//
// When the binary was built WITHOUT embedded engines (e.g. a dev build
// where bin/xray was absent), hasEmbeddedEngines() returns false and the
// path accessors return empty strings — callers fall back to the classic
// bin/xray, bin/sing-box lookup.
//

#include <string>

namespace hunter {
namespace embed {

/// True if this binary carries embedded engine payloads.
bool hasEmbeddedEngines();

/// Decompress + write embedded engines to the cache dir if not already
/// present and valid. Returns true when all embedded engines are usable.
/// Safe to call repeatedly; skips work when the cached copy is intact.
bool ensureExtracted();

/// Absolute path to the extracted xray binary, or "" if unavailable.
std::string xrayPath();

/// Absolute path to the extracted sing-box binary, or "" if unavailable.
std::string singBoxPath();

/// Override the base directory used for extraction.
/// On Android the JNI layer calls this with the app's filesDir so engines
/// land in a writable, app-private location. On desktop this is derived
/// from XDG_CACHE_HOME / LOCALAPPDATA and normally left unset.
void setExtractionBaseDir(const std::string& dir);

/// The directory engines are extracted into (for diagnostics / cleanup).
std::string extractionDir();

} // namespace embed
} // namespace hunter
