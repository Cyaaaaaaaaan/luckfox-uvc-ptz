#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "visca.h"
#include "motor.h"

#define VISCA_PORT 1259

static volatile int s_running = 1;

static void on_signal(int sig) {
    (void)sig;
    s_running = 0;
}

int main(void) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    motor_init();

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(VISCA_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    printf("[visca] listening on UDP port %d\n", VISCA_PORT);

    uint8_t buf[64];
    struct sockaddr_in src;
    socklen_t srclen;

    while (s_running) {
        srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &srclen);
        if (n > 0) {
            visca_handle(sock, (struct sockaddr *)&src, srclen, buf, n);
        }
    }

    printf("[visca] shutting down\n");
    motor_deinit();
    close(sock);
    return 0;
}
