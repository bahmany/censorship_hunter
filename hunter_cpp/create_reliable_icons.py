#!/usr/bin/env python3
"""
Create a simple but reliable Windows icon using built-in Windows resources
"""

import struct
import os

def create_reliable_icon():
    """Create a minimal but reliable Windows ICO file using proper format"""
    
    # ICO file header (6 bytes)
    ico_header = struct.pack('<HHH', 0, 1, 1)  # Reserved, Type, Count
    
    # Directory entry (16 bytes) for 32x32 32-bit icon
    width, height = 32, 32
    color_count = 0  # 0 for 32-bit images
    reserved = 0
    color_planes = 1
    bits_per_pixel = 32
    
    # Calculate image data size
    bitmap_header_size = 40
    bitmap_data_size = width * height * 4  # BGRA
    mask_size = width * height // 8  # 1-bit mask
    image_size = bitmap_header_size + bitmap_data_size + mask_size
    image_offset = 6 + 16  # After header and directory entry
    
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
                            0, 0)  # x, y pixels per meter, colors used, important
    
    # Create a simple blue pattern - more reliable than complex H shape
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            # Create a simple blue square pattern
            if x >= 8 and x < 24 and y >= 8 and y < 24:
                # Blue square in center
                pixels.extend([34, 120, 242, 255])  # BGRA format
            else:
                # Transparent
                pixels.extend([0, 0, 0, 0])
    
    # Create 1-bit mask (all transparent = 1s)
    mask = bytearray()
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

def create_all_reliable_icons():
    """Create all required icon files with reliable format"""
    icons_dir = "src/win32/icons"
    os.makedirs(icons_dir, exist_ok=True)
    
    # Create a reliable icon
    reliable_ico = create_reliable_icon()
    
    # List of icons to create
    icons = [
        "app.ico", "app_small.ico", "app_large.ico",
        "online.ico", "offline.ico", "working.ico", "error.ico",
        "config.ico", "download.ico", "settings.ico", "logs.ico",
        "censorship.ico", "sources.ico", "about.ico"
    ]
    
    print("Creating reliable Windows icons...")
    
    for icon_name in icons:
        icon_path = os.path.join(icons_dir, icon_name)
        with open(icon_path, 'wb') as f:
            f.write(reliable_ico)
        print(f"  - {icon_name} created ({len(reliable_ico)} bytes)")
    
    print(f"\nAll reliable icons created in {icons_dir}/")
    print("Icon format: 32x32 32-bit BGRA with transparent mask")

if __name__ == "__main__":
    create_all_reliable_icons()
