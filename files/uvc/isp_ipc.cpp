#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

extern "C" {
#include "isp.h"
}
#include "isp_ipc.h"

static int      s_sock   = -1;
static int      s_cam_id = 0;
static pthread_t s_thread;
static volatile int s_running = 0;

static void apply_cmd(const char *cmd) {
    printf("[isp_ipc] cmd: %s\n", cmd);

    if (strncmp(cmd, "ae ", 3) == 0) {
        const char *val = cmd + 3;
        /* rkaiq: "auto" or "manual" */
        rk_isp_set_exposure_mode(s_cam_id, val);

    } else if (strncmp(cmd, "wb ", 3) == 0) {
        const char *val = cmd + 3;
        if (strcmp(val, "manual") == 0) {
            rk_isp_set_white_blance_style(s_cam_id, "manualWhiteBalance");
        } else if (strcmp(val, "indoor") == 0) {
            rk_isp_set_white_blance_style(s_cam_id, "manualWhiteBalance");
            rk_isp_set_white_blance_ct(s_cam_id, 3200);
        } else if (strcmp(val, "outdoor") == 0) {
            rk_isp_set_white_blance_style(s_cam_id, "manualWhiteBalance");
            rk_isp_set_white_blance_ct(s_cam_id, 5800);
        } else if (strncmp(val, "ct ", 3) == 0) {
            int kelvin = atoi(val + 3);
            if (kelvin >= 2000 && kelvin <= 10000) {
                rk_isp_set_white_blance_style(s_cam_id, "manualWhiteBalance");
                rk_isp_set_white_blance_ct(s_cam_id, kelvin);
            }
        } else {
            /* "auto" or anything else */
            rk_isp_set_white_blance_style(s_cam_id, "auto");
        }
    } else {
        printf("[isp_ipc] unknown cmd: %s\n", cmd);
    }
}

static void *listener_thread(void *arg) {
    (void)arg;
    char buf[128];
    while (s_running) {
        ssize_t n = recv(s_sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = '\0';
        /* strip trailing newline if any */
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        apply_cmd(buf);
    }
    return NULL;
}

int isp_ipc_start(int cam_id) {
    s_cam_id = cam_id;

    s_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        perror("[isp_ipc] socket");
        return -1;
    }

    /* Remove stale socket file */
    unlink(ISP_IPC_SOCK_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ISP_IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[isp_ipc] bind");
        close(s_sock);
        s_sock = -1;
        return -1;
    }

    s_running = 1;
    if (pthread_create(&s_thread, NULL, listener_thread, NULL) != 0) {
        perror("[isp_ipc] pthread_create");
        s_running = 0;
        close(s_sock);
        s_sock = -1;
        return -1;
    }

    printf("[isp_ipc] listening on %s (cam %d)\n", ISP_IPC_SOCK_PATH, cam_id);
    return 0;
}

void isp_ipc_stop(void) {
    if (!s_running) return;
    s_running = 0;
    /* wake the recv() with a self-send */
    if (s_sock >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, ISP_IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);
        sendto(s_sock, "quit", 4, 0, (struct sockaddr *)&addr, sizeof(addr));
    }
    pthread_join(s_thread, NULL);
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    unlink(ISP_IPC_SOCK_PATH);
    printf("[isp_ipc] stopped\n");
}
