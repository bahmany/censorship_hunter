#!/usr/bin/env python3
"""
Create simple placeholder icons as binary files
"""

import struct
import os

def create_simple_ico():
    """Create a minimal 16x16 ICO file with a simple pattern"""
    
    # ICO file header
    ico_header = struct.pack('<HHH', 0, 1, 1)  # Reserved, Type, Count
    
    # Directory entry for 16x16 32-bit image
    dir_entry = struct.pack('<BBBBHHII', 
                           16, 16, 0, 0, 1, 32,  # width, height, colors, reserved, planes, bits
                           40,  # size in bytes of image data
                           22   # offset to image data
                           )
    
    # BITMAPINFOHEADER (40 bytes)
    bmp_header = struct.pack('<LHHLLLLHH',
                            40,  # header size
                            16, 16,  # width, height
                            1, 1,  # planes, bits
                            0, 0,  # compression, image size
                            0, 0   # x, y pixels per meter
                            )
    
    # Create a simple 16x16 pattern (H for Hunter)
    # 16x16 = 256 pixels, 4 bytes per pixel (BGRA) = 1024 bytes
    pixels = bytearray(1024)
    
    # Create a simple H pattern in blue
    for y in range(16):
        for x in range(16):
            idx = (y * 16 + x) * 4
            
            if (x == 5 or x == 10) and y >= 2 and y <= 13:  # Vertical lines
                pixels[idx] = 242     # B
                pixels[idx+1] = 120   # G
                pixels[idx+2] = 34    # R
                pixels[idx+3] = 255   # A
            elif y == 7 and x >= 5 and x <= 10:  # Horizontal line
                pixels[idx] = 242     # B
                pixels[idx+1] = 120   # G
                pixels[idx+2] = 34    # R
                pixels[idx+3] = 255   # A
            else:
                pixels[idx] = 0       # B
                pixels[idx+1] = 0     # G
                pixels[idx+2] = 0     # R
                pixels[idx+3] = 0     # A (transparent)
    
    # Combine all parts
    ico_data = ico_header + dir_entry + bmp_header + bytes(pixels)
    
    return ico_data

def create_all_icons():
    """Create all required icon files"""
    icons_dir = "src/win32/icons"
    os.makedirs(icons_dir, exist_ok=True)
    
    # Base icon data
    base_ico = create_simple_ico()
    
    # List of icons to create
    icons = [
        "app.ico", "app_small.ico", "app_large.ico",
        "online.ico", "offline.ico", "working.ico", "error.ico",
        "config.ico", "download.ico", "settings.ico", "logs.ico",
        "censorship.ico", "sources.ico", "about.ico"
    ]
    
    print("Creating placeholder icons...")
    
    for icon_name in icons:
        icon_path = os.path.join(icons_dir, icon_name)
        with open(icon_path, 'wb') as f:
            f.write(base_ico)
        print(f"  - {icon_name} created")
    
    print(f"\nAll icons created in {icons_dir}/")
    
    # Show file sizes
    for icon_name in sorted(icons):
        icon_path = os.path.join(icons_dir, icon_name)
        size = os.path.getsize(icon_path)
        print(f"  - {icon_name}: {size} bytes")

if __name__ == "__main__":
    create_all_icons()
