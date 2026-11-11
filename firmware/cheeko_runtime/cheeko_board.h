// Cheeko Gotchi board layer — hardware bring-up for the cheeko_runtime component.
// Drivers are adapted from firmware/hardware_test/main/cheeko_self_test.c.
// Internal to the runtime: apps never include this.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHEEKO_LCD_W 240
#define CHEEKO_LCD_H 296
#define CHEEKO_AUDIO_RATE 16000

// Button indices match cheeko::ButtonEvent::Button order.
enum { CHEEKO_BTN_BOOT = 0, CHEEKO_BTN_POWER, CHEEKO_BTN_VOL_UP, CHEEKO_BTN_VOL_DOWN, CHEEKO_BTN_COUNT };

// NVS, GPIO, I2C scan, LCD + LVGL, touch calibration (loads the one saved by
// hardware_test; runs it if missing or BOOT held at power-up), audio, accel.
void cheeko_board_init(void);

lv_display_t *cheeko_board_display(void);
bool cheeko_board_audio_ready(void);
bool cheeko_board_accel_ready(void);

bool cheeko_board_touch_read(int *x, int *y);         // calibrated screen coords
void cheeko_board_buttons_read(bool down[CHEEKO_BTN_COUNT]);
void cheeko_board_tone(float freq_hz, int ms, int amp);  // blocking
// Blocking write of 16 kHz mono PCM (duplicated to both slots). gain_q8: 256 = unity.
void cheeko_board_pcm_write(const int16_t *mono, int frames, int gain_q8);
int cheeko_board_mic_read(int16_t *mono, int max_frames);  // returns frames, -1 on error
void cheeko_board_mic_slot(int slot);
void cheeko_board_i2s_loopback(bool on);  // bring-up: RX reads our own DOUT  // bring-up: which RX slot is the mic
// Debug: raw codec register access (dev 0 = ES7210 mic ADC, 1 = ES8311 DAC).
bool cheeko_board_codec_reg_read(int dev, uint8_t reg, uint8_t *val);
bool cheeko_board_codec_reg_write(int dev, uint8_t reg, uint8_t val);
bool cheeko_board_accel_read_mg(int *x, int *y, int *z);

#ifdef __cplusplus
}
#endif
