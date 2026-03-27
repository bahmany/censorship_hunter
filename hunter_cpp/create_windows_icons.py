#!/usr/bin/env python3
"""
Create a proper Windows icon file using a simpler approach
"""

import struct
import os

def create_simple_windows_icon():
    """Create a minimal but valid Windows ICO file"""
    
    # ICO file header (6 bytes)
    ico_header = struct.pack('<HHH', 0, 1, 1)  # Reserved, Type, Count
    
    # Directory entry (16 bytes)
    # Create a 32x32 32-bit icon
    width, height = 32, 32
    color_count = 0  # 0 for 32-bit images
    reserved = 0
    color_planes = 1
    bits_per_pixel = 32
    
    # Calculate image data size (BITMAPINFOHEADER + bitmap data + mask)
    bitmap_header_size = 40
    bitmap_data_size = width * height * 4  # BGRA
    mask_size = width * height // 8  # 1-bit mask
    image_size = bitmap_header_size + bitmap_data_size + mask_size
    
    # Image offset in file (header + directory entry)
    image_offset = 6 + 16
    
    dir_entry = struct.pack('<BBBBHHII',
                           width, height, color_count, reserved,
                           color_planes, bits_per_pixel,
                           image_size, image_offset)
    
    # BITMAPINFOHEADER (40 bytes)
    bmp_header = struct.pack('<LHHLLLLHH',
                            40,  # header size
                            width, height * 2,  # width, height (doubled for XOR+AND masks)
                            1, 32,  # planes, bits
                            0, 0,  # compression, image size
                            0, 0)  # x, y pixels per meter
    
    # Create a simple blue "H" pattern
    pixels = bytearray()
    mask = bytearray()
    
    for y in range(height):
        for x in range(width):
            # BGRA format
            if (x == 10 or x == 21) and y >= 8 and y <= 23:  # Vertical lines of H
                pixels.extend([242, 120, 34, 255])  # Blue BGRA
            elif y == 15 and x >= 10 and x <= 21:  # Horizontal line of H
                pixels.extend([242, 120, 34, 255])  # Blue BGRA
            else:
                pixels.extend([0, 0, 0, 0])  # Transparent BGRA
    
    # Create 1-bit mask (all transparent)
    for y in range(height):
        row_mask = 0
        for x in range(width):
            if x % 8 == 0 and x > 0:
                mask.append(row_mask)
                row_mask = 0
            # All pixels transparent (mask bit = 1)
            row_mask |= (1 << (7 - (x % 8)))
        # Add last byte of row
        mask.append(row_mask)
    
    # Combine all parts
    ico_data = ico_header + dir_entry + bmp_header + bytes(pixels) + bytes(mask)
    
    return ico_data

def create_all_icons():
    """Create all required icon files"""
    icons_dir = "src/win32/icons"
    os.makedirs(icons_dir, exist_ok=True)
    
    # Create a basic icon
    base_ico = create_simple_windows_icon()
    
    # List of icons to create (all using the same base icon for now)
    icons = [
        "app.ico", "app_small.ico", "app_large.ico",
        "online.ico", "offline.ico", "working.ico", "error.ico",
        "config.ico", "download.ico", "settings.ico", "logs.ico",
        "censorship.ico", "sources.ico", "about.ico"
    ]
    
    print("Creating Windows icons...")
    
    for icon_name in icons:
        icon_path = os.path.join(icons_dir, icon_name)
        with open(icon_path, 'wb') as f:
            f.write(base_ico)
        print(f"  - {icon_name} created ({len(base_ico)} bytes)")
    
    print(f"\nAll icons created in {icons_dir}/")

if __name__ == "__main__":
    create_all_icons()
