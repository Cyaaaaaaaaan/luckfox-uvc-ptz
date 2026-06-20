#include "focus_score.h"
#include "eptz.h"
#include "menu_osd.h"
#include "uvc_mpi_vi.h"
#include "uvc_log.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "isp.h"
#include "rk_aiq_user_api2_af.h"
#if FOCUS_SCORE_OSD
#include "rk_mpi_rgn.h"
#include "rk_mpi_venc.h"
#endif
#if FOCUS_SCORE_FACE_DETECT
#include "rknn_api.h"
#include "rknn_box_priors.h"
#include <climits>
#endif
#if FOCUS_SCORE_PERSON_DETECT
#include "person_det.h"
#endif
#if FOCUS_SCORE_TARGET_DETECT
#include <math.h>   /* fabsf in the auto-tracking P-controller */
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define FOCUS_SCORE_PATH  "/tmp/focus_score"
#define SCORE_CROP_W      320
#define SCORE_CROP_H      240
/* Detection + focus-score + auto-track cadence. Lower = snappier tracking but
 * more NPU/CPU load. Actual rate is (this sleep + inference time), so the NPU
 * inference time is the real floor. 100 ms ≈ up to 10 Hz. */
#define SCORE_INTERVAL_US 100000
#define GETFRAME_TIMEOUT  200

static pthread_t       s_tid;
static volatile int    s_quit    = 0;
static volatile double s_score   = 0.0;
static int             s_pipe_id = 0;
static int             s_chn_id  = 0;

/* --------------------------------------------------------------------------
 * Tenengrad focus measure: mean squared Sobel gradient energy.
 * Score peaks sharply at focus for any scene content.
 * -------------------------------------------------------------------------- */
static double tenengrad_score(const uint8_t *y, int stride,
                               int cx, int cy, int cw, int ch)
{
    long long sum_sq = 0;
    long      n      = 0;

    for (int row = 1; row < ch - 1; row++) {
        int fy = cy + row;
        const uint8_t *row_cur  = y + fy       * stride;
        const uint8_t *row_prev = y + (fy - 1) * stride;
        const uint8_t *row_next = y + (fy + 1) * stride;
        for (int col = 1; col < cw - 1; col++) {
            int fx = cx + col;
            int gx = (int)row_cur[fx + 1]  - (int)row_cur[fx - 1];
            int gy = (int)row_next[fx]      - (int)row_prev[fx];
            sum_sq += gx * gx + gy * gy;
            n++;
        }
    }
    return (n == 0) ? 0.0 : (double)sum_sq / n;
}

/* ==========================================================================
 * OSD overlay — compiled out when FOCUS_SCORE_OSD == 0
 * ========================================================================== */
#if FOCUS_SCORE_OSD

/* 8×8 bitmap font — digits 0-9, '.', 'F', 'S', ':', ' '
 * Each byte is one row, MSB = leftmost pixel.               */
static const uint8_t s_font[][8] = {
    /* 0 */ { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 },
    /* 1 */ { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 },
    /* 2 */ { 0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00 },
    /* 3 */ { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 },
    /* 4 */ { 0x06, 0x0E, 0x1E, 0x66, 0x7F, 0x06, 0x06, 0x00 },
    /* 5 */ { 0x7F, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 },
    /* 6 */ { 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 },
    /* 7 */ { 0x7F, 0x66, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00 },
    /* 8 */ { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 },
    /* 9 */ { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00 },
    /* . */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 },
    /* F */ { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00 },
    /* S */ { 0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00 },
    /* : */ { 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00 },
    /* ' '*/{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

static int char_idx(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '.')  return 10;
    if (c == 'F')  return 11;
    if (c == 'S')  return 12;
    if (c == ':')  return 13;
    return 14;
}

/* RK_FMT_RGBA8888 in memory: [R, G, B, A] bytes → uint32 = 0xAABBGGRR */
#define RGBA(r,g,b,a) ((uint32_t)((a)<<24|(b)<<16|(g)<<8|(r)))
#define OSD_FG        RGBA(0xFF,0xFF,0xFF,0xFF)  /* white, opaque        */
#define OSD_BG        RGBA(0x00,0x00,0x00,0xA0)  /* black, 63% alpha     */
/* Characters × 16px (8px font × 2× scale) = region width. Must be ×16. */
#define OSD_CHARS        8   /* "FS:NNNNN" */
#define OSD_W           (OSD_CHARS * 16)   /* 128 */
#define OSD_H            16
#define OSD_HANDLE          5   /* text score OVERLAY_RGN                    */
#define FACE_BOX_HANDLE_BASE 6  /* OVERLAY_RGN handles 6-9: top/bot/lft/rgt */
#define FACE_BOX_BORDER_PX  16  /* border thickness — must be multiple of 16 */
/* RGBA8888 in memory: [R,G,B,A] bytes → uint32 LE = 0xAABBGGRR */
#define FACE_BOX_RGBA    RGBA(0x00,0xFF,0x00,0xFF)  /* lime green, opaque   */

static volatile int s_osd_ready      = 0;
static volatile int s_face_box_ready = 0;
static volatile int s_osd_visible      = 0;  /* off by default, toggle with menu btn */
static volatile int s_facebox_visible  = 0;
static int          s_fbw            = 0;  /* VENC frame width  (pixels) */
static int          s_fbh            = 0;  /* VENC frame height (pixels) */
static int          s_fb_vdev        = 0;
static int          s_fb_vchn        = 0;

static void osd_draw_char(uint32_t *pixels, int canvas_w,
                           int x, int y, int idx)
{
    const uint8_t *glyph = s_font[idx];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint32_t color = (glyph[row] & (0x80u >> col)) ? OSD_FG : OSD_BG;
            int px = x + col * 2;
            int py = y + row * 2;
            pixels[ py      * canvas_w + px    ] = color;
            pixels[ py      * canvas_w + px + 1] = color;
            pixels[(py + 1) * canvas_w + px    ] = color;
            pixels[(py + 1) * canvas_w + px + 1] = color;
        }
    }
}

static void osd_update(double score)
{
    if (!s_osd_ready) return;
    if (!s_osd_visible) return;

    RGN_CANVAS_INFO_S canvas;
    memset(&canvas, 0, sizeof(canvas));
    RK_S32 rc = RK_MPI_RGN_GetCanvasInfo(OSD_HANDLE, &canvas);

    static int s_osd_dbg = 0;
    if (!s_osd_dbg) { s_osd_dbg = 1;
        FILE *f = fopen("/tmp/osd_text_dbg", "w");
        if (f) {
            fprintf(f, "GetCanvasInfo rc=%d vaddr=%llx virW=%u virH=%u\n",
                    rc, (unsigned long long)canvas.u64VirAddr,
                    canvas.u32VirWidth, canvas.u32VirHeight);
            fclose(f);
        }
    }
    if (rc != RK_SUCCESS) return;

    uint32_t *pixels = (uint32_t *)(uintptr_t)canvas.u64VirAddr;
    int cw = (int)canvas.u32VirWidth;

    for (int i = 0; i < cw * OSD_H; i++) pixels[i] = OSD_BG;

    char text[OSD_CHARS + 1];
    snprintf(text, sizeof(text), "FS:%5.0f", score);

    int x = 0;
    for (int i = 0; i < OSD_CHARS && text[i]; i++, x += 16)
        osd_draw_char(pixels, cw, x, 0, char_idx(text[i]));

    RK_MPI_RGN_UpdateCanvas(OSD_HANDLE);
}

/* Draw (or clear) the face bounding box.
 * Uses 4 thin OVERLAY_RGN strips (top/bottom/left/right edges) — same canvas
 * API as the working text OSD.  Each strip is Destroy/Create/Fill/Attach on
 * every call; ~16×512px per strip ≈ 32KB each, no memory pressure.
 * x0/y0/x1/y1 are in 10000-scale; pass all zeros to clear. */
static void face_box_draw(int x0_10k, int y0_10k, int x1_10k, int y1_10k)
{
    if (!s_face_box_ready) return;
    if (!s_facebox_visible) {
        /* clear any existing box */
        x0_10k = x1_10k = y0_10k = y1_10k = 0;
    }

    MPP_CHN_S chn = { RK_ID_VENC, s_fb_vdev, s_fb_vchn };
    int b = FACE_BOX_BORDER_PX;

    /* Always tear down previous strips first */
    for (int i = 0; i < 4; i++) {
        RK_MPI_RGN_DetachFromChn(FACE_BOX_HANDLE_BASE + i, &chn);
        RK_MPI_RGN_Destroy(FACE_BOX_HANDLE_BASE + i);
    }

    int show = (x0_10k < x1_10k && y0_10k < y1_10k);
    if (!show) return;

    int px0 = (x0_10k * s_fbw / 10000) & ~1;
    int py0 = (y0_10k * s_fbh / 10000) & ~1;
    int px1 = (x1_10k * s_fbw / 10000) & ~1;
    int py1 = (y1_10k * s_fbh / 10000) & ~1;
    if (px0 < 0) px0 = 0; if (px1 > s_fbw - b) px1 = s_fbw - b;
    if (py0 < 0) py0 = 0; if (py1 > s_fbh - b) py1 = s_fbh - b;
    if (px1 <= px0 + b*2 || py1 <= py0 + b*2) return;

    /* Canvas width must be multiple of 16; position must be even */
    int bw = ((px1 - px0 + 15) & ~15);  /* strip width  (top/bottom) */
    int bh = ((py1 - py0 +  1) & ~1);   /* strip height (left/right) */

    /* Strip geometry: x, y, width, height — indices 0=top 1=bot 2=lft 3=rgt */
    int sx[4] = { px0,   px0,   px0,   (px1-b)&~1 };
    int sy[4] = { py0,   (py1-b)&~1, py0, py0 };
    int sw[4] = { bw,    bw,    b,   b  };
    int sh[4] = { b,     b,     bh,  bh };

    int rc_all = 0;
    for (int i = 0; i < 4; i++) {
        int h = FACE_BOX_HANDLE_BASE + i;

        /* 1. Create region */
        RGN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        attr.enType                             = OVERLAY_RGN;
        attr.unAttr.stOverlay.enPixelFmt        = RK_FMT_RGBA8888;
        attr.unAttr.stOverlay.stSize.u32Width   = (uint32_t)sw[i];
        attr.unAttr.stOverlay.stSize.u32Height  = (uint32_t)sh[i];
        if (RK_MPI_RGN_Create(h, &attr) != RK_SUCCESS) { rc_all |= -1; continue; }

        /* 2. Attach to channel */
        RGN_CHN_ATTR_S ca;
        memset(&ca, 0, sizeof(ca));
        ca.bShow                                = RK_TRUE;
        ca.enType                               = OVERLAY_RGN;
        ca.unChnAttr.stOverlayChn.stPoint.s32X = sx[i];
        ca.unChnAttr.stOverlayChn.stPoint.s32Y = sy[i];
        ca.unChnAttr.stOverlayChn.u32BgAlpha   = 0;
        ca.unChnAttr.stOverlayChn.u32FgAlpha   = 255;
        ca.unChnAttr.stOverlayChn.u32Layer      = 4;
        if (RK_MPI_RGN_AttachToChn(h, &chn, &ca) != RK_SUCCESS) {
            RK_MPI_RGN_Destroy(h); rc_all |= -2; continue;
        }

        /* 3. Fill canvas solid green */
        RGN_CANVAS_INFO_S canvas;
        memset(&canvas, 0, sizeof(canvas));
        if (RK_MPI_RGN_GetCanvasInfo(h, &canvas) == RK_SUCCESS) {
            uint32_t *pix = (uint32_t *)(uintptr_t)canvas.u64VirAddr;
            int n = (int)canvas.u32VirWidth * sh[i];
            for (int j = 0; j < n; j++) pix[j] = FACE_BOX_RGBA;
            RK_MPI_RGN_UpdateCanvas(h);
        } else {
            rc_all |= -3;
        }
    }

    FILE *f = fopen("/tmp/face_box_draw", "w");
    if (f) {
        fprintf(f, "OVERLAY strip px=(%d,%d)-(%d,%d) bw=%d bh=%d rc_all=%d\n",
                px0, py0, px1, py1, bw, bh, rc_all);
        fclose(f);
    }
}

#if FOCUS_SCORE_VENC_ROI
/* Bias the encoder toward the signer: set VENC ROI 0 to the upper ~60% of the
 * person box (head/torso/hands — where signing happens), or disable when no
 * target.  Relative QP −8 → more bits there at the same total bitrate, so the
 * signing stays sharp when the background gets crushed on a Zoom call.
 * Independent of the debug facebox visibility.  Rect aligned to 16 (macroblock). */
#define ROI_INDEX       0
#define ROI_QP_OFFSET   (-8)

static void venc_roi_set_person(int x0_10k, int y0_10k, int x1_10k, int y1_10k, int valid)
{
    if (!s_face_box_ready) return;   /* VENC handle/size not known yet */

    VENC_ROI_ATTR_S roi;
    memset(&roi, 0, sizeof(roi));
    roi.u32Index = ROI_INDEX;
    roi.bAbsQp   = RK_FALSE;

    int show = valid && (x0_10k < x1_10k) && (y0_10k < y1_10k);
    if (show) {
        /* upper ~60% of the body = head/torso/hands */
        int y1_roi = y0_10k + (y1_10k - y0_10k) * 6 / 10;

        int px = x0_10k * s_fbw / 10000;
        int py = y0_10k * s_fbh / 10000;
        int pw = (x1_10k - x0_10k) * s_fbw / 10000;
        int ph = (y1_roi  - y0_10k) * s_fbh / 10000;

        if (px < 0) px = 0;
        if (py < 0) py = 0;
        px &= ~15; py &= ~15;
        pw = (pw + 15) & ~15;
        ph = (ph + 15) & ~15;
        if (px + pw > s_fbw) pw = (s_fbw - px) & ~15;
        if (py + ph > s_fbh) ph = (s_fbh - py) & ~15;
        show = (pw >= 16 && ph >= 16);

        if (show) {
            roi.bEnable          = RK_TRUE;
            roi.s32Qp            = ROI_QP_OFFSET;
            roi.stRect.s32X      = px;
            roi.stRect.s32Y      = py;
            roi.stRect.u32Width  = (RK_U32)pw;
            roi.stRect.u32Height = (RK_U32)ph;
        }
    }
    if (!show) roi.bEnable = RK_FALSE;

    RK_S32 rc = RK_MPI_VENC_SetRoiAttr(s_fb_vchn, &roi);

    static int s_last_en = -1;
    if ((int)roi.bEnable != s_last_en) {        /* log only on enable/disable change */
        s_last_en = (int)roi.bEnable;
        FILE *f = fopen("/tmp/venc_roi", "w");
        if (f) {
            fprintf(f, "en=%d qp=%d rect=%d,%d %dx%d chn=%d rc=%d\n",
                    roi.bEnable, roi.s32Qp, roi.stRect.s32X, roi.stRect.s32Y,
                    roi.stRect.u32Width, roi.stRect.u32Height, s_fb_vchn, rc);
            fclose(f);
        }
    }
}
#endif /* FOCUS_SCORE_VENC_ROI */

#if FOCUS_SCORE_MOTION_DEBLUR
/* VENC hardware motion deblur — sharpens fast motion (signing hands) on the
 * encoded stream.  Applied to the streaming VENC channel; default ON.  Strength
 * is only pushed if explicitly set via IPC (driver default otherwise). */
static int          s_deblur_vchn = -1;
static volatile int s_deblur_on   = 1;    /* default ON */
static volatile int s_deblur_str  = -1;   /* -1 = leave at driver default */

static void deblur_apply(void)
{
    if (s_deblur_vchn < 0) return;
    RK_S32 rc  = RK_MPI_VENC_EnableMotionDeblur(s_deblur_vchn,
                                                s_deblur_on ? RK_TRUE : RK_FALSE);
    RK_S32 rcs = 0;
    if (s_deblur_on && s_deblur_str >= 0)
        rcs = RK_MPI_VENC_SetMotionDeblurStrength(s_deblur_vchn, (RK_U32)s_deblur_str);

    FILE *f = fopen("/tmp/venc_deblur", "w");
    if (f) {
        fprintf(f, "on=%d strength=%d chn=%d rc=%d rcs=%d\n",
                s_deblur_on, s_deblur_str, s_deblur_vchn, rc, rcs);
        fclose(f);
    }
}
#endif /* FOCUS_SCORE_MOTION_DEBLUR */

/* Public: called from uvc_process startProcess() after startVenc() */
void focus_score_osd_attach(int venc_dev, int venc_chn)
{
    s_osd_ready      = 0;
    s_face_box_ready = 0;

    /* ── text score OSD ── */
    {
        RGN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        attr.enType                               = OVERLAY_RGN;
        attr.unAttr.stOverlay.enPixelFmt         = RK_FMT_RGBA8888;
        attr.unAttr.stOverlay.stSize.u32Width    = OSD_W;
        attr.unAttr.stOverlay.stSize.u32Height   = OSD_H;

        if (RK_MPI_RGN_Create(OSD_HANDLE, &attr) != RK_SUCCESS) {
            LOG_WARN("focus_score OSD: RGN_Create failed\n");
            return;
        }

        MPP_CHN_S chn = { RK_ID_VENC, venc_dev, venc_chn };

        RGN_CHN_ATTR_S ca;
        memset(&ca, 0, sizeof(ca));
        ca.bShow                                = RK_TRUE;
        ca.enType                               = OVERLAY_RGN;
        ca.unChnAttr.stOverlayChn.stPoint.s32X = 16;
        ca.unChnAttr.stOverlayChn.stPoint.s32Y = 16;
        ca.unChnAttr.stOverlayChn.u32BgAlpha   = 0;
        ca.unChnAttr.stOverlayChn.u32FgAlpha   = 255;
        ca.unChnAttr.stOverlayChn.u32Layer     = 5;

        if (RK_MPI_RGN_AttachToChn(OSD_HANDLE, &chn, &ca) != RK_SUCCESS) {
            LOG_WARN("focus_score OSD: AttachToChn VENC(%d,%d) failed\n",
                     venc_dev, venc_chn);
            RK_MPI_RGN_Destroy(OSD_HANDLE);
            return;
        }
        s_osd_ready = 1;
        LOG_INFO("focus_score OSD: text attached to VENC(%d,%d)\n",
                 venc_dev, venc_chn);
    }

#if FOCUS_SCORE_TARGET_DETECT
    /* ── subject box: 4 thin OVERLAY_RGN strips, created/destroyed per detection ── */
    {
        VENC_CHN_ATTR_S venc_attr;
        memset(&venc_attr, 0, sizeof(venc_attr));
        if (RK_MPI_VENC_GetChnAttr(venc_chn, &venc_attr) != RK_SUCCESS) {
            LOG_WARN("focus_score: cannot query VENC size — subject box disabled\n");
            return;
        }
        s_fbw     = (int)venc_attr.stVencAttr.u32PicWidth;
        s_fbh     = (int)venc_attr.stVencAttr.u32PicHeight;
        s_fb_vdev = venc_dev;
        s_fb_vchn = venc_chn;
        s_face_box_ready = 1;
        LOG_INFO("focus_score: subject box OVERLAY strips ready %dx%d\n", s_fbw, s_fbh);
    }
#endif /* FOCUS_SCORE_TARGET_DETECT */

#if FOCUS_SCORE_MOTION_DEBLUR
    /* Enable hardware motion deblur on the streaming channel (default ON).
     * Re-runs on every stream/resolution (re)start since this attach does. */
    s_deblur_vchn = venc_chn;
    deblur_apply();
#endif

    menu_osd_attach(venc_dev, venc_chn);
}

/* Public: called from uvc_process stopProcess() before VENC is destroyed */
void focus_score_osd_detach(int venc_dev, int venc_chn)
{
    MPP_CHN_S chn = { RK_ID_VENC, venc_dev, venc_chn };

#if FOCUS_SCORE_TARGET_DETECT
    if (s_face_box_ready) {
#if FOCUS_SCORE_VENC_ROI
        venc_roi_set_person(0, 0, 0, 0, 0);   /* clear encoder ROI before teardown */
#endif
        s_face_box_ready = 0;
        /* Strips may or may not exist depending on last detection — always try */
        for (int i = 0; i < 4; i++) {
            RK_MPI_RGN_DetachFromChn(FACE_BOX_HANDLE_BASE + i, &chn);
            RK_MPI_RGN_Destroy(FACE_BOX_HANDLE_BASE + i);
        }
    }
#endif

    if (s_osd_ready) {
        s_osd_ready = 0;
        RK_MPI_RGN_DetachFromChn(OSD_HANDLE, &chn);
        RK_MPI_RGN_Destroy(OSD_HANDLE);
    }

    menu_osd_detach(venc_dev, venc_chn);
    LOG_INFO("focus_score OSD: detached\n");
}

#endif /* FOCUS_SCORE_OSD */

/* Display mode cycling (called from IPC or VISCA menu button):
 * Mode 0: both OFF (default)
 * Mode 1: face box ON, OSD OFF
 * Mode 2: both ON (debug)
 * Defined unconditionally so isp_ipc.cpp links regardless of FOCUS_SCORE_OSD. */
static int s_display_mode = 0;

void focus_score_set_display_mode(int mode)
{
    s_display_mode = ((mode % 3) + 3) % 3;  /* clamp 0..2, handle negatives */
#if FOCUS_SCORE_OSD
    s_osd_visible     = (s_display_mode == 2) ? 1 : 0;
    s_facebox_visible = (s_display_mode >= 1) ? 1 : 0;
    /* clear face box immediately if hiding */
    if (!s_facebox_visible)
        face_box_draw(0, 0, 0, 0);
    /* clear OSD text immediately if hiding */
    if (!s_osd_visible && s_osd_ready) {
        RGN_CANVAS_INFO_S canvas;
        memset(&canvas, 0, sizeof(canvas));
        if (RK_MPI_RGN_GetCanvasInfo(OSD_HANDLE, &canvas) == RK_SUCCESS) {
            memset((void *)(uintptr_t)canvas.u64VirAddr, 0,
                   canvas.u32VirWidth * OSD_H * 4);
            RK_MPI_RGN_UpdateCanvas(OSD_HANDLE);
        }
    }
    LOG_INFO("focus_score: display mode %d (osd=%d facebox=%d)\n",
             s_display_mode, s_osd_visible, s_facebox_visible);
#endif
}

int focus_score_get_display_mode(void)
{
    return s_display_mode;
}

/* ==========================================================================
 * Subject detection — drives the AF window, focus-score crop, OSD target box
 * and the auto-tracking P-controller.  Two interchangeable NPU back ends:
 *   FOCUS_SCORE_PERSON_DETECT — RockIVA full-body person detect + track IDs
 *   FOCUS_SCORE_FACE_DETECT   — hand-rolled RetinaFace via librknnmrt
 * (mutually exclusive — both use the single NPU core).
 * ========================================================================== */
#if FOCUS_SCORE_TARGET_DETECT

/* Selected subject bbox in 10000-scale coords — updated each detection run.
 * Named s_face_* for historical reasons; under PERSON_DETECT it is a person box. */
static pthread_mutex_t  s_face_mutex  = PTHREAD_MUTEX_INITIALIZER;
static int16_t          s_face_x0 = 0, s_face_y0 = 0;
static int16_t          s_face_x1 = 0, s_face_y1 = 0;
static int              s_face_valid  = 0;
static volatile int     s_autotrack   = 0; /* P-controller → EPTZ auto-tracking */

/* Set rkaiq AF measurement window to subject bbox (pixels), or full frame. */
static void af_set_window(int frame_w, int frame_h,
                          int fx0, int fy0, int fx1, int fy1, int valid);

#endif /* FOCUS_SCORE_TARGET_DETECT */

#if FOCUS_SCORE_FACE_DETECT

#define MODEL_PATH      "/oem/usr/share/models/retinaface.rknn"
#define FACE_DET_EVERY  1    /* run NPU every scoring frame (~500ms interval) */
#define DET_W           640  /* RetinaFace model input width  */
#define DET_H           640  /* RetinaFace model input height */
#define DET_CNT_H       480  /* content height (from 4:3 sensor crop) */
#define DET_PAD         80   /* letterbox top/bottom = (640-480)/2     */
#define FACE_THRESH     0.5f /* detection confidence threshold          */
#define NUM_PRIORS      16800

static rknn_context     s_rknn_ctx     = 0;
static rknn_tensor_mem *s_input_mem    = NULL;
static rknn_tensor_mem *s_out_mems[3] = {NULL, NULL, NULL};
static int32_t          s_out_zp[3]   = {0, 0, 0};
static float            s_out_sc[3]   = {0.f, 0.f, 0.f};
static uint8_t         *s_det_buf     = NULL;
static size_t           s_det_buf_sz  = 0;
static int              s_det_tick    = 0;

static void face_det_init(void)
{
    FILE *tr = fopen("/tmp/face_init", "w");

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, (char *)MODEL_PATH, 0, 0, NULL);
    if (tr) fprintf(tr, "rknn_init ret=%d\n", ret);
    if (ret < 0) {
        if (tr) { fprintf(tr, "FAILED\n"); fclose(tr); }
        LOG_WARN("focus_score: rknn_init(%s) failed ret=%d\n", MODEL_PATH, ret);
        return;
    }

    rknn_input_output_num io;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    if (tr) fprintf(tr, "inputs=%u outputs=%u\n", io.n_input, io.n_output);

    /* Input: uint8 NHWC BGR (640×640×3) */
    rknn_tensor_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.index = 0;
    rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &ia, sizeof(ia));
    ia.type = RKNN_TENSOR_UINT8;
    ia.fmt  = RKNN_TENSOR_NHWC;
    s_input_mem = rknn_create_mem(ctx, ia.size_with_stride);
    rknn_set_io_mem(ctx, s_input_mem, &ia);
    if (tr) fprintf(tr, "input stride_size=%u\n", ia.size_with_stride);

    /* Outputs: location, scores, landmarks */
    for (int i = 0; i < 3; i++) {
        rknn_tensor_attr oa;
        memset(&oa, 0, sizeof(oa));
        oa.index = i;
        rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &oa, sizeof(oa));
        s_out_mems[i] = rknn_create_mem(ctx, oa.size_with_stride);
        rknn_set_io_mem(ctx, s_out_mems[i], &oa);
        s_out_zp[i] = oa.zp;
        s_out_sc[i] = oa.scale;
        if (tr) fprintf(tr, "out[%d] size=%u zp=%d scale=%f\n",
                        i, oa.size_with_stride, oa.zp, oa.scale);
    }

    s_rknn_ctx = ctx;
    if (tr) { fprintf(tr, "OK\n"); fclose(tr); }
    LOG_WARN("focus_score: RetinaFace RKNN ready\n");
}

static void face_det_deinit(void)
{
    if (!s_rknn_ctx) return;
    for (int i = 0; i < 3; i++) {
        if (s_out_mems[i]) { rknn_destroy_mem(s_rknn_ctx, s_out_mems[i]); s_out_mems[i] = NULL; }
    }
    if (s_input_mem) { rknn_destroy_mem(s_rknn_ctx, s_input_mem); s_input_mem = NULL; }
    rknn_destroy(s_rknn_ctx);
    s_rknn_ctx = 0;
    free(s_det_buf); s_det_buf = NULL; s_det_buf_sz = 0;
    pthread_mutex_lock(&s_face_mutex);
    s_face_valid = 0;
    pthread_mutex_unlock(&s_face_mutex);
}

/* Downsample NV12 by an integer power-of-2 scale, removing stride padding. */
static void downsample_nv12(uint8_t *dst, const uint8_t *src,
                              int src_w, int src_h, int stride, int scale)
{
    int dst_w = src_w / scale;
    int dst_h = src_h / scale;
    for (int r = 0; r < dst_h; r++)
        for (int c = 0; c < dst_w; c++)
            dst[r * dst_w + c] = src[(r * scale) * stride + (c * scale)];
    const uint8_t *src_uv = src + (size_t)stride * src_h;
    uint8_t       *dst_uv = dst + (size_t)dst_w  * dst_h;
    for (int r = 0; r < dst_h / 2; r++) {
        const uint8_t *sr = src_uv + (size_t)(r * scale) * stride;
        uint8_t       *dr = dst_uv + (size_t)r * dst_w;
        for (int c = 0; c < dst_w / 2; c++) {
            dr[c * 2 + 0] = sr[c * scale * 2 + 0];
            dr[c * 2 + 1] = sr[c * scale * 2 + 1];
        }
    }
}

/* Synchronous RetinaFace inference on one VI frame.
 * Pipeline: downsample → letterbox NV12→BGR 640×640 → rknn_run → parse. */
static void face_det_run(const uint8_t *nv12, int w, int h, int stride)
{
    if (!s_rknn_ctx || !s_input_mem) return;

    /* 1. Downsample to ≤ 1024px wide (power-of-2) — removes stride at same time */
    int scale = 1;
    while (w / scale > 1024) scale *= 2;
    int ds_w = w / scale;  /* e.g. 648 for 2592×1944 at scale=4 */
    int ds_h = h / scale;

    size_t ds_sz = (size_t)ds_w * ds_h * 3 / 2;
    if (!s_det_buf || s_det_buf_sz < ds_sz) {
        free(s_det_buf);
        s_det_buf = (uint8_t *)malloc(ds_sz);
        if (!s_det_buf) { s_det_buf_sz = 0; return; }
        s_det_buf_sz = ds_sz;
    }
    downsample_nv12(s_det_buf, nv12, w, h, stride, scale);

    /* 2. Build 640×640 BGR letterboxed image in RKNN input buffer.
     *    Center-crop ds_w×ds_h → DET_W×DET_CNT_H, pad top/bottom with 114. */
    int x_off = ((ds_w - DET_W)   / 2) & ~1;  /* even, may be 0 if ds_w < DET_W */
    int y_off = ((ds_h - DET_CNT_H) / 2) & ~1;
    if (x_off < 0) x_off = 0;
    if (y_off < 0) y_off = 0;
    int cnt_w = ds_w - x_off; if (cnt_w > DET_W)     cnt_w = DET_W;
    int cnt_h = ds_h - y_off; if (cnt_h > DET_CNT_H) cnt_h = DET_CNT_H;

    const uint8_t *yp = s_det_buf;
    const uint8_t *up = s_det_buf + (size_t)ds_w * ds_h;

    uint8_t *bgr = (uint8_t *)s_input_mem->virt_addr;
    memset(bgr, 114, (size_t)DET_W * DET_H * 3);  /* fill letterbox gray */

    for (int r = 0; r < cnt_h; r++) {
        const uint8_t *yr  = yp + (size_t)(r + y_off)     * ds_w + x_off;
        const uint8_t *uvr = up + (size_t)(r/2 + y_off/2) * ds_w + x_off;
        uint8_t       *out = bgr + (size_t)(r + DET_PAD)  * DET_W * 3;
        for (int c = 0; c < cnt_w; c++) {
            int Y = (int)yr[c] - 16;
            int U = (int)uvr[c & ~1] - 128;
            int V = (int)uvr[c |  1] - 128;
            int R = (298*Y + 409*V           + 128) >> 8;
            int G = (298*Y - 100*U - 208*V   + 128) >> 8;
            int B = (298*Y + 516*U           + 128) >> 8;
            out[c*3+0] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B);
            out[c*3+1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G);
            out[c*3+2] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R);
        }
    }

    /* 3. Run inference (synchronous) */
    if (rknn_run(s_rknn_ctx, NULL) < 0) {
        FILE *ef = fopen("/tmp/face_run_err", "w");
        if (ef) { fprintf(ef, "rknn_run failed\n"); fclose(ef); }
        return;
    }

    /* 4. Parse outputs — same logic as retinaface.cc example.
     *    Locations and scores are int8 quantized; dequantize with zp/scale. */
    const int8_t *loc    = (const int8_t *)s_out_mems[0]->virt_addr;
    const int8_t *scores = (const int8_t *)s_out_mems[1]->virt_addr;
    float loc_sc = s_out_sc[0]; int32_t loc_zp = s_out_zp[0];
    float sc_sc  = s_out_sc[1]; int32_t sc_zp  = s_out_zp[1];

    static const float VAR[2] = {0.1f, 0.2f};

    int   best_dist = INT_MAX;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    int   found = 0;

    for (int i = 0; i < NUM_PRIORS; i++) {
        float fs = ((float)scores[i*2+1] - sc_zp) * sc_sc;
        if (fs < FACE_THRESH) continue;

        const int8_t *b = loc + i * 4;
        float px = ((float)b[0]-loc_zp)*loc_sc * VAR[0]*BOX_PRIORS_640[i][2] + BOX_PRIORS_640[i][0];
        float py = ((float)b[1]-loc_zp)*loc_sc * VAR[0]*BOX_PRIORS_640[i][3] + BOX_PRIORS_640[i][1];
        float pw = expf(((float)b[2]-loc_zp)*loc_sc*VAR[1]) * BOX_PRIORS_640[i][2];
        float ph = expf(((float)b[3]-loc_zp)*loc_sc*VAR[1]) * BOX_PRIORS_640[i][3];

        float x0 = (px - pw*0.5f) * DET_W;
        float y0 = (py - ph*0.5f) * DET_H;
        float x1 = (px + pw*0.5f) * DET_W;
        float y1 = (py + ph*0.5f) * DET_H;

        /* center in original-frame 10k-scale for distance-to-center selection */
        float cx10k = ((x0+x1)*0.5f + x_off) * scale * 10000.0f / w;
        float cy10k = ((y0+y1)*0.5f - DET_PAD + y_off) * scale * 10000.0f / h;
        int dx = (int)cx10k - 5000, dy = (int)cy10k - 5000;
        int dist = dx*dx + dy*dy;

        if (dist < best_dist) {
            best_dist = dist;
            bx0 = x0; by0 = y0; bx1 = x1; by1 = y1;
            found = 1;
        }
    }

    /* 5. Map best face box from model 640×640 space → 10000-scale in original frame */
    int16_t fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0;
    int valid = 0;

    if (found) {
        float y0c = by0 - DET_PAD; if (y0c < 0)        y0c = 0;
        float y1c = by1 - DET_PAD; if (y1c > DET_CNT_H) y1c = DET_CNT_H;
        if (bx0 < 0) bx0 = 0;     if (bx1 > DET_W)     bx1 = DET_W;

        fx0 = (int16_t)((bx0 + x_off) * scale * 10000 / w);
        fy0 = (int16_t)((y0c + y_off) * scale * 10000 / h);
        fx1 = (int16_t)((bx1 + x_off) * scale * 10000 / w);
        fy1 = (int16_t)((y1c + y_off) * scale * 10000 / h);
        if (fx0 >= 0 && fy0 >= 0 && fx1 > fx0 && fy1 > fy0) valid = 1;
    }

    FILE *dbg = fopen("/tmp/face_detect", "w");
    if (dbg) {
        fprintf(dbg, "found=%d valid=%s box10k=(%d,%d)-(%d,%d)\n",
                found, valid ? "YES" : "NO", fx0, fy0, fx1, fy1);
        fclose(dbg);
    }

    pthread_mutex_lock(&s_face_mutex);
    s_face_x0 = fx0; s_face_y0 = fy0;
    s_face_x1 = fx1; s_face_y1 = fy1;
    s_face_valid = valid;
    pthread_mutex_unlock(&s_face_mutex);

    /* Update ISP AF measurement window to face region (or full frame) */
    af_set_window(w, h, fx0, fy0, fx1, fy1, valid);

#if FOCUS_SCORE_OSD
    face_box_draw(valid ? fx0 : 0, valid ? fy0 : 0,
                  valid ? fx1 : 0, valid ? fy1 : 0);
#endif
}

#endif /* FOCUS_SCORE_FACE_DETECT — RetinaFace inference */

#if FOCUS_SCORE_TARGET_DETECT

/* Set rkaiq AF measurement window to subject bbox (pixels), or full frame.
 * Called after every detection run so the ISP scores the right region. */
static void af_set_window(int frame_w, int frame_h,
                          int fx0, int fy0, int fx1, int fy1, int valid)
{
    rk_aiq_sys_ctx_t *ctx = rkuvc_aiq_get_ctx(s_pipe_id);
    if (!ctx) return;

    rk_aiq_af_attrib_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.sync.sync_mode = RK_AIQ_UAPI_MODE_ASYNC;

    if (valid && fx1 > fx0 && fy1 > fy0) {
        /* Convert 10000-scale bbox to pixels */
        int px0 = fx0 * frame_w / 10000;
        int py0 = fy0 * frame_h / 10000;
        int px1 = fx1 * frame_w / 10000;
        int py1 = fy1 * frame_h / 10000;
        /* Clamp to frame */
        if (px0 < 0) px0 = 0;
        if (py0 < 0) py0 = 0;
        if (px1 > frame_w) px1 = frame_w;
        if (py1 > frame_h) py1 = frame_h;
        attr.h_offs = px0;
        attr.v_offs = py0;
        attr.h_size = (unsigned int)(px1 - px0);
        attr.v_size = (unsigned int)(py1 - py0);
    } else {
        /* Full frame */
        attr.h_offs = 0;
        attr.v_offs = 0;
        attr.h_size = (unsigned int)frame_w;
        attr.v_size = (unsigned int)frame_h;
    }

    rk_aiq_user_api2_af_SetAttrib(ctx, &attr);
}

/* Return the crop rect to score — face bbox with padding, or center crop. */
static void get_crop(int w, int h, int *cx, int *cy, int *cw, int *ch)
{
    pthread_mutex_lock(&s_face_mutex);
    int valid = s_face_valid;
    int fx0   = s_face_x0, fy0 = s_face_y0;
    int fx1   = s_face_x1, fy1 = s_face_y1;
    pthread_mutex_unlock(&s_face_mutex);

    if (valid) {
        int px0 = fx0 * w / 10000;
        int py0 = fy0 * h / 10000;
        int px1 = fx1 * w / 10000;
        int py1 = fy1 * h / 10000;
        /* 50% padding around the face for natural focus response */
        int pad_x = (px1 - px0) / 2;
        int pad_y = (py1 - py0) / 2;
        *cx = px0 - pad_x; if (*cx < 0) *cx = 0;
        *cy = py0 - pad_y; if (*cy < 0) *cy = 0;
        *cw = (px1 + pad_x) - *cx; if (*cx + *cw > w) *cw = w - *cx;
        *ch = (py1 + pad_y) - *cy; if (*cy + *ch > h) *ch = h - *cy;
    } else {
        *cw = (w < SCORE_CROP_W) ? w : SCORE_CROP_W;
        *ch = (h < SCORE_CROP_H) ? h : SCORE_CROP_H;
        *cx = (w - *cw) / 2;
        *cy = (h - *ch) / 2;
    }
}

#else  /* !FOCUS_SCORE_TARGET_DETECT — use static center crop */

static void get_crop(int w, int h, int *cx, int *cy, int *cw, int *ch)
{
    *cw = (w < SCORE_CROP_W) ? w : SCORE_CROP_W;
    *ch = (h < SCORE_CROP_H) ? h : SCORE_CROP_H;
    *cx = (w - *cw) / 2;
    *cy = (h - *ch) / 2;
}

#endif /* FOCUS_SCORE_TARGET_DETECT */

/* ==========================================================================
 * Focus scoring thread
 * ========================================================================== */
static void *focus_thread(void *arg)
{
    (void)arg;

#if FOCUS_SCORE_FACE_DETECT
    face_det_init();
#endif

    while (!s_quit && !uvc_vi_is_isp_running())
        usleep(100000);

    LOG_INFO("focus_score: VI running — scoring started (pipe=%d chn=%d)\n",
             s_pipe_id, s_chn_id);

    uint32_t frame_counter = 0;

    while (!s_quit) {
        VIDEO_FRAME_INFO_S frame;
        memset(&frame, 0, sizeof(frame));

        int ret = RK_MPI_VI_GetChnFrame(s_pipe_id, s_chn_id,
                                         &frame, GETFRAME_TIMEOUT);
        if (ret != RK_SUCCESS) {
            usleep(SCORE_INTERVAL_US);
            continue;
        }

        int w      = (int)frame.stVFrame.u32Width;
        int h      = (int)frame.stVFrame.u32Height;
        int stride = (int)frame.stVFrame.u32VirWidth;
        if (stride <= 0) stride = w;

        /* EPTZ crops in the VI channel's OUTPUT coordinate space (this frame's
         * w×h), NOT the sensor native — keep its source size synced to the live
         * frame so zoom/pan rects are valid and survive resolution switches.    */
        static int s_eptz_src_w = 0, s_eptz_src_h = 0;
        if (w > 0 && h > 0 && (w != s_eptz_src_w || h != s_eptz_src_h)) {
            s_eptz_src_w = w; s_eptz_src_h = h;
            eptz_set_source_size(w, h);
        }

        if (w > 0 && h > 0 && frame.stVFrame.pMbBlk) {
            RK_MPI_SYS_MmzFlushCache(frame.stVFrame.pMbBlk, RK_TRUE);
            const uint8_t *y_plane =
                (const uint8_t *)RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);

            if (y_plane) {
#if FOCUS_SCORE_PERSON_DETECT
                {
                    int dma_fd = RK_MPI_MB_Handle2Fd(frame.stVFrame.pMbBlk);
                    person_det_run_fd(dma_fd, w, h, frame_counter);

                    person_target_t tgt;
                    person_det_get_target(&tgt);

                    pthread_mutex_lock(&s_face_mutex);
                    s_face_x0 = tgt.x0; s_face_y0 = tgt.y0;
                    s_face_x1 = tgt.x1; s_face_y1 = tgt.y1;
                    s_face_valid = tgt.valid;
                    pthread_mutex_unlock(&s_face_mutex);

                    af_set_window(w, h, tgt.x0, tgt.y0, tgt.x1, tgt.y1, tgt.valid);
#if FOCUS_SCORE_OSD
                    face_box_draw(tgt.valid ? tgt.x0 : 0, tgt.valid ? tgt.y0 : 0,
                                  tgt.valid ? tgt.x1 : 0, tgt.valid ? tgt.y1 : 0);
#if FOCUS_SCORE_VENC_ROI
                    /* Encoder ROI follows the actual target (not the debug box). */
                    venc_roi_set_person(tgt.x0, tgt.y0, tgt.x1, tgt.y1, tgt.valid);
#endif
#endif
                    /* ── Auto-tracking P-controller (holds the locked person) ── */
                    if (s_autotrack && tgt.valid) {
                        float fcx = (tgt.x0 + tgt.x1) * 0.5f / 10000.0f;
                        float fcy = (tgt.y0 + tgt.y1) * 0.5f / 10000.0f;
                        float ex  = fcx - 0.5f;
                        float ey  = fcy - 0.5f;
                        if (fabsf(ex) > 0.04f || fabsf(ey) > 0.04f) {
                            eptz_pan(ex  * 0.35f);
                            eptz_tilt(ey * 0.35f);
                        }
                    }
                }
#elif FOCUS_SCORE_FACE_DETECT
                if (++s_det_tick >= FACE_DET_EVERY) {
                    s_det_tick = 0;
                    face_det_run(y_plane, w, h, stride);

                    /* ── Face auto-tracking P-controller ──────────────── */
                    if (s_autotrack) {
                        pthread_mutex_lock(&s_face_mutex);
                        int at_valid = s_face_valid;
                        int at_x0 = s_face_x0, at_y0 = s_face_y0;
                        int at_x1 = s_face_x1, at_y1 = s_face_y1;
                        pthread_mutex_unlock(&s_face_mutex);
                        if (at_valid) {
                            float fcx = (at_x0 + at_x1) * 0.5f / 10000.0f;
                            float fcy = (at_y0 + at_y1) * 0.5f / 10000.0f;
                            float ex  = fcx - 0.5f;
                            float ey  = fcy - 0.5f;
                            if (fabsf(ex) > 0.04f || fabsf(ey) > 0.04f) {
                                eptz_pan(ex  * 0.35f);
                                eptz_tilt(ey * 0.35f);
                            }
                        }
                    }
                }
#endif
                /* Hardware ISP sharpness stat — zero CPU cost, updated every frame.
                 * Falls back to Tenengrad if the ISP context is unavailable. */
                double score = 0.0;
                rk_aiq_sys_ctx_t *aiq_ctx = rkuvc_aiq_get_ctx(s_pipe_id);
                if (aiq_ctx) {
                    rk_aiq_af_sec_path_t path;
                    memset(&path, 0, sizeof(path));
                    if (rk_aiq_user_api2_af_GetSearchPath(aiq_ctx, &path) == XCAM_RETURN_NO_ERROR
                            && path.search_num > 0) {
                        score = (double)path.sharpness[0];
                    }
                }
                if (score == 0.0) {
                    /* fallback: Tenengrad on face/center crop */
                    int cx, cy, cw, ch;
                    get_crop(w, h, &cx, &cy, &cw, &ch);
                    score = tenengrad_score(y_plane, stride, cx, cy, cw, ch);
                }
                s_score = score;

                FILE *f = fopen(FOCUS_SCORE_PATH, "w");
                if (f) { fprintf(f, "%.1f\n", score); fclose(f); }

                LOG_INFO("focus_score: %.1f  (%dx%d)\n", score, w, h);

#if FOCUS_SCORE_OSD
                osd_update(score);
#endif
            }
        }

        RK_MPI_VI_ReleaseChnFrame(s_pipe_id, s_chn_id, &frame);
        frame_counter++;
        usleep(SCORE_INTERVAL_US);
    }

#if FOCUS_SCORE_FACE_DETECT
    face_det_deinit();
#elif FOCUS_SCORE_PERSON_DETECT
    person_det_deinit();
#endif

    remove(FOCUS_SCORE_PATH);
    LOG_INFO("focus_score: thread exited\n");
    return NULL;
}

#if FOCUS_SCORE_TARGET_DETECT
void focus_score_set_autotrack(int on)
{
    s_autotrack = on ? 1 : 0;
    if (on) eptz_enable(1);
}

int focus_score_get_autotrack(void)
{
    return s_autotrack;
}
#else
void focus_score_set_autotrack(int on) { (void)on; }
int  focus_score_get_autotrack(void)   { return 0; }
#endif

/* One-shot commenter lock — only meaningful with RockIVA track IDs. */
#if FOCUS_SCORE_PERSON_DETECT
void focus_score_lock_target(void)      { person_det_lock_current(); }
void focus_score_unlock_target(void)    { person_det_unlock(); }
int  focus_score_target_is_locked(void) { return person_det_is_locked(); }
#else
void focus_score_lock_target(void)      {}
void focus_score_unlock_target(void)    {}
int  focus_score_target_is_locked(void) { return 0; }
#endif

/* VENC motion deblur (defined where the VENC handle/deblur state live). */
#if FOCUS_SCORE_OSD && FOCUS_SCORE_MOTION_DEBLUR
void focus_score_set_deblur(int on)         { s_deblur_on = on ? 1 : 0; deblur_apply(); }
void focus_score_set_deblur_strength(int s) { if (s < 0) s = 0; if (s > 3) s = 3;
                                              s_deblur_str = s; deblur_apply(); }
int  focus_score_get_deblur(void)           { return s_deblur_on; }
#else
void focus_score_set_deblur(int on)         { (void)on; }
void focus_score_set_deblur_strength(int s) { (void)s; }
int  focus_score_get_deblur(void)           { return 0; }
#endif

void focus_score_start(int pipe_id, int chn_id)
{
    s_pipe_id = pipe_id;
    s_chn_id  = chn_id;
    s_quit    = 0;
    s_score   = 0.0;
#if FOCUS_SCORE_OSD
    s_osd_ready      = 0;
    s_face_box_ready = 0;
#endif
    pthread_create(&s_tid, NULL, focus_thread, NULL);
    LOG_INFO("focus_score: thread created (pipe=%d chn=%d)\n", pipe_id, chn_id);
}

void focus_score_stop(void)
{
    s_quit = 1;
#if FOCUS_SCORE_OSD
    s_osd_ready      = 0;
    s_face_box_ready = 0;
#endif
    pthread_join(s_tid, NULL);
}

double focus_score_get(void)
{
    return s_score;
}
