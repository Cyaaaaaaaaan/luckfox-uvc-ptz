#!/bin/bash
# apply_patches.sh
# Directly edits the UVC source files instead of using patch(1).
# Safe to re-run — each edit checks if it's already been applied.

set -euo pipefail

SDK_DIR="${1:-$HOME/luckfox-pico}"
UVC_SRC="$SDK_DIR/project/app/uvc_app_tiny/uvc_app"

info()  { echo -e "\033[1;34m[INFO]\033[0m  $*"; }
ok()    { echo -e "\033[1;32m[ OK ]\033[0m  $*"; }
skip()  { echo -e "\033[1;33m[SKIP]\033[0m  $*"; }
error() { echo -e "\033[1;31m[ERR ]\033[0m  $*" >&2; exit 1; }

[[ -d "$UVC_SRC" ]] || error "UVC source not found: $UVC_SRC"

# ─────────────────────────────────────────────────────────────────────────────
# Helper: replace a block of text in a file
# Usage: replace_block FILE SEARCH_ANCHOR REPLACEMENT
# Uses python3 for reliable multi-line replacement
# ─────────────────────────────────────────────────────────────────────────────
replace_in_file() {
    local file="$1"
    local old="$2"
    local new="$3"
    if ! grep -qF "$old" "$file"; then
        return 1  # not found
    fi
    python3 - "$file" "$old" "$new" << 'PYEOF'
import sys
file, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
with open(file, 'r') as f:
    content = f.read()
if old not in content:
    sys.exit(1)
with open(file, 'w') as f:
    f.write(content.replace(old, new, 1))
PYEOF
}

# ═════════════════════════════════════════════════════════════════════════════
# PATCH 1 — uvc_mpi_vi.cpp
#   - Add static init-state tracking + mutex
#   - Make uvc_vi_config idempotent (skip device/pipe init after first time)
#   - Make uvc_vi_start idempotent (only EnableChn on first call)
# ═════════════════════════════════════════════════════════════════════════════
VI_FILE="$UVC_SRC/uvc/uvc_mpi_vi.cpp"
info "Patch 1: $VI_FILE"

# 1a. Add headers and state variables after the existing includes
if grep -q "s_vi_width" "$VI_FILE"; then
    skip "  1a: state variables already present"
else
    replace_in_file "$VI_FILE" \
'#include "uvc_mpi_vi.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "uvc_log.h"
#include "uvc_video.h"
#include <cstring>' \
'#include "uvc_mpi_vi.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "uvc_log.h"
#include "uvc_video.h"
#include <cstring>
#include <pthread.h>
#include <unistd.h>

/* 0 = uninitialised, 1 = device+pipe+channel configured, 2 = channel enabled */
static int s_vi_dev_inited[2]  = {0, 0};
static int s_vi_width[2]       = {0, 0};  /* last configured output width  */
static int s_vi_height[2]      = {0, 0};  /* last configured output height */
static int s_vi_was_resized[2] = {0, 0};  /* set on resize so vi_start applies longer resync sleep */
static pthread_mutex_t s_vi_init_mutex = PTHREAD_MUTEX_INITIALIZER;' \
    && ok "  1a: state variables added" \
    || error "  1a: could not add state variables — check $VI_FILE manually"
fi

# 1a2. Set stIspOpt.stMaxSize in uvc_get_vi_ctx so in-place SetChnAttr works
if grep -q "stIspOpt.stMaxSize" "$VI_FILE"; then
    skip "  1a2: stMaxSize already set in uvc_get_vi_ctx"
else
    replace_in_file "$VI_FILE" \
'  viCtx.stChnAttr.stSize.u32Width = uvcCfg->vi_cfg[chnType].assign_width;
  viCtx.stChnAttr.stSize.u32Height = uvcCfg->vi_cfg[chnType].assign_height;
  LOG_INFO("chnType:%d uvc out:%dx%d, vi resolution:%dx%d name:%s\n", chnType,' \
'  viCtx.stChnAttr.stSize.u32Width = uvcCfg->vi_cfg[chnType].assign_width;
  viCtx.stChnAttr.stSize.u32Height = uvcCfg->vi_cfg[chnType].assign_height;
  /* stIspOpt.stMaxSize must be non-zero for in-place SetChnAttr (resolution
   * switching without ISP restart). Set it to the sensor'"'"'s native maximum. */
  viCtx.stChnAttr.stIspOpt.stMaxSize.u32Width =
      (RK_U32)uvcCfg->vi_cfg[chnType].max_width;
  viCtx.stChnAttr.stIspOpt.stMaxSize.u32Height =
      (RK_U32)uvcCfg->vi_cfg[chnType].max_height;
  LOG_INFO("chnType:%d uvc out:%dx%d, vi resolution:%dx%d name:%s\n", chnType,' \
    && ok "  1a2: stMaxSize set in uvc_get_vi_ctx" \
    || error "  1a2: could not patch uvc_get_vi_ctx — check $VI_FILE manually"
fi

# 1b. Replace uvc_vi_config — first-time device/pipe guard + channel resize support
if grep -q "s_vi_width\[chnType\]" "$VI_FILE"; then
    skip "  1b: uvc_vi_config resize logic already present"
else
    python3 - "$VI_FILE" << 'PYEOF'
import sys, re
file = sys.argv[1]
with open(file, 'r') as f:
    content = f.read()

pattern = re.compile(
    r'// todo: multi vi from cfg\.\nRK_S32 uvc_vi_config\(UVC_MPI_CFG \*uvcCfg, MpiViChannelType chnType\) \{.*?'
    r'__FAILED:\n  return s32Ret;\n\}',
    re.DOTALL
)

replacement = (
    '// todo: multi vi from cfg.\n'
    'RK_S32 uvc_vi_config(UVC_MPI_CFG *uvcCfg, MpiViChannelType chnType) {\n'
    '  RK_S32 s32Ret = RK_FAILURE;\n'
    '  MPI_VI_CTX_S ctx;\n'
    '  /* Clear stale assign dims so uvc_get_vi_ctx recomputes from the current\n'
    '   * UVC-requested resolution. Without this, assign_width retains the last\n'
    '   * cycle\'s value and VI ignores the host\'s new resolution after format change. */\n'
    '  uvcCfg->vi_cfg[chnType].assign_width = 0;\n'
    '  uvcCfg->vi_cfg[chnType].assign_height = 0;\n'
    '  ctx = uvc_get_vi_ctx(uvcCfg, chnType);\n'
    '\n'
    '  pthread_mutex_lock(&s_vi_init_mutex);\n'
    '  if (!s_vi_dev_inited[chnType]) {\n'
    '    /* One-time device + pipe init. Never repeat while ISP is running. */\n'
    '\n'
    '    // 0. get dev config status\n'
    '    s32Ret = RK_MPI_VI_GetDevAttr(ctx.stChnCtx.devId, &ctx.stDevAttr);\n'
    '    if (s32Ret == RK_ERR_VI_NOT_CONFIG) {\n'
    '      s32Ret = RK_MPI_VI_SetDevAttr(ctx.stChnCtx.devId, &ctx.stDevAttr);\n'
    '      if (s32Ret != RK_SUCCESS) {\n'
    '        LOG_ERROR("RK_MPI_VI_SetDevAttr %x\\n", s32Ret);\n'
    '        pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '        goto __FAILED;\n'
    '      }\n'
    '    }\n'
    '    // 1. get dev enable status\n'
    '    s32Ret = RK_MPI_VI_GetDevIsEnable(ctx.stChnCtx.devId);\n'
    '    if (s32Ret != RK_SUCCESS) {\n'
    '      s32Ret = RK_MPI_VI_EnableDev(ctx.stChnCtx.devId);\n'
    '      if (s32Ret != RK_SUCCESS) {\n'
    '        LOG_ERROR("RK_MPI_VI_EnableDev %x\\n", s32Ret);\n'
    '        pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '        goto __FAILED;\n'
    '      }\n'
    '      ctx.stBindPipe.u32Num = ctx.stChnCtx.pipeId;\n'
    '      ctx.stBindPipe.PipeId[0] = ctx.stChnCtx.pipeId;\n'
    '      s32Ret = RK_MPI_VI_SetDevBindPipe(ctx.stChnCtx.devId, &ctx.stBindPipe);\n'
    '      if (s32Ret != RK_SUCCESS) {\n'
    '        LOG_ERROR("RK_MPI_VI_SetDevBindPipe %x\\n", s32Ret);\n'
    '        pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '        goto __FAILED;\n'
    '      }\n'
    '    }\n'
    '    // 2. config channel\n'
    '    s32Ret = RK_MPI_VI_SetChnAttr(ctx.stChnCtx.pipeId, ctx.stChnCtx.channelId,\n'
    '                                  &ctx.stChnAttr);\n'
    '    if (s32Ret != RK_SUCCESS) {\n'
    '      LOG_ERROR("RK_MPI_VI_SetChnAttr %x\\n", s32Ret);\n'
    '      pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '      goto __FAILED;\n'
    '    }\n'
    '    s_vi_dev_inited[chnType] = 1;\n'
    '    s_vi_width[chnType]  = (int)ctx.stChnAttr.stSize.u32Width;\n'
    '    s_vi_height[chnType] = (int)ctx.stChnAttr.stSize.u32Height;\n'
    '    LOG_INFO("uvc_vi_config: first init chnType %d at %dx%d\\n",\n'
    '             chnType, s_vi_width[chnType], s_vi_height[chnType]);\n'
    '  } else {\n'
    '    int new_w = (int)ctx.stChnAttr.stSize.u32Width;\n'
    '    int new_h = (int)ctx.stChnAttr.stSize.u32Height;\n'
    '    if (new_w == s_vi_width[chnType] && new_h == s_vi_height[chnType]) {\n'
    '      /* Same resolution — nothing to do */\n'
    '      LOG_INFO("uvc_vi_config: chnType %d already at %dx%d, skipping\\n", chnType, new_w, new_h);\n'
    '      s32Ret = RK_SUCCESS;\n'
    '    } else {\n'
    '      /* Resolution changed. Call SetChnAttr on the running channel WITHOUT\n'
    '       * calling DisableChn first. DisableChn triggers a full ISP restart which\n'
    '       * causes AIQ SOF disorder (frame counter reset); after that, on RV1106,\n'
    '       * VI stops delivering frames entirely and GetChnFrame times out\n'
    '       * indefinitely. Calling SetChnAttr in-place avoids the ISP restart. */\n'
    '      LOG_INFO("uvc_vi_config: chnType %d resize %dx%d -> %dx%d (in-place, no ISP restart)\\n",\n'
    '               chnType, s_vi_width[chnType], s_vi_height[chnType], new_w, new_h);\n'
    '      s32Ret = RK_MPI_VI_SetChnAttr(ctx.stChnCtx.pipeId, ctx.stChnCtx.channelId,\n'
    '                                    &ctx.stChnAttr);\n'
    '      if (s32Ret != RK_SUCCESS) {\n'
    '        LOG_ERROR("RK_MPI_VI_SetChnAttr (resize in-place) %x\\n", s32Ret);\n'
    '        pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '        goto __FAILED;\n'
    '      }\n'
    '      s_vi_width[chnType]       = new_w;\n'
    '      s_vi_height[chnType]      = new_h;\n'
    '      s_vi_was_resized[chnType] = 1;\n'
    '      /* State stays 2 — channel is still running, uvc_vi_start is a no-op */\n'
    '    }\n'
    '  }\n'
    '  pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '\n'
    '__FAILED:\n'
    '  return s32Ret;\n'
    '}'
)

m = pattern.search(content)
if not m:
    print("NOT_FOUND")
    sys.exit(1)
result = content[:m.start()] + replacement + content[m.end():]
with open(file, 'w') as f:
    f.write(result)
print("OK")
PYEOF
    RET=$?
    if [ $RET -eq 0 ]; then
        ok "  1b: uvc_vi_config resize logic added"
    else
        error "  1b: could not patch uvc_vi_config — check $VI_FILE manually"
    fi
fi

# 1c. Replace uvc_vi_start: first-boot EnableChn with 500ms settle;
#     in-place resize (state=2) just sleeps 300ms — no ISP restart needed
if grep -q "in-place resize.*300ms" "$VI_FILE"; then
    skip "  1c: uvc_vi_start already patched (in-place resize)"
else
    python3 - "$VI_FILE" << 'PYEOF'
import sys, re
file = sys.argv[1]
with open(file, 'r') as f:
    content = f.read()

pattern = re.compile(
    r'RK_S32 uvc_vi_start\(UVC_MPI_CFG uvcCfg, MpiViChannelType chnType\) \{.*?'
    r'__FAILED:\n  return s32Ret;\n\}',
    re.DOTALL
)

replacement = (
    'RK_S32 uvc_vi_start(UVC_MPI_CFG uvcCfg, MpiViChannelType chnType) {\n'
    '  RK_S32 s32Ret = RK_FAILURE;\n'
    '  UVC_VI_CHN_CTX_S ctx;\n'
    '  ctx = uvc_get_vi_chn_ctx(uvcCfg, chnType);\n'
    '\n'
    '  pthread_mutex_lock(&s_vi_init_mutex);\n'
    '  if (s_vi_dev_inited[chnType] == 1) {\n'
    '    /* First boot: channel configured but not yet running. */\n'
    '    LOG_INFO("uvc_vi_start: EnableChn dev=%d pipe=%d chn=%d\\n",\n'
    '             ctx.devId, ctx.pipeId, ctx.channelId);\n'
    '    s32Ret = RK_MPI_VI_EnableChn(ctx.pipeId, ctx.channelId);\n'
    '    if (s32Ret != RK_SUCCESS) {\n'
    '      LOG_ERROR("RK_MPI_VI_EnableChn %x\\n", s32Ret);\n'
    '      pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '      goto __FAILED;\n'
    '    }\n'
    '    s_vi_dev_inited[chnType] = 2;\n'
    '    pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '    usleep(500000); /* 500ms: ISP startup settle time */\n'
    '    return s32Ret;\n'
    '  } else {\n'
    '    /* Channel already running (state=2). For in-place resize (SetChnAttr\n'
    '     * without ISP restart), rockit may need a short moment to apply. */\n'
    '    int was_resized = s_vi_was_resized[chnType];\n'
    '    s_vi_was_resized[chnType] = 0;\n'
    '    s32Ret = RK_SUCCESS;\n'
    '    pthread_mutex_unlock(&s_vi_init_mutex);\n'
    '    if (was_resized) {\n'
    '      LOG_INFO("uvc_vi_start: VI running, in-place resize — sleeping 300ms\\n");\n'
    '      usleep(300000);\n'
    '    }\n'
    '    return s32Ret;\n'
    '  }\n'
    '\n'
    '__FAILED:\n'
    '  return s32Ret;\n'
    '}'
)

m = pattern.search(content)
if not m:
    print("NOT_FOUND")
    sys.exit(1)
result = content[:m.start()] + replacement + content[m.end():]
with open(file, 'w') as f:
    f.write(result)
print("OK")
PYEOF
    RET=$?
    if [ $RET -eq 0 ]; then
        ok "  1c: uvc_vi_start patched (poll approach)"
    else
        error "  1c: could not patch uvc_vi_start — check $VI_FILE manually"
    fi
fi

ok "Patch 1 complete: $VI_FILE"

# ═════════════════════════════════════════════════════════════════════════════
# PATCH 2 — uvc_process.cpp
#   - Add mViEverStarted flag to UVCProcessCtx
#   - Skip 2s ISP sleep on subsequent streamon
#   - startProcess: only restart VENC (not VI) after first cycle
#   - stopProcess: don't call stopVi() in VENC paths
# ═════════════════════════════════════════════════════════════════════════════
PROC_FILE="$UVC_SRC/uvc/uvc_process.cpp"
info "Patch 2: $PROC_FILE"

# 2a. Add mViEverStarted to UVCProcessCtx struct
if grep -q "mViEverStarted" "$PROC_FILE"; then
    skip "  2a: mViEverStarted already present"
else
    replace_in_file "$PROC_FILE" \
'  int mUvcBuffCount;
  bool mStart;
  std::thread *mThread;' \
'  int mUvcBuffCount;
  bool mStart;
  bool mViEverStarted;   /* VI has been enabled at least once this session */
  std::thread *mThread;' \
    && ok "  2a: mViEverStarted added to struct" \
    || error "  2a: could not add mViEverStarted — check $PROC_FILE manually"
fi

# 2b. Skip 2s sleep on subsequent startVi calls
if grep -q "only stall on first boot" "$PROC_FILE"; then
    skip "  2b: ISP sleep guard already present"
else
    python3 - "$PROC_FILE" << 'PYEOF'
import sys, re
file = sys.argv[1]
with open(file, 'r') as f:
    content = f.read()
pattern = re.compile(
    r'(int UVCProcess::startVi\(\) \{.*?#ifndef RK_ENABLE_FASTBOOT\s*)\n'
    r'(  uvc_vi_start\(ctx->mUvcCfg, MPI_VI_CHANNEL_TYPE_UVC\);)',
    re.DOTALL
)
replacement = (
    r'\1\n'
    r'  if (!ctx->mViEverStarted) usleep(2000000); /* only stall on first boot */\n'
    r'\2'
)
result, n = pattern.subn(replacement, content)
if n == 0:
    print("NOT_FOUND"); sys.exit(1)
with open(file, 'w') as f:
    f.write(result)
print("OK")
PYEOF
    [ $? -eq 0 ] && ok "  2b: ISP sleep guard added" \
                 || error "  2b: could not add ISP sleep guard — check $PROC_FILE manually"
fi

# 2c. startProcess — VI_VENC path: skip startVi on subsequent cycles
if grep -q "mViEverStarted = true" "$PROC_FILE"; then
    skip "  2cd: startProcess guards already present"
else
    python3 - "$PROC_FILE" << 'PYEOF'
import sys, re
file = sys.argv[1]
with open(file, 'r') as f:
    content = f.read()

old = (
    '  case UVC_FMT_TYPE_VI_UVC:\n'
    '    startVi();\n'
    '    break;\n'
    '  case UVC_FMT_TYPE_VI_VPSS_UVC:\n'
    '    startVi();\n'
    '    startVpss();\n'
    '    bindViVpss();\n'
    '    break;\n'
    '  case UVC_FMT_TYPE_VI_VENC_UVC:\n'
    '    startVi();\n'
    '    startVenc();\n'
    '#ifndef RK_ENABLE_FASTBOOT\n'
    '    bindViVenc();\n'
    '#endif\n'
    '    break;\n'
    '  case UVC_FMT_TYPE_VI_VPSS_VENC_UVC:\n'
    '    startVi();\n'
    '    startVpss();\n'
    '    startVenc();\n'
    '    bindViVpss();\n'
    '    bindVpssVenc();\n'
    '    break;'
)

new = (
    '  case UVC_FMT_TYPE_VI_UVC:\n'
    '    startVi();\n'
    '    ctx->mViEverStarted = true;\n'
    '    break;\n'
    '  case UVC_FMT_TYPE_VI_VPSS_UVC:\n'
    '    startVi();\n'
    '    ctx->mViEverStarted = true;\n'
    '    startVpss();\n'
    '    bindViVpss();\n'
    '    break;\n'
    '  case UVC_FMT_TYPE_VI_VENC_UVC:\n'
    '    /* Always call startVi: no-op if channel already running (state==2),\n'
    '     * re-enables the channel if VI was reconfigured for a new resolution (state==1). */\n'
    '    startVi();\n'
    '    ctx->mViEverStarted = true;\n'
    '    startVenc();\n'
    '#ifndef RK_ENABLE_FASTBOOT\n'
    '    bindViVenc();\n'
    '#endif\n'
    '    break;\n'
    '  case UVC_FMT_TYPE_VI_VPSS_VENC_UVC:\n'
    '    if (!ctx->mViEverStarted) {\n'
    '      startVi();\n'
    '      ctx->mViEverStarted = true;\n'
    '      startVpss();\n'
    '      startVenc();\n'
    '      bindViVpss();\n'
    '      bindVpssVenc();\n'
    '    } else {\n'
    '      startVpss();\n'
    '      startVenc();\n'
    '      bindViVpss();\n'
    '      bindVpssVenc();\n'
    '    }\n'
    '    break;'
)

if old not in content:
    print("NOT_FOUND"); sys.exit(1)
with open(file, 'w') as f:
    f.write(content.replace(old, new, 1))
print("OK")
PYEOF
    [ $? -eq 0 ] && ok "  2cd: startProcess guards added" \
                 || error "  2cd: could not patch startProcess — check $PROC_FILE manually"
fi

# 2e. stopProcess — remove stopVi() from VI_VENC and VI_VPSS_VENC paths
if grep -q "VI left running" "$PROC_FILE"; then
    skip "  2e: stopProcess VI suppression already present"
else
    replace_in_file "$PROC_FILE" \
'  case UVC_FMT_TYPE_VI_VENC_UVC:
#ifndef RK_ENABLE_FASTBOOT
    unBindViVenc();
    stopVenc();
    stopVi();
#endif
    break;' \
'  case UVC_FMT_TYPE_VI_VENC_UVC:
#ifndef RK_ENABLE_FASTBOOT
    unBindViVenc();
    stopVenc();
    /* DO NOT stopVi() — VI stays running to keep ISP SOF counter in sync */
    LOG_INFO("stopProcess: VENC destroyed, VI left running\n");
#endif
    break;' \
    && ok "  2e: stopVi suppressed in VI_VENC path" \
    || error "  2e: could not suppress stopVi in VI_VENC — check $PROC_FILE manually"

    replace_in_file "$PROC_FILE" \
'  case UVC_FMT_TYPE_VI_VPSS_VENC_UVC:
    unBindViVpss();
    unBindVpssVenc();
    stopVenc();
    stopVpss();
    stopVi();
    break;' \
'  case UVC_FMT_TYPE_VI_VPSS_VENC_UVC:
    unBindViVpss();
    unBindVpssVenc();
    stopVenc();
    stopVpss();
    /* DO NOT stopVi() — VI stays running to keep ISP SOF counter in sync */
    LOG_INFO("stopProcess: VPSS+VENC destroyed, VI left running\n");
    break;' \
    && ok "  2e: stopVi suppressed in VI_VPSS_VENC path" \
    || error "  2e: could not suppress stopVi in VI_VPSS_VENC — check $PROC_FILE manually"
fi

ok "Patch 2 complete: $PROC_FILE"

# ═════════════════════════════════════════════════════════════════════════════
# PATCH 3 — camera_control.c
#   - camera_control_stop: explicit no-op with comment explaining why
# ═════════════════════════════════════════════════════════════════════════════
CAM_FILE="$UVC_SRC/uvc/camera_control.c"
info "Patch 3: $CAM_FILE"

if grep -q "AIQ context must remain alive" "$CAM_FILE"; then
    skip "  camera_control_stop already patched"
else
    python3 - "$CAM_FILE" << 'PYEOF'
import sys, re
file = sys.argv[1]
with open(file, 'r') as f:
    content = f.read()

pattern = re.compile(
    r'void camera_control_stop\(int id\) \{.*?\}',
    re.DOTALL
)

replacement = (
    'void camera_control_stop(int id) {\n'
    '  LOG_INFO("%s: id:%d\\n", __func__, id);\n'
    '  /* Intentional no-op: do NOT call rk_isp_deinit() here.\n'
    '   * The AIQ context must remain alive between streamoff/streamon cycles\n'
    '   * so the ISP SOF frame counter stays in sync with the hardware.\n'
    '   * Tearing down AIQ resets its internal counters while the hardware\n'
    '   * keeps running, causing "ISP sof disorder" flood on next streamon.\n'
    '   * AIQ is torn down only in camera_control_deinit() on process exit. */\n'
    '  (void)id;\n'
    '}'
)

m = pattern.search(content)
if not m:
    print("NOT_FOUND"); sys.exit(1)
with open(file, 'w') as f:
    f.write(content[:m.start()] + replacement + content[m.end():])
print("OK")
PYEOF
    [ $? -eq 0 ] && ok "  camera_control_stop patched" \
                 || error "  could not patch camera_control_stop — check $CAM_FILE manually"
fi

ok "Patch 3 complete: $CAM_FILE"

# ═════════════════════════════════════════════════════════════════════════════
# PATCH 4 — usb_config.sh
#   - Single UVC gadget only (UVC_MULTI=off): removes "RK UVC", keeps "UVC RGB"
#   - Clean MJPEG/H264/H265 resolution list for MIS5001 (max 2592x1944)
#   - MJPEG dwMaxVideoFrameBufferSize: *2 → *3 for headroom
# ═════════════════════════════════════════════════════════════════════════════
USB_FILE="$SDK_DIR/project/app/uvc_app_tiny/uvc_app/usb_config.sh"
info "Patch 4: $USB_FILE"

# 4a. Single UVC gadget
if grep -q "UVC_MULTI=off" "$USB_FILE"; then
    skip "  4a: UVC_MULTI already off"
else
    replace_in_file "$USB_FILE" \
'UVC_MULTI=one' \
'UVC_MULTI=off' \
    && ok "  4a: UVC_MULTI set to off (single UVC gadget)" \
    || error "  4a: could not set UVC_MULTI=off — check $USB_FILE manually"
fi

# 4b. Remove YUYV and set 4:3-only resolution lists for MJPEG/H264/H265
if grep -q "4:3 resolutions only" "$USB_FILE"; then
    skip "  4b-4d: resolution lists already at 4:3-only (no YUYV)"
else
    python3 - "$USB_FILE" << 'PYEOF'
import sys, re
file = sys.argv[1]
with open(file, 'r') as f:
    content = f.read()

old = re.compile(
    r'  ##YUYV support config\n'
    r'  mkdir /sys/kernel/config/usb_gadget/rockchip/functions/\$UVC_GS/streaming/uncompressed/u\n'
    r'(?:  (?:configure_uvc_resolution_yuyv[^\n]*|#[^\n]*)\n)*'
    r'\n'
    r'  ##mjpeg support config\n'
    r'  mkdir \$\{USB_FUNCTIONS_DIR\}/\$UVC_GS/streaming/mjpeg/m\n'
    r'(?:  configure_uvc_resolution_mjpeg[^\n]*\n)+'
    r'\n'
    r'  ## h\.264 support config\n'
    r'  mkdir \$\{USB_FUNCTIONS_DIR\}/\$UVC_GS/streaming/framebased/f1\n'
    r'(?:  configure_uvc_resolution_h264[^\n]*\n)+'
    r'\n'
    r'  ## h\.265 support config\n'
    r'  mkdir \$\{USB_FUNCTIONS_DIR\}/\$UVC_GS/streaming/framebased/f2\n'
    r'(?:  configure_uvc_resolution_h265[^\n]*\n)+'
    r'\n'
    r'  mkdir /sys/kernel/config/usb_gadget/rockchip/functions/\$UVC_GS/streaming/header/h\n'
    r'  ln -s \$\{USB_FUNCTIONS_DIR\}/\$UVC_GS/streaming/uncompressed/u \$\{USB_FUNCTIONS_DIR\}/\$UVC_GS/streaming/header/h/u\n'
    r'  ln -s \$\{USB_FUNCTIONS_DIR\}/\$UVC_GS/streaming/mjpeg/m',
    re.MULTILINE
)

new = (
    '  ##mjpeg support config — 4:3 resolutions only\n'
    '  mkdir ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming/mjpeg/m\n'
    '  configure_uvc_resolution_mjpeg 640 480\n'
    '  configure_uvc_resolution_mjpeg 1024 768\n'
    '  configure_uvc_resolution_mjpeg 1280 960\n'
    '  configure_uvc_resolution_mjpeg 2048 1536\n'
    '  configure_uvc_resolution_mjpeg 2592 1944\n'
    '\n'
    '  ## h.264 support config — 4:3 resolutions only\n'
    '  mkdir ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming/framebased/f1\n'
    '  configure_uvc_resolution_h264 640 480\n'
    '  configure_uvc_resolution_h264 1280 960\n'
    '  configure_uvc_resolution_h264 2048 1536\n'
    '  configure_uvc_resolution_h264 2592 1944\n'
    '\n'
    '  ## h.265 support config — 4:3 resolutions only\n'
    '  mkdir ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming/framebased/f2\n'
    '  configure_uvc_resolution_h265 640 480\n'
    '  configure_uvc_resolution_h265 1280 960\n'
    '  configure_uvc_resolution_h265 2048 1536\n'
    '  configure_uvc_resolution_h265 2592 1944\n'
    '\n'
    '  mkdir /sys/kernel/config/usb_gadget/rockchip/functions/$UVC_GS/streaming/header/h\n'
    '  ln -s ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming/mjpeg/m'
)

result, count = old.subn(new, content)
if count == 0:
    print("NOT_FOUND")
    sys.exit(1)
with open(file, 'w') as f:
    f.write(result)
print("OK")
PYEOF
    RET=$?
    if [ $RET -eq 0 ]; then
        ok "  4b-4d: YUYV removed, 4:3-only resolution lists set"
    else
        error "  4b-4d: could not update resolution lists — check $USB_FILE manually"
    fi
fi

# 4e. streaming_maxpacket: off/one both use 2048 (3072 is USB3-only, rejects on RV1106)
if grep -q "UVC_MULTI = two" "$USB_FILE" && ! grep -q "echo 3072" "$USB_FILE"; then
    skip "  4e: streaming_maxpacket already fixed"
else
    replace_in_file "$USB_FILE" \
'  if [ $UVC_MULTI = one ];then
     echo 2048 > ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming_maxpacket
  elif [ $UVC_MULTI = two ];then
     echo 1024 > ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming_maxpacket
  else
     echo 3072 > ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming_maxpacket
  fi' \
'  if [ $UVC_MULTI = two ];then
     echo 1024 > ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming_maxpacket
  else
     echo 2048 > ${USB_FUNCTIONS_DIR}/$UVC_GS/streaming_maxpacket
  fi' \
    && ok "  4e: streaming_maxpacket set to 2048 for off/one modes" \
    || error "  4e: could not fix streaming_maxpacket — check $USB_FILE manually"
fi

# 4f. MJPEG buffer size *2 → *3 (scoped to configure_uvc_resolution_mjpeg only)
if grep -q "mjpeg/m/\${UVC_DISPLAY_W}" "$USB_FILE" && \
   grep -A8 "mjpeg/m/\${UVC_DISPLAY_W}" "$USB_FILE" | grep -q "UVC_DISPLAY_H\*3"; then
    skip "  4e: MJPEG dwMaxVideoFrameBufferSize already at *3"
else
    replace_in_file "$USB_FILE" \
'    UVC_DISPLAY_DIR=${USB_FUNCTIONS_DIR}/$UVC_GS/streaming/mjpeg/m/${UVC_DISPLAY_W}_${UVC_DISPLAY_H}p
    mkdir ${UVC_DISPLAY_DIR}
    echo $UVC_DISPLAY_W > ${UVC_DISPLAY_DIR}/wWidth
    echo $UVC_DISPLAY_H > ${UVC_DISPLAY_DIR}/wHeight
    echo 333333 > ${UVC_DISPLAY_DIR}/dwDefaultFrameInterval
    echo $((UVC_DISPLAY_W*UVC_DISPLAY_H*20)) > ${UVC_DISPLAY_DIR}/dwMinBitRate
    echo $((UVC_DISPLAY_W*UVC_DISPLAY_H*20)) > ${UVC_DISPLAY_DIR}/dwMaxBitRate
    echo $((UVC_DISPLAY_W*UVC_DISPLAY_H*2)) > ${UVC_DISPLAY_DIR}/dwMaxVideoFrameBufferSize' \
'    UVC_DISPLAY_DIR=${USB_FUNCTIONS_DIR}/$UVC_GS/streaming/mjpeg/m/${UVC_DISPLAY_W}_${UVC_DISPLAY_H}p
    mkdir ${UVC_DISPLAY_DIR}
    echo $UVC_DISPLAY_W > ${UVC_DISPLAY_DIR}/wWidth
    echo $UVC_DISPLAY_H > ${UVC_DISPLAY_DIR}/wHeight
    echo 333333 > ${UVC_DISPLAY_DIR}/dwDefaultFrameInterval
    echo $((UVC_DISPLAY_W*UVC_DISPLAY_H*20)) > ${UVC_DISPLAY_DIR}/dwMinBitRate
    echo $((UVC_DISPLAY_W*UVC_DISPLAY_H*20)) > ${UVC_DISPLAY_DIR}/dwMaxBitRate
    echo $((UVC_DISPLAY_W*UVC_DISPLAY_H*3)) > ${UVC_DISPLAY_DIR}/dwMaxVideoFrameBufferSize' \
    && ok "  4e: MJPEG dwMaxVideoFrameBufferSize set to W*H*3" \
    || error "  4e: could not fix MJPEG buffer size — check $USB_FILE manually"
fi

ok "Patch 4 complete: $USB_FILE"  # sub-steps 4a–4f

# ═════════════════════════════════════════════════════════════════════════════
# PATCH 5 — isp.c
#   Remove preInit_scene from sample_common_isp_init().
#   AIQ v5.0 on RV1106/MIS5001: any call to preInit_scene (even with empty
#   sub_scene) causes sysctl_init() to deadlock inside XCORE, printing
#   "XCORE:E:invalid main scene len!". The camera-group path already uses
#   ret=0. Apply the same no-op to the single-camera path.
# ═════════════════════════════════════════════════════════════════════════════
ISP_FILE="$UVC_SRC/isp/isp.c"
info "Patch 5: $ISP_FILE"

if grep -q "preInit_scene intentionally omitted" "$ISP_FILE"; then
    skip "  isp.c preInit_scene already removed"
else
    replace_in_file "$ISP_FILE" \
'  if (WDRMode == RK_AIQ_WORKING_MODE_NORMAL)
    ret = rk_aiq_uapi2_sysctl_preInit_scene(
        aiq_static_info.sensor_info.sensor_name, "normal", "day");
  else
    ret = rk_aiq_uapi2_sysctl_preInit_scene(
        aiq_static_info.sensor_info.sensor_name, "hdr", "day");
  if (ret < 0)
    LOG_ERROR("%s: failed to set scene\n",
              aiq_static_info.sensor_info.sensor_name);' \
'  /* preInit_scene intentionally omitted: AIQ v5.0 on RV1106/MIS5001 does not
   * support scene-based calibration with the stock IQ files. Any call to
   * preInit_scene (even with empty sub_scene "") causes sysctl_init() to
   * deadlock inside XCORE, emitting "invalid main scene len!". The camgroup
   * path already skips this call. Use default calibration. */' \
    && ok "  preInit_scene removed from sample_common_isp_init" \
    || error "  could not remove preInit_scene — check $ISP_FILE manually"
fi

ok "Patch 5 complete: $ISP_FILE"

# ═════════════════════════════════════════════════════════════════════════════
# PATCH 6 — uvc_mpi_config.c
#   - VI max resolution: 2560x1440 → 2592x1944 (MIS5001 native)
#     Without this, uvc_get_vi_ctx clips VI output to 2560x1440 while VENC
#     is configured for 2592x1944, causing a resolution mismatch → empty frames.
#   - MJPEG/H264/H265 VENC fps: -1 → 30
#     fps=-1 flows into fr32DstFrameRateNum as 0xFFFFFFFF, which rockit
#     rejects with "illegal param" → VENC hw channel never created ("hw is no create").
#   - uvc_enable_vpss: keep 0 (VPSS disabled — RGA node mapping fails on RV1106).
#     Resolution switching is solved by Patch 1 VI channel-only resize instead.
# ═════════════════════════════════════════════════════════════════════════════
CFG_FILE="$UVC_SRC/uvc/uvc_mpi_config.c"
info "Patch 6: $CFG_FILE"

# 6a. VI max resolution
if grep -q "max_width = 2592" "$CFG_FILE"; then
    skip "  6a: VI max resolution already 2592x1944"
else
    replace_in_file "$CFG_FILE" \
'  mpiCfg->vi_cfg[MPI_VI_CHANNEL_TYPE_UVC].max_width = 2560;
  mpiCfg->vi_cfg[MPI_VI_CHANNEL_TYPE_UVC].max_height = 1440;' \
'  mpiCfg->vi_cfg[MPI_VI_CHANNEL_TYPE_UVC].max_width = 2592;
  mpiCfg->vi_cfg[MPI_VI_CHANNEL_TYPE_UVC].max_height = 1944;' \
    && ok "  6a: VI max resolution set to 2592x1944" \
    || error "  6a: could not set VI max resolution — check $CFG_FILE manually"
fi

# 6b. MJPEG VENC fps
if grep -q "mjpeg_cfg.fps_in = 30" "$CFG_FILE"; then
    skip "  6b: MJPEG VENC fps already 30"
else
    replace_in_file "$CFG_FILE" \
'  mpiCfg->venc_cfg.mjpeg_cfg.fps_in = -1;
  mpiCfg->venc_cfg.mjpeg_cfg.fps_out = -1;' \
'  mpiCfg->venc_cfg.mjpeg_cfg.fps_in = 30;
  mpiCfg->venc_cfg.mjpeg_cfg.fps_out = 30;' \
    && ok "  6b: MJPEG VENC fps set to 30" \
    || error "  6b: could not set MJPEG VENC fps — check $CFG_FILE manually"
fi

# 6c. Keep VPSS disabled (uvc_enable_vpss=0, VI→VENC direct).
#     VPSS was tried (uvc_enable_vpss=1) but RGA node mapping fails on RV1106
#     at 2592x1944: "VPSS GRP 0 CHN 0 map RGA node failed 0xa0068009".
#     Resolution switching is handled instead by Patch 1 (uvc_vi_config
#     channel-only resize: DisableChn → SetChnAttr → re-EnableChn via startVi).
if grep -q "uvc_enable_vpss = 0" "$CFG_FILE"; then
    skip "  6c: uvc_enable_vpss already 0 (VPSS disabled, correct for RV1106)"
else
    replace_in_file "$CFG_FILE" \
'  mpiCfg->common_cfg.uvc_enable_vpss = 1;' \
'  mpiCfg->common_cfg.uvc_enable_vpss = 0;' \
    && ok "  6c: uvc_enable_vpss kept at 0" \
    || error "  6c: could not set uvc_enable_vpss — check $CFG_FILE manually"
fi

# 6d. H264 VENC fps
if grep -q "h264_cfg.fps_in = 30" "$CFG_FILE"; then
    skip "  6d: H264 VENC fps already 30"
else
    replace_in_file "$CFG_FILE" \
'  mpiCfg->venc_cfg.h264_cfg.fps_in = -1;
  mpiCfg->venc_cfg.h264_cfg.fps_out = -1;' \
'  mpiCfg->venc_cfg.h264_cfg.fps_in = 30;
  mpiCfg->venc_cfg.h264_cfg.fps_out = 30;' \
    && ok "  6d: H264 VENC fps set to 30" \
    || error "  6d: could not set H264 VENC fps — check $CFG_FILE manually"
fi

# 6e. H265 VENC fps
if grep -q "h265_cfg.fps_in = 30" "$CFG_FILE"; then
    skip "  6e: H265 VENC fps already 30"
else
    replace_in_file "$CFG_FILE" \
'  mpiCfg->venc_cfg.h265_cfg.fps_in = -1;
  mpiCfg->venc_cfg.h265_cfg.fps_out = -1;' \
'  mpiCfg->venc_cfg.h265_cfg.fps_in = 30;
  mpiCfg->venc_cfg.h265_cfg.fps_out = 30;' \
    && ok "  6e: H265 VENC fps set to 30" \
    || error "  6e: could not set H265 VENC fps — check $CFG_FILE manually"
fi

ok "Patch 6 complete: $CFG_FILE"

echo ""
ok "══════════════════════════════════════════"
ok " All patches applied successfully!"
ok "══════════════════════════════════════════"
echo ""
echo "  Next: run ./build.sh --skip-patches"
echo "        (patches are already in source)"
echo ""
