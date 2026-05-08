################################################################################
# build.ps1 -- Configure and build Kv8 on Windows
#
# Usage (from the repo root):
#   .\scripts\build.ps1               # Release build (default)
#   .\scripts\build.ps1 -Debug        # Debug build
#   .\scripts\build.ps1 -Test         # build + run CTest
#   .\scripts\build.ps1 -Install      # build + cmake --install
#   .\scripts\build.ps1 -Clean        # wipe build/ first, then build
#   .\scripts\build.ps1 -Jobs 8       # parallel jobs (default: CPU count)
#   .\scripts\build.ps1 -Debug -Test  # combine flags freely
#
# Requires:
#   - VCPKG_ROOT env variable (set by bootstrap.ps1)
#   - Visual Studio 2022 Build Tools with C++ workload
#   - CMake >= 3.20 on PATH
################################################################################
param(
    [switch]$Debug,
    [switch]$Test,
    [switch]$Install,
    [switch]$Clean,
    [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
$Config  = if ($Debug) { "Debug"   } else { "Release" }
$Preset  = if ($Debug) { "win-x64-debug" } else { "win-x64-release" }

Write-Host ""
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "  Kv8 Build -- Windows ($Config)" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# Validate VCPKG_ROOT
# ---------------------------------------------------------------------------
if (-not $env:VCPKG_ROOT) {
    Write-Host "[ERROR] VCPKG_ROOT is not set." -ForegroundColor Red
    Write-Host "        Run .\scripts\bootstrap.ps1 first, then open a new terminal." -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) {
    Write-Host "[ERROR] vcpkg.exe not found at VCPKG_ROOT=$env:VCPKG_ROOT" -ForegroundColor Red
    Write-Host "        Run .\scripts\bootstrap.ps1 first." -ForegroundColor Yellow
    exit 1
}
Write-Host "  VCPKG_ROOT = $env:VCPKG_ROOT" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Optional clean
# ---------------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "`n==> Cleaning build directory..."  -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
    Write-Host "  Build directory removed." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
Write-Host "`n==> Configuring (cmake --preset $Preset)..." -ForegroundColor Yellow
Push-Location $RepoRoot
cmake --preset $Preset
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Host "[ERROR] CMake configure failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}
Pop-Location
Write-Host "  Configure: OK" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
Write-Host "`n==> Building ($Config, $Jobs parallel jobs)..." -ForegroundColor Yellow
cmake --build $BuildDir --config $Config --parallel $Jobs
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "  Build: OK" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Install (optional)
# ---------------------------------------------------------------------------
if ($Install) {
    Write-Host "`n==> Installing artifacts..." -ForegroundColor Yellow
    cmake --install $BuildDir --config $Config
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Install failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    $OutBin = Join-Path $BuildDir "_output_\bin"
    Write-Host "  Artifacts installed to: $OutBin" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Test (optional)
# ---------------------------------------------------------------------------
if ($Test) {
    Write-Host "`n==> Running tests..." -ForegroundColor Yellow
    ctest --test-dir $BuildDir -C $Config --output-on-failure --parallel $Jobs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Tests failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "  Tests: PASS" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "  Build complete ($Config)." -ForegroundColor Green

$OutBin = Join-Path $BuildDir "_output_\bin"
if (Test-Path $OutBin) {
    Write-Host "  Binaries: $OutBin" -ForegroundColor White
}

Write-Host ""
Write-Host "  Quick commands:" -ForegroundColor White
Write-Host "    .\scripts\build.ps1 -Test            # rebuild + test" -ForegroundColor White
Write-Host "    .\scripts\test_kafka_e2e.ps1 -SkipBuild  # E2E test" -ForegroundColor White
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host ""
