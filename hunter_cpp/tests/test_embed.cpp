// test_embed.cpp — Verify embedded engine extraction works correctly.
#include "core/engine_embed.h"
#include "core/utils.h"
#include <iostream>
#include <fstream>

int main() {
    std::cout << "=== Engine Embed Test ===" << std::endl;

    if (!hunter::embed::hasEmbeddedEngines()) {
        std::cout << "No embedded engines in this build (expected for non-embedded builds)." << std::endl;
        return 0;
    }

    std::cout << "Embedded engines: YES" << std::endl;
    std::cout << "Extraction dir: " << hunter::embed::extractionDir() << std::endl;

    std::cout << "Extracting..." << std::endl;
    bool ok = hunter::embed::ensureExtracted();
    std::cout << "ensureExtracted() = " << (ok ? "true" : "false") << std::endl;

    std::string xray = hunter::embed::xrayPath();
    std::string singbox = hunter::embed::singBoxPath();

    std::cout << "xrayPath()    = " << (xray.empty() ? "(empty)" : xray) << std::endl;
    std::cout << "singBoxPath() = " << (singbox.empty() ? "(empty)" : singbox) << std::endl;

    // Verify files exist.
    if (!xray.empty()) {
        bool exists = hunter::utils::fileExists(xray);
        std::cout << "xray exists: " << (exists ? "YES" : "NO") << std::endl;
        if (exists) {
            std::ifstream f(xray, std::ios::binary);
            char magic[4];
            f.read(magic, 4);
            std::cout << "xray magic bytes: "
                      << std::hex << (int)(unsigned char)magic[0] << " "
                      << (int)(unsigned char)magic[1] << " "
                      << (int)(unsigned char)magic[2] << " "
                      << (int)(unsigned char)magic[3] << std::dec << std::endl;
            // ELF magic: 7f 45 4c 46
            if ((unsigned char)magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F')
                std::cout << "xray: valid ELF binary" << std::endl;
        }
    }

    if (!singbox.empty()) {
        bool exists = hunter::utils::fileExists(singbox);
        std::cout << "sing-box exists: " << (exists ? "YES" : "NO") << std::endl;
        if (exists) {
            std::ifstream f(singbox, std::ios::binary);
            char magic[4];
            f.read(magic, 4);
            if ((unsigned char)magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F')
                std::cout << "sing-box: valid ELF binary" << std::endl;
        }
    }

    // Check manifest.
    std::string manifest = hunter::embed::extractionDir() + "/MANIFEST.txt";
    if (hunter::utils::fileExists(manifest)) {
        std::cout << "MANIFEST.txt exists: YES" << std::endl;
        std::ifstream f(manifest);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("SHA-256") != std::string::npos || line.find("Source") != std::string::npos)
                std::cout << "  " << line << std::endl;
        }
    } else {
        std::cout << "MANIFEST.txt exists: NO" << std::endl;
    }

    // Test second call (should be fast-path / cached).
    std::cout << "Second ensureExtracted() call (should be cached)..." << std::endl;
    ok = hunter::embed::ensureExtracted();
    std::cout << "Result: " << (ok ? "true" : "false") << std::endl;

    std::cout << "=== Test Complete ===" << std::endl;
    return 0;
}
