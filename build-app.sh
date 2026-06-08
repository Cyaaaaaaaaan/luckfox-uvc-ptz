#!/bin/bash
# =============================================================================
# build-app.sh — Luckfox Pico Pro UVC PTZ Camera build script
#
# Usage:
#   bash build-app.sh [--target <target>] [options]
#
# Targets (comma-separated or repeated --target flags):
#   uvc      Build rk_mpi_uvc (full SDK path, patches applied)
#   visca    Build visca_server (cmake only, fast)
#   scripts  Deploy scripts: RkLunch.sh, usb_config.sh, rkuvc.ini
#   model    Deploy RKNN model: retinaface.rknn
#   all      Everything above (default)
#
# Options:
#   --sdk <path>       SDK root (default: ~/luckfox-pico)
#   --skip-patches     Skip apply_patches.sh (uvc target only)
#   --clean            Clean build artifacts before building
#   -h|--help          Show this help
#
# Examples:
#   bash build-app.sh                          # build + deploy everything
#   bash build-app.sh --target uvc             # only rk_mpi_uvc
#   bash build-app.sh --target visca           # only visca_server (fast)
#   bash build-app.sh --target scripts         # only RkLunch/usb_config/ini
#   bash build-app.sh --target model           # only retinaface.rknn
#   bash build-app.sh --target uvc,visca       # uvc + visca, no scripts/model
#   bash build-app.sh --target all --clean
# =============================================================================
set -euo pipefail

# --------------------------------------------------------------------------- #
#  Defaults
# --------------------------------------------------------------------------- #
SDK_DIR="${SDK_DIR:-$HOME/luckfox-pico}"
SKIP_PATCHES=0
CLEAN_BUILD=0
TARGET_ARG="all"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)       TARGET_ARG="$2"; shift 2 ;;
        --sdk)          SDK_DIR="$2"; shift 2 ;;
        --skip-patches) SKIP_PATCHES=1; shift ;;
        --clean)        CLEAN_BUILD=1; shift ;;
        -h|--help)      sed -n '2,28p' "$0"; exit 0 ;;
        *)              echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Expand "all" and parse comma-separated targets
BUILD_UVC=0; BUILD_VISCA=0; BUILD_SCRIPTS=0; BUILD_MODEL=0

expand_target() {
    local t="$1"
    case "$t" in
        uvc)     BUILD_UVC=1 ;;
        visca)   BUILD_VISCA=1 ;;
        scripts) BUILD_SCRIPTS=1 ;;
        model)   BUILD_MODEL=1 ;;
        all)     BUILD_UVC=1; BUILD_VISCA=1; BUILD_SCRIPTS=1; BUILD_MODEL=1 ;;
        *) echo "Unknown target '$t'. Valid: uvc, visca, scripts, model, all"; exit 1 ;;
    esac
}

IFS=',' read -ra TARGETS <<< "$TARGET_ARG"
for t in "${TARGETS[@]}"; do expand_target "$t"; done

NEED_TOOLCHAIN=$(( BUILD_UVC | BUILD_VISCA ))
NEED_SDK=$BUILD_UVC

# --------------------------------------------------------------------------- #
#  Helpers
# --------------------------------------------------------------------------- #
info()  { echo -e "\033[1;34m[INFO]\033[0m  $*"; }
ok()    { echo -e "\033[1;32m[ OK ]\033[0m  $*"; }
warn()  { echo -e "\033[1;33m[WARN]\033[0m  $*"; }
skip()  { echo -e "\033[1;33m[SKIP]\033[0m  $*"; }
error() { echo -e "\033[1;31m[ERR ]\033[0m  $*" >&2; exit 1; }

info "Targets : $TARGET_ARG"
info "SDK path: $SDK_DIR"

# --------------------------------------------------------------------------- #
#  1. Validate SDK & Toolchain (only when compiling)
# --------------------------------------------------------------------------- #
if [[ $NEED_TOOLCHAIN -eq 1 ]]; then
    [[ -d "$SDK_DIR" ]] || error "SDK directory not found: $SDK_DIR"
    export TOOLCHAIN_BIN_DIR="$SDK_DIR/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin"
    export PATH="$TOOLCHAIN_BIN_DIR:$PATH"
    if ! command -v arm-rockchip830-linux-uclibcgnueabihf-gcc &>/dev/null; then
        error "Toolchain compiler not found. Ensure git-lfs pull was executed."
    fi
    ok "Compiler: $(arm-rockchip830-linux-uclibcgnueabihf-gcc --version | head -1)"
else
    skip "Toolchain check (not needed for scripts/model targets)"
fi

# --------------------------------------------------------------------------- #
#  2. Apply patches (uvc only)
# --------------------------------------------------------------------------- #
if [[ $BUILD_UVC -eq 1 ]]; then
    if [[ $SKIP_PATCHES -eq 0 ]]; then
        info "Applying patches via apply_patches.sh..."
        APPLY_SCRIPT="$SCRIPT_DIR/apply_patches.sh"
        [[ -f "$APPLY_SCRIPT" ]] || error "apply_patches.sh not found at $APPLY_SCRIPT"
        bash "$APPLY_SCRIPT" "$SDK_DIR"
        ok "Patches applied."
    else
        warn "Skipping patch application (--skip-patches)"
    fi
else
    skip "Patches (uvc not in target)"
fi

# --------------------------------------------------------------------------- #
#  3. Build rk_mpi_uvc
# --------------------------------------------------------------------------- #
UVC_BIN=""

if [[ $BUILD_UVC -eq 1 ]]; then
    BOARDCONFIG_LINK="$SDK_DIR/.BoardConfig.mk"
    [[ -f "$BOARDCONFIG_LINK" || -L "$BOARDCONFIG_LINK" ]] || \
        error "No .BoardConfig.mk found. Run './build.sh lunch' in $SDK_DIR first."

    REAL_BOARDCONFIG=$(realpath "$BOARDCONFIG_LINK")
    [[ -f "$REAL_BOARDCONFIG" ]] || error "Target BoardConfig file is missing."

    info "Injecting toolchain definitions into BoardConfig..."
    sed -i '/export RK_TOOLCHAIN_CROSS/d' "$REAL_BOARDCONFIG"
    echo 'export RK_TOOLCHAIN_CROSS=arm-rockchip830-linux-uclibcgnueabihf' >> "$REAL_BOARDCONFIG"
    sed -i '/export RK_CHIP/d' "$REAL_BOARDCONFIG"
    echo 'export RK_CHIP=rv1106' >> "$REAL_BOARDCONFIG"
    sed -i '/export RK_APP_TYPE/d' "$REAL_BOARDCONFIG"
    echo 'export RK_APP_TYPE="UVC_TINY"' >> "$REAL_BOARDCONFIG"

    rm -rf "$SDK_DIR/media/common_algorithm/build" 2>/dev/null || true

    info "Patching CMakeLists.txt version ranges globally..."
    find "$SDK_DIR/project" "$SDK_DIR/media" -name "CMakeLists.txt" \
        -exec sed -i 's/^cmake_minimum_required.*/cmake_minimum_required(VERSION 3.5...3.25)/g' {} +
    ok "CMakeLists.txt files patched."

    if [[ $CLEAN_BUILD -eq 1 ]]; then
        info "Cleaning UVC build artifacts..."
        (cd "$SDK_DIR" && bash build.sh clean app)
        ok "Clean done."
    fi

    info "Building rk_mpi_uvc via SDK build system..."
    info "Build log: $SDK_DIR/build_app.log"
    (
        cd "$SDK_DIR"
        [ -f .BoardConfig.mk ] && source .BoardConfig.mk
        echo "[INFO] Building media subsystem (generating MPI headers)..."
        ./build.sh media
        echo "[INFO] Building UVC app..."
        ./build.sh app
    ) 2>&1 | tee "$SDK_DIR/build_app.log"

    [[ ${PIPESTATUS[0]} -eq 0 ]] || error "SDK app build failed. See: $SDK_DIR/build_app.log"

    UVC_BIN_CANDIDATES=(
        "$SDK_DIR/project/app/uvc_app_tiny/out/bin/rk_mpi_uvc"
        "$SDK_DIR/project/app/out/bin/rk_mpi_uvc"
        "$SDK_DIR/output/out/app_out/bin/rk_mpi_uvc"
    )
    for c in "${UVC_BIN_CANDIDATES[@]}"; do
        [[ -f "$c" ]] && UVC_BIN="$c" && break
    done
    [[ -n "$UVC_BIN" ]] || error "rk_mpi_uvc binary not found after build."
    ok "rk_mpi_uvc: $UVC_BIN"
else
    skip "rk_mpi_uvc (not in target)"
fi

# --------------------------------------------------------------------------- #
#  4. Build visca_server
# --------------------------------------------------------------------------- #
VISCA_SRC="$SCRIPT_DIR/visca"
VISCA_BUILD="$SCRIPT_DIR/visca/build"
VISCA_BIN="$VISCA_BUILD/visca_server"

if [[ $BUILD_VISCA -eq 1 ]]; then
    if [[ $CLEAN_BUILD -eq 1 && -d "$VISCA_BUILD" ]]; then
        info "Cleaning visca build artifacts..."
        rm -rf "$VISCA_BUILD"
        ok "Clean done."
    fi

    info "Building visca_server..."
    cmake -S "$VISCA_SRC" -B "$VISCA_BUILD" \
        -DCMAKE_C_COMPILER=arm-rockchip830-linux-uclibcgnueabihf-gcc \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$VISCA_BUILD/install" \
        -Wno-dev -DCMAKE_SYSTEM_NAME=Linux >/dev/null
    cmake --build "$VISCA_BUILD" -j"$(nproc)"
    [[ -f "$VISCA_BIN" ]] || error "visca_server build failed"
    ok "visca_server: $VISCA_BIN"
else
    skip "visca_server (not in target)"
fi

# --------------------------------------------------------------------------- #
#  5. Verify scripts & model exist
# --------------------------------------------------------------------------- #
UVC_SCRIPTS_DIR="$SDK_DIR/project/app/uvc_app_tiny/uvc_app"
RKLUNCH_SH="$SCRIPT_DIR/RkLunch.sh"
RETINA_MODEL="$SCRIPT_DIR/files/models/retinaface.rknn"

if [[ $BUILD_SCRIPTS -eq 1 ]]; then
    [[ -f "$RKLUNCH_SH" ]]                       || error "RkLunch.sh not found: $RKLUNCH_SH"
    [[ -f "$UVC_SCRIPTS_DIR/usb_config.sh" ]]    || error "usb_config.sh not found"
    [[ -f "$UVC_SCRIPTS_DIR/rkuvc.ini" ]]         || error "rkuvc.ini not found"
    ok "Scripts: RkLunch.sh, usb_config.sh, rkuvc.ini"
else
    skip "Scripts (not in target)"
fi

if [[ $BUILD_MODEL -eq 1 ]]; then
    [[ -f "$RETINA_MODEL" ]] || error "retinaface.rknn not found: $RETINA_MODEL"
    ok "Model: $RETINA_MODEL ($(du -h "$RETINA_MODEL" | cut -f1))"
else
    skip "Model (not in target)"
fi

# --------------------------------------------------------------------------- #
#  6. Print deploy commands
# --------------------------------------------------------------------------- #
echo ""
ok "============================================================"
ok " Done!  targets=$TARGET_ARG"
ok "============================================================"
echo ""
echo "  ── Deploy to Luckfox Pico Pro (via SSH/SCP) ──"
echo "  # /oem is read-only — stage through /tmp then remount+install."
echo ""
echo "  # 1. Stage to /tmp"
[[ $BUILD_UVC     -eq 1 ]] && echo "  scp \"$UVC_BIN\"                        root@\$PICO_IP:/tmp/rk_mpi_uvc"
[[ $BUILD_VISCA   -eq 1 ]] && echo "  scp \"$VISCA_BIN\"                     root@\$PICO_IP:/tmp/visca_server"
[[ $BUILD_SCRIPTS -eq 1 ]] && echo "  scp \"$UVC_SCRIPTS_DIR/usb_config.sh\" root@\$PICO_IP:/tmp/usb_config.sh"
[[ $BUILD_SCRIPTS -eq 1 ]] && echo "  scp \"$UVC_SCRIPTS_DIR/rkuvc.ini\"     root@\$PICO_IP:/tmp/rkuvc.ini.new"
[[ $BUILD_SCRIPTS -eq 1 ]] && echo "  scp \"$RKLUNCH_SH\"                    root@\$PICO_IP:/tmp/RkLunch.sh"
[[ $BUILD_MODEL   -eq 1 ]] && echo "  scp \"$RETINA_MODEL\"                  root@\$PICO_IP:/tmp/retinaface.rknn"

echo ""
echo "  # 2. Kill, remount, install, reboot"

KILL_LIST=""
INSTALL_CMDS=""
CHMOD_LIST=""

[[ $BUILD_UVC     -eq 1 ]] && KILL_LIST+="rk_mpi_uvc "
[[ $BUILD_VISCA   -eq 1 ]] && KILL_LIST+="visca_server "
[[ $BUILD_UVC     -eq 1 ]] && INSTALL_CMDS+="cp /tmp/rk_mpi_uvc      /oem/usr/bin/rk_mpi_uvc && \\\\\n      "
[[ $BUILD_VISCA   -eq 1 ]] && INSTALL_CMDS+="cp /tmp/visca_server    /oem/usr/bin/visca_server && \\\\\n      "
[[ $BUILD_SCRIPTS -eq 1 ]] && INSTALL_CMDS+="cp /tmp/usb_config.sh   /oem/usr/bin/usb_config.sh && \\\\\n      "
[[ $BUILD_SCRIPTS -eq 1 ]] && INSTALL_CMDS+="cp /tmp/rkuvc.ini.new   /oem/usr/share/rkuvc.ini && \\\\\n      "
[[ $BUILD_SCRIPTS -eq 1 ]] && INSTALL_CMDS+="cp /tmp/RkLunch.sh      /oem/usr/bin/RkLunch.sh && \\\\\n      "
[[ $BUILD_MODEL   -eq 1 ]] && INSTALL_CMDS+="mkdir -p /oem/usr/share/models && \\\\\n      cp /tmp/retinaface.rknn /oem/usr/share/models/ && \\\\\n      "
[[ $BUILD_UVC     -eq 1 ]] && CHMOD_LIST+="/oem/usr/bin/rk_mpi_uvc "
[[ $BUILD_VISCA   -eq 1 ]] && CHMOD_LIST+="/oem/usr/bin/visca_server "
[[ $BUILD_SCRIPTS -eq 1 ]] && CHMOD_LIST+="/oem/usr/bin/usb_config.sh /oem/usr/bin/RkLunch.sh "

KILL_CMD="${KILL_LIST:+killall -9 ${KILL_LIST% } 2>/dev/null; }"
CHMOD_CMD="${CHMOD_LIST:+chmod +x ${CHMOD_LIST% } && \\\\}"

echo "  ssh root@\$PICO_IP \\"
echo "    \"${KILL_CMD}mount -o remount,rw /oem && \\"
echo -e "      ${INSTALL_CMDS}${CHMOD_CMD}"
echo "      reboot\""
echo ""
