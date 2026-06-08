#pragma once

/* Direction constants */
#define MOTOR_STOP  0
#define MOTOR_FWD   1
#define MOTOR_REV  -1

void motor_init(void);
void motor_deinit(void);

/* speed: 0–24 for pan/tilt (VISCA VV/WW), 0–7 for zoom/focus (VISCA nibble) */
void motor_pan(int dir, int speed);
void motor_tilt(int dir, int speed);
void motor_zoom(int dir, int speed);
void motor_focus(int dir, int speed);
