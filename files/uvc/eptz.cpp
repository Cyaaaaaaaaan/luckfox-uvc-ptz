#include "eptz.h"
#include "uvc_log.h"
#include "rk_mpi_vi.h"
#include "rk_comm_vi.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>

#define EPTZ_ZOOM_MIN   1.0f
#define EPTZ_ZOOM_MAX   8.0f    /* beyond the lossless limit it just softens */
#define EPTZ_MIN_CROP   64      /* never crop smaller than this (px) */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static int   s_pipe   = 0;
static int   s_chn    = 0;
static int   s_src_w  = 2592;   /* sensor native — crop coordinate space */
static int   s_src_h  = 1944;
static int   s_enabled = 0;     /* OFF by default */
static float s_zoom   = 1.0f;
static float s_cx     = 0.5f;   /* normalized center */
static float s_cy     = 0.5f;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Compute the crop rect from normalized state and push it to the ISP.
 * Caller must hold s_lock. */
static void apply_locked(void)
{
    VI_CROP_INFO_S crop;
    memset(&crop, 0, sizeof(crop));
    crop.enCropCoordinate = VI_CROP_ABS_COOR;

    if (!s_enabled) {
        /* full frame — no crop */
        crop.bEnable = RK_FALSE;
        RK_S32 rc0 = RK_MPI_VI_SetEptz(s_pipe, s_chn, crop);
        FILE *f0 = fopen("/tmp/eptz_dbg", "w");
        if (f0) { fprintf(f0, "DISABLED pipe=%d chn=%d bEnable=0 rc=%d\n",
                          s_pipe, s_chn, rc0); fclose(f0); }
        return;
    }

    float zoom = clampf(s_zoom, EPTZ_ZOOM_MIN, EPTZ_ZOOM_MAX);

    int w = (int)(s_src_w / zoom);
    int h = (int)(s_src_h / zoom);
    if (w < EPTZ_MIN_CROP) w = EPTZ_MIN_CROP;
    if (h < EPTZ_MIN_CROP) h = EPTZ_MIN_CROP;
    if (w > s_src_w) w = s_src_w;
    if (h > s_src_h) h = s_src_h;
    w &= ~1; h &= ~1;            /* even dims */

    /* center (normalized) -> top-left, clamped so the crop stays in-frame */
    int x = (int)(clampf(s_cx, 0.f, 1.f) * s_src_w) - w / 2;
    int y = (int)(clampf(s_cy, 0.f, 1.f) * s_src_h) - h / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > s_src_w - w) x = s_src_w - w;
    if (y > s_src_h - h) y = s_src_h - h;
    x &= ~1; y &= ~1;           /* even offsets */

    crop.bEnable            = RK_TRUE;
    crop.stCropRect.s32X    = x;
    crop.stCropRect.s32Y    = y;
    crop.stCropRect.u32Width  = (RK_U32)w;
    crop.stCropRect.u32Height = (RK_U32)h;

    RK_S32 rc = RK_MPI_VI_SetEptz(s_pipe, s_chn, crop);
    LOG_INFO("[eptz] zoom=%.2f c=(%.2f,%.2f) rect=%d,%d %dx%d (src %dx%d) rc=%d\n",
             (double)zoom, (double)s_cx, (double)s_cy, x, y, w, h,
             s_src_w, s_src_h, rc);

    FILE *f = fopen("/tmp/eptz_dbg", "w");
    if (f) {
        fprintf(f, "ENABLED pipe=%d chn=%d zoom=%.2f c=(%.2f,%.2f) "
                   "rect=%d,%d %dx%d src=%dx%d rc=%d\n",
                s_pipe, s_chn, (double)zoom, (double)s_cx, (double)s_cy,
                x, y, w, h, s_src_w, s_src_h, rc);
        fclose(f);
    }
}

void eptz_init(int vi_pipe, int vi_chn, int src_w, int src_h)
{
    pthread_mutex_lock(&s_lock);
    s_pipe = vi_pipe;
    s_chn  = vi_chn;
    if (src_w > 0) s_src_w = src_w;
    if (src_h > 0) s_src_h = src_h;
    s_enabled = 0;
    s_zoom = 1.0f; s_cx = 0.5f; s_cy = 0.5f;
    pthread_mutex_unlock(&s_lock);
    LOG_INFO("[eptz] init pipe=%d chn=%d src=%dx%d (disabled)\n",
             vi_pipe, vi_chn, s_src_w, s_src_h);
}

void eptz_set_source_size(int src_w, int src_h)
{
    pthread_mutex_lock(&s_lock);
    if (src_w > 0) s_src_w = src_w;
    if (src_h > 0) s_src_h = src_h;
    if (s_enabled) apply_locked();
    pthread_mutex_unlock(&s_lock);
}

void eptz_enable(int on)
{
    pthread_mutex_lock(&s_lock);
    s_enabled = on ? 1 : 0;
    apply_locked();             /* on->apply crop; off->restore full frame */
    pthread_mutex_unlock(&s_lock);
}

int eptz_is_enabled(void)
{
    pthread_mutex_lock(&s_lock);
    int e = s_enabled;
    pthread_mutex_unlock(&s_lock);
    return e;
}

void eptz_set_zoom(float zoom)
{
    pthread_mutex_lock(&s_lock);
    s_zoom = clampf(zoom, EPTZ_ZOOM_MIN, EPTZ_ZOOM_MAX);
    if (s_enabled) apply_locked();
    pthread_mutex_unlock(&s_lock);
}

void eptz_zoom_delta(float dz)
{
    pthread_mutex_lock(&s_lock);
    s_zoom = clampf(s_zoom + dz, EPTZ_ZOOM_MIN, EPTZ_ZOOM_MAX);
    if (s_enabled) apply_locked();
    pthread_mutex_unlock(&s_lock);
}

void eptz_set_center(float cx, float cy)
{
    pthread_mutex_lock(&s_lock);
    s_cx = clampf(cx, 0.f, 1.f);
    s_cy = clampf(cy, 0.f, 1.f);
    if (s_enabled) apply_locked();
    pthread_mutex_unlock(&s_lock);
}

void eptz_pan(float dcx)
{
    pthread_mutex_lock(&s_lock);
    s_cx = clampf(s_cx + dcx, 0.f, 1.f);
    if (s_enabled) apply_locked();
    pthread_mutex_unlock(&s_lock);
}

void eptz_tilt(float dcy)
{
    pthread_mutex_lock(&s_lock);
    s_cy = clampf(s_cy + dcy, 0.f, 1.f);
    if (s_enabled) apply_locked();
    pthread_mutex_unlock(&s_lock);
}

void eptz_reset(void)
{
    pthread_mutex_lock(&s_lock);
    s_zoom = 1.0f; s_cx = 0.5f; s_cy = 0.5f;
    if (s_enabled) apply_locked();
    pthread_mutex_unlock(&s_lock);
}

void eptz_reapply(void)
{
    pthread_mutex_lock(&s_lock);
    apply_locked();
    pthread_mutex_unlock(&s_lock);
}
