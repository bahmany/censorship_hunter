#!/bin/bash

# Hunter v1.0.0 Release Script
# This script creates a complete GitHub release

echo "🚀 Creating Hunter v1.0.0 Release..."

# Check if we're in the right directory
if [ ! -f "hunter_cli.exe" ] || [ ! -f "hunter_dashboard.exe" ]; then
    echo "❌ Error: Not in the correct directory. Run from hunter/bin/"
    exit 1
fi

# Create release directory
RELEASE_DIR="Hunter-v1.0.0"
rm -rf "$RELEASE_DIR"
mkdir "$RELEASE_DIR"

# Copy all files
echo "📦 Copying files..."
cp -r * "$RELEASE_DIR/"

# Create ZIP
echo "🗜️ Creating ZIP archive..."
cd "$RELEASE_DIR"
zip -r "../Hunter-v1.0.0-Final.zip" .
cd ..

# Clean up
rm -rf "$RELEASE_DIR"

echo "✅ Release package created: Hunter-v1.0.0-Final.zip"
echo "📊 Size: $(du -h Hunter-v1.0.0-Final.zip | cut -f1)"

# Instructions for manual GitHub release
echo ""
echo "📋 Manual GitHub Release Instructions:"
echo "1. Upload Hunter-v1.0.0-Final.zip to GitHub Releases"
echo "2. Create release with tag v1.0.0"
echo "3. Copy content from RELEASE_NOTES.md"
echo "4. Set as latest release"
echo ""
echo "🔗 Release URL: https://github.com/bahmany/censorship_hunter/releases/new"
