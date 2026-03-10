# Hunter v1.0.0 Release Script (PowerShell)
# This script creates a complete GitHub release

param(
    [string]$ReleaseDir = "Hunter-v1.0.0",
    [string]$ZipName = "Hunter-v1.0.0-Final.zip"
)

Write-Host "🚀 Creating Hunter v1.0.0 Release..." -ForegroundColor Green

# Check if we're in the right directory
if (-not (Test-Path "hunter_cli.exe") -or -not (Test-Path "hunter_dashboard.exe")) {
    Write-Host "❌ Error: Not in the correct directory. Run from hunter/bin/" -ForegroundColor Red
    exit 1
}

# Clean up previous release
if (Test-Path $ReleaseDir) {
    Remove-Item -Recurse -Force $ReleaseDir
}

# Create release directory
New-Item -ItemType Directory -Force $ReleaseDir | Out-Null

# Copy all files
Write-Host "📦 Copying files..." -ForegroundColor Blue
Copy-Item -Recurse -Force * $ReleaseDir\

# Create ZIP
Write-Host "🗜️ Creating ZIP archive..." -ForegroundColor Blue
Compress-Archive -Path "$ReleaseDir\*" -DestinationPath $ZipName -Force

# Clean up
Remove-Item -Recurse -Force $ReleaseDir

# Get file size
$zipInfo = Get-ChildItem $ZipName
Write-Host "✅ Release package created: $ZipName" -ForegroundColor Green
Write-Host "📊 Size: $([math]::Round($zipInfo.Length / 1MB, 2)) MB" -ForegroundColor Green
Write-Host "📅 Created: $($zipInfo.LastWriteTime)" -ForegroundColor Green

# Instructions for manual GitHub release
Write-Host ""
Write-Host "📋 Manual GitHub Release Instructions:" -ForegroundColor Yellow
Write-Host "1. Go to: https://github.com/bahmany/censorship_hunter/releases/new" -ForegroundColor White
Write-Host "2. Tag: v1.0.0" -ForegroundColor White
Write-Host "3. Title: Hunter v1.0.0 - Complete Anti-Censorship Solution" -ForegroundColor White
Write-Host "4. Upload: $ZipName" -ForegroundColor White
Write-Host "5. Copy release notes from RELEASE_NOTES.md" -ForegroundColor White
Write-Host "6. Check 'Set as the latest release'" -ForegroundColor White
Write-Host "7. Click 'Publish release'" -ForegroundColor White

Write-Host ""
Write-Host "🔗 Direct release URL:" -ForegroundColor Cyan
Write-Host "https://github.com/bahmany/censorship_hunter/releases/new" -ForegroundColor Cyan

# Open browser (optional)
# Start-Process "https://github.com/bahmany/censorship_hunter/releases/new"

Write-Host ""
Write-Host "🎉 Release preparation complete!" -ForegroundColor Green
