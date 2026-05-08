#!/usr/bin/env bash
################################################################################
# bootstrap.sh -- Kv8 prerequisites setup for Linux
#
# Installs or verifies every build dependency using the system package manager
# (apt, dnf, or pacman).  Safe to re-run: each step is idempotent.
#
# Usage (from the repo root):
#   ./scripts/bootstrap.sh              # install everything
#   ./scripts/bootstrap.sh --no-docker  # skip Docker check
#   ./scripts/bootstrap.sh --help
#
# Prerequisites installed / checked:
#   - Git, CMake >= 3.20, Make / Ninja, GCC / G++ or Clang
#   - librdkafka-dev (system package)
#   - libglfw3-dev, libgl-dev, libuv1-dev, nlohmann-json3-dev,
#     zlib1g-dev, libssl-dev  (for UI and TLS)
#   - Docker Engine  (required for Kafka integration tests)
################################################################################

set -euo pipefail

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
NO_DOCKER=false
for arg in "$@"; do
    case "$arg" in
        --no-docker)  NO_DOCKER=true ;;
        --help|-h)
            echo "Usage: $0 [--no-docker]"
            echo "  --no-docker   Skip the Docker check"
            exit 0
            ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}[PASS]${NC} $*"; }
warn() { echo -e "  ${YELLOW}[WARN]${NC} $*"; }
step() { echo -e "\n${CYAN}==> $*${NC}"; }
die()  { echo -e "\n${RED}[ERROR]${NC} $*"; exit 1; }

have() { command -v "$1" &>/dev/null; }

echo ""
echo -e "${CYAN}=========================================================${NC}"
echo -e "${CYAN}  Kv8 Bootstrap -- Linux${NC}"
echo -e "${CYAN}=========================================================${NC}"

# ---------------------------------------------------------------------------
# Detect package manager
# ---------------------------------------------------------------------------
step "Detecting package manager"
if have apt-get; then
    PKG_MGR="apt"
    pass "apt (Debian/Ubuntu)"
elif have dnf; then
    PKG_MGR="dnf"
    pass "dnf (Fedora/RHEL)"
elif have pacman; then
    PKG_MGR="pacman"
    pass "pacman (Arch)"
else
    die "No supported package manager found (apt, dnf, pacman). Install dependencies manually."
fi

# Helper: install a list of packages via the detected manager
install_pkgs() {
    local pkgs=("$@")
    case "$PKG_MGR" in
        apt)
            sudo apt-get update -qq
            sudo apt-get install -y "${pkgs[@]}"
            ;;
        dnf)
            sudo dnf install -y "${pkgs[@]}"
            ;;
        pacman)
            sudo pacman -Sy --noconfirm "${pkgs[@]}"
            ;;
    esac
}

# Map generic names to distro-specific package names
pkg_name() {
    local name="$1"
    case "$PKG_MGR" in
        apt)
            case "$name" in
                cmake)              echo "cmake" ;;
                git)                echo "git" ;;
                gcc)                echo "gcc g++" ;;
                ninja)              echo "ninja-build" ;;
                make)               echo "make" ;;
                librdkafka)         echo "librdkafka-dev" ;;
                libglfw3)           echo "libglfw3-dev" ;;
                libgl)              echo "libgl-dev" ;;
                libuv)              echo "libuv1-dev" ;;
                nlohmann-json)      echo "nlohmann-json3-dev" ;;
                zlib)               echo "zlib1g-dev" ;;
                openssl)            echo "libssl-dev" ;;
                pkg-config)         echo "pkg-config" ;;
            esac
            ;;
        dnf)
            case "$name" in
                cmake)              echo "cmake" ;;
                git)                echo "git" ;;
                gcc)                echo "gcc gcc-c++" ;;
                ninja)              echo "ninja-build" ;;
                make)               echo "make" ;;
                librdkafka)         echo "librdkafka-devel" ;;
                libglfw3)           echo "glfw-devel" ;;
                libgl)              echo "mesa-libGL-devel" ;;
                libuv)              echo "libuv-devel" ;;
                nlohmann-json)      echo "json-devel" ;;
                zlib)               echo "zlib-devel" ;;
                openssl)            echo "openssl-devel" ;;
                pkg-config)         echo "pkgconfig" ;;
            esac
            ;;
        pacman)
            case "$name" in
                cmake)              echo "cmake" ;;
                git)                echo "git" ;;
                gcc)                echo "gcc" ;;
                ninja)              echo "ninja" ;;
                make)               echo "make" ;;
                librdkafka)         echo "librdkafka" ;;
                libglfw3)           echo "glfw" ;;
                libgl)              echo "mesa" ;;
                libuv)              echo "libuv" ;;
                nlohmann-json)      echo "nlohmann-json" ;;
                zlib)               echo "zlib" ;;
                openssl)            echo "openssl" ;;
                pkg-config)         echo "pkgconf" ;;
            esac
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Step 1: Core build tools (git, cmake, gcc, make)
# ---------------------------------------------------------------------------
step "Checking core build tools"

MISSING_CORE=()
if ! have git;   then MISSING_CORE+=( "$(pkg_name git)" ); fi
if ! have cmake; then MISSING_CORE+=( "$(pkg_name cmake)" ); fi
if ! have g++ && ! have clang++; then MISSING_CORE+=( "$(pkg_name gcc)" ); fi
if ! have make && ! have ninja; then
    MISSING_CORE+=( "$(pkg_name make)" "$(pkg_name ninja)" )
fi
if ! have pkg-config; then MISSING_CORE+=( "$(pkg_name pkg-config)" ); fi

if [[ ${#MISSING_CORE[@]} -gt 0 ]]; then
    warn "Installing missing core tools: ${MISSING_CORE[*]}"
    install_pkgs "${MISSING_CORE[@]}"
fi

# Verify cmake version
if have cmake; then
    CMAKE_VER=$(cmake --version | head -1 | sed 's/cmake version //')
    CMAKE_MAJOR=$(echo "$CMAKE_VER" | cut -d. -f1)
    CMAKE_MINOR=$(echo "$CMAKE_VER" | cut -d. -f2)
    if [[ "$CMAKE_MAJOR" -lt 3 ]] || { [[ "$CMAKE_MAJOR" -eq 3 ]] && [[ "$CMAKE_MINOR" -lt 20 ]]; }; then
        warn "CMake $CMAKE_VER is too old (>= 3.20 required)."
        warn "Install a newer CMake from https://cmake.org/download/ or via:"
        warn "  pip3 install cmake --upgrade    # or use Kitware's APT repo"
        # Not fatal -- maybe the user already has a newer cmake elsewhere on PATH
    else
        pass "CMake $CMAKE_VER"
    fi
fi
have git    && pass "Git $(git --version | head -1 | sed 's/git version //')"
have g++    && pass "G++ $(g++ --version | head -1)"
have clang++&& pass "Clang++ $(clang++ --version | head -1)"
have make   && pass "Make $(make --version | head -1)"
have ninja  && pass "Ninja $(ninja --version)"

# ---------------------------------------------------------------------------
# Step 2: Library dependencies
# ---------------------------------------------------------------------------
step "Checking and installing library dependencies"

MISSING_LIBS=()
LIBS_TO_CHECK=(librdkafka libglfw3 libgl libuv zlib openssl)

for lib in "${LIBS_TO_CHECK[@]}"; do
    pkgname=$(pkg_name "$lib")
    # Use pkg-config where possible; fall back to dpkg/rpm/pacman queries
    found=false
    case "$PKG_MGR" in
        apt)
            dpkg -s $pkgname &>/dev/null 2>&1 && found=true || true
            ;;
        dnf)
            rpm -q $pkgname &>/dev/null 2>&1 && found=true || true
            ;;
        pacman)
            pacman -Qi $pkgname &>/dev/null 2>&1 && found=true || true
            ;;
    esac
    if [[ "$found" == false ]]; then
        MISSING_LIBS+=( $pkgname )
    fi
done

# nlohmann-json: header-only, check for the header directly
if [[ ! -f /usr/include/nlohmann/json.hpp ]]; then
    MISSING_LIBS+=( "$(pkg_name nlohmann-json)" )
fi

if [[ ${#MISSING_LIBS[@]} -gt 0 ]]; then
    warn "Installing missing libraries: ${MISSING_LIBS[*]}"
    install_pkgs "${MISSING_LIBS[@]}"
fi
pass "Library dependencies satisfied"

# ---------------------------------------------------------------------------
# Step 3: Docker
# ---------------------------------------------------------------------------
if [[ "$NO_DOCKER" == false ]]; then
    step "Checking Docker (required for Kafka integration tests)"
    if have docker; then
        DOCKER_VER=$(docker --version | sed 's/Docker version //' | cut -d, -f1)
        if docker info &>/dev/null 2>&1; then
            pass "Docker $DOCKER_VER (daemon running)"
        else
            warn "Docker $DOCKER_VER found but daemon is not running."
            warn "Start it with:  sudo systemctl start docker"
        fi
    else
        warn "Docker not found."
        warn "Install Docker Engine: https://docs.docker.com/engine/install/"
        warn "Required for: Kafka broker, kv8bench, kv8bench_log, test_kafka_e2e"
    fi
else
    step "Docker check skipped (--no-docker)"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo -e "${CYAN}=========================================================${NC}"
echo -e "${GREEN}  Bootstrap complete.${NC}"
echo ""
echo "  Next steps:"
echo "    ./scripts/build.sh            # configure + build (Release)"
echo "    ./scripts/build.sh --debug    # Debug build"
echo "    ./scripts/build.sh --test     # build + run tests"
echo -e "${CYAN}=========================================================${NC}"
echo ""
