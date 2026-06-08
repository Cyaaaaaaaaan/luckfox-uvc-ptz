#pragma once

/* Unix datagram socket server — rk_mpi_uvc side.
 * Receives text ISP commands from visca_server and applies them.
 *
 * Protocol (one command per datagram, text, no newline required):
 *   ae auto
 *   ae manual
 *   wb auto
 *   wb indoor          — 3200 K
 *   wb outdoor         — 5800 K
 *   wb manual          — same as wb auto on rkaiq ("manualWhiteBalance" style)
 *   wb ct <kelvin>     — e.g. "wb ct 4500"
 */

#define ISP_IPC_SOCK_PATH "/tmp/visca_isp.sock"

#ifdef __cplusplus
extern "C" {
#endif

int  isp_ipc_start(int cam_id);  /* spawns listener thread; returns 0 on success */
void isp_ipc_stop(void);

#ifdef __cplusplus
}
#endif
