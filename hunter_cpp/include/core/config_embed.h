#pragma once

//
// config_embed.h — Embedded proxy-config bundle extraction.
//
// The Hunter single-file build downloads the latest proxy configs (VLESS,
// VMess, Trojan, SS, Hysteria2, TUIC, ...) from 25+ public GitHub sources
// at BUILD TIME, compresses the deduplicated bundle with zstd, and bakes it
// into the executable via .incbin (see tools/download_configs.py +
// tools/embed_configs.py + the generated config_embedded.h).
//
// At runtime this module decompresses the bundle and exposes the configs as
// a vector of URI strings, which the orchestrator loads into the in-memory
// ConfigDatabase on startup — BEFORE any network scraping begins — so the
// user has immediate proxy candidates on first launch.
//
// When the binary was built WITHOUT embedded configs, hasEmbeddedConfigs()
// returns false and the bundle is empty — callers fall back to live scraping.
//

#include <string>
#include <vector>

namespace hunter {
namespace embed {

/// True if this binary carries an embedded config bundle.
bool hasEmbeddedConfigs();

/// Decompress the embedded config bundle and return all config URIs.
/// Safe to call repeatedly; the decompression is cached after the first call.
/// Returns an empty vector if no embedded configs are present.
const std::vector<std::string>& configs();

/// Number of embedded configs (0 if none).
size_t configCount();

} // namespace embed
} // namespace hunter
