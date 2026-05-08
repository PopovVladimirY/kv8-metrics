################################################################################
# bootstrap.ps1 -- Kv8 prerequisites setup for Windows
#
# Installs or verifies every build dependency and sets VCPKG_ROOT in the user
# environment.  Safe to re-run: each step is idempotent.
#
# Usage (from the repo root):
#   .\scripts\bootstrap.ps1              # install everything
#   .\scripts\bootstrap.ps1 -VcpkgRoot C:\tools\vcpkg   # custom vcpkg path
#   .\scripts\bootstrap.ps1 -SkipVcpkg  # skip vcpkg (already installed)
#
# Prerequisites this script installs / checks:
#   - Git                    (via winget if missing)
#   - CMake >= 3.20          (via winget if missing)
#   - Visual Studio 2022 Build Tools  (reported only; not silently installed)
#   - vcpkg                  (cloned + bootstrapped at VcpkgRoot)
#   - vcpkg packages         (librdkafka, glfw3, opengl, nlohmann-json, libuv,
#                             zlib, openssl)  via vcpkg.json manifest
#   - Docker Desktop         (required for Kafka; detected, not installed)
################################################################################
param(
    [string]$VcpkgRoot  = "$env:USERPROFILE\vcpkg",
    [switch]$SkipVcpkg
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Pass  { param([string]$msg) Write-Host "  [PASS] $msg" -ForegroundColor Green }
function Warn  { param([string]$msg) Write-Host "  [WARN] $msg" -ForegroundColor Yellow }
function Fail  { param([string]$msg) Write-Host "  [FAIL] $msg" -ForegroundColor Red }
function Step  { param([string]$msg) Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Die   { param([string]$msg) Write-Host "`n[ERROR] $msg" -ForegroundColor Red; exit 1 }

function Which {
    param([string]$cmd)
    return Get-Command $cmd -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "  Kv8 Bootstrap -- Windows" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# Step 1: Git
# ---------------------------------------------------------------------------
Step "Checking Git"
if (Which "git") {
    $v = (git --version) -replace "git version ",""
    Pass "Git $v"
} else {
    Warn "Git not found -- installing via winget"
    winget install --id Git.Git -e --source winget --accept-package-agreements --accept-source-agreements
    if (-not (Which "git")) { Die "Git installation failed. Install manually from https://git-scm.com" }
    Pass "Git installed"
}

# ---------------------------------------------------------------------------
# Step 2: CMake
# ---------------------------------------------------------------------------
Step "Checking CMake (>= 3.20 required)"
if (Which "cmake") {
    $raw = (cmake --version | Select-Object -First 1) -replace "cmake version ",""
    $parts = $raw.Split(".")
    $major = [int]$parts[0]; $minor = [int]$parts[1]
    if ($major -gt 3 -or ($major -eq 3 -and $minor -ge 20)) {
        Pass "CMake $raw"
    } else {
        Warn "CMake $raw is too old -- installing a newer version via winget"
        winget install --id Kitware.CMake -e --source winget --accept-package-agreements --accept-source-agreements
        Pass "CMake updated"
    }
} else {
    Warn "CMake not found -- installing via winget"
    winget install --id Kitware.CMake -e --source winget --accept-package-agreements --accept-source-agreements
    if (-not (Which "cmake")) { Die "CMake installation failed. Install manually from https://cmake.org/download/" }
    Pass "CMake installed"
}

# ---------------------------------------------------------------------------
# Step 3: Visual Studio / MSVC
# ---------------------------------------------------------------------------
Step "Checking Visual Studio 2022 Build Tools"
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vsWhere) {
    $vsInfo = & $vsWhere -latest -products * -requires Microsoft.VisualCpp.Tools.HostX64.TargetX64 -property displayName 2>$null
    if ($vsInfo) {
        Pass $vsInfo
    } else {
        Warn "Visual Studio found but C++ workload may be missing."
        Warn "Open the Visual Studio Installer and enable:"
        Warn "  'Desktop development with C++'"
    }
} else {
    Warn "Visual Studio Installer not found."
    Warn "Install Visual Studio 2022 (Community or Build Tools) with the"
    Warn "'Desktop development with C++' workload from:"
    Warn "  https://visualstudio.microsoft.com/downloads/"
    Warn "Then re-run this script."
    # Do not exit -- user may already have MSVC on PATH via vcvars
}

# ---------------------------------------------------------------------------
# Step 4: vcpkg
# ---------------------------------------------------------------------------
if (-not $SkipVcpkg) {
    Step "Setting up vcpkg at $VcpkgRoot"

    # Clone if not already there
    if (-not (Test-Path "$VcpkgRoot\.git")) {
        Write-Host "  Cloning vcpkg..."
        git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
    } else {
        Write-Host "  vcpkg repo already present -- updating"
        Push-Location $VcpkgRoot
        git pull --ff-only
        Pop-Location
    }

    # Bootstrap (downloads a pre-built vcpkg.exe)
    if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
        Write-Host "  Bootstrapping vcpkg..."
        & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
        if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) { Die "vcpkg bootstrap failed" }
    } else {
        Pass "vcpkg.exe already present"
    }

    # Set VCPKG_ROOT in user environment (persists across sessions)
    $current = [System.Environment]::GetEnvironmentVariable("VCPKG_ROOT", "User")
    if ($current -ne $VcpkgRoot) {
        [System.Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "User")
        Write-Host "  VCPKG_ROOT set to $VcpkgRoot (user environment)"
    }
    # Update current session too
    $env:VCPKG_ROOT = $VcpkgRoot
    Pass "VCPKG_ROOT = $VcpkgRoot"
} else {
    Step "Skipping vcpkg setup (-SkipVcpkg)"
    if (-not $env:VCPKG_ROOT) { Die "VCPKG_ROOT is not set and -SkipVcpkg was given. Set it manually." }
    if (-not (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) { Die "vcpkg.exe not found at VCPKG_ROOT=$env:VCPKG_ROOT" }
    Pass "Using existing vcpkg at $env:VCPKG_ROOT"
}

# ---------------------------------------------------------------------------
# Step 5: install vcpkg packages (manifest mode -- reads vcpkg.json)
# ---------------------------------------------------------------------------
Step "Installing vcpkg packages (manifest mode)"
$vcpkgExe = "$env:VCPKG_ROOT\vcpkg.exe"
if (Test-Path "$RepoRoot\vcpkg.json") {
    Write-Host "  Found vcpkg.json manifest -- running vcpkg install"
    Push-Location $RepoRoot
    & $vcpkgExe install --triplet x64-windows 2>&1 | Out-Host
    Pop-Location
    Pass "vcpkg packages installed"
} else {
    # No manifest -- install the known required packages explicitly
    Write-Host "  No vcpkg.json found -- installing packages explicitly"
    $packages = @(
        "librdkafka:x64-windows",
        "glfw3:x64-windows",
        "opengl:x64-windows",
        "nlohmann-json:x64-windows",
        "libuv:x64-windows",
        "zlib:x64-windows",
        "openssl:x64-windows"
    )
    foreach ($pkg in $packages) {
        Write-Host "  Installing $pkg ..."
        & $vcpkgExe install $pkg 2>&1 | Out-Host
    }
    Pass "vcpkg packages installed"
}

# ---------------------------------------------------------------------------
# Step 6: Docker
# ---------------------------------------------------------------------------
Step "Checking Docker (required for Kafka integration tests)"
if (Which "docker") {
    $dv = (docker --version) -replace "Docker version ",""
    try {
        docker info 2>&1 | Out-Null
        Pass "Docker $dv (daemon running)"
    } catch {
        Warn "Docker $dv found but daemon is not running."
        Warn "Start Docker Desktop before running integration tests."
    }
} else {
    Warn "Docker not found."
    Warn "Install Docker Desktop from https://www.docker.com/products/docker-desktop"
    Warn "Required for: Kafka broker, kv8bench, kv8bench_log, test_kafka_e2e"
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "  Bootstrap complete." -ForegroundColor Green
Write-Host ""
Write-Host "  Next steps:" -ForegroundColor White
Write-Host "    .\scripts\build.ps1           # configure + build (Release)" -ForegroundColor White
Write-Host "    .\scripts\build.ps1 -Debug    # Debug build" -ForegroundColor White
Write-Host "    .\scripts\build.ps1 -Test     # build + run tests" -ForegroundColor White
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host ""
