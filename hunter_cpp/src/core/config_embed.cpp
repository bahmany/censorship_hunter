#include "core/config_embed.h"
#include "core/utils.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

// Vendored zstd decoder (amalgamated, self-contained).
#include "zstd.h"

// Build-time generated header. Defines HUNTER_EMBED_HAS_CONFIGS and, when 1:
//   extern const unsigned char kConfigsZst[];
//   extern const unsigned long kConfigsZstLen;
//   static const unsigned long kConfigsOrigLen;
#include "config_embedded.h"

namespace hunter {
namespace embed {

namespace {

struct ConfigState {
    bool decompressed = false;
    std::vector<std::string> configs;
};

ConfigState& cfgState() {
    static ConfigState s;
    return s;
}

} // namespace

bool hasEmbeddedConfigs() {
#if defined(HUNTER_EMBED_HAS_CONFIGS) && HUNTER_EMBED_HAS_CONFIGS
    return true;
#else
    return false;
#endif
}

const std::vector<std::string>& configs() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (!hasEmbeddedConfigs()) {
            cfgState().decompressed = true;
            return;
        }
#if defined(HUNTER_EMBED_HAS_CONFIGS) && HUNTER_EMBED_HAS_CONFIGS
        utils::LogRingBuffer::instance().push(
            "[ConfigEmbed] Decompressing embedded config bundle (" +
            std::to_string(kConfigsZstLen) + " compressed -> " +
            std::to_string(kConfigsOrigLen) + " bytes)");

        std::vector<uint8_t> buf(kConfigsOrigLen);
        size_t r = ZSTD_decompress(buf.data(), kConfigsOrigLen, kConfigsZst, (size_t)kConfigsZstLen);
        if (ZSTD_isError(r)) {
            utils::LogRingBuffer::instance().push(
                std::string("[ConfigEmbed] zstd decompress failed: ") +
                ZSTD_getErrorName(r));
            cfgState().decompressed = true;
            return;
        }
        if (r != kConfigsOrigLen) {
            utils::LogRingBuffer::instance().push(
                "[ConfigEmbed] size mismatch: got " + std::to_string(r) +
                " expected " + std::to_string(kConfigsOrigLen));
            cfgState().decompressed = true;
            return;
        }

        // Parse lines: one URI per line, skip blanks and comments.
        std::vector<std::string> out;
        out.reserve(65536);
        std::string text(reinterpret_cast<const char*>(buf.data()), r);
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            // Trim whitespace.
            size_t a = line.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            size_t b = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(a, b - a + 1);
            if (trimmed.empty() || trimmed[0] == '#') continue;
            if (trimmed.find("://") == std::string::npos) continue;
            out.push_back(trimmed);
        }

        utils::LogRingBuffer::instance().push(
            "[ConfigEmbed] Loaded " + std::to_string(out.size()) +
            " embedded configs");
        cfgState().configs = std::move(out);
        cfgState().decompressed = true;
#endif
    });
    return cfgState().configs;
}

size_t configCount() {
    return configs().size();
}

} // namespace embed
} // namespace hunter
