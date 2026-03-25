################################################################################
# test_kafka_e2e.ps1 -- End-to-end test: Kafka + kv8feeder producer + kv8cli consumer
#
# Usage:
#   .\scripts\test_kafka_e2e.ps1                  # run test, keep Kafka running
#   .\scripts\test_kafka_e2e.ps1 -TearDown        # run test, then stop Kafka
#   .\scripts\test_kafka_e2e.ps1 -SkipBuild       # skip CMake build step
################################################################################

param(
    [switch]$TearDown,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$DockerDir = Join-Path $RepoRoot "docker"
$BuildDir = Join-Path $RepoRoot "build"

# Paths to installed executables
$OutputBin   = Join-Path $BuildDir "_output_\bin"
$ProducerExe = Join-Path $OutputBin "kv8feeder.exe"
$ConsumerExe = Join-Path $OutputBin "kv8cli.exe"

# Kafka connection
$Brokers  = "localhost:19092"
$User     = "kv8producer"
$Pass     = "kv8secret"
$Prefix   = "kv8/e2e_test"

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Kv8 End-to-End Test" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

################################################################################
# Step 1 — Build (unless skipped)
################################################################################
if (-not $SkipBuild) {
    Write-Host "[1/5] Building project..." -ForegroundColor Yellow

    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir | Out-Null
    }

    Push-Location $BuildDir
    try {
        cmake .. -G "Visual Studio 17 2022" -A x64 `
            -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" 2>&1 | Out-Host

        cmake --build . --config Release 2>&1 | Out-Host
        cmake --install . --config Release 2>&1 | Out-Host
    }
    finally {
        Pop-Location
    }

    if (-not (Test-Path $ProducerExe)) {
        Write-Host "[ERROR] Producer executable not found: $ProducerExe" -ForegroundColor Red
        exit 1
    }
    if (-not (Test-Path $ConsumerExe)) {
        Write-Host "[ERROR] Consumer executable not found: $ConsumerExe" -ForegroundColor Red
        exit 1
    }
    Write-Host "[1/5] Build complete." -ForegroundColor Green
}
else {
    Write-Host "[1/5] Build skipped." -ForegroundColor DarkGray
    if (-not (Test-Path $ProducerExe) -or -not (Test-Path $ConsumerExe)) {
        Write-Host "[ERROR] Executables not found. Run without -SkipBuild first." -ForegroundColor Red
        exit 1
    }
}

################################################################################
# Step 2 — Start Kafka
################################################################################
Write-Host ""
Write-Host "[2/5] Starting Kafka..." -ForegroundColor Yellow

Push-Location $DockerDir
try {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    docker compose up -d 2>&1 | Out-Host
    $ErrorActionPreference = $prev
}
finally {
    Pop-Location
}

# Wait for broker to be fully ready (SASL endpoint, not just TCP port)
# The TCP port opens during KRaft init, before the SASL handler is ready.
# We use 'kafka-broker-api-versions.sh' inside the container as a reliable
# readiness probe — it performs a full SASL handshake.
$MaxWait = 60
$Elapsed = 0
Write-Host "       Waiting for broker to be ready (max ${MaxWait}s)..." -NoNewline
while ($Elapsed -lt $MaxWait) {
    $prev2 = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $check = docker exec kv8-kafka /opt/kafka/bin/kafka-broker-api-versions.sh `
        --bootstrap-server localhost:19092 `
        --command-config /opt/kafka-custom/test_client.properties 2>&1
    $ErrorActionPreference = $prev2
    if ($LASTEXITCODE -eq 0) {
        break
    }
    Start-Sleep -Seconds 2
    $Elapsed += 2
    Write-Host "." -NoNewline
}
Write-Host ""

if ($Elapsed -ge $MaxWait) {
    Write-Host "[ERROR] Kafka broker did not become ready within ${MaxWait}s." -ForegroundColor Red
    exit 1
}
Write-Host "[2/5] Kafka is ready." -ForegroundColor Green

################################################################################
# Step 3 — Start kv8cli consumer in background
################################################################################
Write-Host ""
Write-Host "[3/5] Starting kv8cli consumer..." -ForegroundColor Yellow

$ConsumerLog = Join-Path $BuildDir "kv8cli_test_output.log"

$ConsumerArgs = @(
    "--channel", $Prefix,
    "--brokers", $Brokers,
    "--user", $User,
    "--pass", $Pass,
    "--group", "e2e-test-group"
)

$ConsumerProc = Start-Process -FilePath $ConsumerExe `
    -ArgumentList $ConsumerArgs `
    -RedirectStandardOutput $ConsumerLog `
    -RedirectStandardError (Join-Path $BuildDir "kv8cli_test_errors.log") `
    -PassThru -NoNewWindow

Write-Host "       Consumer PID: $($ConsumerProc.Id)" -ForegroundColor DarkGray
Write-Host "[3/5] Consumer running." -ForegroundColor Green

# Give consumer a moment to connect and subscribe
Start-Sleep -Seconds 3

################################################################################
# Step 4 -- Run kv8feeder producer
################################################################################
Write-Host ""
Write-Host "[4/5] Running kv8feeder producer..." -ForegroundColor Yellow

$ProducerArgs = @(
    "/KV8.brokers=$Brokers",
    "/KV8.channel=$Prefix",
    "/KV8.user=$User",
    "/KV8.pass=$Pass",
    "--duration=5"
)

Write-Host "       Command: $ProducerExe $($ProducerArgs -join ' ')" -ForegroundColor DarkGray

$prev3 = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $ProducerExe @ProducerArgs 2>&1 | Out-Host
$ErrorActionPreference = $prev3

Write-Host "[4/5] Producer finished." -ForegroundColor Green

# Wait for messages to be delivered and consumed
Write-Host "       Waiting 5s for message delivery..." -ForegroundColor DarkGray
Start-Sleep -Seconds 5

################################################################################
# Step 5 — Stop consumer and show results
################################################################################
Write-Host ""
Write-Host "[5/5] Stopping consumer and collecting results..." -ForegroundColor Yellow

if (-not $ConsumerProc.HasExited) {
    # Send Ctrl+C via GenerateConsoleCtrlEvent is unreliable for redirected
    # processes, so we use Stop-Process (the consumer handles SIGTERM gracefully
    # on Linux; on Windows this is an immediate kill, but output is already in
    # the log file).
    Stop-Process -Id $ConsumerProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Consumer Output (last 40 lines)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

if (Test-Path $ConsumerLog) {
    Get-Content $ConsumerLog -Tail 40 | ForEach-Object { Write-Host $_ }
    Write-Host ""

    $TotalLines = (Get-Content $ConsumerLog | Measure-Object -Line).Lines
    $DataLines  = (Select-String -Path $ConsumerLog -Pattern "^\[DATA\]" | Measure-Object).Count
    $RegLines   = (Select-String -Path $ConsumerLog -Pattern "^\[REGISTRY\]" | Measure-Object).Count
    $LogLines   = (Select-String -Path $ConsumerLog -Pattern "^\[LOG\]" | Measure-Object).Count

    Write-Host "------------------------------------------------------------" -ForegroundColor DarkGray
    Write-Host "  Total output lines : $TotalLines" -ForegroundColor White
    Write-Host "  [DATA] messages    : $DataLines" -ForegroundColor White
    Write-Host "  [REGISTRY] records : $RegLines" -ForegroundColor White
    Write-Host "  [LOG] entries      : $LogLines" -ForegroundColor White
    Write-Host "------------------------------------------------------------" -ForegroundColor DarkGray
}
else {
    Write-Host "[WARN] Consumer log not found: $ConsumerLog" -ForegroundColor Yellow
}

$ErrorLog = Join-Path $BuildDir "kv8cli_test_errors.log"
if ((Test-Path $ErrorLog) -and ((Get-Item $ErrorLog).Length -gt 0)) {
    Write-Host ""
    Write-Host "  Consumer stderr:" -ForegroundColor Yellow
    Get-Content $ErrorLog | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkYellow }
}

Write-Host ""
Write-Host "[5/5] Done." -ForegroundColor Green

################################################################################
# Tear down (optional)
################################################################################
if ($TearDown) {
    Write-Host ""
    Write-Host "Tearing down Kafka..." -ForegroundColor Yellow
    Push-Location $DockerDir
    try {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        docker compose down -v 2>&1 | Out-Host
        $ErrorActionPreference = $prev
    }
    finally {
        Pop-Location
    }
    Write-Host "Kafka stopped and volumes removed." -ForegroundColor Green
}
else {
    Write-Host ""
    Write-Host "Kafka is still running. To stop:" -ForegroundColor DarkGray
    Write-Host "  cd docker ; docker compose down       # keep data" -ForegroundColor DarkGray
    Write-Host "  cd docker ; docker compose down -v    # remove data" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Test complete." -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
