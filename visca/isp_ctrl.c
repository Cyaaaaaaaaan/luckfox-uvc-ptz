#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "isp_ctrl.h"

static int s_sock = -1;
static struct sockaddr_un s_dst;

void isp_ctrl_init(void) {
    s_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        perror("[isp_ctrl] socket");
        return;
    }
    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sun_family = AF_UNIX;
    strncpy(s_dst.sun_path, ISP_IPC_SOCK_PATH, sizeof(s_dst.sun_path) - 1);
    printf("[isp_ctrl] ready -> %s\n", ISP_IPC_SOCK_PATH);
}

void isp_ctrl_deinit(void) {
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

void isp_ctrl_send(const char *cmd) {
    if (s_sock < 0) return;
    int n = sendto(s_sock, cmd, strlen(cmd), 0,
                   (struct sockaddr *)&s_dst, sizeof(s_dst));
    if (n < 0)
        perror("[isp_ctrl] sendto");
    else
        printf("[isp_ctrl] -> %s\n", cmd);
}
