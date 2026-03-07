param(
  [string]$Configuration = "Release",
  [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

function Find-Exe($name) {
  try {
    $p = (Get-Command $name -ErrorAction Stop).Source
    return $p
  } catch {
    return $null
  }
}

$flutter = Find-Exe "flutter"
if (-not $flutter) {
  throw "flutter not found in PATH. Install Flutter and ensure flutter is in PATH."
}

$heat = Find-Exe "heat.exe"
$candle = Find-Exe "candle.exe"
$light = Find-Exe "light.exe"

if (-not $heat -or -not $candle -or -not $light) {
  Write-Host "WiX Toolset not found (heat/candle/light)."
  Write-Host "Install WiX Toolset v3 (recommended) and make sure these tools are in PATH."
  Write-Host "After install, reopen terminal and re-run this script."
  exit 1
}

$root = Split-Path -Parent $PSScriptRoot
$pubspec = Join-Path $root "pubspec.yaml"
if (-not (Test-Path $pubspec)) { throw "pubspec.yaml not found: $pubspec" }

$versionLine = (Get-Content $pubspec | Where-Object { $_ -match '^version:' } | Select-Object -First 1)
if (-not $versionLine) { throw "version: not found in pubspec.yaml" }

# pubspec version format: 1.2.3+4
$verRaw = ($versionLine -replace '^version:\s*', '').Trim()
$verParts = $verRaw.Split('+')
$productVersion = $verParts[0]

# WiX Product.Version must be 3-part dotted version.
$pvParts = $productVersion.Split('.')
if ($pvParts.Length -lt 3) {
  throw "pubspec version must be Major.Minor.Patch (e.g. 1.0.0). Found: $productVersion"
}
$productVersion = ($pvParts[0..2] -join '.')

Write-Host "Building Flutter Windows $Configuration ..."
Push-Location $root
& $flutter build windows --$($Configuration.ToLower())
Pop-Location

$releaseDir = Join-Path $root "build\windows\$Platform\runner\$Configuration"
if (-not (Test-Path $releaseDir)) { throw "Release output not found: $releaseDir" }

$outDir = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$harvestWxs = Join-Path $outDir "HarvestedFiles.wxs"
$productWxs = Join-Path $PSScriptRoot "Product.wxs"

Write-Host "Harvesting files from: $releaseDir"
& $heat dir $releaseDir `
  -nologo `
  -gg `
  -srd `
  -sreg `
  -sfrag `
  -dr INSTALLFOLDER `
  -cg ReleaseFiles `
  -out $harvestWxs

$wixobjProduct = Join-Path $outDir "Product.wixobj"
$wixobjHarvest = Join-Path $outDir "HarvestedFiles.wixobj"

Write-Host "Compiling WiX sources..."
& $candle -nologo `
  -dProductVersion=$productVersion `
  -dProductName="Hunter Dashboard" `
  -dManufacturer="bahmany" `
  -out $outDir\ `
  $productWxs `
  $harvestWxs

# candle outputs .wixobj beside inputs (in outDir because -out)
# Resolve actual names
$wixobjs = Get-ChildItem $outDir -Filter "*.wixobj" | Select-Object -ExpandProperty FullName

$msiPath = Join-Path $outDir "HunterDashboard-$productVersion-$Configuration.msi"
Write-Host "Linking MSI: $msiPath"
& $light -nologo -ext WixUIExtension -out $msiPath $wixobjs

Write-Host "DONE: $msiPath"
