# deploy.ps1 - Streaming SSH deploy with real-time output
# 
# Usage:
#   .\deploy.ps1                          # Sync code + restart containers (fast)
#   .\deploy.ps1 -Rebuild                 # Full image rebuild (when deps change)
#   .\deploy.ps1 -RebuildBackend          # Rebuild backend image only
#   .\deploy.ps1 -RebuildWebUI            # Rebuild web-ui image only
#   .\deploy.ps1 -Logs                    # Tail container logs
#   .\deploy.ps1 -Status                  # Check container status
#   .\deploy.ps1 -BuildLogs               # Tail last build log
#
# All commands stream output in real-time. No silent waits.

param(
    [switch]$Rebuild,
    [switch]$RebuildBackend,
    [switch]$RebuildWebUI,
    [switch]$Logs,
    [switch]$Status,
    [switch]$BuildLogs,
    [switch]$NoCache,
    [string]$RemoteHost = "192.168.1.77",
    [string]$RemoteUser = "abharcable",
    [string]$RemotePass = "P@ssw0rd",
    [string]$RemotePath = "/home/abharcable/censorship_hunter"
)

$ErrorActionPreference = "Stop"
$Plink = "C:\Program Files\PuTTY\plink.exe"
$Pscp = "C:\Program Files\PuTTY\pscp.exe"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Invoke-SshStream {
    param([string]$Command, [int]$TimeoutSec = 0)
    
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] SSH> $Command" -ForegroundColor Cyan
    
    if ($TimeoutSec -gt 0) {
        $proc = Start-Process -FilePath $Plink -ArgumentList @(
            "-ssh", "$RemoteUser@$RemoteHost", "-pw", $RemotePass, "-batch", $Command
        ) -NoNewWindow -PassThru -RedirectStandardOutput "ssh_stdout.tmp" -RedirectStandardError "ssh_stderr.tmp"
        
        $waited = 0
        while (-not $proc.HasExited -and ($TimeoutSec -eq 0 -or $waited -lt $TimeoutSec)) {
            Start-Sleep -Milliseconds 500
            $waited += 500
            if ($waited % 10000 -eq 0) {
                $ts = Get-Date -Format "HH:mm:ss"
                Write-Host "[$ts] ... still running ($($waited/1000)s)" -ForegroundColor DarkGray
            }
        }
        
        if (-not $proc.HasExited) {
            Write-Host "[TIMEOUT] Process exceeded ${TimeoutSec}s" -ForegroundColor Yellow
            return $false
        }
        
        if (Test-Path "ssh_stdout.tmp") {
            $stdout = Get-Content "ssh_stdout.tmp" -Raw 2>$null
            if ($stdout) { Write-Host $stdout -ForegroundColor White }
            Remove-Item "ssh_stdout.tmp" -Force 2>$null
        }
        if (Test-Path "ssh_stderr.tmp") {
            $stderr = Get-Content "ssh_stderr.tmp" -Raw 2>$null
            if ($stderr) { Write-Host $stderr -ForegroundColor Red }
            Remove-Item "ssh_stderr.tmp" -Force 2>$null
        }
        
        return $proc.ExitCode -eq 0
    } else {
        # No timeout - stream directly
        & $Plink -ssh "$RemoteUser@$RemoteHost" -pw $RemotePass -batch $Command 2>&1 | ForEach-Object {
            $ts = Get-Date -Format "HH:mm:ss"
            Write-Host "[$ts] $_" -ForegroundColor Gray
        }
        return $LASTEXITCODE -eq 0
    }
}

function Sync-Files {
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] Syncing source files to server..." -ForegroundColor Green
    
    # Sync C++ source (not build artifacts or third_party)
    $cppFiles = @(
        "hunter_cpp\include",
        "hunter_cpp\src",
        "hunter_cpp\scripts",
        "hunter_cpp\CMakeLists.txt"
    )
    
    foreach ($f in $cppFiles) {
        $localPath = Join-Path $RepoRoot $f
        $remoteDest = "$RemoteUser@${RemoteHost}:$RemotePath/$f"
        if (Test-Path $localPath) {
            $ts = Get-Date -Format "HH:mm:ss"
            Write-Host "[$ts]   Uploading $f..." -ForegroundColor DarkGray
            & $Pscp -pw $RemotePass -r $localPath $remoteDest 2>&1 | ForEach-Object {
                Write-Host "  $_" -ForegroundColor DarkGray
            }
        }
    }
    
    # Sync web-ui source (not node_modules or .next)
    $uiFiles = @(
        "web-ui\app",
        "web-ui\public",
        "web-ui\next.config.js",
        "web-ui\package.json",
        "web-ui\tsconfig.json",
        "web-ui\tailwind.config.ts",
        "web-ui\postcss.config.js"
    )
    
    foreach ($f in $uiFiles) {
        $localPath = Join-Path $RepoRoot $f
        $remoteDest = "$RemoteUser@${RemoteHost}:$RemotePath/$f"
        if (Test-Path $localPath) {
            $ts = Get-Date -Format "HH:mm:ss"
            Write-Host "[$ts]   Uploading $f..." -ForegroundColor DarkGray
            & $Pscp -pw $RemotePass -r $localPath $remoteDest 2>&1 | ForEach-Object {
                Write-Host "  $_" -ForegroundColor DarkGray
            }
        }
    }
    
    # Sync top-level files
    $topFiles = @("Dockerfile", "Dockerfile.dev", "docker-compose.yml", "docker-compose.dev.yml", "dev-entrypoint.sh", "web-ui\Dockerfile.dev")
    foreach ($f in $topFiles) {
        $localPath = Join-Path $RepoRoot $f
        $remoteDest = "$RemoteUser@${RemoteHost}:$RemotePath/$f"
        if (Test-Path $localPath) {
            & $Pscp -pw $RemotePass $localPath $remoteDest 2>&1 | ForEach-Object {
                Write-Host "  $_" -ForegroundColor DarkGray
            }
        }
    }
    
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] Sync complete." -ForegroundColor Green
}

function Invoke-RemoteBuild {
    param([string]$Services = "", [switch]$NoCacheFlag)
    
    $noCacheArg = if ($NoCacheFlag) { "--no-cache" } else { "" }
    $serviceArg = if ($Services) { $Services } else { "" }
    
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] Starting remote build: docker-compose build $noCacheArg $serviceArg" -ForegroundColor Green
    
    # Use a named pipe approach for streaming
    $buildCmd = "cd $RemotePath && docker-compose build $noCacheArg $serviceArg 2>&1 | while IFS= read -r line; do echo `"[$(date +%H:%M:%S)] $line`"; done; echo BUILD_EXIT_CODE=$?"
    
    & $Plink -ssh "$RemoteUser@$RemoteHost" -pw $RemotePass -batch $buildCmd 2>&1 | ForEach-Object {
        Write-Host $_ -ForegroundColor Gray
    }
    
    return $LASTEXITCODE -eq 0
}

# ─── Main Logic ───

if ($Status) {
    Write-Host "=== Container Status ===" -ForegroundColor Yellow
    Invoke-SshStream "cd $RemotePath && docker-compose ps 2>&1; echo '---'; docker ps --format '{{.Names}}\t{{.Status}}\t{{.Ports}}' | grep hunter"
    exit 0
}

if ($Logs) {
    Write-Host "=== Tailing container logs (Ctrl+C to stop) ===" -ForegroundColor Yellow
    & $Plink -ssh "$RemoteUser@$RemoteHost" -pw $RemotePass -batch "cd $RemotePath && docker-compose logs -f --tail=50 2>&1" 2>&1 | ForEach-Object {
        Write-Host $_ -ForegroundColor Gray
    }
    exit 0
}

if ($BuildLogs) {
    Write-Host "=== Last build log ===" -ForegroundColor Yellow
    Invoke-SshStream "tail -50 /tmp/hunter-build*.log 2>/dev/null || echo 'No build logs found'"
    exit 0
}

# Default action: sync + restart (fast path)
if ($Rebuild -or $RebuildBackend -or $RebuildWebUI) {
    Sync-Files
    
    $services = ""
    if ($RebuildBackend) { $services = "hunter-backend" }
    if ($RebuildWebUI) { $services = "hunter-web-ui" }
    if ($Rebuild) { $services = "" }
    
    $ok = Invoke-RemoteBuild -Services $services -NoCacheFlag:$NoCache
    if (-not $ok) {
        Write-Host "[ERROR] Build failed!" -ForegroundColor Red
        exit 1
    }
    
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] Restarting containers..." -ForegroundColor Green
    Invoke-SshStream "cd $RemotePath && docker-compose up -d $services 2>&1"
    
    Start-Sleep -Seconds 5
    Invoke-SshStream "docker ps --format '{{.Names}}\t{{.Status}}' | grep hunter"
} else {
    # Fast path: just sync files and restart containers
    Sync-Files
    
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] Restarting containers (no rebuild)..." -ForegroundColor Green
    Invoke-SshStream "cd $RemotePath && docker-compose restart hunter-backend hunter-web-ui 2>&1"
    
    Start-Sleep -Seconds 5
    Invoke-SshStream "docker ps --format '{{.Names}}\t{{.Status}}' | grep hunter"
}

$ts = Get-Date -Format "HH:mm:ss"
Write-Host "[$ts] Done." -ForegroundColor Green
