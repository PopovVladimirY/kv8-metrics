#!/usr/bin/env bash
################################################################################
# test_ts_e2e.sh -- Kafka timestamp end-to-end integrity test
#
# Runs kv8probe (producer) then kv8verify (consumer) against a dedicated
# Kafka topic. Verifies:
#   - All N samples arrive exactly once, in order
#   - qwTimer[i] == START_TICK + i * TICK_INTERVAL  (exact, no jitter)
#   - dbValue[i] == i % 1024                        (rolling counter)
#
# Prerequisites:
#   - Kafka running (docker compose up -d in ./docker)
#   - kv8probe and kv8verify built and in PATH or BIN_DIR
#
# Usage:
#   ./scripts/test_ts_e2e.sh [options]
#
# Options:
#   --session-id <id>      Topic base name; auto-generated if omitted.
#                          Pass an existing ID with --verify-only to re-verify.
#   --verify-only          Skip kv8probe, run kv8verify only (requires --session-id).
#   --bin-dir <path>       Directory to search for kv8probe / kv8verify.
#   --brokers <b>          Kafka bootstrap brokers.
#   --count <N>            Number of samples.
#   --start-tick <t>       Initial synthetic qwTimer value.
#   --wid <id>             Counter wID.
#   --security-proto <p>   plaintext|sasl_plaintext|sasl_ssl
#   --sasl-mechanism <m>   PLAIN|SCRAM-SHA-256|SCRAM-SHA-512
#   --user <u>             SASL username.
#   --pass <p>             SASL password.
#   --help                 Show this help.
################################################################################

set -euo pipefail

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

# ── Defaults ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

SESSION_ID=""
VERIFY_ONLY=false
BIN_DIR="$REPO_ROOT/build/_output_/bin"
BROKERS="localhost:19092"
COUNT=1000
START_TICK=1000000
WID=0
SECURITY_PROTO="sasl_plaintext"
SASL_MECHANISM="PLAIN"
USER="p7producer"
PASS="p7secret"

# ── Argument parsing ──────────────────────────────────────────────────────────
usage() {
    sed -n '/#.*Usage/,/^####/p' "$0" | grep -v '^####' | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --session-id)    SESSION_ID="$2";    shift 2 ;;
        --verify-only)   VERIFY_ONLY=true;   shift   ;;
        --bin-dir)       BIN_DIR="$2";       shift 2 ;;
        --brokers)       BROKERS="$2";       shift 2 ;;
        --count)         COUNT="$2";         shift 2 ;;
        --start-tick)    START_TICK="$2";    shift 2 ;;
        --wid)           WID="$2";           shift 2 ;;
        --security-proto) SECURITY_PROTO="$2"; shift 2 ;;
        --sasl-mechanism) SASL_MECHANISM="$2"; shift 2 ;;
        --user)          USER="$2";          shift 2 ;;
        --pass)          PASS="$2";          shift 2 ;;
        --help|-h)       usage ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if $VERIFY_ONLY && [[ -z "$SESSION_ID" ]]; then
    echo "[ERROR] --verify-only requires --session-id <existing-session-id>" >&2
    exit 1
fi

# ── Locate binaries ───────────────────────────────────────────────────────────
find_exe() {
    local name="$1"
    # Try PATH first
    if command -v "$name" &>/dev/null; then
        command -v "$name"
        return
    fi
    # Search BIN_DIR recursively; prefer Release build
    local release
    release=$(find "$BIN_DIR" -name "$name" -path '*/Release/*' 2>/dev/null | head -1)
    if [[ -n "$release" ]]; then
        echo "$release"
        return
    fi
    local any
    any=$(find "$BIN_DIR" -name "$name" 2>/dev/null | head -1)
    if [[ -n "$any" ]]; then
        echo "$any"
        return
    fi
    echo "[ERROR] Cannot find '$name'. Build the project first or pass --bin-dir." >&2
    exit 1
}

PROBE=$(find_exe "kv8probe")
VERIFY=$(find_exe "kv8verify")

# ── Session ID / topic ────────────────────────────────────────────────────────
if [[ -z "$SESSION_ID" ]]; then
    # ISO-8601 compact UTC + 6 random hex chars (mirrors PowerShell/C++ format)
    SESSION_ID="$(date -u '+%Y%m%dT%H%M%SZ')-$(head -c 3 /dev/urandom | xxd -p | tr 'a-f' 'A-F')"
fi

if [[ "$SESSION_ID" == kv8ts.* ]]; then
    TOPIC="$SESSION_ID"
else
    TOPIC="kv8ts.$SESSION_ID"
fi

# ── Banner ────────────────────────────────────────────────────────────────────
echo ""
echo "============================================================"
echo "  Kafka Timestamp End-to-End Test"
echo "============================================================"
echo "  Session   : $SESSION_ID"
echo "  Topic     : $TOPIC"
echo "  Brokers   : $BROKERS"
echo "  Count     : $COUNT"
echo "  StartTick : $START_TICK"
echo "  Wid       : $WID"
echo "  Probe     : $PROBE"
echo "  Verify    : $VERIFY"
$VERIFY_ONLY && echo "  Mode      : verify-only (skipping probe)"
echo "============================================================"
echo ""

# ── Common Kafka args ─────────────────────────────────────────────────────────
KAFKA_ARGS=(--brokers "$BROKERS")
if [[ "$SECURITY_PROTO" != "plaintext" ]]; then
    KAFKA_ARGS+=(
        --security-proto "$SECURITY_PROTO"
        --sasl-mechanism "$SASL_MECHANISM"
        --user           "$USER"
        --pass           "$PASS"
    )
fi

# ── Step 1: Run producer ──────────────────────────────────────────────────────
if ! $VERIFY_ONLY; then
    echo "[TEST] Step 1: Running kv8probe..."
    echo ""
    "$PROBE" \
        --topic       "$TOPIC" \
        --count       "$COUNT" \
        --start-tick  "$START_TICK" \
        --wid         "$WID" \
        "${KAFKA_ARGS[@]}"
    probe_exit=$?
    echo ""

    if [[ $probe_exit -ne 0 ]]; then
        echo -e "${RED}[TEST] FAIL: kv8probe exited with code $probe_exit${NC}"
        exit 1
    fi

    echo "[TEST] kv8probe finished OK."
    echo ""

    echo "[TEST] Waiting 2s for broker to settle..."
    sleep 2
    echo ""
else
    echo "[TEST] Step 1: Skipped (--verify-only mode)."
    echo ""
fi

# ── Step 2: Run verifier ──────────────────────────────────────────────────────
echo "[TEST] Step 2: Running kv8verify..."
echo ""
"$VERIFY" \
    --topic       "$TOPIC" \
    --count       "$COUNT" \
    --start-tick  "$START_TICK" \
    --wid         "$WID" \
    "${KAFKA_ARGS[@]}"
verify_exit=$?
echo ""

# ── Result ────────────────────────────────────────────────────────────────────
echo "============================================================"
if [[ $verify_exit -eq 0 ]]; then
    echo -e "${GREEN}  OVERALL TEST RESULT: PASS${NC}"
else
    echo -e "${RED}  OVERALL TEST RESULT: FAIL  (verify exit=$verify_exit)${NC}"
fi
echo "============================================================"
echo ""

exit $verify_exit
