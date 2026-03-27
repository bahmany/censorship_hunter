$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

Write-Host "========================================"
Write-Host "huntercensor Setup Builder (PowerShell)"
Write-Host "========================================"
Write-Host ""

Write-Host "[1/4] Building native app and syncing staging artifacts..."
& "$repoRoot\hunter_cpp\build.bat" --no-pause
if ($LASTEXITCODE -ne 0) {
    throw "build.bat failed with exit code $LASTEXITCODE"
}

Write-Host "[2/4] Verifying required installer staging files..."
$required = @(
    "installer\staging\huntercensor.exe",
    "installer\staging\bin\sing-box.exe",
    "installer\staging\app_icon.ico"
)

$missing = @()
foreach ($rel in $required) {
    $full = Join-Path $repoRoot $rel
    if (-not (Test-Path -LiteralPath $full)) {
        $missing += $rel
    }
}

if ($missing.Count -gt 0) {
    $missingList = ($missing -join ", ")
    throw "Staging is incomplete. Missing: $missingList"
}

Write-Host "[3/4] Running Inno Setup compiler..."
$isccCandidates = @(
    "C:\Program Files (x86)\Inno Setup 6\iscc.exe",
    "C:\Program Files\Inno Setup 6\iscc.exe"
)
$iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) {
    throw "Inno Setup compiler not found. Install Inno Setup 6."
}

$issFile = Join-Path $repoRoot "installer\hunter_setup.iss"
& $iscc $issFile
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compile failed with exit code $LASTEXITCODE"
}

Write-Host "[4/4] Done."
Write-Host "[OK] Setup executable created under installer\output\"
