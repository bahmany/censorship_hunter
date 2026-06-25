# dev.ps1 - Fast development sync + restart (no image rebuild)
#
# Usage:
#   .\dev.ps1                  # Sync changed files + restart backend only
#   .\dev.ps1 -BackendOnly     # Sync C++ + restart backend only
#   .\dev.ps1 -WebUIOnly       # Sync web-ui + restart web-ui only
#   .\dev.ps1 -RebuildBackend  # Rebuild backend binary inside container (no image rebuild)
#   .\dev.ps1 -Watch           # Watch for changes and auto-sync+restart
#   .\dev.ps1 -Logs            # Stream backend logs
#   .\dev.ps1 -Status          # Check status
#
# This is the FAST path - no Docker image rebuilds.
# Source files are synced and containers restarted in seconds.

param(
    [switch]$BackendOnly,
    [switch]$WebUIOnly,
    [switch]$RebuildBackend,
    [switch]$Watch,
    [switch]$Logs,
    [switch]$Status,
    [string]$RemoteHost = "192.168.1.77",
    [string]$RemoteUser = "abharcable",
    [string]$RemotePass = "P@ssw0rd",
    [string]$RemotePath = "/home/abharcable/censorship_hunter"
)

$ErrorActionPreference = "Stop"
$Plink = "C:\Program Files\PuTTY\plink.exe"
$Pscp = "C:\Program Files\PuTTY\pscp.exe"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Log {
    param([string]$Msg, [string]$Color = "White")
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] $Msg" -ForegroundColor $Color
}

function Sync-Cpp {
    Log "Syncing C++ source..." "Green"
    $dirs = @("hunter_cpp\include", "hunter_cpp\src", "hunter_cpp\scripts", "hunter_cpp\CMakeLists.txt")
    foreach ($d in $dirs) {
        $local = Join-Path $RepoRoot $d
        $remote = "$RemoteUser@${RemoteHost}:$RemotePath/$d"
        if (Test-Path $local) {
            & $Pscp -pw $RemotePass -r $local $remote 2>&1 | Out-Null
        }
    }
    Log "C++ sync done." "Green"
}

function Sync-WebUI {
    Log "Syncing web-ui source..." "Green"
    $items = @("web-ui\app", "web-ui\public", "web-ui\next.config.js", "web-ui\package.json", "web-ui\tsconfig.json", "web-ui\tailwind.config.ts", "web-ui\postcss.config.js")
    foreach ($item in $items) {
        $local = Join-Path $RepoRoot $item
        $remote = "$RemoteUser@${RemoteHost}:$RemotePath/$item"
        if (Test-Path $local) {
            & $Pscp -pw $RemotePass -r $local $remote 2>&1 | Out-Null
        }
    }
    Log "Web-UI sync done." "Green"
}

function Restart-Backend {
    Log "Restarting backend container..." "Yellow"
    & $Plink -ssh "$RemoteUser@$RemoteHost" -pw $RemotePass -batch "cd $RemotePath && docker-compose restart hunter-backend 2>&1" 2>&1 | ForEach-Object {
        Log $_ "Gray"
    }
}

function Restart-WebUI {
    Log "Restarting web-ui container..." "Yellow"
    & $Plink -ssh "$RemoteUser@$RemoteHost" -pw $RemotePass -batch "cd $RemotePath && docker-compose restart hunter-web-ui 2>&1" 2>&1 | ForEach-Object {
        Log $_ "Gray"
    }
}

function Rebuild-BackendInContainer {
    Log "Rebuilding backend binary inside container (incremental)..." "Green"
    $cmd = "docker exec hunter-backend sh -c 'cd /app/hunter_cpp && ninja -C /app/build hunter_backend 2>&1 && cp /app/build/hunter_backend /app/hunter_backend && echo REBUILD_OK'"
    & $Plink -ssh "$RemoteUser@$RemoteHost" -pw $RemotePass -batch $cmd 2>&1 | ForEach-Object {
        Log $_ "Gray"
    }
    if ($LASTEXITCODE -eq 0) {
        Log "Binary rebuilt. Restarting backend..." "Green"
        Restart-Backend
    } else {
        Log "In-container rebuild failed. Falling back to full restart." "Red"
        Restart-Backend
    }
}

function Show-Status {
    Log "=== Container Status ===" "Yellow"
    & $Plink -ssh "$RemoteUser@$RemoteHost" -pw $RemotePass -batch "docker ps --format '{{.Names}}\t{{.Status}}\t{{.Ports}}' | grep hunter 2>&1" 2>&1 | ForEach-Object {
        Write-Host "  $_" -ForegroundColor Gray
    }
}

# ─── Main Logic ───

if ($Status) {
    Show-Status
    exit 0
}

if ($Logs) {
    Log "Tailing backend logs (Ctrl+C to stop)..." "Yellow"
    & $Plink -ssh "$RemoteUser@$RemoteHost" -pw $RemotePass -batch "docker logs -f --tail=50 hunter-backend 2>&1" 2>&1 | ForEach-Object {
        Write-Host $_ -ForegroundColor Gray
    }
    exit 0
}

if ($Watch) {
    Log "Watching for file changes. Press Ctrl+C to stop." "Yellow"
    
    # Track last modified times
    $watchPaths = @(
        @{Path = Join-Path $RepoRoot "hunter_cpp\src"; Type = "cpp"; Service = "backend"},
        @{Path = Join-Path $RepoRoot "hunter_cpp\include"; Type = "cpp"; Service = "backend"},
        @{Path = Join-Path $RepoRoot "web-ui\app"; Type = "ui"; Service = "webui"}
    )
    
    $lastCheck = Get-Date
    
    while ($true) {
        Start-Sleep -Seconds 3
        
        $cppChanged = $false
        $uiChanged = $false
        
        foreach ($wp in $watchPaths) {
            if (-not (Test-Path $wp.Path)) { continue }
            $changed = Get-ChildItem -Path $wp.Path -Recurse -File | Where-Object {
                $_.LastWriteTime -gt $lastCheck -and $_.Extension -in '.cpp', '.h', '.tsx', '.ts', '.js', '.json'
            }
            if ($changed) {
                if ($wp.Service -eq "backend") { $cppChanged = $true }
                else { $uiChanged = $true }
                foreach ($c in $changed) {
                    Log "Changed: $($c.FullName.Replace($RepoRoot, ''))" "Cyan"
                }
            }
        }
        
        if ($cppChanged) {
            $lastCheck = Get-Date
            Sync-Cpp
            if ($RebuildBackend) {
                Rebuild-BackendInContainer
            } else {
                Restart-Backend
            }
        }
        
        if ($uiChanged) {
            $lastCheck = Get-Date
            Sync-WebUI
            Restart-WebUI
        }
    }
}

# Single-shot mode
if ($RebuildBackend) {
    Sync-Cpp
    Rebuild-BackendInContainer
    Start-Sleep -Seconds 3
    Show-Status
    exit 0
}

if ($BackendOnly) {
    Sync-Cpp
    Restart-Backend
    Start-Sleep -Seconds 3
    Show-Status
    exit 0
}

if ($WebUIOnly) {
    Sync-WebUI
    Restart-WebUI
    Start-Sleep -Seconds 3
    Show-Status
    exit 0
}

# Default: sync both + restart both
Sync-Cpp
Sync-WebUI
Restart-Backend
Restart-WebUI
Start-Sleep -Seconds 3
Show-Status
