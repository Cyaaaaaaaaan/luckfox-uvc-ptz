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

/* Set 1 to enable NPU face detection (RKNN RetinaFace).
 * When a face is detected, the ISP AF measurement window is set to that face
 * bbox so the hardware sharpness score reflects only the face region.
 * Requires /oem/usr/share/models/retinaface.rknn on the board.            */
#define FOCUS_SCORE_FACE_DETECT 1

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

/* Face auto-tracking: P-controller that nudges the EPTZ crop to keep the
 * largest detected face centered.  Calling set_autotrack(1) also calls
 * eptz_enable(1) so EPTZ activates automatically.  Requires FOCUS_SCORE_FACE_DETECT. */
void focus_score_set_autotrack(int on);
int  focus_score_get_autotrack(void);

#ifdef __cplusplus
}
#endif

#endif
