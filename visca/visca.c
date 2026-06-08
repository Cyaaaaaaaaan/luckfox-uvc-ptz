#include <stdio.h>
#include <string.h>
#include "visca.h"
#include "motor.h"

static const uint8_t ACK[]  = {0x90, 0x41, 0xFF};
static const uint8_t COMP[] = {0x90, 0x51, 0xFF};

static void reply(int sock, struct sockaddr *src, socklen_t srclen) {
    sendto(sock, ACK,  sizeof(ACK),  0, src, srclen);
    sendto(sock, COMP, sizeof(COMP), 0, src, srclen);
}

/* ── Packet dispatch ─────────────────────────────────────────────────────── */

static void handle_pan_tilt(int sock, struct sockaddr *src, socklen_t srclen,
                             const uint8_t *p, int len) {
    /* 81 01 06 01 VV WW XX YY FF  (len=9) */
    if (len < 9) return;
    int pan_spd  = p[4];
    int tilt_spd = p[5];
    int pan_dir  = p[6];   /* 01=left, 02=right, 03=stop */
    int tilt_dir = p[7];   /* 01=up,   02=down,  03=stop */

    int pd = (pan_dir  == 0x01) ? MOTOR_REV :
             (pan_dir  == 0x02) ? MOTOR_FWD : MOTOR_STOP;
    int td = (tilt_dir == 0x01) ? MOTOR_FWD :
             (tilt_dir == 0x02) ? MOTOR_REV : MOTOR_STOP;

    motor_pan(pd, pan_spd);
    motor_tilt(td, tilt_spd);
    reply(sock, src, srclen);
}

static void handle_zoom(int sock, struct sockaddr *src, socklen_t srclen,
                        const uint8_t *p, int len) {
    /* 81 01 04 07 pp FF  (len=6)
     * pp: 00=stop, 2s=tele speed s, 3s=wide speed s */
    if (len < 6) return;
    uint8_t pp  = p[4];
    int dir   = (pp == 0x00)        ? MOTOR_STOP :
                ((pp & 0xF0) == 0x20) ? MOTOR_FWD  : MOTOR_REV;
    int speed = pp & 0x0F;

    motor_zoom(dir, speed);
    reply(sock, src, srclen);
}

static void handle_focus(int sock, struct sockaddr *src, socklen_t srclen,
                         const uint8_t *p, int len) {
    /* 81 01 04 08 pp FF  (len=6)
     * pp: 00=stop, 2s=far speed s, 3s=near speed s */
    if (len < 6) return;
    uint8_t pp  = p[4];
    int dir   = (pp == 0x00)        ? MOTOR_STOP :
                ((pp & 0xF0) == 0x20) ? MOTOR_FWD  : MOTOR_REV;
    int speed = pp & 0x0F;

    motor_focus(dir, speed);
    reply(sock, src, srclen);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void visca_handle(int sock, struct sockaddr *src, socklen_t srclen,
                  const uint8_t *buf, int len) {
    if (len < 3 || buf[0] != 0x81 || buf[1] != 0x01) return;
    if (buf[len - 1] != 0xFF) return;

    /* Print raw packet for debugging */
    printf("[visca] rx %d bytes:", len);
    for (int i = 0; i < len; i++) printf(" %02x", buf[i]);
    printf("\n");

    uint8_t cat = buf[2];
    uint8_t cmd = buf[3];

    if (cat == 0x06 && cmd == 0x01) {
        handle_pan_tilt(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x07) {
        handle_zoom(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x08) {
        handle_focus(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x38) {
        /* Focus mode (auto/manual) — just ACK */
        reply(sock, src, srclen);
    } else {
        printf("[visca] unknown command %02x %02x — ignoring\n", cat, cmd);
        reply(sock, src, srclen);
    }
}
