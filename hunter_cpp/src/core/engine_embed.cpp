#include "core/engine_embed.h"
#include "core/utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <mutex>
#include <vector>
#include <filesystem>
#include <chrono>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// Vendored zstd decoder (amalgamated, self-contained).
#include "zstd.h"

// Build-time generated header. Defines HUNTER_EMBED_HAS_ENGINES and, when 1:
//   extern const unsigned char kXrayZst[];      extern const unsigned long kXrayZstLen;
//   extern const unsigned long kXrayOrigLen;    extern const char kXraySha256[];
//   (same set for sing-box)
#include "engine_embedded.h"

namespace fs = std::filesystem;

namespace hunter {
namespace embed {

// ─────────────────────────────────────────────────────────────────────────────
// Minimal SHA-256 (public-domain style, self-contained).
// Used only to verify extracted engine binaries match the baked-in digest.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
};

constexpr uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR256(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

void sha256_transform(Sha256Ctx* c, const uint8_t* d) {
    uint32_t a,b,cc,dd,e,f,g,h,t1,t2,m[64];
    for (int i = 0; i < 16; ++i)
        m[i] = (uint32_t(d[i*4])<<24)|(uint32_t(d[i*4+1])<<16)|(uint32_t(d[i*4+2])<<8)|uint32_t(d[i*4+3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ROTR256(m[i-15],7) ^ ROTR256(m[i-15],18) ^ (m[i-15] >> 3);
        uint32_t s1 = ROTR256(m[i-2],17) ^ ROTR256(m[i-2],19) ^ (m[i-2] >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a=c->state[0];b=c->state[1];cc=c->state[2];dd=c->state[3];
    e=c->state[4];f=c->state[5];g=c->state[6];h=c->state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ROTR256(e,6) ^ ROTR256(e,11) ^ ROTR256(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + K256[i] + m[i];
        uint32_t S0 = ROTR256(a,2) ^ ROTR256(a,13) ^ ROTR256(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + mj;
        h=g;g=f;f=e;e=dd+t1;dd=cc;cc=b;b=a;a=t1+t2;
    }
    c->state[0]+=a;c->state[1]+=b;c->state[2]+=cc;c->state[3]+=dd;
    c->state[4]+=e;c->state[5]+=f;c->state[6]+=g;c->state[7]+=h;
}

void sha256_init(Sha256Ctx* c) {
    c->datalen=0; c->bitlen=0;
    c->state[0]=0x6a09e667;c->state[1]=0xbb67ae85;c->state[2]=0x3c6ef372;c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f;c->state[5]=0x9b05688c;c->state[6]=0x1f83d9ab;c->state[7]=0x5be0cd19;
}

void sha256_update(Sha256Ctx* c, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        c->data[c->datalen++] = data[i];
        if (c->datalen == 64) {
            sha256_transform(c, c->data);
            c->bitlen += 512;
            c->datalen = 0;
        }
    }
}

void sha256_final(Sha256Ctx* c, uint8_t out[32]) {
    uint64_t bitlen = c->bitlen + (uint64_t)c->datalen * 8;
    c->data[c->datalen++] = 0x80;
    if (c->datalen < 56) {
        while (c->datalen < 56) c->data[c->datalen++] = 0x00;
    } else {
        while (c->datalen < 64) c->data[c->datalen++] = 0x00;
        sha256_transform(c, c->data);
        c->datalen = 0;
        while (c->datalen < 56) c->data[c->datalen++] = 0x00;
    }
    for (int i = 7; i >= 0; --i) c->data[c->datalen++] = uint8_t((bitlen >> (i*8)) & 0xff);
    sha256_transform(c, c->data);
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = uint8_t((c->state[i] >> 24) & 0xff);
        out[i*4+1] = uint8_t((c->state[i] >> 16) & 0xff);
        out[i*4+2] = uint8_t((c->state[i] >> 8) & 0xff);
        out[i*4+3] = uint8_t(c->state[i] & 0xff);
    }
}

std::string sha256_hex(const uint8_t* data, size_t len) {
    Sha256Ctx c; sha256_init(&c);
    sha256_update(&c, data, len);
    uint8_t dig[32]; sha256_final(&c, dig);
    static const char* hx = "0123456789abcdef";
    std::string s; s.reserve(64);
    for (int i = 0; i < 32; ++i) { s.push_back(hx[dig[i]>>4]); s.push_back(hx[dig[i]&0xf]); }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cache directory resolution.
// ─────────────────────────────────────────────────────────────────────────────

std::string& baseDirOverride() {
    static std::string s;
    return s;
}

std::string resolveCacheBase() {
    const std::string& ov = baseDirOverride();
    if (!ov.empty()) return ov;

#ifdef _WIN32
    char path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)) && path[0])
        return std::string(path) + "\\Hunter";
    return "Hunter";
#elif defined(__ANDROID__)
    // Android: must be set via setExtractionBaseDir() from JNI; fallback to /data/local/tmp.
    const char* tmp = std::getenv("HUNTER_ENGINE_DIR");
    if (tmp && *tmp) return tmp;
    return "/data/local/tmp/hunter";
#else
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/hunter";
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.cache/hunter";
    return "/tmp/hunter";
#endif
}

std::string engineExeName(const char* engine) {
#ifdef _WIN32
    return std::string(engine) + ".exe";
#else
    return engine;
#endif
}

bool writeAllBytes(const std::string& path, const uint8_t* data, size_t len) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), (std::streamsize)len);
    return f.good();
}

bool readAllBytes(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

void makeExecutable(const std::string& path) {
#ifndef _WIN32
    try { fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read,
                          fs::perm_options::replace); } catch (...) {}
    chmod(path.c_str(), 0700);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// MANIFEST.txt — AV transparency.
//
// A human-readable manifest is written alongside the extracted engines so that
// antivirus analysts, sandboxes, and curious users can immediately see what the
// extracted files are, where they came from, and verify their integrity. This
// is the single most effective non-signing mitigation against false-positive
// AV detections on the "dropper" pattern (extract-then-exec).
// ─────────────────────────────────────────────────────────────────────────────

std::string isoTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

struct EngineRecord {
    const char* name;
    const char* sha256;
    unsigned long orig_size;
    bool extracted;
    std::string path;
};

[[maybe_unused]] void writeManifest(const std::string& dir, const std::vector<EngineRecord>& engines) {
    std::ostringstream ss;
    ss << "════════════════════════════════════════════════════════════════\n";
    ss << "  Hunter — Embedded Engine Extraction Manifest\n";
    ss << "  Generated: " << isoTimestamp() << "\n";
    ss << "════════════════════════════════════════════════════════════════\n";
    ss << "\n";
    ss << "This directory contains proxy engine binaries that were embedded\n";
    ss << "inside the Hunter executable (single-file distribution mode) and\n";
    ss << "extracted here on first run. They are NOT malware.\n";
    ss << "\n";
    ss << "Hunter is an open-source anti-censorship proxy tool (MIT licensed):\n";
    ss << "  https://github.com/bahmany/censorship_hunter\n";
    ss << "\n";
    ss << "The engines below are official releases from their upstream\n";
    ss << "projects, compressed with zstd and embedded at build time. Their\n";
    ss << "SHA-256 digests are baked into the Hunter binary and verified\n";
    ss << "after extraction — if a digest does not match, the file is\n";
    ss << "rejected and not executed.\n";
    ss << "\n";
    ss << "────────────────────────────────────────────────────────────────\n";
    ss << "  Extracted Engines\n";
    ss << "────────────────────────────────────────────────────────────────\n";
    for (const auto& e : engines) {
        ss << "\n";
        ss << "  Name:       " << e.name << "\n";
        ss << "  Status:     " << (e.extracted ? "extracted OK" : "extraction FAILED") << "\n";
        ss << "  Path:       " << (e.path.empty() ? "(none)" : e.path) << "\n";
        ss << "  Size:       " << e.orig_size << " bytes\n";
        ss << "  SHA-256:    " << (e.sha256 ? e.sha256 : "(none)") << "\n";
        if (std::string(e.name) == "xray")
            ss << "  Source:     https://github.com/XTLS/Xray-core/releases\n";
        else if (std::string(e.name) == "sing-box")
            ss << "  Source:     https://github.com/SagerNet/sing-box/releases\n";
        ss << "  License:    MIT (Xray) / GPL-3.0 (sing-box)\n";
    }
    ss << "\n";
    ss << "────────────────────────────────────────────────────────────────\n";
    ss << "  Verification\n";
    ss << "────────────────────────────────────────────────────────────────\n";
    ss << "\n";
    ss << "You can verify any extracted file independently:\n";
    ss << "  sha256sum <engine_name>\n";
    ss << "  # Compare with the SHA-256 listed above.\n";
    ss << "\n";
    ss << "If the digest does not match, delete the file — Hunter will\n";
    ss << "re-extract a fresh copy from the embedded payload on next run.\n";
    ss << "\n";
    ss << "To force re-extraction, delete this entire directory.\n";
    ss << "════════════════════════════════════════════════════════════════\n";

    const std::string manifest_path = dir + "/MANIFEST.txt";
    writeAllBytes(manifest_path,
                  reinterpret_cast<const uint8_t*>(ss.str().data()),
                  ss.str().size());
}

// Decompress one embedded engine into the cache dir. Returns final path on success.
// All operations are logged transparently — no silent extraction.
[[maybe_unused]] bool extractEngine(const char* engine,
                   const unsigned char* zst_data, unsigned long zst_len,
                   unsigned long orig_len, const char* sha_expected) {
    const std::string dir = resolveCacheBase() + "/engines";
    try { fs::create_directories(dir); } catch (...) {}

    const std::string path = dir + "/" + engineExeName(engine);
    const std::string stamp = dir + "/" + std::string(engine) + ".sha256";

    // Fast path: cached file exists and stored digest matches expected.
    std::string cached_digest;
    if (utils::fileExists(path) && readAllBytes(stamp, cached_digest)) {
        while (!cached_digest.empty() && (cached_digest.back() == '\n' || cached_digest.back() == '\r' ||
               cached_digest.back() == ' ' || cached_digest.back() == '\t'))
            cached_digest.pop_back();
        if (cached_digest == sha_expected) {
            utils::LogRingBuffer::instance().push(
                std::string("[EngineEmbed] ") + engine + " already extracted and verified (cached)");
            makeExecutable(path);
            return true;
        }
        utils::LogRingBuffer::instance().push(
            std::string("[EngineEmbed] ") + engine + " cached copy digest mismatch — re-extracting");
    }

    utils::LogRingBuffer::instance().push(
        std::string("[EngineEmbed] Extracting embedded ") + engine + " (" +
        std::to_string(zst_len) + " compressed -> " + std::to_string(orig_len) +
        " bytes) to " + path);

    // Decompress.
    std::vector<uint8_t> buf(orig_len);
    size_t r = ZSTD_decompress(buf.data(), orig_len, zst_data, (size_t)zst_len);
    if (ZSTD_isError(r)) {
        utils::LogRingBuffer::instance().push(
            std::string("[EngineEmbed] zstd decompress failed for ") + engine + ": " +
            ZSTD_getErrorName(r));
        return false;
    }
    if (r != orig_len) {
        utils::LogRingBuffer::instance().push(
            std::string("[EngineEmbed] size mismatch for ") + engine + ": got " +
            std::to_string(r) + " expected " + std::to_string(orig_len));
        return false;
    }

    // Verify digest — reject if tampered.
    std::string actual = sha256_hex(buf.data(), r);
    if (sha_expected && sha_expected[0] && actual != sha_expected) {
        utils::LogRingBuffer::instance().push(
            std::string("[EngineEmbed] SECURITY: sha256 mismatch for ") + engine +
            " — extracted data does not match embedded digest, rejecting");
        return false;
    }
    utils::LogRingBuffer::instance().push(
        std::string("[EngineEmbed] ") + engine + " SHA-256 verified: " + actual.substr(0, 12) + "...");

    // Write atomically: temp file then rename (prevents partial-file execution).
    const std::string tmp = path + ".part";
    if (!writeAllBytes(tmp, buf.data(), r)) {
        utils::LogRingBuffer::instance().push(
            std::string("[EngineEmbed] failed to write ") + tmp);
        return false;
    }
    makeExecutable(tmp);
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // rename failed (cross-device?); fall back to copy.
        try { fs::copy_file(tmp, path, fs::copy_options::overwrite_existing); fs::remove(tmp); }
        catch (...) { return false; }
        makeExecutable(path);
    }

    // Stamp digest for fast-path verification on next run.
    writeAllBytes(stamp, reinterpret_cast<const uint8_t*>(actual.c_str()), actual.size());
    utils::LogRingBuffer::instance().push(
        std::string("[EngineEmbed] ") + engine + " ready at " + path);
    return true;
}

struct ExtractedState {
    bool extracted = false;
    bool ok_xray = false;
    bool ok_singbox = false;
    std::string xray_path;
    std::string singbox_path;
};

ExtractedState& state() {
    static ExtractedState s;
    return s;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

bool hasEmbeddedEngines() {
#if defined(HUNTER_EMBED_HAS_ENGINES) && HUNTER_EMBED_HAS_ENGINES
    return true;
#else
    return false;
#endif
}

bool ensureExtracted() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (hasEmbeddedEngines()) {
#if defined(HUNTER_EMBED_HAS_ENGINES) && HUNTER_EMBED_HAS_ENGINES
            const std::string dir = resolveCacheBase() + "/engines";
            state().ok_xray    = extractEngine("xray",     kXrayZst,     kXrayZstLen,     kXrayOrigLen,     kXraySha256);
            state().ok_singbox = extractEngine("sing-box", kSingboxZst,  kSingboxZstLen,  kSingboxOrigLen,  kSingboxSha256);
            if (state().ok_xray)    state().xray_path    = dir + "/" + engineExeName("xray");
            if (state().ok_singbox) state().singbox_path = dir + "/" + engineExeName("sing-box");

            // Write AV-transparency manifest alongside extracted engines.
            std::vector<EngineRecord> records = {
                {"xray",     kXraySha256,    kXrayOrigLen,    state().ok_xray,    state().xray_path},
                {"sing-box", kSingboxSha256, kSingboxOrigLen, state().ok_singbox, state().singbox_path},
            };
            writeManifest(dir, records);
#endif
        } else {
            // No embedded engines — check if engines were placed externally
            // (e.g. Android: Java extracted APK assets to filesDir/engines/).
            const std::string dir = resolveCacheBase() + "/engines";
            const std::string xp = dir + "/" + engineExeName("xray");
            const std::string sp = dir + "/" + engineExeName("sing-box");
            if (utils::fileExists(xp)) {
                state().ok_xray = true;
                state().xray_path = xp;
                makeExecutable(xp);
            }
            if (utils::fileExists(sp)) {
                state().ok_singbox = true;
                state().singbox_path = sp;
                makeExecutable(sp);
            }
        }
        state().extracted = true;
    });
    return state().ok_xray || state().ok_singbox;
}

std::string xrayPath() {
    ensureExtracted();
    return state().xray_path;
}

std::string singBoxPath() {
    ensureExtracted();
    return state().singbox_path;
}

void setExtractionBaseDir(const std::string& dir) {
    baseDirOverride() = dir;
}

std::string extractionDir() {
    return resolveCacheBase() + "/engines";
}

} // namespace embed
} // namespace hunter
