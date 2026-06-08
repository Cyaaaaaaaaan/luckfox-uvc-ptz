#ifndef __FOCUS_SCORE_H__
#define __FOCUS_SCORE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Set 1 to burn the focus score onto the video as an OSD overlay.
 * Set 0 to disable — score still written to /tmp/focus_score.
 * Monitor via:  watch -n0.5 cat /tmp/focus_score              */
#define FOCUS_SCORE_OSD 1

/* Set 1 to enable NPU face detection (RockIVA + object_detection_pfp.data).
 * When a face is detected near center, Tenengrad scores that face bbox instead
 * of the fixed center crop.  Falls back to center crop when no face present.
 * Requires /oem/usr/share/models/object_detection_pfp.data on the board.   */
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

#ifdef __cplusplus
}
#endif

#endif
