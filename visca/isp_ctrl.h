#pragma once

/* Unix datagram socket that visca_server uses to send ISP commands to
 * rk_mpi_uvc. Text protocol, one command per datagram, newline optional.
 *
 * Commands:
 *   ae auto
 *   ae manual
 *   wb auto
 *   wb manual
 *   wb ct <kelvin>     e.g. "wb ct 3200"
 */

#define ISP_IPC_SOCK_PATH "/tmp/visca_isp.sock"

void isp_ctrl_init(void);
void isp_ctrl_deinit(void);
void isp_ctrl_send(const char *cmd);
