#pragma once

#include <cstddef>
#include <string>

struct ImFont;

namespace hunter {
namespace gui {

// Loads the embedded UI font (Noto Sans Regular, OFL-1.1) as the default font
// at `base_size_px` design pixels. Must run after ImGui::CreateContext() and
// before the first NewFrame(). Falls back to ImGui's built-in font if the
// embedded data cannot be decoded. DPI scaling is applied separately through
// ImGuiStyle::FontScaleDpi, so glyphs are rasterized at the real on-screen
// size (imgui >= 1.92 dynamic fonts) instead of being stretched.
ImFont* loadUiFonts(float base_size_px);

// Returns `text` with only the code points the embedded font can draw
// (Latin, Greek, Cyrillic, common punctuation/symbols). Emoji, flag pairs and
// unsupported scripts are dropped instead of rendering as tofu boxes;
// whitespace runs are collapsed and the result trimmed. Thread-safe.
std::string displaySafeText(const std::string& text);

// Truncates UTF-8 `text` to at most `max_bytes` bytes plus "..." without
// splitting a multi-byte sequence.
std::string truncateUtf8(const std::string& text, size_t max_bytes);

}  // namespace gui
}  // namespace hunter
