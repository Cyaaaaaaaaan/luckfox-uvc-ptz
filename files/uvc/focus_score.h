#ifndef __FOCUS_SCORE_H__
#define __FOCUS_SCORE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Set 1 to compile in the OSD overlay (score text + green face box).
 * The overlay is still OFF at runtime by default (display mode 0) — cycle it
 * on with the VISCA menu button or the "display N" IPC command.
 * Set 0 to compile it out entirely. Score is always written to
 * /tmp/focus_score regardless.  Monitor via: watch -n0.5 cat /tmp/focus_score */
#define FOCUS_SCORE_OSD 1

/* Set 1 to bias the H.264/H.265 encoder toward the detected subject: each
 * detection cycle sets a VENC ROI (lower QP = more bits) on the upper ~60% of
 * the person box — the head/torso/hands where signing happens — so the signer
 * stays sharp even when bandwidth crushes the background. Independent of the
 * debug facebox; needs FOCUS_SCORE_OSD (for the VENC handle) + a detector. */
#define FOCUS_SCORE_VENC_ROI 1

/* Set 1 to enable VENC hardware motion deblur — sharpens fast motion (signing
 * hands) on the encoded stream.  Applied to the streaming VENC channel at
 * attach, default ON.  Toggle live via IPC "deblur on|off"; optional
 * "deblur strength N" (0..3, driver default if never set).  Needs FOCUS_SCORE_OSD
 * (for the VENC handle). */
#define FOCUS_SCORE_MOTION_DEBLUR 1

/* Subject detection back end — pick ONE (both use the single NPU core):
 *
 *   FOCUS_SCORE_PERSON_DETECT — RockIVA full-body person detection + persistent
 *     track IDs.  Robust at steep meeting-room angles where a face is too small
 *     for RetinaFace, and the track IDs enable one-shot "lock onto the
 *     commenter, then hold" behaviour.  Requires the PFP model deployed to
 *     /oem/usr/share/iva (see PATCH 12).
 *
 *   FOCUS_SCORE_FACE_DETECT — legacy hand-rolled RetinaFace via librknnmrt.
 *     Requires /oem/usr/share/models/retinaface.rknn on the board.
 *
 * When either is on, the detected subject bbox drives the ISP AF measurement
 * window, the focus-score crop, the OSD target box, and the auto-tracking
 * P-controller. */
#define FOCUS_SCORE_PERSON_DETECT 1
#define FOCUS_SCORE_FACE_DETECT   0

#define FOCUS_SCORE_TARGET_DETECT (FOCUS_SCORE_FACE_DETECT || FOCUS_SCORE_PERSON_DETECT)
#if FOCUS_SCORE_PERSON_DETECT && FOCUS_SCORE_FACE_DETECT
#error "Enable only one of FOCUS_SCORE_PERSON_DETECT / FOCUS_SCORE_FACE_DETECT (shared NPU)"
#endif

void focus_score_start(int pipe_id, int chn_id);
void focus_score_stop(void);

/* Latest Tenengrad score. 0 = not yet computed or VI not running. */
double focus_score_get(void);

#if FOCUS_SCORE_OSD
/* Call focus_score_osd_attach() from uvc_process startProcess() after VENC
 * is created — that is the only moment the RGN attach is guaranteed to work.
 * Call focus_score_osd_detach() from stopProcess() before VENC is destroyed. */
void focus_score_osd_attach(int venc_dev, int venc_chn);
void focus_score_osd_detach(int venc_dev, int venc_chn);
#endif

/* Display mode:
 *   0 = all OFF (default)
 *   1 = face box ON, score OSD OFF
 *   2 = face box ON, score OSD ON (full debug)
 * Cycles 0→1→2→0 on each call to focus_score_next_display_mode(). */
void focus_score_set_display_mode(int mode);
int  focus_score_get_display_mode(void);

/* Auto-tracking: P-controller that nudges the EPTZ crop to keep the selected
 * subject centered.  Calling set_autotrack(1) also calls eptz_enable(1) so EPTZ
 * activates automatically.  Requires FOCUS_SCORE_TARGET_DETECT. */
void focus_score_set_autotrack(int on);
int  focus_score_get_autotrack(void);

/* One-shot commenter lock (PERSON_DETECT only — no-ops otherwise):
 *   lock pins tracking to the currently selected person's track ID so the
 *   camera holds that individual; unlock resumes closest-to-centre selection. */
void focus_score_lock_target(void);
void focus_score_unlock_target(void);
int  focus_score_target_is_locked(void);

/* VENC motion deblur (no-ops if compiled out). strength clamps to 0..3. */
void focus_score_set_deblur(int on);
void focus_score_set_deblur_strength(int strength);
int  focus_score_get_deblur(void);

#ifdef __cplusplus
}
#endif

#endif
