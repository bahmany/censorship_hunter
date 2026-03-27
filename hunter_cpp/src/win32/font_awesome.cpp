#include "win32/font_awesome.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>

namespace hunter {
namespace win32 {

bool FontAwesome::font_loaded_ = false;
std::string FontAwesome::font_data_;

bool FontAwesome::LoadFont() {
    if (font_loaded_) {
        return true;
    }
    
    try {
        const std::string candidates[] = {
            "src/win32/fonts/fa-solid-900.ttf",
            "src/win32/fonts/fa-solid-900.otf",
            "third_party/imgui/misc/fonts/fontawesome-webfont.ttf",
            "third_party/imgui/misc/fonts/fa-solid-900.ttf"
        };

        for (const auto& path : candidates) {
            if (!std::filesystem::exists(path)) {
                continue;
            }

            std::ifstream font_file(path, std::ios::binary);
            if (!font_file.is_open()) {
                continue;
            }

            std::stringstream buffer;
            buffer << font_file.rdbuf();
            font_data_ = buffer.str();
            font_loaded_ = !font_data_.empty();
            if (font_loaded_) {
                std::cout << "[FontAwesome] Font loaded successfully from " << path
                          << " (" << font_data_.size() << " bytes)" << std::endl;
                return true;
            }
        }

        std::cout << "[FontAwesome] FontAwesome TTF not found; icons will use text fallback" << std::endl;
        return false;
        
    } catch (const std::exception& e) {
        std::cout << "[FontAwesome] Failed to load font: " << e.what() << std::endl;
        return false;
    }
}

bool FontAwesome::IsLoaded() {
    return font_loaded_;
}

const char* FontAwesome::GetFontData() {
    return font_data_.c_str();
}

size_t FontAwesome::GetFontSize() {
    return font_data_.size();
}

} // namespace win32
} // namespace hunter
