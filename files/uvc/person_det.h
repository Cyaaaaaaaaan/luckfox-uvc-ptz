#ifndef __PERSON_DET_H__
#define __PERSON_DET_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RockIVA person detection + multi-object tracking wrapper.
 *
 * Replaces the hand-rolled RetinaFace path with the SDK's RockIVA library:
 *   - full-body PERSON detection (works at steep back-row meeting angles where
 *     a face is too small/oblique for RetinaFace),
 *   - persistent per-target track IDs (the foundation for one-shot
 *     "lock onto the commenter, then hold" behaviour),
 *   - hardware RGA resize + NPU inference, fed the NV12 frame by DMA fd
 *     (no CPU colour-convert / letterbox like the RetinaFace path needed).
 *
 * Model: object_detection_pfp.data (person/face/pet) deployed to
 *        PERSON_DET_MODEL_DIR on the target.  See README / PATCH 12.
 */

/* One tracked target.  Bounding box is in 0..10000 normalised scale, matching
 * the convention used throughout focus_score.cpp / eptz / the OSD box. */
typedef struct {
    uint32_t id;          /* persistent RockIVA track ID (0 = no target)      */
    int      valid;       /* 1 = a target is currently selected               */
    int16_t  x0, y0, x1, y1; /* bbox, 10000-scale                             */
    int      score;       /* detection confidence 1..100                      */
} person_target_t;

/* Initialise the detector for frames of (img_w x img_h) NV12.  Safe to call
 * again with a different size after a resolution switch — it re-inits.
 * Returns 0 on success, <0 on failure (detector then stays inert and
 * person_det_get_target() always reports valid=0). */
int  person_det_init(int img_w, int img_h);

/* Push one NV12 frame by its DMA buffer fd (RK_MPI_MB_Handle2Fd) and block
 * until RockIVA has finished detecting on it (ROCKIVA_WaitFinish).  The result
 * callback updates the internal target set + selection before this returns, so
 * the caller may release the VI frame immediately afterwards.
 * Re-inits transparently if (w,h) differ from the last init. */
void person_det_run_fd(int dma_fd, int w, int h, uint32_t frame_id);

/* Copy the currently selected target out (thread-safe). */
void person_det_get_target(person_target_t *out);

/* One-shot lock controls for the commenter use-case:
 *   lock_current() pins selection to whichever track ID is selected right now,
 *   so the camera holds that specific person even if others are more central.
 *   unlock() returns to automatic selection (closest-to-centre). */
void person_det_lock_current(void);
void person_det_unlock(void);
int  person_det_is_locked(void);

void person_det_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __PERSON_DET_H__ */
