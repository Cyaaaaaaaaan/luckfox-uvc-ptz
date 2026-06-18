#include "menu_osd.h"
#include "isp_ipc.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_venc.h"
#include "uvc_log.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── Canvas geometry ───────────────────────────────────────────────────── */
#define MENU_HANDLE   11          /* OVERLAY_RGN handle (5-10 used by focus_score) */
#define MENU_COLS     24          /* characters per row */
#define MENU_CHAR_W   16          /* px per char (8-px glyph × 2× scale) */
#define MENU_ROW_H    16          /* px per row */
#define MENU_N_ITEMS  14
#define MENU_TOTAL_ROWS (1 + MENU_N_ITEMS)   /* title + 14 items = 15 rows */
#define MENU_W        (MENU_COLS  * MENU_CHAR_W)   /* 384 */
#define MENU_H        (MENU_TOTAL_ROWS * MENU_ROW_H)   /* 240 */
#define MENU_POS_X    16
#define MENU_POS_Y    40

/* RGBA8888: (a<<24)|(b<<16)|(g<<8)|r  (same layout as focus_score OSD) */
#define MRGBA(r,g,b,a) ((uint32_t)((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(g)<<8|(r)))
#define COL_TRANSP   MRGBA(0x00,0x00,0x00,0x00)  /* invisible            */
#define COL_TITLE_BG MRGBA(0x20,0x20,0xA0,0xFF)  /* dark blue            */
#define COL_TITLE_FG MRGBA(0xFF,0xFF,0xFF,0xFF)  /* white                */
#define COL_NORM_BG  MRGBA(0x18,0x18,0x18,0xEC)  /* near-opaque dark     */
#define COL_NORM_FG  MRGBA(0xFF,0xFF,0xFF,0xFF)  /* white                */
#define COL_SEL_BG   MRGBA(0xFF,0xFF,0xFF,0xFF)  /* white highlight      */
#define COL_SEL_FG   MRGBA(0x00,0x00,0x00,0xFF)  /* black text           */

/* ── 8×8 bitmap font, ASCII 32–90 (index = char - 32) ─────────────────── */
static const uint8_t g_font[59][8] = {
    /* 32 ' ' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 33 '!' */{ 0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00 },
    /* 34 '"' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 35 '#' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 36 '$' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 37 '%' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 38 '&' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 39 '\''*/{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 40 '(' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 41 ')' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 42 '*' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 43 '+' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 44 ',' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 45 '-' */{ 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 },
    /* 46 '.' */{ 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 },
    /* 47 '/' */{ 0x00,0x06,0x0C,0x18,0x30,0x60,0x00,0x00 },
    /* 48 '0' */{ 0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00 },
    /* 49 '1' */{ 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 },
    /* 50 '2' */{ 0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00 },
    /* 51 '3' */{ 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 },
    /* 52 '4' */{ 0x06,0x0E,0x1E,0x66,0x7F,0x06,0x06,0x00 },
    /* 53 '5' */{ 0x7F,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 },
    /* 54 '6' */{ 0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00 },
    /* 55 '7' */{ 0x7F,0x66,0x0C,0x18,0x18,0x18,0x18,0x00 },
    /* 56 '8' */{ 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 },
    /* 57 '9' */{ 0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00 },
    /* 58 ':' */{ 0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00 },
    /* 59 ';' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 60 '<' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 61 '=' */{ 0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00 },
    /* 62 '>' */{ 0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00 },
    /* 63 '?' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 64 '@' */{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 65 'A' */{ 0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 },
    /* 66 'B' */{ 0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00 },
    /* 67 'C' */{ 0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00 },
    /* 68 'D' */{ 0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00 },
    /* 69 'E' */{ 0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00 },
    /* 70 'F' */{ 0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00 },
    /* 71 'G' */{ 0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00 },
    /* 72 'H' */{ 0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 },
    /* 73 'I' */{ 0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00 },
    /* 74 'J' */{ 0x1E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00 },
    /* 75 'K' */{ 0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00 },
    /* 76 'L' */{ 0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00 },
    /* 77 'M' */{ 0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00 },
    /* 78 'N' */{ 0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00 },
    /* 79 'O' */{ 0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },
    /* 80 'P' */{ 0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00 },
    /* 81 'Q' */{ 0x3C,0x66,0x66,0x66,0x6E,0x3C,0x0E,0x00 },
    /* 82 'R' */{ 0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00 },
    /* 83 'S' */{ 0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00 },
    /* 84 'T' */{ 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 },
    /* 85 'U' */{ 0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },
    /* 86 'V' */{ 0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00 },
    /* 87 'W' */{ 0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00 },
    /* 88 'X' */{ 0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00 },
    /* 89 'Y' */{ 0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00 },
    /* 90 'Z' */{ 0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00 },
};

/* ── Menu items ────────────────────────────────────────────────────────── */
typedef enum {
    MI_AE_MODE = 0, MI_BRIGHTNESS, MI_CONTRAST, MI_SATURATION,
    MI_SHARPNESS,   MI_WB_MODE,    MI_NR_LEVEL,  MI_BLC,
    MI_HLC,         MI_FLICKER,    MI_MIRROR,    MI_DAYNIGHT,
    MI_EPTZ,        MI_AUTOTRACK,
    MI_COUNT
} MenuItemId;

static const char *s_labels[MI_COUNT] = {
    "AE MODE",  "BRIGHTNESS", "CONTRAST",  "SATURATION",
    "SHARPNESS","WB MODE",    "NR LEVEL",  "BLC",
    "HLC",      "ANTI-FLICKER","MIRROR",   "DAY/NIGHT",
    "EPTZ",     "AUTO-TRACK",
};

/* max value for each item (min always 0) */
static const int s_max[MI_COUNT]  = { 1,  255, 255, 255, 15, 7,  5, 1, 1, 1, 1, 1, 1, 1 };
/* increment step */
static const int s_step[MI_COUNT] = { 1,    8,   8,   8,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1 };
/* wrap-around (1) vs clamp (0) */
static const int s_wrap[MI_COUNT] = { 1,    0,   0,   0,  0, 1,  1, 1, 1, 1, 1, 1, 1, 1 };

/* current values (defaults match visca.c shadow state) */
static int s_values[MI_COUNT] = { 0, 128, 128, 128, 8, 0, 3, 0, 0, 0, 0, 0, 0, 0 };

static int s_cursor   = 0;
static int s_menu_open  = 0;
static int s_menu_ready = 0;
static int s_vdev = 0, s_vchn = 0;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void menu_value_str(int item, char *buf, int bufsz)
{
    int v = s_values[item];
    switch ((MenuItemId)item) {
    case MI_AE_MODE:
        snprintf(buf, (size_t)bufsz, "%s", v ? "MANUAL" : "AUTO"); break;
    case MI_BRIGHTNESS: case MI_CONTRAST: case MI_SATURATION:
        snprintf(buf, (size_t)bufsz, "%d", v); break;
    case MI_SHARPNESS:
        snprintf(buf, (size_t)bufsz, "%d", v); break;
    case MI_WB_MODE: {
        const char *wb[] = {"AUTO","INDOOR","OUTDOOR","FLUORESC","INCAN","WARM","NATURAL","LOCK"};
        snprintf(buf, (size_t)bufsz, "%s", wb[v]); break;
    }
    case MI_NR_LEVEL:
        if (v == 0) snprintf(buf, (size_t)bufsz, "OFF");
        else        snprintf(buf, (size_t)bufsz, "LVL %d", v);
        break;
    case MI_BLC: case MI_HLC: case MI_MIRROR:
    case MI_EPTZ: case MI_AUTOTRACK:
        snprintf(buf, (size_t)bufsz, "%s", v ? "ON" : "OFF"); break;
    case MI_FLICKER:
        snprintf(buf, (size_t)bufsz, "%s", v ? "60HZ" : "50HZ"); break;
    case MI_DAYNIGHT:
        snprintf(buf, (size_t)bufsz, "%s", v ? "NIGHT" : "DAY"); break;
    default:
        snprintf(buf, (size_t)bufsz, "%d", v); break;
    }
}

static void menu_apply(int item)
{
    char cmd[64];
    int v = s_values[item];
    switch ((MenuItemId)item) {
    case MI_AE_MODE:
        isp_apply_cmd(v ? "ae manual" : "ae auto"); break;
    case MI_BRIGHTNESS:
        snprintf(cmd, sizeof(cmd), "brightness %d", v);
        isp_apply_cmd(cmd); break;
    case MI_CONTRAST:
        snprintf(cmd, sizeof(cmd), "contrast %d", v);
        isp_apply_cmd(cmd); break;
    case MI_SATURATION:
        snprintf(cmd, sizeof(cmd), "saturation %d", v);
        isp_apply_cmd(cmd); break;
    case MI_SHARPNESS:
        snprintf(cmd, sizeof(cmd), "sharpness %d", (v * 100) / 15);
        isp_apply_cmd(cmd); break;
    case MI_WB_MODE: {
        const char *wb_cmds[] = {
            "wb auto","wb indoor","wb outdoor",
            "wb fluorescent","wb incandescent","wb warm","wb natural","wb lock"
        };
        isp_apply_cmd(wb_cmds[v]); break;
    }
    case MI_NR_LEVEL:
        if (v == 0) {
            isp_apply_cmd("nr mode close");
        } else {
            isp_apply_cmd("nr mode mixnr");
            snprintf(cmd, sizeof(cmd), "nr spatial %d", v * 20);
            isp_apply_cmd(cmd);
            snprintf(cmd, sizeof(cmd), "nr temporal %d", v * 20);
            isp_apply_cmd(cmd);
        }
        break;
    case MI_BLC:
        isp_apply_cmd(v ? "blc on" : "blc off"); break;
    case MI_HLC:
        isp_apply_cmd(v ? "hlc on" : "hlc off"); break;
    case MI_FLICKER:
        isp_apply_cmd(v ? "flicker 60hz" : "flicker 50hz"); break;
    case MI_MIRROR:
        isp_apply_cmd(v ? "flip mirror" : "flip close"); break;
    case MI_DAYNIGHT:
        isp_apply_cmd(v ? "daynight night" : "daynight day"); break;
    case MI_EPTZ:
        isp_apply_cmd(v ? "eptz on" : "eptz off"); break;
    case MI_AUTOTRACK:
        isp_apply_cmd(v ? "autotrack on" : "autotrack off"); break;
    default: break;
    }
}

/* ── Rendering ─────────────────────────────────────────────────────────── */

static void menu_draw_char(uint32_t *pixels, int canvas_w,
                            int x, int y, char c, uint32_t fg, uint32_t bg)
{
    int idx = (int)(unsigned char)c - 32;
    if (idx < 0 || idx >= 59) return;
    const uint8_t *glyph = g_font[idx];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint32_t color = (glyph[row] & (0x80u >> col)) ? fg : bg;
            int px = x + col * 2;
            int py = y + row * 2;
            /* 2× scale — fill 2×2 block */
            pixels[ py      * canvas_w + px    ] = color;
            pixels[ py      * canvas_w + px + 1] = color;
            pixels[(py + 1) * canvas_w + px    ] = color;
            pixels[(py + 1) * canvas_w + px + 1] = color;
        }
    }
}

static void menu_draw_row(uint32_t *pixels, int canvas_w,
                           int row_idx, const char *text,
                           uint32_t fg, uint32_t bg)
{
    int y0 = row_idx * MENU_ROW_H;
    /* Background fill */
    for (int py = y0; py < y0 + MENU_ROW_H; py++)
        for (int px = 0; px < MENU_W; px++)
            pixels[py * canvas_w + px] = bg;
    /* Characters */
    for (int i = 0; i < MENU_COLS && text[i]; i++)
        menu_draw_char(pixels, canvas_w, i * MENU_CHAR_W, y0, text[i], fg, bg);
}

static void menu_render(void)
{
    if (!s_menu_ready) return;

    RGN_CANVAS_INFO_S canvas;
    memset(&canvas, 0, sizeof(canvas));
    if (RK_MPI_RGN_GetCanvasInfo(MENU_HANDLE, &canvas) != RK_SUCCESS) return;

    uint32_t *pixels = (uint32_t *)(uintptr_t)canvas.u64VirAddr;
    int cw = (int)canvas.u32VirWidth;

    if (!s_menu_open) {
        for (int i = 0; i < cw * MENU_H; i++) pixels[i] = COL_TRANSP;
        RK_MPI_RGN_UpdateCanvas(MENU_HANDLE);
        return;
    }

    /* Title row */
    char trow[25];
    snprintf(trow, sizeof(trow), "%-24s", "    CAMERA MENU");
    menu_draw_row(pixels, cw, 0, trow, COL_TITLE_FG, COL_TITLE_BG);

    /* Item rows */
    for (int i = 0; i < MI_COUNT; i++) {
        char vbuf[12];
        menu_value_str(i, vbuf, sizeof(vbuf));
        char row[25];
        snprintf(row, sizeof(row), "%c%-12s: %-9s",
                 (i == s_cursor) ? '>' : ' ',
                 s_labels[i], vbuf);
        uint32_t fg = (i == s_cursor) ? COL_SEL_FG : COL_NORM_FG;
        uint32_t bg = (i == s_cursor) ? COL_SEL_BG : COL_NORM_BG;
        menu_draw_row(pixels, cw, i + 1, row, fg, bg);
    }

    RK_MPI_RGN_UpdateCanvas(MENU_HANDLE);
}

/* ── Public API ────────────────────────────────────────────────────────── */

void menu_osd_attach(int venc_dev, int venc_chn)
{
    s_menu_ready = 0;
    s_vdev = venc_dev;
    s_vchn = venc_chn;

    RGN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.enType                            = OVERLAY_RGN;
    attr.unAttr.stOverlay.enPixelFmt      = RK_FMT_RGBA8888;
    attr.unAttr.stOverlay.stSize.u32Width  = MENU_W;
    attr.unAttr.stOverlay.stSize.u32Height = MENU_H;

    if (RK_MPI_RGN_Create(MENU_HANDLE, &attr) != RK_SUCCESS) {
        LOG_WARN("menu_osd: RGN_Create(%dx%d) failed\n", MENU_W, MENU_H);
        return;
    }

    MPP_CHN_S chn = { RK_ID_VENC, venc_dev, venc_chn };

    RGN_CHN_ATTR_S ca;
    memset(&ca, 0, sizeof(ca));
    ca.bShow                                = RK_TRUE;
    ca.enType                               = OVERLAY_RGN;
    ca.unChnAttr.stOverlayChn.stPoint.s32X = MENU_POS_X;
    ca.unChnAttr.stOverlayChn.stPoint.s32Y = MENU_POS_Y;
    ca.unChnAttr.stOverlayChn.u32BgAlpha   = 0;
    ca.unChnAttr.stOverlayChn.u32FgAlpha   = 255;
    ca.unChnAttr.stOverlayChn.u32Layer     = 6;

    if (RK_MPI_RGN_AttachToChn(MENU_HANDLE, &chn, &ca) != RK_SUCCESS) {
        LOG_WARN("menu_osd: AttachToChn VENC(%d,%d) failed\n", venc_dev, venc_chn);
        RK_MPI_RGN_Destroy(MENU_HANDLE);
        return;
    }

    s_menu_ready = 1;
    LOG_INFO("menu_osd: attached to VENC(%d,%d) canvas %dx%d\n",
             venc_dev, venc_chn, MENU_W, MENU_H);

    /* Re-render (restores open state after resolution switch) */
    menu_render();
}

void menu_osd_detach(int venc_dev, int venc_chn)
{
    if (!s_menu_ready) return;
    s_menu_ready = 0;
    MPP_CHN_S chn = { RK_ID_VENC, venc_dev, venc_chn };
    RK_MPI_RGN_DetachFromChn(MENU_HANDLE, &chn);
    RK_MPI_RGN_Destroy(MENU_HANDLE);
    LOG_INFO("menu_osd: detached\n");
}

void menu_osd_open(void)
{
    s_menu_open = 1;
    menu_render();
    printf("[menu] opened (cursor=%d)\n", s_cursor);
}

void menu_osd_close(void)
{
    s_menu_open = 0;
    menu_render();
    printf("[menu] closed\n");
}

int menu_osd_is_open(void) { return s_menu_open; }

void menu_osd_cursor_up(void)
{
    if (--s_cursor < 0) s_cursor = MI_COUNT - 1;
    menu_render();
    printf("[menu] cursor -> %d (%s)\n", s_cursor, s_labels[s_cursor]);
}

void menu_osd_cursor_down(void)
{
    if (++s_cursor >= MI_COUNT) s_cursor = 0;
    menu_render();
    printf("[menu] cursor -> %d (%s)\n", s_cursor, s_labels[s_cursor]);
}

static void menu_change_value(int delta)
{
    int v  = s_values[s_cursor];
    int mx = s_max[s_cursor];
    int st = s_step[s_cursor];

    if (s_wrap[s_cursor]) {
        v = (v + mx + 1 + delta) % (mx + 1);
    } else {
        v += delta * st;
        if (v < 0)  v = 0;
        if (v > mx) v = mx;
    }
    s_values[s_cursor] = v;
    menu_apply(s_cursor);
    menu_render();
    char vbuf[12];
    menu_value_str(s_cursor, vbuf, sizeof(vbuf));
    printf("[menu] %s = %s\n", s_labels[s_cursor], vbuf);
}

void menu_osd_value_inc(void) { menu_change_value(+1); }
void menu_osd_value_dec(void) { menu_change_value(-1); }
