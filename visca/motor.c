#include <stdio.h>
#include "motor.h"

/* ── Stub implementation ─────────────────────────────────────────────────────
 * Replace each function body with real GPIO/UART/PWM calls once hardware
 * is wired:
 *   Pan/Tilt  → TMC2209 via UART (STEP/DIR or PDN_UART register writes)
 *   Zoom      → N20 gear motor via DRV8833 PWM
 *   Focus     → N20 gear motor (or small stepper) via DRV8833 PWM
 *   Iris      → N20 gear motor via DRV8833 PWM (spare channel)
 * ─────────────────────────────────────────────────────────────────────────── */

void motor_init(void) {
    printf("[motor] init (stub)\n");
}

void motor_deinit(void) {
    printf("[motor] deinit (stub)\n");
}

void motor_pan(int dir, int speed) {
    printf("[motor] pan  dir=%+d speed=%d\n", dir, speed);
}

void motor_tilt(int dir, int speed) {
    printf("[motor] tilt dir=%+d speed=%d\n", dir, speed);
}

void motor_zoom(int dir, int speed) {
    printf("[motor] zoom dir=%+d speed=%d\n", dir, speed);
}

void motor_focus(int dir, int speed) {
    printf("[motor] focus dir=%+d speed=%d\n", dir, speed);
}
