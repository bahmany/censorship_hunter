#!/usr/bin/env python3
"""
Simple icon generator for Hunter Censor project
Creates basic .ico files programmatically
"""

import struct
import os
from PIL import Image, ImageDraw, ImageFont
import sys

def create_simple_icon(size, color, text="H"):
    """Create a simple icon with the given size, color, and text"""
    # Create image with transparent background
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    
    # Draw background circle
    margin = size // 8
    draw.ellipse([margin, margin, size-margin, size-margin], fill=color)
    
    # Draw text
    try:
        font_size = size // 3
        font = ImageFont.truetype("arial.ttf", font_size)
    except:
        font = ImageFont.load_default()
    
    # Get text bounding box
    bbox = draw.textbbox((0, 0), text, font=font)
    text_width = bbox[2] - bbox[0]
    text_height = bbox[3] - bbox[1]
    
    # Center text
    x = (size - text_width) // 2
    y = (size - text_height) // 2
    draw.text((x, y), text, fill="white", font=font)
    
    return img

def create_app_icon():
    """Create main application icon"""
    img = create_simple_icon(256, (34, 120, 242), "H")  # Blue background
    return img

def create_status_icons():
    """Create status icons"""
    icons = {}
    
    # Online - green
    icons['online'] = create_simple_icon(64, (34, 197, 94), "✓")
    
    # Offline - gray
    icons['offline'] = create_simple_icon(64, (107, 114, 128), "✕")
    
    # Working - yellow
    icons['working'] = create_simple_icon(64, (234, 179, 8), "⟳")
    
    # Error - red
    icons['error'] = create_simple_icon(64, (239, 68, 68), "!")
    
    return icons

def create_function_icons():
    """Create function-specific icons"""
    icons = {}
    
    # Config - gear icon
    icons['config'] = create_simple_icon(48, (59, 130, 246), "⚙")
    
    # Download - arrow down
    icons['download'] = create_simple_icon(48, (34, 197, 94), "↓")
    
    # Settings - sliders
    icons['settings'] = create_simple_icon(48, (168, 85, 247), "☰")
    
    # Logs - document
    icons['logs'] = create_simple_icon(48, (107, 114, 128), "📄")
    
    # Censorship - shield
    icons['censorship'] = create_simple_icon(48, (234, 179, 8), "🛡")
    
    # Sources - network
    icons['sources'] = create_simple_icon(48, (34, 120, 242), "🌐")
    
    # About - info
    icons['about'] = create_simple_icon(48, (59, 130, 246), "ⓘ")
    
    return icons

def save_ico_file(img, filepath):
    """Save PIL Image as .ico file"""
    # ICO files need multiple sizes for best quality
    sizes = [16, 32, 48, 64, 128, 256]
    
    # Filter sizes that are larger than the image
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

def main():
    # Create icons directory
    icons_dir = "src/win32/icons"
    os.makedirs(icons_dir, exist_ok=True)
    
    print("Creating icons for Hunter Censor...")
    
    # Create main app icon
    print("Creating app icon...")
    app_img = create_app_icon()
    save_ico_file(app_img, f"{icons_dir}/app.ico")
    
    # Create different sizes of app icon
    for size in [16, 32, 48, 64, 128, 256]:
        if size != 256:
            resized = app_img.resize((size, size), Image.Resampling.LANCZOS)
            resized.save(f"{icons_dir}/app_{size}x{size}.ico", format='ICO')
    
    # Create status icons
    print("Creating status icons...")
    status_icons = create_status_icons()
    for name, img in status_icons.items():
        save_ico_file(img, f"{icons_dir}/{name}.ico")
    
    # Create function icons
    print("Creating function icons...")
    func_icons = create_function_icons()
    for name, img in func_icons.items():
        save_ico_file(img, f"{icons_dir}/{name}.ico")
    
    print(f"Icons created successfully in {icons_dir}/")
    
    # List created files
    for file in os.listdir(icons_dir):
        if file.endswith('.ico'):
            print(f"  - {file}")

if __name__ == "__main__":
    try:
        main()
    except ImportError as e:
        print(f"Error: {e}")
        print("Please install Pillow: pip install Pillow")
        sys.exit(1)
    except Exception as e:
        print(f"Error creating icons: {e}")
        sys.exit(1)
