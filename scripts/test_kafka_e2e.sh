#!/usr/bin/env bash
################################################################################
# test_kafka_e2e.sh -- End-to-end test: Kafka + kv8feeder producer + kv8cli consumer
#
# Usage:
#   ./scripts/test_kafka_e2e.sh                   # run test, keep Kafka running
#   ./scripts/test_kafka_e2e.sh --teardown         # run test, then stop Kafka
#   ./scripts/test_kafka_e2e.sh --skip-build       # skip CMake build step
#   ./scripts/test_kafka_e2e.sh --teardown --skip-build
################################################################################

set -euo pipefail

# ── Parse arguments ──────────────────────────────────────────────────────────
TEARDOWN=false
SKIP_BUILD=false

for arg in "$@"; do
    case "$arg" in
        --teardown)   TEARDOWN=true ;;
        --skip-build) SKIP_BUILD=true ;;
        --help|-h)
            echo "Usage: $0 [--teardown] [--skip-build]"
            echo "  --teardown    Stop Kafka and remove volumes after test"
            echo "  --skip-build  Skip the CMake build step"
            exit 0
            ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# ── Paths ────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
DOCKER_DIR="$REPO_ROOT/docker"
BUILD_DIR="$REPO_ROOT/build"

OUTPUT_BIN="$BUILD_DIR/_output_/bin"
PRODUCER_EXE="$OUTPUT_BIN/kv8feeder"
CONSUMER_EXE="$OUTPUT_BIN/kv8cli"

# Kafka connection
BROKERS="localhost:19092"
USER="kv8producer"
PASS="kv8secret"
PREFIX="kv8/e2e_test"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m' # No Color

CONSUMER_PID=""

# ── Cleanup trap ─────────────────────────────────────────────────────────────
cleanup() {
    if [[ -n "$CONSUMER_PID" ]] && kill -0 "$CONSUMER_PID" 2>/dev/null; then
        echo -e "${YELLOW}Stopping consumer (PID $CONSUMER_PID)...${NC}"
        kill "$CONSUMER_PID" 2>/dev/null || true
        wait "$CONSUMER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo ""
echo -e "${CYAN}============================================================${NC}"
echo -e "${CYAN}  Kv8 End-to-End Test${NC}"
echo -e "${CYAN}============================================================${NC}"
echo ""

################################################################################
# Step 1 — Build (unless skipped)
################################################################################
if [[ "$SKIP_BUILD" == false ]]; then
    echo -e "${YELLOW}[1/5] Building project...${NC}"

    mkdir -p "$BUILD_DIR"
    pushd "$BUILD_DIR" > /dev/null

    cmake_args=()

    # Use vcpkg toolchain if VCPKG_ROOT is set
    if [[ -n "${VCPKG_ROOT:-}" ]]; then
        cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake")
    fi

    cmake .. "${cmake_args[@]}" 2>&1
    cmake --build . --config Release -j"$(nproc)" 2>&1
    cmake --install . --config Release 2>&1

    popd > /dev/null

    if [[ ! -x "$PRODUCER_EXE" ]]; then
        echo -e "${RED}[ERROR] Producer executable not found: $PRODUCER_EXE${NC}"
        exit 1
    fi
    if [[ ! -x "$CONSUMER_EXE" ]]; then
        echo -e "${RED}[ERROR] Consumer executable not found: $CONSUMER_EXE${NC}"
        exit 1
    fi
    echo -e "${GREEN}[1/5] Build complete.${NC}"
else
    echo -e "${GRAY}[1/5] Build skipped.${NC}"
    if [[ ! -x "$PRODUCER_EXE" ]] || [[ ! -x "$CONSUMER_EXE" ]]; then
        echo -e "${RED}[ERROR] Executables not found. Run without --skip-build first.${NC}"
        exit 1
    fi
fi

################################################################################
# Step 2 — Start Kafka
################################################################################
echo ""
echo -e "${YELLOW}[2/5] Starting Kafka...${NC}"

pushd "$DOCKER_DIR" > /dev/null
docker compose up -d 2>&1
popd > /dev/null

# Wait for broker to be ready
MAX_WAIT=30
ELAPSED=0
echo -n "       Waiting for broker to be ready (max ${MAX_WAIT}s)..."
while [[ $ELAPSED -lt $MAX_WAIT ]]; do
    if (echo > /dev/tcp/localhost/19092) 2>/dev/null; then
        break
    fi
    sleep 2
    ELAPSED=$((ELAPSED + 2))
    echo -n "."
done
echo ""

if [[ $ELAPSED -ge $MAX_WAIT ]]; then
    echo -e "${RED}[ERROR] Kafka broker did not become ready within ${MAX_WAIT}s.${NC}"
    exit 1
fi
echo -e "${GREEN}[2/5] Kafka is ready.${NC}"

################################################################################
# Step 3 — Start kv8cli consumer in background
################################################################################
echo ""
echo -e "${YELLOW}[3/5] Starting kv8cli consumer...${NC}"

CONSUMER_LOG="$BUILD_DIR/kv8cli_test_output.log"
CONSUMER_ERR="$BUILD_DIR/kv8cli_test_errors.log"

"$CONSUMER_EXE" \
    --channel "$PREFIX" \
    --brokers "$BROKERS" \
    --user "$USER" \
    --pass "$PASS" \
    --group "e2e-test-group" \
    > "$CONSUMER_LOG" 2> "$CONSUMER_ERR" &

CONSUMER_PID=$!

echo -e "${GRAY}       Consumer PID: $CONSUMER_PID${NC}"
echo -e "${GREEN}[3/5] Consumer running.${NC}"

# Give consumer a moment to connect and subscribe
sleep 3

################################################################################
# Step 4 -- Run kv8feeder producer
################################################################################
echo ""
echo -e "${YELLOW}[4/5] Running kv8feeder producer...${NC}"

echo -e "${GRAY}       Command: $PRODUCER_EXE /KV8.brokers=$BROKERS /KV8.channel=$PREFIX /KV8.user=$USER /KV8.pass=$PASS --duration=5${NC}"

"$PRODUCER_EXE" \
    "/KV8.brokers=$BROKERS" \
    "/KV8.channel=$PREFIX" \
    "/KV8.user=$USER" \
    "/KV8.pass=$PASS" \
    "--duration=5" \
    2>&1

echo -e "${GREEN}[4/5] Producer finished.${NC}"

# Wait for messages to be delivered and consumed
echo -e "${GRAY}       Waiting 5s for message delivery...${NC}"
sleep 5

################################################################################
# Step 5 — Stop consumer and show results
################################################################################
echo ""
echo -e "${YELLOW}[5/5] Stopping consumer and collecting results...${NC}"

if kill -0 "$CONSUMER_PID" 2>/dev/null; then
    kill -INT "$CONSUMER_PID" 2>/dev/null || true
    # Give it a moment for graceful shutdown
    sleep 2
    if kill -0 "$CONSUMER_PID" 2>/dev/null; then
        kill -TERM "$CONSUMER_PID" 2>/dev/null || true
    fi
    wait "$CONSUMER_PID" 2>/dev/null || true
fi
CONSUMER_PID=""  # Prevent cleanup trap from killing again

echo ""
echo -e "${CYAN}============================================================${NC}"
echo -e "${CYAN}  Consumer Output (last 40 lines)${NC}"
echo -e "${CYAN}============================================================${NC}"

if [[ -f "$CONSUMER_LOG" ]]; then
    tail -40 "$CONSUMER_LOG"
    echo ""

    TOTAL_LINES=$(wc -l < "$CONSUMER_LOG")
    DATA_LINES=$(grep -c '^\[DATA\]' "$CONSUMER_LOG" 2>/dev/null || echo 0)
    REG_LINES=$(grep -c '^\[REGISTRY\]' "$CONSUMER_LOG" 2>/dev/null || echo 0)
    LOG_LINES=$(grep -c '^\[LOG\]' "$CONSUMER_LOG" 2>/dev/null || echo 0)

    echo -e "${GRAY}------------------------------------------------------------${NC}"
    echo "  Total output lines : $TOTAL_LINES"
    echo "  [DATA] messages    : $DATA_LINES"
    echo "  [REGISTRY] records : $REG_LINES"
    echo "  [LOG] entries      : $LOG_LINES"
    echo -e "${GRAY}------------------------------------------------------------${NC}"
else
    echo -e "${YELLOW}[WARN] Consumer log not found: $CONSUMER_LOG${NC}"
fi

if [[ -f "$CONSUMER_ERR" ]] && [[ -s "$CONSUMER_ERR" ]]; then
    echo ""
    echo -e "${YELLOW}  Consumer stderr:${NC}"
    sed 's/^/    /' "$CONSUMER_ERR"
fi

echo ""
echo -e "${GREEN}[5/5] Done.${NC}"

################################################################################
# Tear down (optional)
################################################################################
if [[ "$TEARDOWN" == true ]]; then
    echo ""
    echo -e "${YELLOW}Tearing down Kafka...${NC}"
    pushd "$DOCKER_DIR" > /dev/null
    docker compose down -v 2>&1
    popd > /dev/null
    echo -e "${GREEN}Kafka stopped and volumes removed.${NC}"
else
    echo ""
    echo -e "${GRAY}Kafka is still running. To stop:${NC}"
    echo -e "${GRAY}  cd docker && docker compose down       # keep data${NC}"
    echo -e "${GRAY}  cd docker && docker compose down -v    # remove data${NC}"
fi

echo ""
echo -e "${CYAN}============================================================${NC}"
echo -e "${CYAN}  Test complete.${NC}"
echo -e "${CYAN}============================================================${NC}"
