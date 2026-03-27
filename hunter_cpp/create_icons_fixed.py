#!/usr/bin/env python3
"""
Simple icon generator for Hunter Censor project - Fixed version
Creates basic .ico files using PIL
"""

import struct
import os
from PIL import Image, ImageDraw
import sys

def create_simple_icon(size, color, text_symbol="H"):
    """Create a simple icon with the given size, color, and symbol"""
    # Create image with transparent background
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    
    # Draw background circle
    margin = size // 8
    draw.ellipse([margin, margin, size-margin, size-margin], fill=color)
    
    # Draw simple symbol using basic shapes
    center = size // 2
    
    if text_symbol == "H":
        # Draw H shape
        line_width = size // 12
        # Left vertical line
        draw.rectangle([center - size//4, margin, center - size//4 + line_width, size - margin], fill="white")
        # Right vertical line  
        draw.rectangle([center + size//4 - line_width, margin, center + size//4, size - margin], fill="white")
        # Horizontal line
        draw.rectangle([center - size//4, center - line_width//2, center + size//4, center + line_width//2], fill="white")
    elif text_symbol == "✓":
        # Draw checkmark
        draw.polygon([
            (center - size//4, center),
            (center - size//8, center + size//6),
            (center + size//4, center - size//6),
            (center + size//4, center - size//8),
            (center - size//8, center + size//8),
            (center - size//4, center + size//4)
        ], fill="white")
    elif text_symbol == "✕":
        # Draw X
        line_width = size // 12
        draw.rectangle([center - size//4 - line_width//2, center - line_width//2, center - size//4 + line_width//2, center + line_width//2], fill="white")
        draw.rectangle([center + size//4 - line_width//2, center - line_width//2, center + size//4 + line_width//2, center + line_width//2], fill="white")
    else:
        # Draw simple circle for unknown symbols
        draw.ellipse([center - size//8, center - size//8, center + size//8, center + size//8], fill="white")
    
    return img

def save_ico_file(img, filepath):
    """Save PIL Image as .ico file"""
    try:
        # ICO files need multiple sizes for best quality
        sizes = [16, 32, 48, 64, 128, 256]
        
        # Filter sizes that are not larger than the image
        available_sizes = [s for s in sizes if s <= img.width]
        
        # Create images for each size
        images = []
        for size in available_sizes:
            if size == img.width:
                images.append(img)
            else:
                # Resize image
                resized = img.resize((size, size), Image.Resampling.LANCZOS)
                images.append(resized)
        
        # Save as ICO
        img.save(filepath, format='ICO', sizes=available_sizes)
        return True
    except Exception as e:
        print(f"Error saving {filepath}: {e}")
        return False

def main():
    # Create icons directory
    icons_dir = "src/win32/icons"
    os.makedirs(icons_dir, exist_ok=True)
    
    print("Creating icons for Hunter Censor...")
    
    # Create main app icon (blue)
    print("Creating app icon...")
    app_img = create_simple_icon(256, (34, 120, 242), "H")
    if save_ico_file(app_img, f"{icons_dir}/app.ico"):
        print("  - app.ico created")
    
    # Create small app icon
    app_small = create_simple_icon(32, (34, 120, 242), "H")
    if save_ico_file(app_small, f"{icons_dir}/app_small.ico"):
        print("  - app_small.ico created")
        
    # Create large app icon
    app_large = create_simple_icon(48, (34, 120, 242), "H")
    if save_ico_file(app_large, f"{icons_dir}/app_large.ico"):
        print("  - app_large.ico created")
    
    # Create status icons
    print("Creating status icons...")
    
    # Online - green checkmark
    online_img = create_simple_icon(32, (34, 197, 94), "✓")
    if save_ico_file(online_img, f"{icons_dir}/online.ico"):
        print("  - online.ico created")
    
    # Offline - gray X
    offline_img = create_simple_icon(32, (107, 114, 128), "✕")
    if save_ico_file(offline_img, f"{icons_dir}/offline.ico"):
        print("  - offline.ico created")
    
    # Working - yellow (circle)
    working_img = create_simple_icon(32, (234, 179, 8), "○")
    if save_ico_file(working_img, f"{icons_dir}/working.ico"):
        print("  - working.ico created")
    
    # Error - red (circle)
    error_img = create_simple_icon(32, (239, 68, 68), "●")
    if save_ico_file(error_img, f"{icons_dir}/error.ico"):
        print("  - error.ico created")
    
    # Create function icons
    print("Creating function icons...")
    
    # Config - blue gear
    config_img = create_simple_icon(32, (59, 130, 246), "H")
    if save_ico_file(config_img, f"{icons_dir}/config.ico"):
        print("  - config.ico created")
    
    # Download - green arrow
    download_img = create_simple_icon(32, (34, 197, 94), "✓")
    if save_ico_file(download_img, f"{icons_dir}/download.ico"):
        print("  - download.ico created")
    
    # Settings - purple
    settings_img = create_simple_icon(32, (168, 85, 247), "H")
    if save_ico_file(settings_img, f"{icons_dir}/settings.ico"):
        print("  - settings.ico created")
    
    # Logs - gray
    logs_img = create_simple_icon(32, (107, 114, 128), "○")
    if save_ico_file(logs_img, f"{icons_dir}/logs.ico"):
        print("  - logs.ico created")
    
    # Censorship - yellow shield
    censorship_img = create_simple_icon(32, (234, 179, 8), "H")
    if save_ico_file(censorship_img, f"{icons_dir}/censorship.ico"):
        print("  - censorship.ico created")
    
    # Sources - blue network
    sources_img = create_simple_icon(32, (34, 120, 242), "○")
    if save_ico_file(sources_img, f"{icons_dir}/sources.ico"):
        print("  - sources.ico created")
    
    # About - blue info
    about_img = create_simple_icon(32, (59, 130, 246), "○")
    if save_ico_file(about_img, f"{icons_dir}/about.ico"):
        print("  - about.ico created")
    
    print(f"\nIcons created successfully in {icons_dir}/")
    
    # List created files
    print("Created files:")
    for file in sorted(os.listdir(icons_dir)):
        if file.endswith('.ico'):
            size = os.path.getsize(os.path.join(icons_dir, file))
            print(f"  - {file} ({size} bytes)")

if __name__ == "__main__":
    try:
        main()
    except ImportError as e:
        print(f"Error: {e}")
        print("Please install Pillow: pip install Pillow")
        sys.exit(1)
    except Exception as e:
        print(f"Error creating icons: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
