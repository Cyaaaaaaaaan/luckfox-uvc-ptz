#!/bin/bash
# =============================================================================
# build-app.sh — Luckfox Pico Pro UVC PTZ Camera build script
#
# What this does:
#   1. Injects toolchain configuration directly into .BoardConfig.mk
#   2. Applies all patches in patches/ to the SDK source tree
#   3. Drives the SDK's own build system: ./build.sh app
# =============================================================================
set -euo pipefail

# --------------------------------------------------------------------------- #
#  Defaults & Setup
# --------------------------------------------------------------------------- #
SDK_DIR="${SDK_DIR:-$HOME/luckfox-pico}"
SKIP_PATCHES=0
CLEAN_BUILD=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk)          SDK_DIR="$2"; shift 2 ;;
        --skip-patches) SKIP_PATCHES=1; shift ;;
        --clean)        CLEAN_BUILD=1; shift ;;
        -h|--help)      sed -n '2,25p' "$0"; exit 0 ;;
        *)              echo "Unknown option: $1"; exit 1 ;;
    esac
done

# --------------------------------------------------------------------------- #
#  Helpers
# --------------------------------------------------------------------------- #
info()  { echo -e "\033[1;34m[INFO]\033[0m  $*"; }
ok()    { echo -e "\033[1;32m[ OK ]\033[0m  $*"; }
warn()  { echo -e "\033[1;33m[WARN]\033[0m  $*"; }
skip()  { echo -e "\033[1;33m[SKIP]\033[0m  $*"; }
error() { echo -e "\033[1;31m[ERR ]\033[0m  $*" >&2; exit 1; }

# --------------------------------------------------------------------------- #
#  1. Validate SDK & Toolchain
# --------------------------------------------------------------------------- #
info "SDK path: $SDK_DIR"
[[ -d "$SDK_DIR" ]] || error "SDK directory not found: $SDK_DIR"

# Global toolchain path variables just in case our shell needs them
export TOOLCHAIN_BIN_DIR="$SDK_DIR/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin"
export PATH="$TOOLCHAIN_BIN_DIR:$PATH"

BOARDCONFIG_LINK="$SDK_DIR/.BoardConfig.mk"
if [[ ! -f "$BOARDCONFIG_LINK" && ! -L "$BOARDCONFIG_LINK" ]]; then
    error "No .BoardConfig.mk found. You must run './build.sh lunch' in $SDK_DIR first."
fi

if ! command -v arm-rockchip830-linux-uclibcgnueabihf-gcc &>/dev/null; then
    error "Toolchain compiler not found. Ensure git-lfs pull was executed."
fi
ok "Compiler: $(arm-rockchip830-linux-uclibcgnueabihf-gcc --version | head -1)"

# --------------------------------------------------------------------------- #
#  2. Apply patches via apply_patches.sh
# --------------------------------------------------------------------------- #
if [[ $SKIP_PATCHES -eq 0 ]]; then
    info "Applying patches via apply_patches.sh..."
    APPLY_SCRIPT="$SCRIPT_DIR/apply_patches.sh"
    [[ -f "$APPLY_SCRIPT" ]] || error "apply_patches.sh not found at $APPLY_SCRIPT"
    bash "$APPLY_SCRIPT" "$SDK_DIR"
    ok "Patches applied."
else
    warn "Skipping patch application (--skip-patches)"
fi

if [[ $CLEAN_BUILD -eq 1 ]]; then
    info "Cleaning app build artifacts..."
    (cd "$SDK_DIR" && bash build.sh clean app)
    ok "Clean done."
fi

# --------------------------------------------------------------------------- #
#  3. Build via SDK build system
# --------------------------------------------------------------------------- #
info "Preparing SDK configuration files..."

REAL_BOARDCONFIG=$(realpath "$BOARDCONFIG_LINK")
[[ -f "$REAL_BOARDCONFIG" ]] || error "Target BoardConfig file is missing."

# Inject toolchain variables directly to prevent SDK empty '[]' error
info "Injecting toolchain definitions directly into BoardConfig..."
sed -i '/export RK_TOOLCHAIN_CROSS/d' "$REAL_BOARDCONFIG"
echo 'export RK_TOOLCHAIN_CROSS=arm-rockchip830-linux-uclibcgnueabihf' >> "$REAL_BOARDCONFIG"

sed -i '/export RK_CHIP/d' "$REAL_BOARDCONFIG"
echo 'export RK_CHIP=rv1106' >> "$REAL_BOARDCONFIG"

sed -i '/export RK_APP_TYPE/d' "$REAL_BOARDCONFIG"
echo 'export RK_APP_TYPE="UVC_TINY"' >> "$REAL_BOARDCONFIG"

# Clean any previously broken CMake caches in the media folder
rm -rf "$SDK_DIR/media/common_algorithm/build" 2>/dev/null || true

# Patch ALL CMake files across the SDK to support modern compilers
info "Patching CMakeLists.txt version ranges globally..."
find "$SDK_DIR/project" "$SDK_DIR/media" -name "CMakeLists.txt" -exec sed -i 's/^cmake_minimum_required.*/cmake_minimum_required(VERSION 3.5...3.25)/g' {} +
ok "CMakeLists.txt files patched."

info "Building app via SDK build system (./build.sh app)..."
info "Build log: $SDK_DIR/build_app.log"

(
    cd "$SDK_DIR"
    # Ensure BoardConfig is sourced
    [ -f .BoardConfig.mk ] && source .BoardConfig.mk
    
    # 1. Build the media libraries FIRST to generate the missing .h files
    echo "[INFO] Building media subsystem (generating MPI headers)..."
    ./build.sh media
    
    # 2. Now build the app
    echo "[INFO] Building UVC app..."
    ./build.sh app 
) 2>&1 | tee "$SDK_DIR/build_app.log"

BUILD_EXIT=${PIPESTATUS[0]}
if [[ $BUILD_EXIT -ne 0 ]]; then
    error "SDK app build failed (exit $BUILD_EXIT).\n  See: $SDK_DIR/build_app.log"
fi

# --------------------------------------------------------------------------- #
#  4. Locate output binary & Deploy
# --------------------------------------------------------------------------- #
UVC_BIN_CANDIDATES=(
    "$SDK_DIR/project/app/uvc_app_tiny/out/bin/rk_mpi_uvc"
    "$SDK_DIR/project/app/out/bin/rk_mpi_uvc"
    "$SDK_DIR/output/out/app_out/bin/rk_mpi_uvc"
)
UVC_BIN=""
for c in "${UVC_BIN_CANDIDATES[@]}"; do
    if [[ -f "$c" ]]; then
        UVC_BIN="$c"
        break
    fi
done

[[ -n "$UVC_BIN" ]] || error "rk_mpi_uvc binary not found after build."

ok "Binary: $UVC_BIN"

UVC_SCRIPTS_DIR="$SDK_DIR/project/app/uvc_app_tiny/uvc_app"

echo ""
ok "============================================================"
ok " Build complete!"
ok "============================================================"
echo ""
echo "  Binary : $UVC_BIN"
RKLUNCH_SH="$(dirname "$(realpath "$0")")/RkLunch.sh"
UVC_SCRIPTS_DIR="$SDK_DIR/project/app/uvc_app_tiny/uvc_app"

echo ""
echo "  ── Deploy to Luckfox Pico Pro (via SSH/SCP) ──"
echo "  # NOTE: usb_config.sh and RkLunch.sh do not exist in factory firmware."
echo "  # We deploy all four files. RkLunch.sh keeps SSH alive by calling network_init."
echo ""
echo "  # 1. Copy files to board"
echo "  scp \"$UVC_BIN\" root@\$PICO_IP:/oem/usr/bin/rk_mpi_uvc"
echo "  scp \"$UVC_SCRIPTS_DIR/usb_config.sh\" root@\$PICO_IP:/oem/usr/bin/usb_config.sh"
echo "  scp \"$UVC_SCRIPTS_DIR/rkuvc.ini\" root@\$PICO_IP:/oem/usr/share/rkuvc.ini"
echo "  scp \"$RKLUNCH_SH\" root@\$PICO_IP:/oem/usr/bin/RkLunch.sh"
echo ""
echo "  # 2. Set permissions"
echo "  ssh root@\$PICO_IP \"chmod +x /oem/usr/bin/rk_mpi_uvc /oem/usr/bin/usb_config.sh /oem/usr/bin/RkLunch.sh\""
echo ""
echo "  # 3. Reboot to verify autostart and SSH survives"
echo "  ssh root@\$PICO_IP reboot"
echo ""