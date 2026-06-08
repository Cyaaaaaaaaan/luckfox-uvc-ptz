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

static const char *dir_name(int dir) {
    return dir == MOTOR_FWD ? "FWD" : dir == MOTOR_REV ? "REV" : "STOP";
}

/* ── Packet dispatch ─────────────────────────────────────────────────────── */

static void handle_pan_tilt(int sock, struct sockaddr *src, socklen_t srclen,
                             const uint8_t *p, int len) {
    /* 81 01 06 01 VV WW XX YY FF  (9 bytes)
     * VV = pan  speed 0x00–0x18 (0–24)
     * WW = tilt speed 0x00–0x14 (0–20)
     * XX = pan  dir  01=left 02=right 03=stop
     * YY = tilt dir  01=up   02=down  03=stop */
    if (len < 9) return;

    int pan_spd  = p[4];
    int tilt_spd = p[5];
    int pan_raw  = p[6];
    int tilt_raw = p[7];

    int pd = (pan_raw  == 0x01) ? MOTOR_REV  :
             (pan_raw  == 0x02) ? MOTOR_FWD  : MOTOR_STOP;
    int td = (tilt_raw == 0x01) ? MOTOR_FWD  :
             (tilt_raw == 0x02) ? MOTOR_REV  : MOTOR_STOP;

    printf("[pan/tilt] pan=%s spd=%d  tilt=%s spd=%d\n",
           dir_name(pd), pan_spd, dir_name(td), tilt_spd);

    motor_pan(pd, pan_spd);
    motor_tilt(td, tilt_spd);
    reply(sock, src, srclen);
}

static void handle_zoom(int sock, struct sockaddr *src, socklen_t srclen,
                        const uint8_t *p, int len) {
    /* 81 01 04 07 pp FF  (6 bytes)
     * pp: 00=stop, 2s=tele (in) speed s, 3s=wide (out) speed s
     * speed s = 0x0–0x7 */
    if (len < 6) return;

    uint8_t pp  = p[4];
    int dir   = (pp == 0x00)          ? MOTOR_STOP :
                ((pp & 0xF0) == 0x20) ? MOTOR_FWD  : MOTOR_REV;
    int speed = pp & 0x0F;

    printf("[zoom]     dir=%s  spd=%d  (raw=0x%02x)\n",
           dir_name(dir), speed, pp);

    motor_zoom(dir, speed);
    reply(sock, src, srclen);
}

static void handle_focus(int sock, struct sockaddr *src, socklen_t srclen,
                         const uint8_t *p, int len) {
    /* 81 01 04 08 pp FF  (6 bytes)
     * pp: 00=stop, 2s=far speed s, 3s=near speed s */
    if (len < 6) return;

    uint8_t pp  = p[4];
    int dir   = (pp == 0x00)          ? MOTOR_STOP :
                ((pp & 0xF0) == 0x20) ? MOTOR_FWD  : MOTOR_REV;
    int speed = pp & 0x0F;

    printf("[focus]    dir=%s  spd=%d  (raw=0x%02x)\n",
           dir_name(dir), speed, pp);

    motor_focus(dir, speed);
    reply(sock, src, srclen);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void visca_handle(int sock, struct sockaddr *src, socklen_t srclen,
                  const uint8_t *buf, int len) {
    if (len < 3 || buf[0] != 0x81 || buf[1] != 0x01) return;
    if (buf[len - 1] != 0xFF) return;

    /* Raw hex dump */
    printf("[visca] rx:");
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
        printf("[focus]    mode set (ACK only)\n");
        reply(sock, src, srclen);
    } else {
        printf("[visca] unknown cmd %02x %02x\n", cat, cmd);
        reply(sock, src, srclen);
    }
}
