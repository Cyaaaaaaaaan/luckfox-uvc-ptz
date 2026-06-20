# SDK Scrub Notes — reusable capabilities in `~/luckfox-pico`

A full sweep of the Luckfox/Rockchip RV1106 SDK for things useful to this project
(UVC PTZ camera for FSL congregation meetings streamed on Zoom; NPU person
detection done; pan/tilt + optical-zoom motors coming). Ranked by value to us.
Date: 2026-06-20.

Conventions: `RK_MPI_*` = Rockit media API (headers in
`media/out/include/`); `rk_aiq_user_api2_*` = ISP/AIQ tuning
(`media/out/include/rkaiq/uAPI2/`). Most ISP knobs are wireable to VISCA/IPC
exactly like the existing picture controls (Patch 8 / `isp_ipc.cpp`).

---

## Tier 1 — high value, low effort, directly for FSL + Zoom

### Encoder (VENC) — the stream Zoom actually sees
- **Motion deblur** — `RK_MPI_VENC_EnableMotionDeblur(chn, RK_TRUE)` +
  `RK_MPI_VENC_SetMotionDeblurStrength(chn, strength)`. Reduces blur on **fast
  motion** = signing hands. One-line enable. **Best single quality win for FSL.**
- **Super-frame strategy** — `RK_MPI_VENC_SetSuperFrameStrategy()` caps encoded
  frame size so a big I-frame can't blow the bitrate budget and stall the Zoom
  pipe. Pairs with…
- **Intra refresh** — `RK_MPI_VENC_SetIntraRefresh()` spreads I-data across
  frames instead of periodic big keyframes → smoother bitrate + better packet-loss
  resilience for streaming.
- **QP map** — `RK_MPI_VENC_SetQpmap(chn, blk)` per-macroblock QP (finer than the
  8-box ROI we use for person-ROI; could gradient-weight the signing area).
- **SVC / hierarchical QP** — `RK_MPI_VENC_EnableSvc()`,
  `SetHierarchicalQp()` — temporal layers for bandwidth adaptation.
- **RC modes** (`rk_comm_rc.h`): H264/H265 CBR/VBR/**AVBR**/FIXQP. AVBR is the
  sweet spot for streaming (quality-targeted, bitrate-bounded).
- **`RK_MPI_VENC_RequestIDR()`** — force a keyframe (useful on Zoom reconnect /
  first-frame).
- Already used: **`RK_MPI_VENC_SetRoiAttr`** (person-ROI, `FOCUS_SCORE_VENC_ROI`).

### IVE — Image Vision Engine (hardware CV, header `media/ive/ive/include/rk_mpi_ive.h`)
A full HW computer-vision library. Two concrete uses for us:
- **Offload focus scoring**: `RK_MPI_IVE_Sobel` / `NormGrad` / `MagAndAng` compute
  the Tenengrad gradient energy in **hardware** → frees the CPU the fallback path
  currently uses.
- **Cheap "someone stood up" without the NPU**: `RK_MPI_IVE_GMM` /
  `MatchBgModel` / `UpdateBgModel` (background modeling → foreground/motion) or
  `LKOpticalFlow` / `LKOpticalFlowPyr` (optical flow). Could gate the NPU person
  detector (only run heavy inference when motion is seen) to cut power/heat.
- Also available: `CannyEdge`, `Erode`/`Dilate`/`Filter`/`Thresh`, `Hist`/
  `EqualizeHist`, `Integ` (integral image), `CCL` (blob labeling), `SAD`/`NCC`
  (template match), `STCorner` (Shi-Tomasi), `LBP`, `Pyramid`, arithmetic.

### ISP (rkaiq) — corrections aimed at the wide 5–50mm lens
- **Lens distortion correction**: `rk_aiq_user_api2_aldch_*` (LDCH) and
  `rk_aiq_user_api2_afec_*` (FEC). Straightens barrel distortion at wide zoom.
  (FEC needs a mesh/calib; LDCH is lighter.)
- **Lens shading / vignetting**: `..._alsc_*` — fixes dark corners common on wide
  glass.
- **Chromatic aberration**: `..._acac_*` — kills color fringing at edges.
- **Dynamic range**: `..._adrc_*`, `..._atmo_*`, `..._amerge_*` — for high-contrast
  rooms (bright windows + dim audience), keep both readable.
- Plus the already-noted dehaze, gamma, 3DLUT (`a3dlut`), CCM (`accm`), and the
  many NR/sharpen versions (`acnr/aynr/abayernr/asharp_*`).

---

## Tier 2 — useful with motors / near-future

- **VPSS** (`rk_mpi_vpss.h`) — `RK_MPI_VPSS_SetChnCrop`/`SetGrpCrop` +
  `SetChnRotation`/`SetChnRotationEx`. The **proper** crop+scale+rotate block (what
  EPTZ should have used). Rotation matters for whatever orientation the board ends
  up mounted in the cage. The correct digital-zoom path **if ever wanted** — not
  VI EPTZ.
- **TDE** (`rk_mpi_tde.h`) — 2D graphics engine: `RK_TDE_QuickResize` (crop+scale),
  `QuickFill` (solid boxes), `Bitblit` (blend/compose), `Rotate`, `QuickCopy`.
  Hardware-accelerated OSD (vs our manual RGN canvas) and the compositing engine
  for a future **FSL caption overlay**.
- **IVS** (`rk_mpi_ivs.h`) — Intelligent Video: `SetMdAttr` (motion detect),
  `SetOdAttr` (occlusion/tamper), `SendFrame`/`GetResults`. A cheap MPI motion
  channel — audience-movement trigger to wake the detector.
- **Audio capture + voice enhancement** (`rk_mpi_ai.h`) — `EnableVqe` =
  AEC/ANR/AGC voice quality enhancement; `EnableAed` = acoustic-event detection;
  resample, volume curve. `rk_mpi_aenc.h` encodes it. Only relevant if we add a
  **UAC audio interface** so Zoom gets clean room audio (currently video-only).
- **Thermal** — in-kernel `thermal_core` + `cpufreq_cooling`/`devfreq_cooling` +
  step-wise governor. Read temp via `/sys/class/thermal/thermal_zone*/temp`;
  matters for an **enclosed printed cage** running hour-long meetings (watch for
  throttle). NPU/encoder at higher cadence = more heat.
- **Scaler lib** (`libRkScalerApi.h`) — `RkScalerInit/Processor/Deinit`, a
  standalone scaler; alternate crop+scale path.

---

## Tier 3 — reference / niche

- **rkipc OSD font engine** (`project/app/rkipc/rkipc/common/osd/`:
  `font_factory.c`, `draw_paint.c`, `bmp_reader.c`, `color_table.c`) — TTF/BMP
  rasteriser; foundation for the FSL caption overlay + a nicer menu than our 8×8
  bitmap font.
- **rkipc RockIVA reference** (`.../common/rockiva/rockiva.c`) — working
  detection→OSD→event wiring to crib from for `person_det`.
- **VENC extras** — `EnableThumbnail` (JPEG thumbnail track), `SetDeBreathEffect`
  (kills quality "breathing" around I-frames), `SetAntiLine`/`SetAntiRing`
  (artifact suppression), `EnableMotionStaticSwitch` (bitrate savings when static).
- **RTSP** (`librtsp.a`, `rkipc/common/rtsp/rtsp.c`, `rtsp_demo.h`) — network
  video. Not directly Zoom-usable (Zoom wants the UVC webcam); needs a PC bridge.
- **ipcweb** (`project/app/ipcweb`) — browser control-panel backend (www-rkipc);
  config UI without the KC2000, but significant to adapt to our config system.
- **Motor kernel driver** (`sysdrv/drv_ko/motor/`) — 28BYJ-48 unipolar 2-axis,
  ioctl `MOTOR_MOVE/RESET/STOP/SPEED/GOBACK/CRUISE` (`_IOW('M', …)`). Ready
  alternative to TMC2209 for pan/tilt; good API template either way.
- **Userspace HW helpers** (`media/sysutils/`, `media/luckfox/examples/`):
  `rk_adc` (2-ch SARADC `ff3c0000.saradc`, 0–1.8V, 10-bit → motor current sense
  without an INA3221, needs a sense amp), `rk_pwm`, `rk_gpio`, `rk_watchdog`
  (auto-reboot on hang — good for unattended meetings), plus copy-paste
  `luckfox_{adc,gpio,i2c,pwm,spi,uart}_test.c`.
- **Sensor drivers** — many in `sysdrv/.../media/i2c/` (active path = mis5001 @
  2592×1944; board also provisions sc3336/sc4336). VCM AF drivers
  (dw9714/dw9768/ak7375/cn3927) exist but **rkaiq AF only actuates VCMs** — our
  gear-motor lens is not a VCM, so the custom hill-climb (using rkaiq sharpness
  measurement only) stays correct.

---

## Ready reference implementations (`media/samples/`)

Copy-paste-grade examples for specific features — handy when implementing the
items above. (`simple_test/` = minimal single-feature; `example/demo/` = fuller.)

- **`simple_vi_bind_ivs.c`** — VI → IVS motion detection. Template for an
  NPU-free "someone moved/stood up" trigger that gates the heavy person detector.
- **`simple_vi_get_frame_tde.c`** — TDE 2D ops on VI frames. Template for
  hardware OSD / the FSL caption-overlay compositor.
- **`simple_vi_bind_vpss_bind_venc.c`** — VI → VPSS → VENC. The proper digital
  crop+scale+rotate path (what EPTZ should have been).
- **`simple_vi_bind_venc_jpeg.c`** — JPEG snapshot encode. For grabbing a still of
  the commenter (e.g. attendance/snapshot).
- **`simple_vi_bind_venc_change_resolution_rv1106.c`** — the canonical RV1106
  resolution-switch pattern; cross-check against our in-place `SetChnAttr` resize.
- **`simple_vi_bind_venc_combo_rv1106.c`** — multiple encoders (MJPEG + H.264) off
  one VI simultaneously.
- **`simple_vi_bind_venc_wrap_rv1106.c`** + **`example/demo/sample_rv1103_dual_memory_opt.c`**
  — buffer **wrap / low-memory** mode (`RK_MPI_VENC_SetChnBufWrapAttr`). Lever if
  we hit RAM limits running NPU + encode + detection together on the RV1106.
- **`simple_vi_bind_venc_svc_rtsp.c`** — SVC temporal layers for bandwidth adapt.
- **`simple_vi_get_frame_rkaiq.c`** — VI frame access + rkaiq control (our exact
  pattern; reference for ISP calls).

Also noted: **rockit TGI** (TaskGraph Interface, `media/rockit`) builds the media
pipeline from a **config file** instead of MPI calls — Rockchip ships it for
UVC/UAC products. We use MPI directly; informational only. The `rkaudio_sed`
module is a **speech AGC** (not sound-event detection — that's MPI `AI_EnableAed`).

---

## Confirmed NOT useful — do not revisit

- **EPTZ** (`RK_MPI_VI_SetEptz`) — **dead end** in this pipeline (live call returns
  rc=0 but never renders; only commits on channel reinit; crashes on resize while
  zoomed). See README / project memory. Use VPSS or motors instead.
- **AVS** stitching (`rk_mpi_avs.h`, `media/avs`) — multi-camera panorama; we're
  single-cam.
- **VO** (76 funcs, display output) / **VDEC** (decode) — no local display or
  decode need for a USB webcam.
- **media/security** — secure-boot/crypto tooling.
- **rk_smart_door / fastboot_client / wifi_app** — unrelated reference apps.
