#!/usr/bin/env bash
################################################################################
# build.sh -- Configure and build Kv8 on Linux
#
# Usage (from the repo root):
#   ./scripts/build.sh               # Release build (default)
#   ./scripts/build.sh --debug       # Debug build
#   ./scripts/build.sh --test        # build + run CTest
#   ./scripts/build.sh --install     # build + cmake --install
#   ./scripts/build.sh --clean       # wipe build/ first, then build
#   ./scripts/build.sh --jobs 8      # parallel jobs (default: nproc)
#   ./scripts/build.sh --debug --test  # combine flags freely
#   ./scripts/build.sh --help
################################################################################

set -euo pipefail

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
BUILD_TYPE="Release"
PRESET="linux-release"
DO_TEST=false
DO_INSTALL=false
DO_CLEAN=false
JOBS=$(nproc 2>/dev/null || echo 4)

for arg in "$@"; do
    case "$arg" in
        --debug)    BUILD_TYPE="Debug"; PRESET="linux-debug" ;;
        --test)     DO_TEST=true ;;
        --install)  DO_INSTALL=true ;;
        --clean)    DO_CLEAN=true ;;
        --jobs)     shift; JOBS="$1" ;;
        --help|-h)
            echo "Usage: $0 [--debug] [--test] [--install] [--clean] [--jobs N]"
            exit 0
            ;;
        --jobs=*)   JOBS="${arg#--jobs=}" ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$REPO_ROOT/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo ""
echo -e "${CYAN}=========================================================${NC}"
echo -e "${CYAN}  Kv8 Build -- Linux ($BUILD_TYPE)${NC}"
echo -e "${CYAN}=========================================================${NC}"

# ---------------------------------------------------------------------------
# Validate tools
# ---------------------------------------------------------------------------
if ! command -v cmake &>/dev/null; then
    echo -e "${RED}[ERROR]${NC} cmake not found. Run ./scripts/bootstrap.sh first."
    exit 1
fi
if ! command -v g++ &>/dev/null && ! command -v clang++ &>/dev/null; then
    echo -e "${RED}[ERROR]${NC} No C++ compiler found. Run ./scripts/bootstrap.sh first."
    exit 1
fi

# ---------------------------------------------------------------------------
# Optional clean
# ---------------------------------------------------------------------------
if [[ "$DO_CLEAN" == true ]] && [[ -d "$BUILD_DIR" ]]; then
    echo -e "\n${YELLOW}==> Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"
    echo -e "  ${GREEN}Build directory removed.${NC}"
fi

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
echo -e "\n${YELLOW}==> Configuring (cmake --preset $PRESET)...${NC}"
cd "$REPO_ROOT"
cmake --preset "$PRESET"
echo -e "  ${GREEN}Configure: OK${NC}"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
echo -e "\n${YELLOW}==> Building ($BUILD_TYPE, $JOBS parallel jobs)...${NC}"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$JOBS"
echo -e "  ${GREEN}Build: OK${NC}"

# ---------------------------------------------------------------------------
# Install (optional)
# ---------------------------------------------------------------------------
if [[ "$DO_INSTALL" == true ]]; then
    echo -e "\n${YELLOW}==> Installing artifacts...${NC}"
    cmake --install "$BUILD_DIR" --config "$BUILD_TYPE"
    echo -e "  ${GREEN}Artifacts installed to: $BUILD_DIR/_output_${NC}"
fi

# ---------------------------------------------------------------------------
# Test (optional)
# ---------------------------------------------------------------------------
if [[ "$DO_TEST" == true ]]; then
    echo -e "\n${YELLOW}==> Running tests...${NC}"
    ctest --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" \
          --output-on-failure --parallel "$JOBS"
    echo -e "  ${GREEN}Tests: PASS${NC}"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo -e "${CYAN}=========================================================${NC}"
echo -e "${GREEN}  Build complete ($BUILD_TYPE).${NC}"

if [[ -d "$BUILD_DIR/_output_/bin" ]]; then
    echo "  Binaries: $BUILD_DIR/_output_/bin"
fi

echo ""
echo "  Quick commands:"
echo "    ./scripts/build.sh --test            # rebuild + test"
echo "    ./scripts/test_kafka_e2e.sh --skip-build  # E2E test"
echo -e "${CYAN}=========================================================${NC}"
echo ""
