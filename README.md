# luckfox-uvc-ptz

Patches and build scripts for running the Luckfox Pico Pro Max as a USB UVC camera (MJPEG / H.264 / H.265).

Fixes resolution switching, removes YUYV, enforces 4:3-only resolutions, and sets up reliable boot autostart without rkipc.

## Hardware

| Item | Value |
|------|-------|
| Board | Luckfox Pico Pro Max |
| SoC | Rockchip RV1106 |
| Sensor | MIS5001 (5 MP, 2592×1944 native) |
| USB | USB 2.0 High-Speed (UVC gadget via configfs) |

## What the patches do

| Patch | Description |
|-------|-------------|
| `0001` | VI start is idempotent — safe to call multiple times without restarting the ISP |
| `0002` | VENC restart on resolution change without tearing down the pipeline |
| `0003` | ISP keepalive — prevents AIQ SOF disorder on RV1106 after resolution switch |
| `0004` | USB gadget config: 4:3 resolutions only, YUYV removed, MJPEG + H.264 + H.265 |
| `0005` | `usb_config.sh`: silences spurious insmod/mkdir/udev stderr noise |

**Key fix:** `RK_MPI_VI_DisableChn` on RV1106 triggers a full ISP restart → AIQ detects SOF disorder (frame counter reset) → VI permanently stops delivering frames. These patches replace DisableChn with in-place `RK_MPI_VI_SetChnAttr`, requiring `stIspOpt.stMaxSize` to be set to the sensor native max (2592×1944).

**Supported resolutions:** 640×480 · 1024×768 · 1280×960 · 2048×1536 · 2592×1944

## Prerequisites

- Luckfox Pico SDK cloned at `~/luckfox-pico` (or set `SDK_DIR`)
  ```sh
  git clone https://github.com/LuckfoxTECH/luckfox-pico.git ~/luckfox-pico
  ```
- ARM cross-toolchain (the build script sets this up automatically from the SDK)
- Board flashed with factory firmware (the `/oem` partition must exist)
- SSH access to the board

## Build

```sh
git clone https://github.com/Cyaaaaaaaaan/luckfox-uvc-ptz.git ~/luckfox-uvc-ptz
cd ~/luckfox-uvc-ptz

# Patch the SDK and build (SDK assumed to be at ~/luckfox-pico)
bash build-app.sh

# If your SDK is elsewhere:
SDK_DIR=/path/to/luckfox-pico bash build-app.sh
```

After a successful build, the script prints the exact deploy commands with resolved paths. Follow those, or use the commands below substituting your board IP.

## Deploy

```sh
export PICO_IP=<board IP>        # e.g. 192.168.1.149

# 0. One-time: back up the factory launcher
ssh root@$PICO_IP "cp /oem/usr/bin/RkLunch.sh /oem/usr/bin/RkLunch.sh.factory"

# 1. Copy build artifacts
scp build/rk_mpi_uvc                                       root@$PICO_IP:/oem/usr/bin/
scp ~/luckfox-pico/project/app/uvc_app_tiny/uvc_app/usb_config.sh  root@$PICO_IP:/oem/usr/bin/
scp ~/luckfox-pico/project/app/uvc_app_tiny/uvc_app/rkuvc.ini      root@$PICO_IP:/oem/usr/share/rkuvc.ini
scp ~/luckfox-uvc-ptz/RkLunch.sh                           root@$PICO_IP:/oem/usr/bin/RkLunch.sh

# 2. Set permissions
ssh root@$PICO_IP "chmod +x /oem/usr/bin/rk_mpi_uvc /oem/usr/bin/usb_config.sh /oem/usr/bin/RkLunch.sh"

# 3. One-time: disable S99usb0config
#    It spawns a usb_reset loop that calls "S50usbdevice restart" on USB disconnect,
#    which wipes the UVC gadget configfs setup.
ssh root@$PICO_IP "chmod -x /etc/init.d/S99usb0config"

# 4. Reboot — UVC camera starts automatically
ssh root@$PICO_IP reboot
```

## Boot flow

```
inittab → rcS → S99test → /usr/bin/t → /oem/usr/bin/RkLunch.sh
                                               │
                                               ├─ kills rkipc, usb_reset
                                               ├─ unbinds UDC (so configfs can be remounted)
                                               ├─ usb_config.sh  (sets up UVC gadget)
                                               └─ rk_mpi_uvc  (foreground, auto-restarts on exit)
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
