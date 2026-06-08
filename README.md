# luckfox-uvc-ptz

Patches and build scripts for running the Luckfox Pico Pro Max as a USB UVC camera (MJPEG / H.264 / H.265), with NPU-based face detection and autofocus scoring.

Fixes resolution switching, removes YUYV, enforces 4:3-only resolutions, sets up reliable boot autostart without rkipc, and adds real-time face detection using the onboard RKNN NPU.

## Hardware

| Item | Value |
|------|-------|
| Board | Luckfox Pico Pro Max |
| SoC | Rockchip RV1106 (includes RKNN NPU) |
| Sensor | MIS5001 (5 MP, 2592×1944 native) |
| USB | USB 2.0 High-Speed (UVC gadget via configfs) |

## What the patches do

Patches are applied by `apply_patches.sh` (idempotent — safe to re-run). The `patches/` directory holds reference diffs for patches 1–5; patches 6–7 are self-contained in `apply_patches.sh` and `files/`.

| Patch | File(s) | Description |
|-------|---------|-------------|
| 1 | `uvc_mpi_vi.cpp` | VI start is idempotent — safe to call multiple times without restarting the ISP |
| 2 | `uvc_process.cpp` | VENC restart on resolution change without tearing down the pipeline |
| 3 | `camera_control.c` | ISP keepalive — prevents AIQ SOF disorder on RV1106 after resolution switch |
| 4 | `usb_config.sh` | USB gadget config: 4:3 resolutions only, YUYV removed, MJPEG + H.264 + H.265 |
| 5 | `isp.c` | Silences spurious insmod/mkdir/udev stderr noise |
| 6 | `uvc_mpi_config.c` | Sets native 2592×1944 max size, disables VPSS (incompatible on RV1106 at full res) |
| 7 | `focus_score.cpp` + wiring | RKNN RetinaFace face detection, Tenengrad focus scoring, green OSD bounding box |

**Key VI fix:** `RK_MPI_VI_DisableChn` on RV1106 triggers a full ISP restart → AIQ detects SOF disorder (frame counter reset) → VI permanently stops delivering frames. These patches replace DisableChn with in-place `RK_MPI_VI_SetChnAttr`, requiring `stIspOpt.stMaxSize` to be set to the sensor native max (2592×1944).

**Supported resolutions:** 640×480 · 1024×768 · 1280×960 · 2048×1536 · 2592×1944

## Face detection (Patch 7)

Patch 7 adds always-on face detection using the RKNN NPU:

- **Model:** RetinaFace (640×640 BGR, 16800 prior anchors) — loaded from `/oem/usr/share/models/retinaface.rknn`
- **Pipeline:** NV12 frame → 4× integer downsample → 640×640 letterbox BGR → RKNN inference → decode anchors → select most-centred face above 0.5 confidence
- **Focus scoring:** The Tenengrad sharpness score crops around the detected face (with 50% padding). Falls back to a fixed 320×240 centre crop when no face is present.
- **OSD green box:** Drawn as four thin OVERLAY_RGN strips (one per edge). Full-frame overlay and COVER_RGN are non-functional at 2592×1944 on this SDK — per-edge strips work reliably.
- **Detection rate:** Runs every scoring frame (~500 ms update)

The model file must be deployed to the board (see Deploy section).

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
- **RetinaFace model** — bundled in this repo at `files/models/retinaface.rknn` (637 KB). No separate download needed; `build-app.sh` references it automatically.

## Build

```sh
git clone https://github.com/Cyaaaaaaaaan/luckfox-uvc-ptz.git ~/luckfox-uvc-ptz
cd ~/luckfox-uvc-ptz

# Patch the SDK and build (SDK assumed to be at ~/luckfox-pico)
bash build-app.sh

# If your SDK is elsewhere:
SDK_DIR=/path/to/luckfox-pico bash build-app.sh
```

After a successful build, the script prints exact deploy commands with resolved paths.

## Deploy

`/oem` is a read-only squashfs partition. Stage everything through `/tmp` first, then remount and install in a single SSH call.

```sh
export PICO_IP=<board IP>        # e.g. 192.168.1.149

# Paths (adjust if your SDK/repo are elsewhere)
UVC_BIN=~/luckfox-pico/project/app/uvc_app_tiny/out/bin/rk_mpi_uvc
UVC_SRC=~/luckfox-pico/project/app/uvc_app_tiny/uvc_app
RETINA_MODEL=~/luckfox-uvc-ptz/files/models/retinaface.rknn

# 0. One-time: back up the factory launcher
ssh root@$PICO_IP "cp /oem/usr/bin/RkLunch.sh /oem/usr/bin/RkLunch.sh.factory"

# 1. One-time: disable S99usb0config
#    It spawns a usb_reset loop that calls "S50usbdevice restart" on USB disconnect,
#    which wipes the UVC gadget configfs setup.
ssh root@$PICO_IP "chmod -x /etc/init.d/S99usb0config"

# 2. Stage files to /tmp (always writable, no kill or remount needed)
scp "$UVC_BIN"                        root@$PICO_IP:/tmp/rk_mpi_uvc
scp "$UVC_SRC/usb_config.sh"         root@$PICO_IP:/tmp/usb_config.sh
scp "$UVC_SRC/rkuvc.ini"             root@$PICO_IP:/tmp/rkuvc.ini.new
scp ~/luckfox-uvc-ptz/RkLunch.sh     root@$PICO_IP:/tmp/RkLunch.sh
scp "$RETINA_MODEL"                   root@$PICO_IP:/tmp/retinaface.rknn

# 3. Kill binary, remount /oem rw, install everything, reboot
ssh root@$PICO_IP \
  "killall -9 rk_mpi_uvc 2>/dev/null; mount -o remount,rw /oem && \
    cp /tmp/rk_mpi_uvc        /oem/usr/bin/rk_mpi_uvc && \
    cp /tmp/usb_config.sh     /oem/usr/bin/usb_config.sh && \
    cp /tmp/rkuvc.ini.new     /oem/usr/share/rkuvc.ini && \
    cp /tmp/RkLunch.sh        /oem/usr/bin/RkLunch.sh && \
    mkdir -p /oem/usr/share/models && \
    cp /tmp/retinaface.rknn   /oem/usr/share/models/ && \
    chmod +x /oem/usr/bin/rk_mpi_uvc /oem/usr/bin/usb_config.sh /oem/usr/bin/RkLunch.sh && \
    reboot"
```

## Boot flow

```
inittab → rcS → S99test → /usr/bin/t → /oem/usr/bin/RkLunch.sh
                                               │
                                               ├─ kills rkipc, usb_reset
                                               ├─ unbinds UDC (so configfs can be remounted)
                                               ├─ usb_config.sh  (sets up UVC gadget)
                                               └─ rk_mpi_uvc  (foreground, auto-restarts on exit)
                                                       │
                                                       ├─ face_det_init()  (loads retinaface.rknn)
                                                       ├─ focus scoring thread (Tenengrad sharpness)
                                                       └─ OSD: green box overlay on detected face
```

`RkLunch.sh` (this repo) replaces the factory launcher that starts `rkipc`. On each boot it kills any conflicting factory processes, reconfigures the USB gadget for UVC, and runs `rk_mpi_uvc` in a watchdog loop that restarts it automatically if it crashes.

## Rolling back

Every stable state is tagged:

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
