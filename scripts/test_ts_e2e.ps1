#!/usr/bin/env pwsh
################################################################################
# test_ts_e2e.ps1 -- Kafka timestamp end-to-end integrity test
#
# Runs kv8probe (producer) then kv8verify (consumer) against a dedicated
# Kafka topic. Verifies:
#   - All N samples arrive exactly once, in order
#   - qwTimer[i] == START_TICK + i * TICK_INTERVAL  (exact, no jitter)
#   - dbValue[i] == i % 1024                        (rolling counter)
#
# Prerequisites:
#   - Kafka running (docker compose up -d in ./docker)
#   - kv8probe.exe and kv8verify.exe built and in PATH or BIN_DIR
#
# Usage:
#   .\test_ts_e2e.ps1 [options]
#
# Options:
#   -SessionId <id>    Topic base name; auto-generated if omitted.
#                      Pass an existing ID with -VerifyOnly to re-verify.
#   -VerifyOnly        Skip kv8probe, run kv8verify only (requires -SessionId).
#   -BinDir <path>     Directory to search for kv8probe/kv8verify executables.
#   -Brokers <b>       Kafka bootstrap brokers.
#   -Count <N>         Number of samples.
#   -StartTick <t>     Initial synthetic qwTimer value.
#   -Wid <id>          Counter wID (must match probe configuration).
#   -SecurityProto <p> plaintext|sasl_plaintext|sasl_ssl
#   -SaslMechanism <m> PLAIN|SCRAM-SHA-256|SCRAM-SHA-512
#   -User / -Pass      SASL credentials
################################################################################
param(
    [string]$SessionId     = "",
    [switch]$VerifyOnly,
    [string]$BinDir        = "$PSScriptRoot\..\build\tools",
    [string]$Brokers       = "localhost:19092",
    [int]   $Count         = 1000,
    [uint64]$StartTick     = 1000000,
    [int]   $Wid           = 0,
    [string]$SecurityProto = "sasl_plaintext",
    [string]$SaslMechanism = "PLAIN",
    [string]$User          = "p7producer",
    [string]$Pass          = "p7secret"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($VerifyOnly -and $SessionId -eq "") {
    Write-Error "-VerifyOnly requires -SessionId <existing-session-id>"
    exit 1
}

# ── Locate binaries ───────────────────────────────────────────────────────────
function Find-Exe([string]$name) {
    # Try PATH first
    $inPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($inPath) { return $inPath.Source }
    # Search BinDir recursively; prefer Release over Debug
    $candidates = Get-ChildItem -Recurse -Filter "$name.exe" -Path $BinDir `
                  -ErrorAction SilentlyContinue
    if (-not $candidates) {
        throw "Cannot find $name.exe. Build the project first or pass -BinDir."
    }
    $release = $candidates | Where-Object { $_.DirectoryName -match '\\Release$' } |
               Select-Object -First 1
    if ($release) { return $release.FullName }
    return ($candidates | Select-Object -First 1).FullName
}

$probe  = Find-Exe "kv8probe"
$verify = Find-Exe "kv8verify"

# ── Session ID / topic ────────────────────────────────────────────────────────
if ($SessionId -eq "") {
    $runId     = (Get-Date -Format "yyyyMMddTHHmmssZ") + "-" + `
                 ([System.Guid]::NewGuid().ToString("N").Substring(0,6).ToUpper())
    $SessionId = $runId
}
if ($SessionId -like "kv8ts.*") {
    $topic = $SessionId
} else {
    $topic = "kv8ts.$SessionId"
}

Write-Host ""
Write-Host "============================================================"
Write-Host "  Kafka Timestamp End-to-End Test"
Write-Host "============================================================"
Write-Host "  Session   : $SessionId"
Write-Host "  Topic     : $topic"
Write-Host "  Brokers   : $Brokers"
Write-Host "  Count     : $Count"
Write-Host "  StartTick : $StartTick"
Write-Host "  Wid       : $Wid"
Write-Host "  Probe     : $probe"
Write-Host "  Verify    : $verify"
if ($VerifyOnly) {
    Write-Host "  Mode      : verify-only (skipping probe)"
}
Write-Host "============================================================"
Write-Host ""

# ── Common Kafka args ─────────────────────────────────────────────────────────
$kafkaArgs = @("--brokers", $Brokers)
if ($SecurityProto -ne "plaintext") {
    $kafkaArgs += "--security-proto", $SecurityProto,
                  "--sasl-mechanism", $SaslMechanism,
                  "--user",           $User,
                  "--pass",           $Pass
}

# ── Step 1: Run producer (skipped in VerifyOnly mode) ────────────────────────
if (-not $VerifyOnly) {
    Write-Host "[TEST] Step 1: Running kv8probe..."
    Write-Host ""
    $probeArgs = @("--topic", $topic, "--count", $Count,
                   "--start-tick", $StartTick, "--wid", $Wid) + $kafkaArgs
    & $probe @probeArgs
    $probeExit = $LASTEXITCODE
    Write-Host ""

    # Exit code 2 = stale topic (messages already exist from a prior run)
    if ($probeExit -eq 2) {
        Write-Host "[TEST] FAIL: data topic already has messages from a previous run." -ForegroundColor Red
        Write-Host "[TEST]       Delete the topic in Kafka (or use a new -SessionId) then retry." -ForegroundColor Red
        exit 2
    }
    if ($probeExit -ne 0) {
        Write-Host "[TEST] FAIL: kv8probe exited with code $probeExit" -ForegroundColor Red
        exit 1
    }

    Write-Host "[TEST] kv8probe finished OK."
    Write-Host ""

    # Brief pause to let Kafka flush/replicate
    Write-Host "[TEST] Waiting 2 s for broker to settle..."
    Start-Sleep -Seconds 2
    Write-Host ""
} else {
    Write-Host "[TEST] Step 1: Skipped (--verify-only mode)."
    Write-Host ""
}

# ── Step 2: Run verifier ──────────────────────────────────────────────────────
Write-Host "[TEST] Step 2: Running kv8verify..."
Write-Host ""
$verifyArgs = @("--topic", $topic, "--count", $Count,
                "--start-tick", $StartTick, "--wid", $Wid) + $kafkaArgs
& $verify @verifyArgs
$verifyExit = $LASTEXITCODE
Write-Host ""

# ── Result ────────────────────────────────────────────────────────────────────
Write-Host "============================================================"
if ($verifyExit -eq 0) {
    Write-Host "  OVERALL TEST RESULT: PASS" -ForegroundColor Green
} else {
    Write-Host "  OVERALL TEST RESULT: FAIL  (verify exit=$verifyExit)" -ForegroundColor Red
}
Write-Host "============================================================"
Write-Host ""

exit $verifyExit
