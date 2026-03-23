#include "win32/qr_code.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace hunter {
namespace win32 {
namespace qr {
namespace {

constexpr int kMinVersion = 1;
constexpr int kMaxVersion = 40;
constexpr int kEccFormatLow = 1;

constexpr std::array<int, 41> kEccCodewordsPerBlock = {
    -1,
    7, 10, 15, 20, 26, 18, 20, 24, 30, 18,
    20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
    28, 30, 30, 26, 28, 30, 30, 30, 30, 30,
    30, 30, 30, 30, 30, 30, 30, 30, 30, 30
};

constexpr std::array<int, 41> kNumErrorCorrectionBlocks = {
    -1,
    1, 1, 1, 1, 1, 2, 2, 2, 2, 4,
    4, 4, 4, 4, 6, 6, 6, 6, 7, 8,
    8, 9, 9, 10, 12, 12, 12, 13, 14, 15,
    16, 17, 18, 19, 19, 20, 21, 22, 24, 25
};

bool GetBit(int value, int index) {
    return ((value >> index) & 1) != 0;
}

int GetNumRawDataModules(int version) {
    int result = (16 * version + 128) * version + 64;
    if (version >= 2) {
        int num_align = version / 7 + 2;
        result -= (25 * num_align - 10) * num_align - 55;
        if (version >= 7) result -= 36;
    }
    return result;
}

int GetNumDataCodewords(int version) {
    return GetNumRawDataModules(version) / 8 -
           kEccCodewordsPerBlock[static_cast<size_t>(version)] *
           kNumErrorCorrectionBlocks[static_cast<size_t>(version)];
}

int GetCharCountBits(int version) {
    return version <= 9 ? 8 : 16;
}

uint8_t ReedSolomonMultiply(uint8_t x, uint8_t y) {
    uint8_t z = 0;
    while (y != 0) {
        if ((y & 1U) != 0) z ^= x;
        bool carry = (x & 0x80U) != 0;
        x = static_cast<uint8_t>(x << 1U);
        if (carry) x ^= 0x1DU;
        y = static_cast<uint8_t>(y >> 1U);
    }
    return z;
}

std::vector<uint8_t> ReedSolomonComputeDivisor(int degree) {
    std::vector<uint8_t> result(static_cast<size_t>(degree));
    result.back() = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            result[static_cast<size_t>(j)] = ReedSolomonMultiply(result[static_cast<size_t>(j)], root);
            if (j + 1 < degree) result[static_cast<size_t>(j)] ^= result[static_cast<size_t>(j + 1)];
        }
        root = ReedSolomonMultiply(root, 0x02U);
    }
    return result;
}

std::vector<uint8_t> ReedSolomonComputeRemainder(const std::vector<uint8_t>& data,
                                                 const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> result(divisor.size());
    for (uint8_t b : data) {
        uint8_t factor = static_cast<uint8_t>(b ^ result.front());
        std::rotate(result.begin(), result.begin() + 1, result.end());
        result.back() = 0;
        for (size_t i = 0; i < divisor.size(); ++i) {
            result[i] ^= ReedSolomonMultiply(divisor[i], factor);
        }
    }
    return result;
}

std::vector<int> GetAlignmentPatternPositions(int version) {
    if (version == 1) return {};
    int num_align = version / 7 + 2;
    int size = version * 4 + 17;
    int step = (version == 32) ? 26 : ((version * 4 + num_align * 2 + 1) / (num_align * 2 - 2)) * 2;
    std::vector<int> result(static_cast<size_t>(num_align));
    result[0] = 6;
    for (int i = num_align - 1, pos = size - 7; i >= 1; --i, pos -= step) {
        result[static_cast<size_t>(i)] = pos;
    }
    return result;
}

struct BitBuffer {
    std::vector<bool> bits;

    void appendBits(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) bits.push_back(((value >> i) & 1U) != 0U);
    }

    std::vector<uint8_t> finish(int data_codewords) const {
        std::vector<bool> work = bits;
        const int capacity_bits = data_codewords * 8;
        if (static_cast<int>(work.size()) > capacity_bits) return {};
        const int terminator = std::min(4, capacity_bits - static_cast<int>(work.size()));
        work.insert(work.end(), static_cast<size_t>(terminator), false);
        while ((work.size() % 8U) != 0U) work.push_back(false);
        for (bool flip = true; static_cast<int>(work.size()) < capacity_bits; flip = !flip) {
            uint8_t pad = flip ? 0xECU : 0x11U;
            for (int i = 7; i >= 0; --i) work.push_back(((pad >> i) & 1U) != 0U);
        }
        std::vector<uint8_t> result;
        result.reserve(static_cast<size_t>(data_codewords));
        for (size_t i = 0; i < work.size(); i += 8) {
            uint8_t value = 0;
            for (int j = 0; j < 8; ++j) value = static_cast<uint8_t>((value << 1U) | (work[i + static_cast<size_t>(j)] ? 1U : 0U));
            result.push_back(value);
        }
        return result;
    }
};

struct BlockPair {
    std::vector<uint8_t> data;
    std::vector<uint8_t> ecc;
};

std::vector<uint8_t> AddErrorCorrectionAndInterleave(const std::vector<uint8_t>& data, int version) {
    const int num_blocks = kNumErrorCorrectionBlocks[static_cast<size_t>(version)];
    const int ecc_len = kEccCodewordsPerBlock[static_cast<size_t>(version)];
    const int raw_codewords = GetNumRawDataModules(version) / 8;
    const int num_short_blocks = num_blocks - raw_codewords % num_blocks;
    const int short_block_len = raw_codewords / num_blocks;
    const int short_data_len = short_block_len - ecc_len;
    const int max_data_len = short_data_len + 1;

    std::vector<BlockPair> blocks;
    blocks.reserve(static_cast<size_t>(num_blocks));
    std::vector<uint8_t> divisor = ReedSolomonComputeDivisor(ecc_len);

    size_t offset = 0;
    for (int i = 0; i < num_blocks; ++i) {
        int data_len = short_data_len + (i >= num_short_blocks ? 1 : 0);
        BlockPair block;
        block.data.insert(block.data.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                          data.begin() + static_cast<std::ptrdiff_t>(offset + static_cast<size_t>(data_len)));
        offset += static_cast<size_t>(data_len);
        block.ecc = ReedSolomonComputeRemainder(block.data, divisor);
        blocks.push_back(std::move(block));
    }

    std::vector<uint8_t> result;
    result.reserve(static_cast<size_t>(raw_codewords));
    for (int i = 0; i < max_data_len; ++i) {
        for (const auto& block : blocks) {
            if (i < static_cast<int>(block.data.size())) result.push_back(block.data[static_cast<size_t>(i)]);
        }
    }
    for (int i = 0; i < ecc_len; ++i) {
        for (const auto& block : blocks) result.push_back(block.ecc[static_cast<size_t>(i)]);
    }
    return result;
}

class QrBuilder {
public:
    explicit QrBuilder(int version)
        : version_(version), size_(version * 4 + 17), modules_(static_cast<size_t>(size_ * size_)),
          is_function_(static_cast<size_t>(size_ * size_)) {}

    void drawFunctionPatterns() {
        drawFinderPattern(3, 3);
        drawFinderPattern(size_ - 4, 3);
        drawFinderPattern(3, size_ - 4);
        for (int i = 0; i < size_; ++i) {
            if (!isFunction(6, i)) setFunctionModule(6, i, (i % 2) == 0);
            if (!isFunction(i, 6)) setFunctionModule(i, 6, (i % 2) == 0);
        }
        std::vector<int> align = GetAlignmentPatternPositions(version_);
        for (int y : align) {
            for (int x : align) {
                bool skip = (x == 6 && y == 6) || (x == 6 && y == size_ - 7) || (x == size_ - 7 && y == 6);
                if (!skip) drawAlignmentPattern(x, y);
            }
        }
        for (int i = 0; i < 9; ++i) {
            if (i != 6) {
                setFunctionModule(8, i, false);
                setFunctionModule(i, 8, false);
            }
        }
        for (int i = 0; i < 8; ++i) setFunctionModule(size_ - 1 - i, 8, false);
        for (int i = 0; i < 7; ++i) setFunctionModule(8, size_ - 1 - i, false);
        setFunctionModule(8, size_ - 8, true);
        if (version_ >= 7) drawVersion();
    }

    void drawCodewords(const std::vector<uint8_t>& data) {
        std::vector<bool> bits;
        bits.reserve(data.size() * 8U);
        for (uint8_t b : data) {
            for (int i = 7; i >= 0; --i) bits.push_back(((b >> i) & 1U) != 0U);
        }
        size_t bit_index = 0;
        for (int right = size_ - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size_; ++vert) {
                int y = (((right + 1) & 2) == 0) ? (size_ - 1 - vert) : vert;
                for (int j = 0; j < 2; ++j) {
                    int x = right - j;
                    if (!isFunction(x, y) && bit_index < bits.size()) {
                        setModule(x, y, bits[bit_index++]);
                    }
                }
            }
        }
    }

    void applyMask(int mask) {
        for (int y = 0; y < size_; ++y) {
            for (int x = 0; x < size_; ++x) {
                if (isFunction(x, y)) continue;
                bool invert = false;
                switch (mask) {
                    case 0: invert = ((x + y) % 2) == 0; break;
                    case 1: invert = (y % 2) == 0; break;
                    case 2: invert = (x % 3) == 0; break;
                    case 3: invert = ((x + y) % 3) == 0; break;
                    case 4: invert = (((y / 2) + (x / 3)) % 2) == 0; break;
                    case 5: invert = ((x * y) % 2 + (x * y) % 3) == 0; break;
                    case 6: invert = ((((x * y) % 2) + ((x * y) % 3)) % 2) == 0; break;
                    case 7: invert = ((((x + y) % 2) + ((x * y) % 3)) % 2) == 0; break;
                    default: break;
                }
                if (invert) toggleModule(x, y);
            }
        }
    }

    void drawFormatBits(int mask) {
        int data = (kEccFormatLow << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ (((rem >> 9) & 1) != 0 ? 0x537 : 0);
        int bits = ((data << 10) | rem) ^ 0x5412;

        for (int i = 0; i <= 5; ++i) setFunctionModule(8, i, GetBit(bits, i));
        setFunctionModule(8, 7, GetBit(bits, 6));
        setFunctionModule(8, 8, GetBit(bits, 7));
        setFunctionModule(7, 8, GetBit(bits, 8));
        for (int i = 9; i < 15; ++i) setFunctionModule(14 - i, 8, GetBit(bits, i));

        for (int i = 0; i < 8; ++i) setFunctionModule(size_ - 1 - i, 8, GetBit(bits, i));
        for (int i = 8; i < 15; ++i) setFunctionModule(8, size_ - 15 + i, GetBit(bits, i));
        setFunctionModule(8, size_ - 8, true);
    }

    long penaltyScore() const {
        long result = 0;
        for (int y = 0; y < size_; ++y) {
            bool color = get(0, y);
            int run = 1;
            for (int x = 1; x < size_; ++x) {
                bool current = get(x, y);
                if (current == color) {
                    ++run;
                } else {
                    if (run >= 5) result += 3 + (run - 5);
                    color = current;
                    run = 1;
                }
            }
            if (run >= 5) result += 3 + (run - 5);
        }
        for (int x = 0; x < size_; ++x) {
            bool color = get(x, 0);
            int run = 1;
            for (int y = 1; y < size_; ++y) {
                bool current = get(x, y);
                if (current == color) {
                    ++run;
                } else {
                    if (run >= 5) result += 3 + (run - 5);
                    color = current;
                    run = 1;
                }
            }
            if (run >= 5) result += 3 + (run - 5);
        }
        for (int y = 0; y < size_ - 1; ++y) {
            for (int x = 0; x < size_ - 1; ++x) {
                bool c = get(x, y);
                if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1)) result += 3;
            }
        }
        static const std::array<bool, 11> p1 = {true, false, true, true, true, false, true, false, false, false, false};
        static const std::array<bool, 11> p2 = {false, false, false, false, true, false, true, true, true, false, true};
        for (int y = 0; y < size_; ++y) {
            for (int x = 0; x <= size_ - 11; ++x) {
                bool match1 = true;
                bool match2 = true;
                for (int i = 0; i < 11; ++i) {
                    bool v = get(x + i, y);
                    match1 = match1 && (v == p1[static_cast<size_t>(i)]);
                    match2 = match2 && (v == p2[static_cast<size_t>(i)]);
                }
                if (match1 || match2) result += 40;
            }
        }
        for (int x = 0; x < size_; ++x) {
            for (int y = 0; y <= size_ - 11; ++y) {
                bool match1 = true;
                bool match2 = true;
                for (int i = 0; i < 11; ++i) {
                    bool v = get(x, y + i);
                    match1 = match1 && (v == p1[static_cast<size_t>(i)]);
                    match2 = match2 && (v == p2[static_cast<size_t>(i)]);
                }
                if (match1 || match2) result += 40;
            }
        }
        int dark = 0;
        for (uint8_t v : modules_) dark += v != 0 ? 1 : 0;
        int total = size_ * size_;
        result += (std::abs(dark * 20 - total * 10) / total) * 10;
        return result;
    }

    Matrix toMatrix() const {
        Matrix out;
        out.size = size_;
        out.modules = modules_;
        return out;
    }

private:
    int version_;
    int size_;
    std::vector<uint8_t> modules_;
    std::vector<uint8_t> is_function_;

    size_t index(int x, int y) const {
        return static_cast<size_t>(y * size_ + x);
    }

    bool isFunction(int x, int y) const {
        return is_function_[index(x, y)] != 0;
    }

    bool get(int x, int y) const {
        return modules_[index(x, y)] != 0;
    }

    void setModule(int x, int y, bool black) {
        modules_[index(x, y)] = black ? 1U : 0U;
    }

    void toggleModule(int x, int y) {
        modules_[index(x, y)] ^= 1U;
    }

    void setFunctionModule(int x, int y, bool black) {
        modules_[index(x, y)] = black ? 1U : 0U;
        is_function_[index(x, y)] = 1U;
    }

    void drawFinderPattern(int x, int y) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                int xx = x + dx;
                int yy = y + dy;
                if (0 <= xx && xx < size_ && 0 <= yy && yy < size_) {
                    int dist = std::max(std::abs(dx), std::abs(dy));
                    bool black = dist != 2 && dist != 4;
                    setFunctionModule(xx, yy, black);
                }
            }
        }
    }

    void drawAlignmentPattern(int x, int y) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                setFunctionModule(x + dx, y + dy, dist != 1);
            }
        }
    }

    void drawVersion() {
        int rem = version_;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ (((rem >> 11) & 1) != 0 ? 0x1F25 : 0);
        int bits = (version_ << 12) | rem;
        for (int i = 0; i < 18; ++i) {
            bool bit = GetBit(bits, i);
            setFunctionModule(size_ - 11 + (i % 3), i / 3, bit);
            setFunctionModule(i / 3, size_ - 11 + (i % 3), bit);
        }
    }
};

} // namespace

bool EncodeText(const std::string& text, Matrix& out, std::string& error) {
    out = Matrix{};
    error.clear();
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max() / 8)) {
        error = "QR payload too large";
        return false;
    }

    int version = 0;
    BitBuffer bits;
    for (int ver = kMinVersion; ver <= kMaxVersion; ++ver) {
        bits.bits.clear();
        bits.appendBits(0x4U, 4);
        bits.appendBits(static_cast<uint32_t>(text.size()), GetCharCountBits(ver));
        for (unsigned char ch : text) bits.appendBits(ch, 8);
        const int used = static_cast<int>(bits.bits.size());
        if (used <= GetNumDataCodewords(ver) * 8) {
            version = ver;
            break;
        }
    }
    if (version == 0) {
        error = "Config URI is too long for QR generation";
        return false;
    }

    std::vector<uint8_t> data_codewords = bits.finish(GetNumDataCodewords(version));
    if (data_codewords.empty()) {
        error = "Failed to pack QR payload";
        return false;
    }
    std::vector<uint8_t> all_codewords = AddErrorCorrectionAndInterleave(data_codewords, version);

    QrBuilder base(version);
    base.drawFunctionPatterns();
    base.drawCodewords(all_codewords);

    long best_penalty = std::numeric_limits<long>::max();
    bool found = false;
    Matrix best;
    for (int mask = 0; mask < 8; ++mask) {
        QrBuilder trial = base;
        trial.applyMask(mask);
        trial.drawFormatBits(mask);
        long penalty = trial.penaltyScore();
        if (!found || penalty < best_penalty) {
            found = true;
            best_penalty = penalty;
            best = trial.toMatrix();
        }
    }
    if (!best.valid()) {
        error = "Failed to finalize QR matrix";
        return false;
    }
    out = std::move(best);
    return true;
}

} // namespace qr
} // namespace win32
} // namespace hunter
