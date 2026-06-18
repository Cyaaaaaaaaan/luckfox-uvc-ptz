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

static int     s_menu_open     = 0;    /* 1 = OSD camera menu is open, intercept nav */
static uint8_t s_ae_mode       = 0x00; /* 00=auto 03=manual */
static uint8_t s_wb_mode       = 0x00; /* 00=auto 01=indoor 02=outdoor 05=manual */
static uint8_t s_sharpness     = 0x08; /* 0x00-0x0F, mid=8 */
static uint8_t s_pic_effect    = 0x00; /* 00=color 04=B&W */
static uint8_t s_backlight     = 0x03; /* 02=on 03=off */
static uint8_t s_mirror        = 0x03; /* 02=on 03=off */
static uint8_t s_flicker       = 0x01; /* 00=off 01=50hz 02=60hz */
static uint8_t s_nr_level      = 0x03; /* 0x00-0x05 */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static const char *dir_name(int dir) {
    return dir == MOTOR_FWD ? "FWD" : dir == MOTOR_REV ? "REV" : "STOP";
}

/* encode a 0-255 value as four nibbles: 00 00 0p 0q */
static void encode_nibbles(uint8_t val, uint8_t *out) {
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = (val >> 4) & 0x0F;
    out[3] =  val       & 0x0F;
}

/* decode four-nibble value from VISCA direct command (0p 0q 0r 0s) */
static uint16_t decode_nibbles4(const uint8_t *p) {
    return ((p[0] & 0x0F) << 12) | ((p[1] & 0x0F) << 8) |
           ((p[2] & 0x0F) <<  4) |  (p[3] & 0x0F);
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

    /* When the OSD menu is open, intercept tilt for cursor navigation */
    if (s_menu_open) {
        if (td == MOTOR_FWD && tilt_spd > 0)
            isp_ctrl_send("menu up");
        else if (td == MOTOR_REV && tilt_spd > 0)
            isp_ctrl_send("menu down");
        /* pan is ignored in menu mode */
        reply(sock, src, srclen);
        return;
    }

    printf("[pan/tilt] pan=%s spd=%d  tilt=%s spd=%d\n",
           dir_name(pd), pan_spd, dir_name(td), tilt_spd);
    motor_pan(pd, pan_spd);
    motor_tilt(td, tilt_spd);

    /* EPTZ digital pan/tilt (Phase 0). No-op until "eptz on" in rk_mpi_uvc. */
    if (pd != MOTOR_STOP) {
        float step = 0.012f + (pan_spd / 24.0f) * 0.040f;
        char c[40];
        snprintf(c, sizeof(c), "eptz pan %.4f", pd == MOTOR_FWD ? step : -step);
        isp_ctrl_send(c);
    }
    if (td != MOTOR_STOP) {
        float step = 0.012f + (tilt_spd / 20.0f) * 0.040f;
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

    /* When the OSD menu is open, intercept zoom for value editing */
    if (s_menu_open && dir != MOTOR_STOP) {
        isp_ctrl_send(dir == MOTOR_FWD ? "menu inc" : "menu dec");
        reply(sock, src, srclen);
        return;
    }

    printf("[zoom]     dir=%s spd=%d (raw=0x%02x)\n", dir_name(dir), speed, pp);
    motor_zoom(dir, speed);

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
    /* 81 01 04 39 pp FF — 00=auto 03=manual 0A=shutter 0B=iris */
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
     * 00=auto 01=indoor 02=outdoor 05=manual
     * Extensions: 06=fluorescent 07=incandescent 08=warm 09=natural 0A=lock */
    if (len < 6) return;
    s_wb_mode = p[4];
    const char *cmd;
    switch (s_wb_mode) {
        case 0x01: cmd = "wb indoor";      break;
        case 0x02: cmd = "wb outdoor";     break;
        case 0x05: cmd = "wb manual";      break;
        case 0x06: cmd = "wb fluorescent"; break;
        case 0x07: cmd = "wb incandescent";break;
        case 0x08: cmd = "wb warm";        break;
        case 0x09: cmd = "wb natural";     break;
        case 0x0A: cmd = "wb lock";        break;
        default:   cmd = "wb auto";        break;
    }
    printf("[wb]       mode=0x%02x -> %s\n", s_wb_mode, cmd);
    isp_ctrl_send(cmd);
    reply(sock, src, srclen);
}

static void handle_sharpness(int sock, struct sockaddr *src, socklen_t srclen,
                              const uint8_t *p, int len) {
    /* 81 01 04 A2 pp FF — 02=up 03=down 00=reset
     * sharpness shadow 0x00-0x0F (4-bit), maps to ISP [0,100] */
    if (len < 6) return;
    uint8_t pp = p[4];
    if (pp == 0x02 && s_sharpness < 0x0F) s_sharpness++;
    else if (pp == 0x03 && s_sharpness > 0x00) s_sharpness--;
    else if (pp == 0x00) s_sharpness = 0x08;
    char c[32];
    snprintf(c, sizeof(c), "sharpness %d", (s_sharpness * 100) / 15);
    isp_ctrl_send(c);
    printf("[sharpness] level=%d -> ISP %d\n", s_sharpness, (s_sharpness * 100) / 15);
    reply(sock, src, srclen);
}

static void handle_sharpness_direct(int sock, struct sockaddr *src, socklen_t srclen,
                                    const uint8_t *p, int len) {
    /* 81 01 04 42 0p 0q 0r 0s FF — direct sharpness value (0x0000-0x000F) */
    if (len < 9) return;
    uint16_t val = decode_nibbles4(p + 4);
    s_sharpness = (uint8_t)(val & 0x0F);
    char c[32];
    snprintf(c, sizeof(c), "sharpness %d", (s_sharpness * 100) / 15);
    isp_ctrl_send(c);
    printf("[sharpness] direct=%d -> ISP %d\n", s_sharpness, (s_sharpness * 100) / 15);
    reply(sock, src, srclen);
}

static void handle_picture_effect(int sock, struct sockaddr *src, socklen_t srclen,
                                   const uint8_t *p, int len) {
    /* 81 01 04 3D pp FF — 00=color 04=B&W(mono)
     * Also repurposed as day/night: color=day, B&W=night */
    if (len < 6) return;
    s_pic_effect = p[4];
    if (s_pic_effect == 0x04) {
        isp_ctrl_send("daynight night");
        printf("[effect]   B&W / night mode\n");
    } else {
        isp_ctrl_send("daynight day");
        printf("[effect]   color / day mode\n");
    }
    reply(sock, src, srclen);
}

static void handle_backlight(int sock, struct sockaddr *src, socklen_t srclen,
                              const uint8_t *p, int len) {
    /* 81 01 04 33 02 FF = BLC on, 81 01 04 33 03 FF = BLC off */
    if (len < 6) return;
    s_backlight = p[4];
    if (s_backlight == 0x02) {
        isp_ctrl_send("blc on");
        printf("[blc]      on\n");
    } else {
        isp_ctrl_send("blc off");
        printf("[blc]      off\n");
    }
    reply(sock, src, srclen);
}

static void handle_lr_reverse(int sock, struct sockaddr *src, socklen_t srclen,
                               const uint8_t *p, int len) {
    /* 81 01 04 61 02 FF = mirror on, 03 FF = mirror off */
    if (len < 6) return;
    s_mirror = p[4];
    if (s_mirror == 0x02) {
        isp_ctrl_send("flip mirror");
        printf("[mirror]   on\n");
    } else {
        isp_ctrl_send("flip close");
        printf("[mirror]   off\n");
    }
    reply(sock, src, srclen);
}

static void handle_flicker(int sock, struct sockaddr *src, socklen_t srclen,
                            const uint8_t *p, int len) {
    /* 81 01 04 23 pp FF — 00=off 01=50Hz 02=60Hz */
    if (len < 6) return;
    s_flicker = p[4];
    if (s_flicker == 0x02) {
        isp_ctrl_send("flicker 60hz");
        printf("[flicker]  60Hz\n");
    } else if (s_flicker == 0x00) {
        /* no explicit "off" — default to 50Hz (PAL) */
        isp_ctrl_send("flicker 50hz");
        printf("[flicker]  50Hz (off→default)\n");
    } else {
        isp_ctrl_send("flicker 50hz");
        printf("[flicker]  50Hz\n");
    }
    reply(sock, src, srclen);
}

static void handle_nr(int sock, struct sockaddr *src, socklen_t srclen,
                      const uint8_t *p, int len) {
    /* 81 01 04 53 pp FF — 00=off 01-05=NR levels */
    if (len < 6) return;
    s_nr_level = p[4];
    char c[32];
    if (s_nr_level == 0x00) {
        isp_ctrl_send("nr mode close");
        printf("[nr]       off\n");
    } else {
        /* map 1-5 → 20-100 for spatial NR level */
        int lvl = s_nr_level * 20;
        isp_ctrl_send("nr mode mixnr");
        snprintf(c, sizeof(c), "nr spatial %d", lvl);
        isp_ctrl_send(c);
        snprintf(c, sizeof(c), "nr temporal %d", lvl);
        isp_ctrl_send(c);
        printf("[nr]       level=%d -> %d\n", s_nr_level, lvl);
    }
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
        data[0] = s_ae_mode;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      ae_mode -> 0x%02x\n", s_ae_mode);
    } else if (cat == 0x04 && cmd == 0x35) {
        data[0] = s_wb_mode;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      wb_mode -> 0x%02x\n", s_wb_mode);
    } else if (cat == 0x04 && cmd == 0x42) {
        /* Sharpness / aperture */
        encode_nibbles(s_sharpness, data);
        reply_inq(sock, src, srclen, data, 4);
        printf("[inq]      sharpness -> 0x%02x\n", s_sharpness);
    } else if (cat == 0x04 && cmd == 0x3D) {
        /* Picture effect */
        data[0] = s_pic_effect;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      pic_effect -> 0x%02x\n", s_pic_effect);
    } else if (cat == 0x04 && cmd == 0x33) {
        /* Backlight */
        data[0] = s_backlight;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      backlight -> 0x%02x\n", s_backlight);
    } else if (cat == 0x04 && cmd == 0x61) {
        /* Mirror / LR reverse */
        data[0] = s_mirror;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      mirror -> 0x%02x\n", s_mirror);
    } else if (cat == 0x04 && cmd == 0x23) {
        /* Flicker */
        data[0] = s_flicker;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      flicker -> 0x%02x\n", s_flicker);
    } else if (cat == 0x04 && cmd == 0x53) {
        /* NR level */
        data[0] = s_nr_level;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      nr_level -> 0x%02x\n", s_nr_level);
    } else if (cat == 0x04 && (cmd == 0x43 || cmd == 0x44 ||
                                cmd == 0x20 || cmd == 0x12 || cmd == 0x13)) {
        /* R/B gain, iris, shutter, gain — return zeros */
        encode_nibbles(0, data);
        reply_inq(sock, src, srclen, data, 4);
        printf("[inq]      04 %02x -> 0 (stub)\n", cmd);
    } else if (cat == 0x04 && cmd == 0xA9) {
        data[0] = 0x01;
        reply_inq(sock, src, srclen, data, 1);
        printf("[inq]      ae_sensitivity -> 0x01 (stub)\n");
    } else if (cat == 0x06 && cmd == 0x06) {
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

    printf("[visca] rx:");
    for (int i = 0; i < len; i++) printf(" %02x", buf[i]);
    printf("\n");

    if (buf[0] != 0x81) return;

    uint8_t type = buf[1];
    uint8_t cat  = buf[2];
    uint8_t cmd  = (len > 3) ? buf[3] : 0x00;

    if (type == 0x09) {
        handle_inquiry(sock, src, srclen, buf, len);
        return;
    }

    if (type != 0x01) return;

    /* ── Commands ──────────────────────────────────────────────────────── */
    if (cat == 0x06 && cmd == 0x01) {
        handle_pan_tilt(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x07) {
        handle_zoom(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x08) {
        handle_focus(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x38) {
        printf("[focus]    mode set\n");
        reply(sock, src, srclen);
    } else if (cat == 0x04 && cmd == 0x39) {
        handle_ae_mode(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x35) {
        handle_wb_mode(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0xA2) {
        /* CAM_Aperture (sharpness up/down/reset) */
        handle_sharpness(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x42) {
        /* CAM_ApertureDirect (sharpness direct value) */
        handle_sharpness_direct(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x3D) {
        /* CAM_PictureEffect: 00=color 04=B&W / day-night */
        handle_picture_effect(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x33) {
        /* CAM_BackLight: 02=on 03=off */
        handle_backlight(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x61) {
        /* CAM_LR_Reverse (mirror): 02=on 03=off */
        handle_lr_reverse(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x23) {
        /* CAM_FlickerMode: 00=off 01=50Hz 02=60Hz */
        handle_flicker(sock, src, srclen, buf, len);
    } else if (cat == 0x04 && cmd == 0x53) {
        /* CAM_NR (noise reduction): 00=off 01-05=levels */
        handle_nr(sock, src, srclen, buf, len);
    } else if (cat == 0x06 && cmd == 0x06) {
        /* Menu button: toggle OSD camera menu.
         * Open: tilt = cursor up/down; zoom = change value.
         * Close: returns tilt/zoom to normal PTZ/EPTZ use. */
        if (s_menu_open) {
            s_menu_open = 0;
            isp_ctrl_send("menu close");
            printf("[menu]     closed\n");
        } else {
            s_menu_open = 1;
            isp_ctrl_send("menu open");
            printf("[menu]     opened\n");
        }
        reply(sock, src, srclen);
    } else if (cat == 0x04 && cmd == 0x3F) {
        printf("[memory]   recall preset 0x%02x — ignored\n", (len > 5) ? buf[5] : 0);
        reply(sock, src, srclen);
    } else {
        printf("[visca] unknown cmd %02x %02x\n", cat, cmd);
        reply(sock, src, srclen);
    }
}
