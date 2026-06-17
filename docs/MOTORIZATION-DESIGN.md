# PTZ Motorization — Design Document

**Status:** design/planning only. No motor code is implemented yet (`visca/motor.c` is still a printf stub). This document captures the mechanical, electronic, and control-architecture decisions so implementation can start cleanly when the hardware arrives.

Board: **Luckfox Pico Pro Max** (Rockchip **RV1106**). Active device tree: `rv1106g-luckfox-pico-pro-max.dts`. Controller: **SMTAV KC2000** over VISCA-over-IP (UDP 1259).

---

## 1. Axes & actuators

| Axis | Actuator | Driver | Feedback |
|------|----------|--------|----------|
| Pan | NEMA-class / geared stepper | TMC2209 (UART) | (steps; optional) |
| Tilt | stepper, ±45° via GT2 belt | TMC2209 (UART) | (steps; optional) |
| Focus | N20 DC gear motor | DRV8833 (PWM) | AS5600L on motor shaft |
| Zoom | N20 DC gear motor | DRV8833 (PWM) | AS5600L on motor shaft |
| Iris | N20 DC gear motor (likely left **manual** at F1.4) | DRV8833 (PWM) | optional |

N20s on hand: 3.7 V rated (1.5–5 V usable), ~90 rpm, **3 mm D-shaft**.

---

## 2. Mechanical (see memory `ptz-gear-cage-specs` for full numbers)

- **Module 0.8, 20° pressure angle**, external ring gear. Module raised from 0.5 (which slipped) — 0.8 nearly doubles tooth height (1.8 mm) and is aftermarket-stockable.
- **Ring gear 67T** (PD 53.6, OD 55.2) — sized to fit the **actual 58 mm** cage clearance (not the nominal 65 mm bore).
- **Pinion 18T** (PD 14.4, OD 16), D-bore for the 3 mm D-shaft, no setscrew. 18T is viable only because of the 20° PA (undercut limit 17T).
- **Center distance 34 mm**, ratio 67/18 = **3.72:1**. Motor mounts **diagonally in a cage corner** (pinion reaches 42 mm radius — clears the ~46 mm corner diagonal but not the 32.5 mm flat wall).
- **Two-part ring gear:** PETG outer (teeth) + TPU 95A inner sleeve (1.5 mm wall) that stretches over the 38.3 mm iris ring to seat on a 37 mm zoom/focus ring.
- **Tilt:** camera ear on 6706 bearing (30 ID / 37 OD), GT2 6 mm arc pulley (~120°), 20T motor pulley, ~3:1, TMC2209-driven.
- Print gears **flat** (axis vertical) — tooth profile is then an XY feature, immune to the 0.16 mm layer limit on the Bambu P1S (PETG).

---

## 3. Pin budget — researched against the actual DTS

All four buses we need are **disabled by default** (free to claim) in `rv1106g-luckfox-pico-pro-max.dts`:

```
&uart3 { status = "disabled"; }   // UART3_M1
&uart4 { status = "disabled"; }   // UART4_M1
&i2c1  { status = "disabled"; }   // I2C1_M1
&i2c3  { status = "disabled"; }   // I2C3_M1
```

### Functional → GPIO map (from `rv1106-pinctrl.dtsi`)

| Function | Pins | Mux | Notes |
|----------|------|-----|-------|
| UART3_M1 | TX **GPIO1_D0**, RX **GPIO1_D1** | 5 | free; pinctrl pre-wired by `ipc.dtsi` |
| UART4_M1 | TX **GPIO1_C5**, RX **GPIO1_C4** | 4 | free; pinctrl pre-wired |
| I2C3     | SCL **GPIO1_D3**, SDA **GPIO1_D2** (M1) **+** GPIO2_A6/A7 (M0) | 3/5 | board claims **both** mux groups; M1 is adjacent to UART3 |
| I2C1_M1  | SCL **GPIO2_B0**, SDA **GPIO2_B1** | 2 | free; pinctrl pre-wired |

The board's `ipc.dtsi` already sets `pinctrl-0` for all four (uart3=M1, uart4=M1, i2c1=M1, i2c3=M1+M0), so **enabling is literally just `status = "okay"`** — no pinctrl editing needed. Verified device nodes: `uart3`→`/dev/ttyS3`, `i2c3`→`/dev/i2c-3`. (`i2c4` is also available if a third I²C bus is ever wanted.)

### Hardware PWM is effectively unavailable — the key finding

The RV1106 has 12 PWM channels, but on *this board* almost none reach a free, broken-out pin:

| PWM | Pin | Blocked by |
|-----|-----|-----------|
| pwm1/2/3/5/6 | GPIO0_A1/A2/A4/A5/A6 | PMU "always-on" domain (bank 0) — not general-purpose |
| pwm4 | GPIO1_A1 | **WiFi/PA** `pa-ctl-gpios = <&gpio1 RK_PA1>` |
| pwm8 | GPIO3_A3 | **SDIO** `sdmmc0_d0` |
| pwm9 | GPIO3_A2 | **SDIO** data |
| pwm10 | GPIO3_A4 | **SDIO** `sdmmc0_clk` |
| pwm11 | GPIO3_A5 | **SDIO** `sdmmc0_cmd` |
| pwm0 | GPIO1_A2 | possibly free |
| pwm7 | GPIO1_A0 | possibly free |

Only ~2 PWM channels are plausibly free, and we need **6** (3 DC motors × IN1/IN2). Also claimed elsewhere: camera pwdn/reset **GPIO3_C5**, LEDs **GPIO1_C7** & **GPIO3_C6**.

> If onboard WiFi/SDIO is genuinely unused, disabling `&sdmmc0` would reclaim GPIO3_A2–A5 for pwm8–11 — but that's a tradeoff and still only gets to 4 PWM. Not recommended.

---

## 4. Recommended architecture — move motor I/O onto the buses

Because hardware PWM pins are scarce, **don't drive DRV8833 from SoC PWM pins.** Use expander/serial control. This collapses the entire motor+encoder system onto **one UART + one I²C bus = 4 SoC pins**.

```
              ┌──────────────────────── I2C3 (GPIO1_D2/D3) ───────────────────────┐
RV1106 ───────┤  PCA9685         →  DRV8833 ×2  →  focus / zoom / iris DC motors   │
              │  AS5600L ×3       ← magnets on each pinion                         │
              └────────────────────────────────────────────────────────────────────┘
RV1106 ─ UART3 (GPIO1_D0/D1) ─ TMC2209 ×2 (pan + tilt) via UART addressing, velocity mode
```

**⚠ I²C address collision to plan around:** PCA9685 defaults to **0x40**, and **AS5600L also defaults to 0x40**. They cannot coexist unconfigured. Resolution: strap the PCA9685's A0–A5 address pins to a non-0x40 address (e.g. **0x41**), and program each AS5600L to a distinct address via its `I2C_ADDR`/`I2C_UPDT` registers (e.g. **0x36, 0x37, 0x38** for focus/zoom/iris). The original AS5600 (non-L) is fixed at 0x36 and can't be re-addressed — so **AS5600L is mandatory** for >1 encoder on the bus.

Final I²C3 inventory: PCA9685 @0x41, AS5600L @0x36/0x37/0x38 — all distinct.

**TMC2209 over UART (no STEP/DIR pins):**
- TMC2209 supports a single-wire UART and **4 selectable addresses** (MS1/MS2 straps), so **both pan & tilt drivers share one UART bus**.
- In UART mode you can use **velocity mode (`VACTUAL` register)** — the driver generates steps internally; you just write a target velocity. No STEP/DIR GPIO needed. Perfect for VISCA's continuous "pan at speed N" model.
- Cost: 2 pins (UART3 TX/RX), or even 1 with true single-wire.

**DRV8833 via PCA9685 (I²C PWM expander):**
- One PCA9685 = 16× 12-bit PWM channels over I²C. 3 motors × 2 = 6 channels, plenty of headroom.
- Global PWM frequency (~1 kHz is fine for DC motor speed control; DRV8833 inputs tolerate it).
- DRV8833 per-motor: forward = IN1=PWM,IN2=0; reverse = IN1=0,IN2=PWM (slow-decay). PCA9685 drives both INs.
- Cost: 0 extra SoC pins (shares I²C3).

**AS5600L encoders (focus/zoom position):**
- On-axis magnetic encoder; **magnet embedded in the pinion end face**, chip on a coaxial cage boss, 1–2 mm air gap. Diametrically-magnetized 6×2.5 mm magnet.
- 3.72:1 gearing → ring travel spans ~2.8 motor revs → multi-turn; track turns in software after boot homing. Resolution ≈ 15 000 counts over full ring travel.
- AS5600**L** (programmable address) so all three share the I²C bus alongside the PCA9685.
- Free stall detection: commanded-to-move but angle frozen ⇒ hit a hard stop.

**Net SoC usage: UART3 (2 pins) + I²C3 (2 pins) = 4 pins.** Everything else lives on the buses. I²C3 bus inventory: PCA9685 ×1 + AS5600L ×3 (distinct addresses) — no conflict.

### 4a. Process placement — where does each piece run? (important)

The two processes already split cleanly, and this dictates where the motor + AF code goes:

| Concern | Process | Why |
|---------|---------|-----|
| Focus **score** (ISP sharpness, face window) | **rk_mpi_uvc** | owns the VI frames + rkaiq singleton; already writes `/tmp/focus_score` every ~500 ms |
| **Motor** control (UART/I²C/PCA9685/TMC2209) | **visca_server** | owns the VISCA protocol and is the natural home for actuator hardware |
| **Autofocus** hill-climb loop | **visca_server** | needs to *both* read the score and drive the focus motor |

**The autofocus controller belongs in `visca_server`**, and the interface between the two processes is the file **`/tmp/focus_score` that `rk_mpi_uvc` already publishes** — no new IPC needed. `visca_server` reads that file, runs the hill-climb, and calls its local `motor_focus()`. The ~500 ms file-update cadence naturally rate-limits the AF loop (matches §6c).

This also means **manual-focus VISCA commands and AF live in the same process**, so coordination is trivial: a manual `04 08` focus command pauses/overrides the AF loop, and AF resumes on a trigger. No cross-process locking.

(If a tighter AF loop than 500 ms is ever needed, either lower `SCORE_INTERVAL_US` in `focus_score.cpp` or expose the score over the existing `/tmp/visca_isp.sock` — but the file interface is the simplest correct option and should be the starting point.)

---

## 5. Enabling the buses (DTS edit — for later)

In `rv1106g-luckfox-pico-pro-max.dts`, flip the relevant nodes:
```dts
&uart3 { status = "okay"; };   // TMC2209 pan+tilt
&i2c3  { status = "okay"; clock-frequency = <400000>; };  // PCA9685 + AS5600L
```
(Keep UART4/I2C1 as spare. Bump I²C to 400 kHz for snappier encoder reads.) Rebuild the kernel/dtb via the SDK and reflash the boot/dtb partition. This is a kernel change, *not* part of `apply_patches.sh` (which only patches the UVC app) — track it separately.

---

## 6. Control logic design (pseudocode — not yet implemented)

### 6a. Homing (per DC axis, on boot)
```
home(axis):
    drive(axis, dir=toward_min, duty=LOW)      # gentle
    last = encoder(axis); stalled = 0
    loop every 20ms:
        if |encoder(axis) - last| < EPS: stalled++ else stalled = 0
        last = encoder(axis)
        if stalled >= 3:                        # ~60ms no movement = hard stop
            stop(axis); pos[axis] = 0; turns[axis] = 0; return
```

### 6b. Multi-turn absolute position
```
on each encoder read:
    raw = as5600_angle(axis)                    # 0..4095
    if raw - prev_raw >  2048: turns[axis]--     # wrapped backward
    if raw - prev_raw < -2048: turns[axis]++     # wrapped forward
    prev_raw = raw
    pos[axis] = turns[axis]*4096 + raw           # absolute since home
```

### 6c. Hill-climbing autofocus (uses focus_score_get(), now reliable)
```
autofocus():
    step = COARSE; dir = +1; best = score(); best_pos = pos[FOCUS]
    loop:
        move(FOCUS, dir, step)                  # bounded by [0, pos_max]
        wait_for_score_update()                 # score updates ~500ms; settle first
        s = score()
        if s > best: best = s; best_pos = pos[FOCUS]    # keep climbing
        else:
            dir = -dir; step = step/2           # overshoot → reverse, refine
            if step < MIN_STEP: break           # converged
    move_to(FOCUS, best_pos); lock()
re-trigger when: score drops below 0.5*best (subject moved / zoom changed)
```
Notes: the **~500 ms** score cadence (`SCORE_INTERVAL_US`) dominates seek time — settle one interval after each move before reading. Score is windowed to the detected face (Patch 9), so AF tracks the face, not the background. Could lower the interval later for faster seeks.

### 6d. VISCA → motor mapping (replaces motor.c stubs)
```
motor_pan(dir,speed):  tmc2209_vactual(PAN,  dir*map(speed))    # UART velocity mode
motor_tilt(dir,speed): tmc2209_vactual(TILT, dir*map(speed))    # clamp to ±45° via pos
motor_zoom(dir,speed): drv8833_pwm(ZOOM, dir, map(speed))       # via PCA9685, bound by encoder limits
motor_focus(dir,speed):drv8833_pwm(FOCUS,dir, map(speed))       # manual focus overrides AF
```
Tilt must clamp at the ±45° software limits derived from homing; pan is continuous (no limit) unless a slip-ring/cable constraint applies.

---

## 7. Open questions / decisions pending

- **Iris:** motorize or leave manual at F1.4? Leaning manual (one fewer motor, frees a DRV8833 channel).
- **Pan range:** continuous or limited? Affects whether the camera↔board cabling needs a slip ring. Current plan keeps the board on the tilt head and runs only Ethernet+power across the axes (service loop), avoiding a slip ring for the fragile 20-pin FPC.
- **Encoder on tilt/pan?** Steppers are open-loop; TMC2209 `StallGuard` can do sensorless homing, so AS5600L may be unnecessary on the stepper axes.
- **PCA9685 vs. 2 free PWM pins:** if iris stays manual, only 2 DC motors (4 PWM) are needed — still more than the ~2 free SoC PWM pins, so PCA9685 (or a TB6612 + expander) remains the cleaner path.
- **DRV8833 current limiting:** set via the sense resistor on the *bare IC*; most modules fix it. Even without current *readout*, the encoder-stall method (6a) covers homing.

---

## 8. Implementation order

**Phase 0 — EPTZ digital PTZ (NO hardware, do this first; see §9.3):**
0a. Add an EPTZ helper in `rk_mpi_uvc` calling `RK_MPI_VI_SetEptz` (crop-rect state: center x/y + zoom factor).
0b. Add IPC commands over the existing `/tmp/visca_isp.sock`: `zoom <0..100>`, `pan <delta>`, `tilt <delta>` (or absolute `eptz x y w h`).
0c. In `visca.c`, route the VISCA zoom (`04 07`) and pan/tilt (`06 01`) handlers to those IPC commands *in addition to* `motor_*`. Result: **working digital PTZ over the KC2000 with zero motor hardware.**
0d. Verify EPTZ behaves on our patched pipeline (VPSS disabled, 2592×1944 max — see §9.3 caveat).

**Phase 1+ — mechanical (when hardware arrives):**
1. DTS: enable `uart3` + `i2c3`, rebuild dtb, confirm `/dev/ttyS3` and `/dev/i2c-3` appear.
2. Bring up I²C: detect PCA9685 + AS5600L addresses (`i2cdetect -y 3`).
3. `motor.c`: PCA9685 PWM driver → DRV8833; verify focus/zoom motors spin both directions.
4. AS5600L read + multi-turn tracking; implement homing (6a/6b).
5. TMC2209 UART velocity mode for pan/tilt; wire to `motor_pan/tilt`. (Or evaluate the rk_motor 28BYJ-48 path — §9.1.)
6. Hill-climbing autofocus (6c) on top of the now-reliable focus score.
7. VISCA preset recall (`04 3F`) → store/restore encoder positions (+ EPTZ state).

---

## 9. SDK assets discovered (research)

### 9.1 Ready-made pan/tilt stepper driver — `sysdrv/drv_ko/motor/`
A Rockchip kernel module for **2-axis (X/Y) pan/tilt** of **28BYJ-48 unipolar steppers** (via ULN2003, 4 GPIO/motor = 8 GPIO total). Clean `/dev` ioctl API: `MOTOR_MOVE / RESET / STOP / SPEED / GOBACK / CRUISE / GET_STATUS`, plus a userspace API (`rk_motor_move`, `rk_motor_reset`, `rk_motor_get_status`, …) in `src/rk_motor.h`. Open-loop step counting with **software limits** (`HORIZONTAL_MAX_STEPS 4000`, `VERTICAL_MAX_STEPS 2000`) → no physical endstops needed. DTS binding `compatible="motor"` with `motorA..H-gpios` (example uses GPIO1_C0..C7 — note GPIO1_C7 is our LED and C4/C5 are UART4, so remap if used).

**Strategic option:** 28BYJ-48 steppers are tiny, cheap, 5V — matching the "small & cheap, not NEMA/TMC2209" goal stated earlier. This driver gives pan/tilt with homing + limits + a built-in `CRUISE`/patrol mode essentially for free. Trade-off: 8 GPIO (we're pin-constrained — see §3), lower torque, no microstep smoothness. **Decision pending:** 28BYJ-48 + rk_motor (simple, GPIO-heavy) vs. TMC2209-over-UART (pin-light, smoother, custom driver). The rk_motor API is a good template either way.

### 9.2 rkaiq AF actuates VCMs, not our lens — validates the custom AF
`drivers/media/i2c/` ships VCM drivers (`dw9714`, `dw9768`, `dw9800w`, `dw9807-vcm`, `ak7375`) and a thin `hall-dc-motor` v4l2 subdev (`compatible="rockchip,hall-dc"`). rkaiq's AF (`rk_aiq_user_api2_af_*`, `final_pos`, search path) is built to drive these **VCM** actuators. Our focus is a **DC gear motor on a C/CS ring** — not a VCM — so rkaiq cannot actuate it. This **confirms the design**: use rkaiq's sharpness *measurement* (Patch 9) + our own hill-climb to drive the DC motor (§6c). Do **not** expect rkaiq AF to move the lens.

### 9.3 EPTZ — motorless digital pan/tilt/zoom (high value, do first)
`RK_MPI_VI_SetEptz(ViPipe, ViChn, VI_CROP_INFO_S)` is in the shipped headers (`media/out/include/rk_mpi_vi.h`) and the app already uses `RK_MPI_VI`. It crops a rect from the **2592×1944** sensor and scales it to the output — i.e. **digital zoom (shrink rect), pan/tilt (move rect origin)**, live on a running stream (the sample calls it in a loop).

```c
VI_CROP_INFO_S c = {0};
c.bEnable = RK_TRUE;
c.enCropCoordinate = VI_CROP_ABS_COOR;      // absolute pixels (RATIO_COOR=0 also available)
c.stCropRect = (RECT_S){ x, y, w, h };       // smaller w/h centered = zoom in; move x/y = pan/tilt
RK_MPI_VI_SetEptz(0, 0, c);
```
**Architecture fit is perfect:** VISCA zoom/pan/tilt → existing `/tmp/visca_isp.sock` IPC → `SetEptz` in `rk_mpi_uvc`. Reuses the exact plumbing built for AE/WB. Gives **working PTZ over the KC2000 with no motors** as an immediate milestone, and stays useful afterwards as fast/fine digital trim, electronic stabilization, and zoom range before the optical zoom motor exists.
- **Caveat to test:** our pipeline disabled VPSS (Patch 6) and runs 2592×1944 max (Patch 1 set `stMaxSize`). Confirm `SetEptz` crop+scale works in this config and doesn't re-trigger the VI/ISP SOF-disorder that Patches 1–3 fixed. Validate before relying on it.
- **Res-switch interaction:** the app changes UVC resolution via in-place `RK_MPI_VI_SetChnAttr` (`uvc_mpi_vi.cpp`). That will reset the crop, so the EPTZ state must be **re-applied after every resolution switch** (re-call `SetEptz` at the end of the resize path). Canonical helper to copy: `media/samples/example/common/sample_comm_vi.c` (gated by `bIfOpenEptz`, calls `RK_MPI_VI_SetEptz` at channel setup).
- **Coordinate choice:** all SDK samples use `VI_CROP_ABS_COOR` (absolute pixels) — use that. `VI_CROP_RATIO_COOR` also exists and would survive res changes without rescaling, but its scale is undocumented in the shipped headers — verify empirically before relying on it. Simplest robust approach: keep EPTZ state as a normalized center+zoom in our own code and recompute the absolute rect (against the current output size) each time we apply it.
- **Quality note:** digital zoom trades resolution; pair coarse optical zoom (motor) with fine EPTZ later. Pan/tilt range is limited to the slack between crop and full sensor.

### 9.4 Userspace hardware access (kernel is already configured for it)
From `luckfox_rv1106_linux_defconfig`: `CONFIG_I2C_CHARDEV=y` (`/dev/i2c-N`), `CONFIG_SPI_SPIDEV=y`, `CONFIG_GPIO_SYSFS=y` (legacy `/sys/class/gpio`), `CONFIG_PWM_ROCKCHIP=y`, `CONFIG_SERIAL_8250=y` (RV1106 UARTs are 8250/dw-compatible → `/dev/ttyS3`). **Not** set: `CONFIG_PWM_SYSFS` (no `/sys/class/pwm` — reconfirms SoC PWM is impractical, use PCA9685) and `CONFIG_GPIO_CDEV` (no libgpiod — use legacy sysfs GPIO).

So `motor.c` userspace toolkit, no kernel rebuild needed beyond enabling the bus nodes (§5):
- **TMC2209:** `/dev/ttyS3` + termios
- **PCA9685 + AS5600L:** `/dev/i2c-3` + `I2C_SLAVE` ioctl + read/write
- **Direct GPIO** (TMC2209 EN, DRV8833 sleep, etc.): legacy `/sys/class/gpio` export/direction/value
- **No** usable SoC hardware PWM from userspace → PCA9685 is the actuator path for DC motors.
