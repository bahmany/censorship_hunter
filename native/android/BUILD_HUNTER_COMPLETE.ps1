# Build script for Hunter Android app - Complete and Final

# Set JAVA_HOME to Android Studio JBR
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"

# Ensure we are in the project directory
Set-Location -Path $PSScriptRoot

# Clean previous build (optional)
Write-Host "Cleaning previous build..."
.\\gradlew.bat clean

# Build the app
Write-Host "Building Hunter Android app..."
.\\gradlew.bat build

# Check if build succeeded
if ($LASTEXITCODE -eq 0) {
    Write-Host "Build successful!"
    Write-Host "Debug APK: $PSScriptRoot\\app\\build\\outputs\\apk\\debug\\app-debug.apk"
    Write-Host "Release APK: $PSScriptRoot\\app\\build\\outputs\\apk\\release\\app-release.apk"
} else {
    Write-Host "Build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}
