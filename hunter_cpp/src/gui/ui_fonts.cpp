#include "gui/ui_fonts.h"

#include "imgui.h"

namespace {
#include "gui/fonts/noto_sans_regular.inc"
}  // namespace

namespace hunter {
namespace gui {

ImFont* loadUiFonts(float base_size_px) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    ImFont* font = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        noto_sans_regular_compressed_data_base85, base_size_px, &cfg);
    if (!font) {
        ImFontConfig fallback;
        fallback.SizePixels = base_size_px;
        font = io.Fonts->AddFontDefault(&fallback);
    }
    io.FontDefault = font;
    ImGui::GetStyle().FontSizeBase = base_size_px;
    return font;
}

namespace {

bool fontCovers(unsigned cp) {
    return (cp >= 0x20 && cp <= 0x7E) ||     // ASCII
           (cp >= 0xA0 && cp <= 0x24F) ||    // Latin-1, Latin Extended A/B
           (cp >= 0x370 && cp <= 0x3FF) ||   // Greek
           (cp >= 0x400 && cp <= 0x4FF) ||   // Cyrillic
           (cp >= 0x1E00 && cp <= 0x1EFF) || // Latin Extended Additional
           (cp >= 0x2010 && cp <= 0x205E) || // General punctuation
           (cp >= 0x20A0 && cp <= 0x20C0) || // Currency
           (cp >= 0x2100 && cp <= 0x214F);   // Letterlike symbols
}

}  // namespace

std::string displaySafeText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        if (i + len > text.size()) len = 1;
        unsigned cp = c;
        if (len == 2) cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
        else if (len == 3) cp = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
        else if (len == 4) cp = ((c & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) |
                                ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
        const bool space = cp == ' ' || cp == '\t' || cp == 0xA0;
        if (space) {
            pending_space = !out.empty();
        } else if (fontCovers(cp) && !(len == 1 && c >= 0x80)) {
            if (pending_space) out += ' ';
            pending_space = false;
            out.append(text, i, len);
        }
        i += len;
    }
    return out;
}

std::string truncateUtf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "...";
}

}  // namespace gui
}  // namespace hunter
