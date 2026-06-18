#include <stdio.h>
#include <string.h>
#include "visca.h"
#include "motor.h"
#include "isp_ctrl.h"

/* ── VISCA framing ───────────────────────────────────────────────────────── */

static const uint8_t ACK[]  = {0x90, 0x41, 0xFF};
static const uint8_t COMP[] = {0x90, 0x51, 0xFF};
static const uint8_t ERR[]  = {0x90, 0x60, 0x02, 0xFF}; /* cmd not supported */

static void reply(int sock, struct sockaddr *src, socklen_t srclen) {
    sendto(sock, ACK,  sizeof(ACK),  0, src, srclen);
    sendto(sock, COMP, sizeof(COMP), 0, src, srclen);
}

static void reply_err(int sock, struct sockaddr *src, socklen_t srclen) {
    sendto(sock, ERR, sizeof(ERR), 0, src, srclen);
}

/* inquiry answer: 90 50 [data...] FF */
static void reply_inq(int sock, struct sockaddr *src, socklen_t srclen,
                      const uint8_t *data, int dlen) {
    uint8_t buf[16];
    buf[0] = 0x90;
    buf[1] = 0x50;
    memcpy(buf + 2, data, dlen);
    buf[2 + dlen] = 0xFF;
    sendto(sock, buf, 3 + dlen, 0, src, srclen);
}

/* ── Shadow state (for inquiry responses) ────────────────────────────────── */

static uint8_t s_ae_mode = 0x00;  /* 00=auto 03=manual 0A=shutter 0B=iris */
static uint8_t s_wb_mode = 0x00;  /* 00=auto 01=indoor 02=outdoor 05=manual */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static const char *dir_name(int dir) {
    return dir == MOTOR_FWD ? "FWD" : dir == MOTOR_REV ? "REV" : "STOP";
}

/* encode a 0-255 value as two nibbles: 0p 0q */
static void encode_nibbles(uint8_t val, uint8_t *out) {
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = (val >> 4) & 0x0F;
    out[3] =  val       & 0x0F;
}

/* ── Command handlers (81 01 ...) ────────────────────────────────────────── */

static void handle_pan_tilt(int sock, struct sockaddr *src, socklen_t srclen,
                             const uint8_t *p, int len) {
    /* 81 01 06 01 VV WW XX YY FF
     * VV=pan speed 0x00-0x18, WW=tilt speed 0x00-0x14
     * XX: 01=left 02=right 03=stop
     * YY: 01=up   02=down  03=stop */
    if (len < 9) return;
    int pan_spd  = p[4];
    int tilt_spd = p[5];
    int pd = (p[6] == 0x01) ? MOTOR_REV  :
             (p[6] == 0x02) ? MOTOR_FWD  : MOTOR_STOP;
    int td = (p[7] == 0x01) ? MOTOR_FWD  :
             (p[7] == 0x02) ? MOTOR_REV  : MOTOR_STOP;

    printf("[pan/tilt] pan=%s spd=%d  tilt=%s spd=%d\n",
           dir_name(pd), pan_spd, dir_name(td), tilt_spd);
    motor_pan(pd, pan_spd);
    motor_tilt(td, tilt_spd);

    /* EPTZ digital pan/tilt (Phase 0). No-op until "eptz on" in rk_mpi_uvc.
     * Each packet nudges the crop center; the KC2000 streams packets while
     * the stick is held, so motion is continuous. +cx=right, +cy=down. */
    if (pd != MOTOR_STOP) {
        float step = 0.012f + (pan_spd / 24.0f) * 0.040f;
        char c[40];
        snprintf(c, sizeof(c), "eptz pan %.4f", pd == MOTOR_FWD ? step : -step);
        isp_ctrl_send(c);
    }
    if (td != MOTOR_STOP) {
        float step = 0.012f + (tilt_spd / 20.0f) * 0.040f;
        /* td FWD = up = move crop up = decrease cy */
        char c[40];
        snprintf(c, sizeof(c), "eptz tilt %.4f", td == MOTOR_FWD ? -step : step);
        isp_ctrl_send(c);
    }
    reply(sock, src, srclen);
}

static void handle_zoom(int sock, struct sockaddr *src, socklen_t srclen,
                        const uint8_t *p, int len) {
    /* 81 01 04 07 pp FF — 2s=tele spd s, 3s=wide spd s, 00=stop */
    if (len < 6) return;
    uint8_t pp = p[4];
    int dir   = (pp == 0x00)          ? MOTOR_STOP :
                ((pp & 0xF0) == 0x20) ? MOTOR_FWD  : MOTOR_REV;
    int speed = pp & 0x0F;
    printf("[zoom]     dir=%s spd=%d (raw=0x%02x)\n", dir_name(dir), speed, pp);
    motor_zoom(dir, speed);

    /* Hybrid zoom (Phase 0): no optical zoom motor yet, so the whole range is
     * EPTZ. tele/in (FWD) = zoom in (+), wide/out (REV) = zoom out (-).
     * No-op until "eptz on" in rk_mpi_uvc. */
    if (dir != MOTOR_STOP) {
        float step = 0.05f + (speed / 7.0f) * 0.10f;
        char c[40];
        snprintf(c, sizeof(c), "eptz dzoom %.4f", dir == MOTOR_FWD ? step : -step);
        isp_ctrl_send(c);
    }
    reply(sock, src, srclen);
}

static void handle_focus(int sock, struct sockaddr *src, socklen_t srclen,
                         const uint8_t *p, int len) {
    /* 81 01 04 08 pp FF — 2s=far, 3s=near, 00=stop */
    if (len < 6) return;
    uint8_t pp = p[4];
    int dir   = (pp == 0x00)          ? MOTOR_STOP :
                ((pp & 0xF0) == 0x20) ? MOTOR_FWD  : MOTOR_REV;
    int speed = pp & 0x0F;
    printf("[focus]    dir=%s spd=%d (raw=0x%02x)\n", dir_name(dir), speed, pp);
    motor_focus(dir, speed);
    reply(sock, src, srclen);
}

static void handle_ae_mode(int sock, struct sockaddr *src, socklen_t srclen,
                           const uint8_t *p, int len) {
    /* 81 01 04 39 pp FF
     * 00=full auto, 03=manual, 0A=shutter priority, 0B=iris priority */
    if (len < 6) return;
    s_ae_mode = p[4];
    const char *mode = (s_ae_mode == 0x00) ? "auto" : "manual";
    printf("[ae]       mode=0x%02x -> %s\n", s_ae_mode, mode);
    isp_ctrl_send(s_ae_mode == 0x00 ? "ae auto" : "ae manual");
    reply(sock, src, srclen);
}

static void handle_wb_mode(int sock, struct sockaddr *src, socklen_t srclen,
                           const uint8_t *p, int len) {
    /* 81 01 04 35 pp FF
     * 00=auto, 01=indoor(3200K), 02=outdoor(5800K), 05=manual */
    if (len < 6) return;
    s_wb_mode = p[4];
    const char *cmd;
    switch (s_wb_mode) {
        case 0x01: cmd = "wb indoor";  break;
        case 0x02: cmd = "wb outdoor"; break;
        case 0x05: cmd = "wb manual";  break;
        default:   cmd = "wb auto";    break;
    }
    printf("[wb]       mode=0x%02x -> %s\n", s_wb_mode, cmd);
    isp_ctrl_send(cmd);
    reply(sock, src, srclen);
}

/* ── Inquiry handlers (81 09 ...) ────────────────────────────────────────── */

static void handle_inquiry(int sock, struct sockaddr *src, socklen_t srclen,
                           const uint8_t *p, int len) {
    if (len < 4) return;
    uint8_t cat = p[2];
    uint8_t cmd = p[3];
    uint8_t data[4];

    if (cat == 0x04 && cmd == 0x39) {
        /* AE mode */
        data[0] = s_ae_mode;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      ae_mode -> 0x%02x\n", s_ae_mode);
    } else if (cat == 0x04 && cmd == 0x35) {
        /* WB mode */
        data[0] = s_wb_mode;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      wb_mode -> 0x%02x\n", s_wb_mode);
    } else if (cat == 0x04 && (cmd == 0x43 || cmd == 0x44 ||
                                cmd == 0x20 || cmd == 0x12 || cmd == 0x13)) {
        /* R/B gain, iris, shutter, gain — return zeros (auto managed) */
        encode_nibbles(0, data);
        reply_inq(sock, src, srclen, data, 4);
        printf("[inq]      04 %02x -> 0 (stub)\n", cmd);
    } else if (cat == 0x04 && cmd == 0xA9) {
        /* AE sensitivity — return mid-range */
        data[0] = 0x01;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      ae_sensitivity -> 0x01 (stub)\n");
    } else if (cat == 0x06 && cmd == 0x06) {
        /* Menu state — always report closed (controller won't show menu UI) */
        data[0] = 0x02;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      menu -> closed\n");
    } else {
        printf("[inq]      unknown %02x %02x — error reply\n", cat, cmd);
        reply_err(sock, src, srclen);
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void visca_handle(int sock, struct sockaddr *src, socklen_t srclen,
                  const uint8_t *buf, int len) {
    if (len < 3 || buf[len - 1] != 0xFF) return;

    /* Print raw hex */
    printf("[visca] rx:");
    for (int i = 0; i < len; i++) printf(" %02x", buf[i]);
    printf("\n");

    if (buf[0] != 0x81) return;

    uint8_t type = buf[1];
    uint8_t cat  = buf[2];
    uint8_t cmd  = (len > 3) ? buf[3] : 0x00;

    if (type == 0x09) {
        /* Inquiry */
        handle_inquiry(sock, src, srclen, buf, len);
        return;
    }

    if (type != 0x01) return;

    /* Commands */
    if (cat == 0x06 && cmd == 0x01) {
        handle_pan_tilt(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x07) {
        handle_zoom(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x08) {
        handle_focus(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x38) {
        /* Focus mode switch — ACK only */
        printf("[focus]    mode set\n");
        reply(sock, src, srclen);
    } else if (cat == 0x04 && cmd == 0x39) {
        handle_ae_mode(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x35) {
        handle_wb_mode(sock, src, srclen, buf, len);
    } else if (cat == 0x06 && cmd == 0x06) {
        /* Repurpose menu button: cycle display mode 0→1→2→0
         * 0=all off  1=facebox only  2=facebox+OSD */
        static int s_menu_mode = 0;
        s_menu_mode = (s_menu_mode + 1) % 3;
        char ipc_cmd[32];
        snprintf(ipc_cmd, sizeof(ipc_cmd), "display %d", s_menu_mode);
        isp_ctrl_send(ipc_cmd);
        printf("[menu]     display mode -> %d\n", s_menu_mode);
        reply(sock, src, srclen);
    } else if (cat == 0x04 && cmd == 0x3F) {
        /* Memory recall — ACK only */
        printf("[memory]   recall preset 0x%02x — ignored\n", (len > 5) ? buf[5] : 0);
        reply(sock, src, srclen);
    } else {
        printf("[visca] unknown cmd %02x %02x\n", cat, cmd);
        reply(sock, src, srclen);
    }
}
