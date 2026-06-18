# luckfox-uvc-ptz

Patches and build scripts for running the Luckfox Pico Pro Max as a USB UVC camera (MJPEG / H.264 / H.265), with NPU-based face detection, autofocus scoring, and VISCA-over-IP PTZ control.

Fixes resolution switching, removes YUYV, enforces 4:3-only resolutions, sets up reliable boot autostart without rkipc, adds real-time face detection using the onboard RKNN NPU, and listens for PTZ commands from commercial PTZ controllers via VISCA-over-IP.

## Hardware

| Item | Value |
|------|-------|
| Board | Luckfox Pico Pro Max |
| SoC | Rockchip RV1106 (includes RKNN NPU) |
| Sensor | MIS5001 (5 MP, 2592×1944 native) |
| USB | USB 2.0 High-Speed (UVC gadget via configfs) |
| Network | 100 Mbps Ethernet (VISCA-over-IP) |

## Repository layout

```
apply_patches.sh       — idempotent SDK patcher (patches 1–10)
build-app.sh           — build rk_mpi_uvc + visca_server, print deploy commands
RkLunch.sh             — boot launcher (replaces factory rkipc launcher)
files/
  uvc/                 — new source files injected by patches 7–10
    focus_score.cpp    — RKNN face detection + hardware ISP focus scorer
    focus_score.h
    rknn_api.h
    rknn_box_priors.h  — RetinaFace 640×640 prior anchors
    isp_ipc.cpp        — Unix socket IPC server (AE/WB/display/EPTZ cmds from visca_server)
    isp_ipc.h
    eptz.cpp           — digital pan/tilt/zoom via RK_MPI_VI_SetEptz
    eptz.h
  models/
    retinaface.rknn    — bundled RetinaFace model (637 KB)
patches/               — reference diffs for patches 1–5
visca/                 — VISCA-over-IP PTZ server
  main.c               — UDP socket, signal handling
  visca.c              — KC2000 packet parser + dispatcher
  motor.c/h            — motor control layer (stub — replace with real GPIO/UART)
  isp_ctrl.c/h         — Unix socket IPC client (sends AE/WB cmds to rk_mpi_uvc)
  CMakeLists.txt
```

## What the patches do

Patches are applied by `apply_patches.sh` (idempotent — safe to re-run).

| Patch | File(s) | Description |
|-------|---------|-------------|
| 1 | `uvc_mpi_vi.cpp` | VI start is idempotent — safe to call multiple times without restarting the ISP |
| 2 | `uvc_process.cpp` | VENC restart on resolution change without tearing down the pipeline |
| 3 | `camera_control.c` | ISP keepalive — prevents AIQ SOF disorder on RV1106 after resolution switch |
| 4 | `usb_config.sh` | USB gadget config: 4:3 resolutions only, YUYV removed, MJPEG + H.264 + H.265 |
| 5 | `isp.c` | Silences spurious insmod/mkdir/udev stderr noise |
| 6 | `uvc_mpi_config.c` | Sets native 2592×1944 max size, disables VPSS (incompatible on RV1106 at full res) |
| 7 | `focus_score.cpp` + wiring | RKNN RetinaFace face detection, Tenengrad focus scoring, green OSD bounding box |
| 8 | `isp_ipc.cpp` + wiring | Unix socket IPC server — receives AE/WB commands from `visca_server` and applies them via rkaiq |
| 9 | `focus_score.cpp` + `isp.h` | Replace CPU Tenengrad with hardware ISP sharpness stats via `rk_aiq_user_api2_af_GetSearchPath()` — zero CPU cost, updates every frame |
| 10 | `eptz.cpp` + wiring | Digital pan/tilt/zoom via `RK_MPI_VI_SetEptz` (crop+scale of the sensor frame), driven by VISCA — motorless PTZ |

**Key VI fix:** `RK_MPI_VI_DisableChn` on RV1106 triggers a full ISP restart → AIQ detects SOF disorder (frame counter reset) → VI permanently stops delivering frames. These patches replace DisableChn with in-place `RK_MPI_VI_SetChnAttr`, requiring `stIspOpt.stMaxSize` to be set to the sensor native max (2592×1944).

**Supported resolutions:** 640×480 · 1024×768 · 1280×960 · 2048×1536 · 2592×1944

## Face detection & focus scoring (Patches 7 & 9)

Always-on face detection using the RKNN NPU, feeding a hardware-computed focus score:

- **Model:** RetinaFace (640×640 BGR, 16800 prior anchors) — loaded from `/oem/usr/share/models/retinaface.rknn`
- **Pipeline:** NV12 frame → 4× integer downsample → 640×640 letterbox BGR → RKNN inference → decode anchors → select most-centred face above 0.5 confidence
- **Focus scoring (Patch 9):** Uses the **hardware ISP sharpness statistic** via `rk_aiq_user_api2_af_GetSearchPath()` — zero CPU cost, updated every frame. After each detection the ISP AF measurement window (`h_offs/v_offs/h_size/v_size`) is set to the face bbox via `rk_aiq_user_api2_af_SetAttrib()`, so the score reflects only the face region. With no face the window resets to full frame. Falls back to a CPU Tenengrad crop only if the rkaiq context is unavailable.
  - Typical range: **1–10** fully blurred, **90–150+** sharp on a face — a clean monotonic peak for hill-climbing autofocus.
  - Without the face-aware window, busy backgrounds (e.g. patterned bedsheets) scored 400+ regardless of subject focus; the windowed approach fixes this.
- **OSD green box:** Drawn as four thin OVERLAY_RGN strips (one per edge). Full-frame overlay and COVER_RGN are non-functional at 2592×1944 on this SDK — per-edge strips work reliably.
- **Detection rate:** Runs every scoring frame (~500 ms update)

### Display modes (debug overlays)

The score OSD and green face box are **off by default** (mode 0) — production streams are clean. Three verbosity levels, cycled by the KC2000 menu button or set via IPC:

| Mode | Face box | Score OSD |
|------|----------|-----------|
| 0 (default) | off | off |
| 1 | on | off |
| 2 | on | on |

The score is always written to `/tmp/focus_score` regardless of display mode (`watch -n0.5 cat /tmp/focus_score`).

## VISCA-over-IP PTZ control

`visca_server` runs as a background process alongside `rk_mpi_uvc` and listens for commands from any commercial PTZ controller.

- **Protocol:** VISCA over UDP, port 1259 (PTZOptics convention)
- **Tested controller:** SMTAV KC2000
- **Supported commands:**

| Command | VISCA bytes | Action |
|---------|------------|--------|
| Pan left/right | `81 01 06 01 VV WW XX YY FF` | `motor_pan()` + EPTZ pan |
| Tilt up/down | `81 01 06 01 VV WW XX YY FF` | `motor_tilt()` + EPTZ tilt |
| Pan/tilt stop | `81 01 06 01 00 00 03 03 FF` | stop both (EPTZ holds) |
| Zoom in/out | `81 01 04 07 pp FF` | `motor_zoom()` + EPTZ zoom |
| Zoom stop | `81 01 04 07 00 FF` | stop zoom (EPTZ holds) |
| Focus near/far | `81 01 04 08 pp FF` | `motor_focus(dir, speed)` |
| Focus mode | `81 01 04 38 03 FF` | ACK only |
| AE mode | `81 01 04 39 pp FF` | 00=auto, 03/0A/0B=manual → ISP exposure mode |
| WB mode | `81 01 04 35 pp FF` | 00=auto, 01=indoor(3200K), 02=outdoor(5800K), 05=manual, 06=fluorescent, 07=incandescent, 08=warm, 09=natural, 0A=lock |
| Sharpness | `81 01 04 A2 pp FF` | 02=up, 03=down, 00=reset → ISP sharpness [0-100] |
| Sharpness direct | `81 01 04 42 0p 0q 0r 0s FF` | direct 4-bit value → ISP sharpness |
| Picture effect | `81 01 04 3D pp FF` | 00=color (day), 04=B&W (night) → ISP color mode + IRCut GPIO |
| Backlight comp. | `81 01 04 33 pp FF` | 02=BLC on, 03=BLC off |
| Mirror (LR flip) | `81 01 04 61 pp FF` | 02=mirror on, 03=mirror off → hardware ISP flip |
| Anti-flicker | `81 01 04 23 pp FF` | 00=off→50Hz, 01=50Hz, 02=60Hz |
| Noise reduction | `81 01 04 53 pp FF` | 00=off, 01-05=NR levels → mixnr + spatial/temporal |
| CAM menu button | `81 01 06 06 pp FF` | cycles debug display mode 0→1→2→0 |
| Memory recall | `81 01 04 3F 02 pp FF` | ACK only |

Responses: `90 41 FF` (ACK) + `90 51 FF` (completion) per command.
Inquiries (`81 09 ...`): shadow state maintained for AE/WB/sharpness/picture-effect/backlight/mirror/flicker/NR.

The motor layer (`visca/motor.c`) is currently stubbed — it prints commands to stdout. Replace the function bodies with real TMC2209 UART and DRV8833 PWM calls when hardware is connected.

### AE/WB IPC architecture

`visca_server` and `rk_mpi_uvc` are separate processes. rkaiq (the ISP library) is a singleton that must be owned by one process — `rk_mpi_uvc` owns it. When the PTZ controller sends an exposure or white-balance command, `visca_server` forwards it as a text datagram over a Unix socket (`/tmp/visca_isp.sock`):

```
KC2000 ──UDP:1259──▶ visca_server ──Unix DGRAM──▶ rk_mpi_uvc
                        (client)    /tmp/visca_isp.sock  (server → rkaiq)
```

Text command protocol (one per datagram):
```
# Exposure / White balance
ae auto | manual               — ISP AE mode
wb auto | indoor | outdoor | manual | fluorescent | incandescent | warm | natural | lock
wb ct <kelvin>                 — manual WB, 2000–10000 K

# Picture quality
contrast <0-255>               — ISP contrast
brightness <0-255>             — ISP brightness
saturation <0-255>             — ISP saturation
hue <0-255>                    — ISP hue
sharpness <0-100>              — ISP sharpness (hardware)

# Noise reduction
nr mode close|2dnr|3dnr|mixnr  — NR algorithm
nr spatial <0-100>             — spatial NR strength
nr temporal <0-100>            — temporal NR strength

# Anti-flicker
flicker 50hz | 60hz            — power line frequency

# Backlight / Highlight compensation
blc on | off                   — backlight compensation
blc level <1-100>              — BLC strength
hlc on | off                   — highlight compensation
hlc level <1-100>              — HLC level

# Image geometry
flip close|mirror|flip|centrosymmetric — hardware ISP mirror/flip (zero CPU)

# Day/Night mode
daynight day                   — color mode + insert IR-cut filter (GPIO 36)
daynight night                 — B&W mode + remove IR-cut filter (GPIO 35)

# Debug overlays
display <0|1|2>                — verbosity (0=off, 1=box, 2=box+score)
osd on | off
facebox on | off

# Digital PTZ
eptz on | off                  — enable/disable digital PTZ
eptz reset                     — recenter, zoom = 1.0
eptz zoom <f>                  — absolute zoom (1.0 = full frame .. 8.0)
eptz dzoom <f>                 — relative zoom step
eptz pan <f>                   — relative crop-center move (+ = right)
eptz tilt <f>                  — relative crop-center move (+ = down)
eptz center <cx> <cy>          — absolute center, normalized 0..1

# Face auto-tracking
autotrack on | off             — P-controller: nudges EPTZ center to keep face centered
                                 (also enables EPTZ automatically when turned on)
```

### EPTZ — motorless digital pan/tilt/zoom (Patch 10)

`RK_MPI_VI_SetEptz` crops a rectangle from the native 2592×1944 sensor frame and
scales it to the output — shrink the rect to zoom in, move it to pan/tilt. It is
a live operation (no channel teardown) and **off by default**. The VISCA zoom
(`04 07`) and pan/tilt (`06 01`) handlers send EPTZ deltas over the IPC socket,
so once enabled the KC2000 drives digital PTZ with **no motor hardware**.

Lossless digital zoom depends on output resolution (crop ≥ output): up to ~4×
at 640×480, ~2× at 1280×960, 1× at full 2592×1944. EPTZ state is normalized
(center + zoom) so it survives resolution switches; it is re-applied after each
in-place resize.

Enable and test directly from the board (no controller needed):
```sh
S=/tmp/visca_isp.sock
send(){ python3 -c "import socket,sys;s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);s.connect('$S');s.send(sys.argv[1].encode())" "$1"; }
send "eptz on"; send "eptz zoom 2.0"; send "eptz pan 0.1"; send "eptz reset"; send "eptz off"
```

> EPTZ is untested on hardware as of this commit — verify the crop+scale renders with
> VPSS disabled (Patch 6) at the native 2592 config before relying on it.

### Face auto-tracking (Phase 0.5)

When enabled, the focus-score thread runs a proportional controller after each
face detection (~500 ms cadence) that nudges the EPTZ crop center to keep the
detected face centered in frame.  **No new thread** — runs inside the existing
face detection cycle.

Algorithm:
- Face center from bbox (10000-scale coords): `fcx = (x0+x1)/2 / 10000`
- Error: `ex = fcx - 0.5, ey = fcy - 0.5` (both in [−0.5, 0.5])
- Dead-zone: ±4% of frame width/height — no movement if face is already near center
- EPTZ adjustment: `eptz_pan(ex × 0.35)`, `eptz_tilt(ey × 0.35)` per detection cycle
- Enabling autotrack automatically enables EPTZ; disabling leaves EPTZ at current position

Enable via IPC:
```sh
S=/tmp/visca_isp.sock
send(){ python3 -c "import socket,sys;s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);s.connect('$S');s.send(sys.argv[1].encode())" "$1"; }
send "autotrack on"   # EPTZ activates + face tracking starts
send "autotrack off"  # tracking stops, EPTZ holds current position
send "eptz reset"     # optionally recenter after disabling
```

These IPC commands can also be sent directly from the board for testing:
```sh
python3 -c "import socket; s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); s.connect('/tmp/visca_isp.sock'); s.send(b'display 2')"
```

### Testing without hardware

Run `visca_server` over SSH and watch the output:

```sh
ssh root@$PICO_IP /oem/usr/bin/visca_server
# move the KC2000 joystick — output appears immediately:
# [visca] rx 9 bytes: 81 01 06 01 01 00 01 03 ff
# [motor] pan  dir=-1 speed=1
```

Or test on your PC without the board:

```sh
cd ~/luckfox-uvc-ptz/visca
gcc -o visca_server_test main.c visca.c motor.c
./visca_server_test &
# send a pan-left packet:
echo -ne '\x81\x01\x06\x01\x01\x00\x01\x03\xff' | nc -u -q1 127.0.0.1 1259
```

## Prerequisites

- Luckfox Pico SDK cloned at `~/luckfox-pico` (or set `SDK_DIR`)
  ```sh
  git clone https://github.com/LuckfoxTECH/luckfox-pico.git ~/luckfox-pico
  cd ~/luckfox-pico && git lfs pull
  ./build.sh lunch   # select Luckfox Pico Pro Max
  ```
- ARM cross-toolchain (included in the SDK via git-lfs)
- Board flashed with factory firmware (the `/oem` partition must exist)
- SSH access to the board
- **RetinaFace model** — bundled at `files/models/retinaface.rknn` (637 KB), no separate download needed

## Build

```sh
git clone https://github.com/Cyaaaaaaaaan/luckfox-uvc-ptz.git ~/luckfox-uvc-ptz
cd ~/luckfox-uvc-ptz

# Build everything (default)
bash build-app.sh

# Build specific targets (comma-separated or single)
bash build-app.sh --target uvc            # only rk_mpi_uvc (full SDK build)
bash build-app.sh --target visca          # only visca_server (fast, cmake only)
bash build-app.sh --target scripts        # only RkLunch.sh / usb_config.sh / rkuvc.ini
bash build-app.sh --target model          # only retinaface.rknn
bash build-app.sh --target uvc,visca      # uvc + visca, skip scripts and model

# Other options
bash build-app.sh --target uvc --skip-patches   # skip apply_patches.sh
bash build-app.sh --target all --clean          # clean before build
bash build-app.sh --sdk /path/to/luckfox-pico   # custom SDK path
```

After a successful build, the script prints exact `scp`/`ssh` deploy commands — only for the targets that were built. `scripts` and `model` targets need no compilation and skip the toolchain check entirely.

## Deploy

`/oem` is a read-only squashfs partition. Stage everything through `/tmp` first, then remount and install in a single SSH call.

```sh
export PICO_IP=<board IP>        # e.g. 192.168.1.149

# Paths (adjust if your SDK/repo are elsewhere)
UVC_BIN=~/luckfox-pico/project/app/uvc_app_tiny/build/rk_mpi_uvc
VISCA_BIN=~/luckfox-uvc-ptz/visca/build/visca_server
UVC_SRC=~/luckfox-pico/project/app/uvc_app_tiny/uvc_app
RETINA_MODEL=~/luckfox-uvc-ptz/files/models/retinaface.rknn

# 0. One-time: back up the factory launcher
ssh root@$PICO_IP "cp /oem/usr/bin/RkLunch.sh /oem/usr/bin/RkLunch.sh.factory"

# 1. One-time: disable S99usb0config
#    It spawns a usb_reset loop that wipes the UVC gadget configfs setup.
ssh root@$PICO_IP "chmod -x /etc/init.d/S99usb0config"

# 2. Stage files to /tmp
scp "$UVC_BIN"                    root@$PICO_IP:/tmp/rk_mpi_uvc
scp "$VISCA_BIN"                  root@$PICO_IP:/tmp/visca_server
scp "$UVC_SRC/usb_config.sh"     root@$PICO_IP:/tmp/usb_config.sh
scp "$UVC_SRC/rkuvc.ini"         root@$PICO_IP:/tmp/rkuvc.ini.new
scp ~/luckfox-uvc-ptz/RkLunch.sh root@$PICO_IP:/tmp/RkLunch.sh
scp "$RETINA_MODEL"               root@$PICO_IP:/tmp/retinaface.rknn

# 3. Kill binaries, remount /oem rw, install everything, reboot
ssh root@$PICO_IP \
  "killall -9 rk_mpi_uvc visca_server 2>/dev/null; mount -o remount,rw /oem && \
    cp /tmp/rk_mpi_uvc      /oem/usr/bin/rk_mpi_uvc && \
    cp /tmp/visca_server    /oem/usr/bin/visca_server && \
    cp /tmp/usb_config.sh   /oem/usr/bin/usb_config.sh && \
    cp /tmp/rkuvc.ini.new   /oem/usr/share/rkuvc.ini && \
    cp /tmp/RkLunch.sh      /oem/usr/bin/RkLunch.sh && \
    mkdir -p /oem/usr/share/models && \
    cp /tmp/retinaface.rknn /oem/usr/share/models/ && \
    chmod +x /oem/usr/bin/rk_mpi_uvc /oem/usr/bin/visca_server \
             /oem/usr/bin/usb_config.sh /oem/usr/bin/RkLunch.sh && \
    reboot"
```

## Boot flow

```
inittab → rcS → S99test → /usr/bin/t → /oem/usr/bin/RkLunch.sh
                                               │
                                               ├─ kills rkipc, usb_reset
                                               ├─ network_init (Ethernet / DHCP)
                                               ├─ usb_config.sh  (UVC gadget)
                                               ├─ visca_server   (UDP :1259, watchdog)
                                               └─ rk_mpi_uvc     (UVC stream, watchdog)
                                                       │
                                                       ├─ face_det_init()  (loads retinaface.rknn)
                                                       ├─ focus scoring thread (Tenengrad sharpness)
                                                       └─ OSD: green box overlay on detected face
```

`RkLunch.sh` watchdogs both `rk_mpi_uvc` and `visca_server` — either process is restarted automatically if it crashes.

## Rolling back

```sh
# List available versions
git tag

# Switch to a previous version
git checkout v1.0-working-boot

# Rebuild and redeploy
bash build-app.sh
```

To restore the factory launcher on the board:

```sh
ssh root@$PICO_IP "cp /oem/usr/bin/RkLunch.sh.factory /oem/usr/bin/RkLunch.sh && reboot"
```
