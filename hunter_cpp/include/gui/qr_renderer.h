#pragma once

#include <string>
#include <vector>

#include "imgui.h"
#include "qrcodegen.hpp"

namespace hunter {
namespace gui {

/**
 * @brief Generate a QR code from text and render it with ImGui draw calls.
 *
 * Uses the Nayuki QR-Code-generator library to produce the module matrix,
 * then draws each dark module as a filled rectangle via ImGui::GetWindowDrawList().
 * No OpenGL textures are involved — this keeps it simple and avoids texture
 * lifecycle management. The QR is drawn with a quiet zone (4-module margin)
 * as required by the QR spec for reliable scanning.
 *
 * @param text         The string to encode (e.g. a v2ray URI).
 * @param origin       Top-left screen position for the QR drawing.
 * @param total_size   Total pixel size of the rendered QR (including quiet zone).
 * @param out_drawn    If non-null, receives the pixel size actually drawn, which
 *                     can exceed total_size because modules are clamped to a
 *                     minimum size. Callers reserve layout space with this.
 * @return true if the QR was generated and drawn successfully.
 */
inline bool renderQrCode(const std::string& text, ImVec2 origin, float total_size,
                         float* out_drawn = nullptr) {
    if (out_drawn) *out_drawn = 0.0f;
    if (text.empty()) return false;

    // encodeText throws data_too_long when the payload does not fit the chosen
    // ECC level. A long vless:// URI (big remark, long path) does exactly that,
    // and an uncaught throw here runs straight through the render loop and kills
    // the app — so try MEDIUM, fall back to LOW, and give up quietly after that.
    auto encode = [&](qrcodegen::QrCode::Ecc ecc, qrcodegen::QrCode& out) {
        try {
            out = qrcodegen::QrCode::encodeText(text.c_str(), ecc);
            return true;
        } catch (...) {
            return false;
        }
    };

    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText("", qrcodegen::QrCode::Ecc::LOW);
    if (!encode(qrcodegen::QrCode::Ecc::MEDIUM, qr) &&
        !encode(qrcodegen::QrCode::Ecc::LOW, qr)) {
        return false;
    }

    int size = qr.getSize();            // modules per side (not counting quiet zone)
    int quiet = 4;                      // mandatory quiet-zone margin
    int total_modules = size + 2 * quiet;

    float module_size = total_size / static_cast<float>(total_modules);
    if (module_size < 2.0f) module_size = 2.0f; // minimum visible size
    if (out_drawn) *out_drawn = total_modules * module_size;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg_color = IM_COL32(255, 255, 255, 255);
    ImU32 fg_color = IM_COL32(0, 0, 0, 255);

    // Draw white background (full QR area including quiet zone).
    dl->AddRectFilled(origin,
                      ImVec2(origin.x + total_modules * module_size,
                             origin.y + total_modules * module_size),
                      bg_color);

    // Draw dark modules.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (qr.getModule(x, y)) {
                float px = origin.x + (x + quiet) * module_size;
                float py = origin.y + (y + quiet) * module_size;
                dl->AddRectFilled(ImVec2(px, py),
                                  ImVec2(px + module_size, py + module_size),
                                  fg_color);
            }
        }
    }
    return true;
}

/**
 * @brief Get the recommended total pixel size for a QR encoding the given text.
 *
 * Returns a size that keeps each module at least 4px (scannable by phone
 * cameras) while staying within a reasonable max (e.g. 300px).
 */
inline float recommendedQrSize(const std::string& text, float max_size = 280.0f) {
    if (text.empty()) return max_size;
    // Mirror renderQrCode's MEDIUM-then-LOW fallback so the size we reserve
    // matches the QR that actually gets drawn.
    for (auto ecc : {qrcodegen::QrCode::Ecc::MEDIUM, qrcodegen::QrCode::Ecc::LOW}) {
        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), ecc);
            int total_modules = qr.getSize() + 8; // +8 for quiet zone (4 each side)
            float size = total_modules * 4.0f;    // 4px per module minimum
            if (size > max_size) size = max_size;
            if (size < 100.0f) size = 100.0f;
            return size;
        } catch (...) {
            continue;
        }
    }
    return max_size;
}

} // namespace gui
} // namespace hunter
