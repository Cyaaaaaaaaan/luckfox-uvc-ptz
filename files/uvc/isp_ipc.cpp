#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

extern "C" {
#include "isp.h"
#include "focus_score.h"
#include "eptz.h"
}
#include "isp_ipc.h"
#include "rk_aiq_user_api2_imgproc.h"

static int      s_sock   = -1;
static int      s_cam_id = 0;
static pthread_t s_thread;
static volatile int s_running = 0;

/* ── IRCut filter helper (GPIO 36=day/insert, GPIO 35=night/remove) ───────── */

static void gpio_write(int gpio, int value) {
    char path[64];
    /* Export if not already exported */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);
    if (access(path, F_OK) != 0) {
        int fd = open("/sys/class/gpio/export", O_WRONLY);
        if (fd >= 0) {
            char buf[8];
            int n = snprintf(buf, sizeof(buf), "%d", gpio);
            write(fd, buf, n);
            close(fd);
            usleep(20000);
        }
    }
    /* Set direction */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, "out", 3); close(fd); }
    /* Write value */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, value ? "1" : "0", 1);
        close(fd);
    }
}

static void ircut_set(int day_mode) {
    /* day_mode=1: insert IR-cut filter (day/color)
     * day_mode=0: remove IR-cut filter (night/IR) */
    if (day_mode) {
        gpio_write(36, 1);
        usleep(60000);
        gpio_write(36, 0);
    } else {
        gpio_write(35, 1);
        usleep(60000);
        gpio_write(35, 0);
    }
}

/* ── IPC command dispatcher ──────────────────────────────────────────────── */

static void apply_cmd(const char *cmd) {
    printf("[isp_ipc] cmd: %s\n", cmd);

    /* ── AE / Exposure ────────────────────────────────────────────────── */
    if (strncmp(cmd, "ae ", 3) == 0) {
        rk_isp_set_exposure_mode(s_cam_id, cmd + 3);

    /* ── WB (includes extended presets) ──────────────────────────────── */
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
        } else if (strcmp(val, "fluorescent") == 0) {
            rk_isp_set_white_blance_style(s_cam_id, "fluorescentLamp");
        } else if (strcmp(val, "incandescent") == 0) {
            rk_isp_set_white_blance_style(s_cam_id, "incandescent");
        } else if (strcmp(val, "warm") == 0) {
            rk_isp_set_white_blance_style(s_cam_id, "warmLight");
        } else if (strcmp(val, "natural") == 0) {
            rk_isp_set_white_blance_style(s_cam_id, "naturalLight");
        } else if (strcmp(val, "lock") == 0) {
            rk_isp_set_white_blance_style(s_cam_id, "lockingWhiteBalance");
        } else if (strncmp(val, "ct ", 3) == 0) {
            int kelvin = atoi(val + 3);
            if (kelvin >= 2000 && kelvin <= 10000) {
                rk_isp_set_white_blance_style(s_cam_id, "manualWhiteBalance");
                rk_isp_set_white_blance_ct(s_cam_id, kelvin);
            }
        } else {
            rk_isp_set_white_blance_style(s_cam_id, "auto");
        }

    /* ── Display mode (face box / OSD) ───────────────────────────────── */
    } else if (strncmp(cmd, "display ", 8) == 0) {
        focus_score_set_display_mode(atoi(cmd + 8));
    } else if (strcmp(cmd, "osd on") == 0) {
        focus_score_set_display_mode(2);
    } else if (strcmp(cmd, "osd off") == 0) {
        int mode = focus_score_get_display_mode();
        focus_score_set_display_mode(mode >= 1 ? 1 : 0);
    } else if (strcmp(cmd, "facebox on") == 0) {
        int mode = focus_score_get_display_mode();
        focus_score_set_display_mode(mode >= 1 ? mode : 1);
    } else if (strcmp(cmd, "facebox off") == 0) {
        focus_score_set_display_mode(0);

    /* ── EPTZ (digital pan/tilt/zoom) ────────────────────────────────── */
    } else if (strcmp(cmd, "eptz on") == 0) {
        eptz_enable(1);
    } else if (strcmp(cmd, "eptz off") == 0) {
        eptz_enable(0);
    } else if (strcmp(cmd, "eptz reset") == 0) {
        eptz_reset();
    } else if (strncmp(cmd, "eptz zoom ", 10) == 0) {
        eptz_set_zoom((float)atof(cmd + 10));
    } else if (strncmp(cmd, "eptz dzoom ", 11) == 0) {
        eptz_zoom_delta((float)atof(cmd + 11));
    } else if (strncmp(cmd, "eptz pan ", 9) == 0) {
        eptz_pan((float)atof(cmd + 9));
    } else if (strncmp(cmd, "eptz tilt ", 10) == 0) {
        eptz_tilt((float)atof(cmd + 10));
    } else if (strncmp(cmd, "eptz center ", 12) == 0) {
        float cx = 0.5f, cy = 0.5f;
        if (sscanf(cmd + 12, "%f %f", &cx, &cy) == 2)
            eptz_set_center(cx, cy);

    /* ── Picture quality ─────────────────────────────────────────────── */
    } else if (strncmp(cmd, "contrast ", 9) == 0) {
        rk_isp_set_contrast(s_cam_id, atoi(cmd + 9));
    } else if (strncmp(cmd, "brightness ", 11) == 0) {
        rk_isp_set_brightness(s_cam_id, atoi(cmd + 11));
    } else if (strncmp(cmd, "saturation ", 11) == 0) {
        rk_isp_set_saturation(s_cam_id, atoi(cmd + 11));
    } else if (strncmp(cmd, "hue ", 4) == 0) {
        rk_isp_set_hue(s_cam_id, atoi(cmd + 4));
    } else if (strncmp(cmd, "sharpness ", 10) == 0) {
        rk_isp_set_sharpness(s_cam_id, atoi(cmd + 10));

    /* ── Noise reduction ─────────────────────────────────────────────── */
    } else if (strncmp(cmd, "nr mode ", 8) == 0) {
        /* values: close 2dnr 3dnr mixnr */
        rk_isp_set_noise_reduce_mode(s_cam_id, cmd + 8);
    } else if (strncmp(cmd, "nr spatial ", 11) == 0) {
        rk_isp_set_spatial_denoise_level(s_cam_id, atoi(cmd + 11));
    } else if (strncmp(cmd, "nr temporal ", 12) == 0) {
        rk_isp_set_temporal_denoise_level(s_cam_id, atoi(cmd + 12));

    /* ── Anti-flicker ────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "flicker 50hz") == 0) {
        rk_isp_set_power_line_frequency_mode(s_cam_id, "PAL(50HZ)");
    } else if (strcmp(cmd, "flicker 60hz") == 0) {
        rk_isp_set_power_line_frequency_mode(s_cam_id, "NTSC(60HZ)");

    /* ── BLC / HLC ───────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "blc on") == 0) {
        rk_isp_set_blc_region(s_cam_id, "open");
    } else if (strcmp(cmd, "blc off") == 0) {
        rk_isp_set_blc_region(s_cam_id, "close");
    } else if (strncmp(cmd, "blc level ", 10) == 0) {
        rk_isp_set_blc_strength(s_cam_id, atoi(cmd + 10));
    } else if (strcmp(cmd, "hlc on") == 0) {
        rk_isp_set_hlc(s_cam_id, "open");
    } else if (strcmp(cmd, "hlc off") == 0) {
        rk_isp_set_hlc(s_cam_id, "close");
    } else if (strncmp(cmd, "hlc level ", 10) == 0) {
        rk_isp_set_hlc_level(s_cam_id, atoi(cmd + 10));

    /* ── Image flip / mirror ─────────────────────────────────────────── */
    } else if (strncmp(cmd, "flip ", 5) == 0) {
        /* values: close | mirror | flip | centrosymmetric */
        rk_isp_set_image_flip(s_cam_id, cmd + 5);

    /* ── Day/Night mode ──────────────────────────────────────────────── */
    } else if (strcmp(cmd, "daynight day") == 0) {
        ircut_set(1);
        rk_aiq_uapi2_setColorMode(rkuvc_aiq_get_ctx(s_cam_id), 0);
    } else if (strcmp(cmd, "daynight night") == 0) {
        ircut_set(0);
        rk_aiq_uapi2_setColorMode(rkuvc_aiq_get_ctx(s_cam_id), 1);

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
