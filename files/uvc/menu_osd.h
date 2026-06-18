#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Camera settings OSD menu — rendered as OVERLAY_RGN (handle 11).
 *
 * Lifecycle:
 *   menu_osd_attach()  — call from focus_score_osd_attach() after VENC is ready
 *   menu_osd_detach()  — call from focus_score_osd_detach() before VENC teardown
 *
 * Navigation (driven by IPC "menu open/close/up/down/inc/dec"):
 *   open  — show menu, render
 *   close — hide menu (transparent canvas)
 *   up    — move cursor up one row, re-render
 *   down  — move cursor down one row, re-render
 *   inc   — increment current item value, apply ISP change, re-render
 *   dec   — decrement current item value, apply ISP change, re-render
 *
 * ISP changes applied immediately via isp_apply_cmd() so the user
 * can see the effect in real time as they navigate.
 */

void menu_osd_attach(int venc_dev, int venc_chn);
void menu_osd_detach(int venc_dev, int venc_chn);

void menu_osd_open(void);
void menu_osd_close(void);
int  menu_osd_is_open(void);

void menu_osd_cursor_up(void);
void menu_osd_cursor_down(void);
void menu_osd_value_inc(void);
void menu_osd_value_dec(void);

#ifdef __cplusplus
}
#endif
