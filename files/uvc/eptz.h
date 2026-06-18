#ifndef __EPTZ_H__
#define __EPTZ_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Electronic pan/tilt/zoom — digital PTZ via RK_MPI_VI_SetEptz (crop+scale).
 *
 * State is normalized so it survives resolution switches:
 *   zoom : 1.0 = full frame (no zoom), >1.0 = zoomed in (smaller crop)
 *   cx,cy: crop center, 0..1 across the source frame (0.5,0.5 = centered)
 *
 * OFF by default — until eptz_enable(1) the crop is full-frame (no change to
 * the stream). Even when enabled, the default state (zoom=1, centered) is a
 * full-frame crop, so nothing visibly changes until a zoom/pan/tilt command.
 *
 * All entry points are thread-safe. */

void eptz_init(int vi_pipe, int vi_chn, int src_w, int src_h);
void eptz_enable(int on);          /* 0 = full frame (disabled), 1 = apply crop */
int  eptz_is_enabled(void);

void eptz_set_zoom(float zoom);    /* absolute, clamped to [1, EPTZ_ZOOM_MAX] */
void eptz_zoom_delta(float dz);    /* relative */
void eptz_set_center(float cx, float cy); /* absolute, 0..1 */
void eptz_pan(float dcx);          /* relative center move, +right */
void eptz_tilt(float dcy);         /* relative center move, +down  */
void eptz_reset(void);             /* center, zoom=1 */

void eptz_reapply(void);           /* re-push current state (after a res switch) */

/* If testing shows the crop coordinate space is the output resolution rather
 * than the sensor native size, call this from the resize path. Default source
 * is the sensor native max set in eptz_init(). */
void eptz_set_source_size(int src_w, int src_h);

#ifdef __cplusplus
}
#endif

#endif /* __EPTZ_H__ */
