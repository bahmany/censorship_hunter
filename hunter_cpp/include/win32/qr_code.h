#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hunter {
namespace win32 {
namespace qr {

struct Matrix {
    int size = 0;
    std::vector<uint8_t> modules;

    bool valid() const {
        return size > 0 && modules.size() == static_cast<size_t>(size * size);
    }

    bool get(int x, int y) const {
        return valid() && x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<size_t>(y * size + x)] != 0;
    }
};

bool EncodeText(const std::string& text, Matrix& out, std::string& error);

} // namespace qr
} // namespace win32
} // namespace hunter
