/* RockIVA person detection + tracking — replacement for the RetinaFace path.
 * See person_det.h for the rationale.  This file is compiled only when
 * FOCUS_SCORE_PERSON_DETECT == 1 (CMake adds it to the source list via PATCH 12).
 */
#include "person_det.h"
#include "uvc_log.h"

#include "rockiva/rockiva_common.h"
#include "rockiva/rockiva_det_api.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Directory holding object_detection_pfp.data on the target (deployed by PATCH 12). */
#define PERSON_DET_MODEL_DIR "/oem/usr/share/iva"

/* PERSON detections below this confidence are ignored. 0 lets RockIVA auto-pick. */
#define PERSON_DET_SCORE      50
/* Require this many stable frames before a new track is reported (false-positive filter). */
#define PERSON_DET_MIN_COUNT  2
/* WaitFinish timeout per pushed frame (ms). */
#define PERSON_DET_WAIT_MS    300

static RockIvaHandle    s_handle   = NULL;
static int              s_inited   = 0;
static int              s_w = 0, s_h = 0;

static pthread_mutex_t  s_mutex    = PTHREAD_MUTEX_INITIALIZER;
static person_target_t  s_target   = {0, 0, 0, 0, 0, 0, 0};
static int              s_locked   = 0;
static uint32_t         s_locked_id = 0;

/* ---- result callback: runs inside ROCKIVA_WaitFinish, picks one target ---- */
static void detect_cb(const RockIvaDetectResult *result,
                      const RockIvaExecuteStatus status, void *userdata)
{
    (void)userdata;

    person_target_t sel;
    memset(&sel, 0, sizeof(sel));

    if (status == ROCKIVA_SUCCESS && result && result->objNum > 0) {
        pthread_mutex_lock(&s_mutex);
        int      locked    = s_locked;
        uint32_t locked_id = s_locked_id;
        pthread_mutex_unlock(&s_mutex);

        int best_center = 0x7fffffff;  /* closest-to-centre fallback selection  */
        int have_center = 0;
        person_target_t center_pick;  memset(&center_pick, 0, sizeof(center_pick));
        int have_locked = 0;
        person_target_t locked_pick;  memset(&locked_pick, 0, sizeof(locked_pick));

        for (uint32_t i = 0; i < result->objNum; i++) {
            const RockIvaObjectInfo *o = &result->objInfo[i];
            if (o->type != ROCKIVA_OBJECT_TYPE_PERSON) continue;

            person_target_t t;
            t.id    = o->objId;
            t.valid = 1;
            t.x0    = o->rect.topLeft.x;
            t.y0    = o->rect.topLeft.y;
            t.x1    = o->rect.bottomRight.x;
            t.y1    = o->rect.bottomRight.y;
            t.score = (int)o->score;

            if (locked && o->objId == locked_id) {
                locked_pick = t;
                have_locked = 1;
            }

            int cx = (t.x0 + t.x1) / 2 - 5000;
            int cy = (t.y0 + t.y1) / 2 - 5000;
            int d  = cx * cx + cy * cy;
            if (d < best_center) {
                best_center = d;
                center_pick = t;
                have_center = 1;
            }
        }

        if (locked) {
            /* Hold the locked person; if they are momentarily lost, report
             * invalid (camera stays put) but keep the lock to re-acquire. */
            if (have_locked) sel = locked_pick;
        } else if (have_center) {
            sel = center_pick;
        }
    }

    pthread_mutex_lock(&s_mutex);
    s_target = sel;
    pthread_mutex_unlock(&s_mutex);

    FILE *f = fopen("/tmp/person_det", "w");
    if (f) {
        fprintf(f, "objNum=%u locked=%d id=%u valid=%d box10k=(%d,%d)-(%d,%d) score=%d\n",
                result ? result->objNum : 0, s_locked, sel.id, sel.valid,
                sel.x0, sel.y0, sel.x1, sel.y1, sel.score);
        fclose(f);
    }
}

/* ---- frame release: sync model owns the VI frame, so this is a no-op ---- */
static void release_cb(const RockIvaReleaseFrames *frames, void *userdata)
{
    (void)frames; (void)userdata;   /* extData is NULL; nothing to free here */
}

void person_det_deinit(void)
{
    if (!s_inited) return;
    if (s_handle) {
        ROCKIVA_DETECT_Release(s_handle);
        ROCKIVA_Release(s_handle);
        s_handle = NULL;
    }
    s_inited = 0;
    s_w = s_h = 0;
    pthread_mutex_lock(&s_mutex);
    memset(&s_target, 0, sizeof(s_target));
    pthread_mutex_unlock(&s_mutex);
    LOG_INFO("person_det: released\n");
}

int person_det_init(int img_w, int img_h)
{
    if (s_inited && img_w == s_w && img_h == s_h) return 0;
    if (s_inited) person_det_deinit();

    RockIvaInitParam ip;
    memset(&ip, 0, sizeof(ip));
    snprintf(ip.modelPath, sizeof(ip.modelPath), "%s", PERSON_DET_MODEL_DIR);
    ip.logLevel               = ROCKIVA_LOG_ERROR;
    ip.detModel               = ROCKIVA_DET_MODEL_PFP;   /* person/face/pet model */
    ip.imageInfo.width        = (uint16_t)img_w;
    ip.imageInfo.height       = (uint16_t)img_h;
    ip.imageInfo.format       = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    ip.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;

    RockIvaRetCode rc = ROCKIVA_Init(&s_handle, ROCKIVA_MODE_VIDEO, &ip, NULL);
    if (rc != ROCKIVA_RET_SUCCESS) {
        LOG_WARN("person_det: ROCKIVA_Init failed rc=%d (model dir %s)\n",
                 rc, PERSON_DET_MODEL_DIR);
        s_handle = NULL;
        return -1;
    }

    RockIvaDetTaskParams dp;
    memset(&dp, 0, sizeof(dp));
    dp.detObjectType = ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON);
    dp.scores[ROCKIVA_OBJECT_TYPE_PERSON] = PERSON_DET_SCORE;
    dp.min_det_count = PERSON_DET_MIN_COUNT;

    rc = ROCKIVA_DETECT_Init(s_handle, &dp, detect_cb);
    if (rc != ROCKIVA_RET_SUCCESS) {
        LOG_WARN("person_det: ROCKIVA_DETECT_Init failed rc=%d\n", rc);
        ROCKIVA_Release(s_handle);
        s_handle = NULL;
        return -1;
    }

    ROCKIVA_SetFrameReleaseCallback(s_handle, release_cb);

    s_w = img_w; s_h = img_h; s_inited = 1;
    LOG_INFO("person_det: RockIVA PFP ready (%dx%d, model %s)\n",
             img_w, img_h, PERSON_DET_MODEL_DIR);
    return 0;
}

void person_det_run_fd(int dma_fd, int w, int h, uint32_t frame_id)
{
    if (!s_inited || w != s_w || h != s_h) {
        if (person_det_init(w, h) != 0) return;
    }
    if (!s_handle || dma_fd < 0) return;

    RockIvaImage img;
    memset(&img, 0, sizeof(img));
    img.info.width         = (uint16_t)w;
    img.info.height        = (uint16_t)h;
    img.info.format        = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
    img.info.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;
    img.frameId            = frame_id;
    img.dataAddr           = NULL;
    img.dataPhyAddr        = NULL;
    img.dataFd             = dma_fd;
    img.extData            = NULL;   /* sync: caller owns the frame */

    RockIvaRetCode rc = ROCKIVA_PushFrame(s_handle, &img, NULL);
    if (rc != ROCKIVA_RET_SUCCESS) return;

    /* Block until this frame's detection completes; detect_cb fires here. */
    ROCKIVA_WaitFinish(s_handle, (long)frame_id, PERSON_DET_WAIT_MS);
}

void person_det_get_target(person_target_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&s_mutex);
    *out = s_target;
    pthread_mutex_unlock(&s_mutex);
}

void person_det_lock_current(void)
{
    pthread_mutex_lock(&s_mutex);
    if (s_target.valid && s_target.id != 0) {
        s_locked_id = s_target.id;
        s_locked    = 1;
        LOG_INFO("person_det: locked onto track id %u\n", s_locked_id);
    }
    pthread_mutex_unlock(&s_mutex);
}

void person_det_unlock(void)
{
    pthread_mutex_lock(&s_mutex);
    s_locked    = 0;
    s_locked_id = 0;
    pthread_mutex_unlock(&s_mutex);
    LOG_INFO("person_det: unlocked\n");
}

int person_det_is_locked(void)
{
    pthread_mutex_lock(&s_mutex);
    int v = s_locked;
    pthread_mutex_unlock(&s_mutex);
    return v;
}
