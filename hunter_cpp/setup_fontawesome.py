#!/usr/bin/env python3
"""
Download and setup FontAwesome font for the application
"""

import requests
import os
from pathlib import Path

def download_fontawesome():
    """Download FontAwesome font file"""
    print("[START] Downloading FontAwesome font...")
    
    # FontAwesome 6 Free Solid font URL
    font_url = "https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/webfonts/fa-solid-900.woff2"
    
    try:
        print("[DOWNLOAD] Fetching FontAwesome font...")
        response = requests.get(font_url, timeout=30)
        response.raise_for_status()
        
        # Create fonts directory
        fonts_dir = Path("src/win32/fonts")
        fonts_dir.mkdir(exist_ok=True)
        
        # Save font file
        font_path = fonts_dir / "fa-solid-900.woff2"
        with open(font_path, 'wb') as f:
            f.write(response.content)
        
        print(f"[SUCCESS] Font saved to: {font_path}")
        print(f"[INFO] Font size: {len(response.content):,} bytes")
        
        return font_path
        
    except Exception as e:
        print(f"[ERROR] Failed to download font: {e}")
        return None

def create_fontawesome_header():
    """Create FontAwesome icon definitions header"""
    print("[CREATE] Creating FontAwesome header...")
    
    header_content = '''#pragma once
#include <string>

// FontAwesome 6 Free Solid Icons
// Icon definitions as UTF-8 strings

#define ICON_FA_EYE     "\\uf06e"   // eye
#define ICON_FA_EDIT    "\\uf044"   // edit
#define ICON_FA_TRASH   "\\uf1f8"   // trash
#define ICON_FA_PLUS    "\\uf067"   // plus
#define ICON_FA_MINUS   "\\uf068"   // minus
#define ICON_FA_CHECK   "\\uf00c"   // check
#define ICON_FA_TIMES   "\\uf00d"   // times
#define ICON_FA_SEARCH  "\\uf002"   // search
#define ICON_FA_DOWNLOAD "\\uf019" // download
#define ICON_FA_UPLOAD  "\\uf093"   // upload
#define ICON_FA_REFRESH "\\uf021"   // refresh
#define ICON_FA_SETTINGS "\\uf013" // settings
#define ICON_FA_INFO    "\\uf05a"   // info
#define ICON_FA_WARNING "\\uf071"   // warning
#define ICON_FA_ERROR   "\\uf071"   // error/exclamation
#define ICON_FA_SUCCESS "\\uf058"   // check-circle
#define ICON_FA_GEAR    "\\uf013"   // gear
#define ICON_FA_LINK    "\\uf0c1"   // link
#define ICON_FA_COPY    "\\uf0c5"   // copy
#define ICON_FA_PASTE   "\\uf0ea"   // paste
#define ICON_FA_PLAY    "\\uf04b"   // play
#define ICON_FA_PAUSE   "\\uf04c"   // pause
#define ICON_FA_STOP    "\\uf04d"   // stop

namespace hunter {
namespace win32 {

// FontAwesome font loading
class FontAwesome {
public:
    static bool LoadFont();
    static bool IsLoaded();
    static const char* GetFontData();
    static size_t GetFontSize();
    
private:
    static bool font_loaded_;
    static std::string font_data_;
};

} // namespace win32
} // namespace hunter
'''
    
    header_path = Path("include/win32/font_awesome.h")
    header_path.parent.mkdir(exist_ok=True)
    
    with open(header_path, 'w') as f:
        f.write(header_content)
    
    print(f"[SUCCESS] Header created: {header_path}")
    return header_path

def create_fontawesome_impl():
    """Create FontAwesome implementation file"""
    print("[CREATE] Creating FontAwesome implementation...")
    
    impl_content = '''#include "win32/font_awesome.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace hunter {
namespace win32 {

bool FontAwesome::font_loaded_ = false;
std::string FontAwesome::font_data_;

bool FontAwesome::LoadFont() {
    if (font_loaded_) {
        return true;
    }
    
    try {
        // Try to load font from file
        std::ifstream font_file("src/win32/fonts/fa-solid-900.woff2", std::ios::binary);
        if (!font_file.is_open()) {
            std::cout << "[FontAwesome] Font file not found, using fallback icons" << std::endl;
            return false;
        }
        
        // Read font data
        std::stringstream buffer;
        buffer << font_file.rdbuf();
        font_data_ = buffer.str();
        
        font_loaded_ = true;
        std::cout << "[FontAwesome] Font loaded successfully (" << font_data_.size() << " bytes)" << std::endl;
        return true;
        
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
'''
    
    impl_path = Path("src/win32/font_awesome.cpp")
    
    with open(impl_path, 'w') as f:
        f.write(impl_content)
    
    print(f"[SUCCESS] Implementation created: {impl_path}")
    return impl_path

def update_cmake():
    """Update CMakeLists.txt to include FontAwesome files"""
    print("[CMAKE] Updating CMakeLists.txt...")
    
    cmake_path = Path("CMakeLists.txt")
    if not cmake_path.exists():
        print("[ERROR] CMakeLists.txt not found")
        return False
    
    try:
        with open(cmake_path, 'r') as f:
            content = f.read()
        
        # Add FontAwesome implementation to sources
        if "font_awesome.cpp" not in content:
            # Find the add_executable line and add our file
            lines = content.split('\n')
            new_lines = []
            
            for i, line in enumerate(lines):
                new_lines.append(line)
                
                # Add font_awesome.cpp after imgui_sources_page.cpp
                if "imgui_sources_page.cpp" in line and "add_executable" in lines[max(0, i-5):i+1]:
                    new_lines.append("    src/win32/font_awesome.cpp")
                    print("[CMAKE] Added font_awesome.cpp to executable sources")
            
            with open(cmake_path, 'w') as f:
                f.write('\n'.join(new_lines))
            
            print("[SUCCESS] CMakeLists.txt updated")
            return True
            
    except Exception as e:
        print(f"[ERROR] Failed to update CMakeLists.txt: {e}")
        return False

if __name__ == "__main__":
    print("[SETUP] FontAwesome Integration Setup")
    print("=" * 50)
    
    # Download font
    font_path = download_fontawesome()
    
    # Create header
    header_path = create_fontawesome_header()
    
    # Create implementation
    impl_path = create_fontawesome_impl()
    
    # Update CMake
    cmake_updated = update_cmake()
    
    print("\n" + "=" * 50)
    print("[SUMMARY] Setup Results:")
    print(f"Font file: {'OK' if font_path else 'FAIL'}")
    print(f"Header file: {'OK' if header_path else 'FAIL'}")
    print(f"Implementation: {'OK' if impl_path else 'FAIL'}")
    print(f"CMake update: {'OK' if cmake_updated else 'FAIL'}")
    
    if font_path and header_path and impl_path:
        print("\n[SUCCESS] FontAwesome integration setup complete!")
        print("[NEXT] Rebuild the application with: ./build.bat")
    else:
        print("\n[PARTIAL] Some components failed to setup")
        print("[NOTE] The application will still work with fallback icons")
