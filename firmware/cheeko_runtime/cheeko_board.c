// Cheeko Gotchi board layer. Pin map, panel init, codec register tables, touch
// and accelerometer reads are taken from firmware/hardware_test (known-good on
// Cheeko Gotchi v1.2); only the UI/test loop was removed.

#include <stdlib.h>
#include "cheeko_board.h"

#include "sdkconfig.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"

// I2S framing. 0 = plain 16-bit stereo (default, speaker verified).
// 1 = TX+RX both TDM, 4 x 16-bit slots (BCLK 64 fs), WS 50% duty so the
// ES8311 still sees I2S (left = slot 0, right = slot 2). Bring-up only.
// KNOWN ISSUE (both modes): the ES7210 drives data in only 2 of every 16 LRCK
// frames (rest exact zeros), at any MCLK/BCLK/fs and every clock/format/TDM
// register; RX itself is fine (DOUT->DIN loopback is clean). Suspect board
// wiring/strap. See tools/voice_agent/README.md "Mic status".
#ifndef CHEEKO_DUMP_CODECS_AT_BOOT
#define CHEEKO_DUMP_CODECS_AT_BOOT 0  // 1: log codec regs left by the previous firmware
#endif
#ifndef CHEEKO_MCLK_MULT
#define CHEEKO_MCLK_MULT 256  // TDM mode: MCLK 4.096 MHz at 16 kHz
#endif
#ifndef CHEEKO_I2S_TDM
#define CHEEKO_I2S_TDM 0
#endif
#if CHEEKO_I2S_TDM
#define I2S_SLOTS 4
#else
#define I2S_SLOTS 2
#endif
#include "driver/spi_master.h"
#include "esp_lcd_jd9853.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "cheeko-board";

// ---- Pin map — Cheeko Gotchi v1.2 ----
#define PIN_BOOT_BUTTON 0
#define PIN_POWER_OFF 2
#define PIN_POWER_KEY 3
#define PIN_PA_CTRL 4
#define PIN_I2S_MCLK 5
#define PIN_I2S_DOUT 6
#define PIN_I2S_DIN 7
#define PIN_LCD_DC 8
#define PIN_LCD_SCLK 9
#define PIN_LCD_MOSI 10
#define PIN_I2C_SCL 11
#define PIN_I2C_SDA 12
#define PIN_LCD_BL 13
#define PIN_LCD_CS 14
#define PIN_I2S_BCLK 15
#define PIN_I2S_LRCK 16
#define PIN_LCD_RST 17
#define PIN_TOUCH_IRQ 1
#define PIN_CHARGE_DET 37
#define PIN_VOLUME_DOWN 39
#define PIN_VOLUME_UP 40

#define CST810_ADDR 0x15
#define ES8311_ADDR 0x18
#define ES7210_ADDR 0x40
#define LIS2DH12_ADDR 0x19
#define LIS2DH12_WHO_AM_I 0x33
#define LCD_SPI_HOST SPI2_HOST

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_dev_cst810, s_dev_es8311, s_dev_es7210, s_dev_accel;
static i2s_chan_handle_t s_tx_chan, s_rx_chan;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static bool s_audio_ready, s_accel_ready;
static SemaphoreHandle_t s_tx_lock;

// ---- Touch calibration (same NVS blob as hardware_test, so it carries over) ----
static float s_cal[6] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
static bool s_have_cal;

static bool solve3(const double rx[3], const double ry[3], const double s[3], double o[3]) {
  double det = rx[0] * (ry[1] - ry[2]) - ry[0] * (rx[1] - rx[2]) + (rx[1] * ry[2] - rx[2] * ry[1]);
  if (fabs(det) < 1e-6) return false;
  o[0] = (s[0] * (ry[1] - ry[2]) - ry[0] * (s[1] - s[2]) + (s[1] * ry[2] - s[2] * ry[1])) / det;
  o[1] = (rx[0] * (s[1] - s[2]) - s[0] * (rx[1] - rx[2]) + (rx[1] * s[2] - rx[2] * s[1])) / det;
  o[2] = (rx[0] * (ry[1] * s[2] - ry[2] * s[1]) - ry[0] * (rx[1] * s[2] - rx[2] * s[1]) +
          s[0] * (rx[1] * ry[2] - rx[2] * ry[1])) / det;
  return true;
}
static void cal_load(void) {
  nvs_handle_t h;
  if (nvs_open("selftest", NVS_READONLY, &h) != ESP_OK) return;
  size_t sz = sizeof(s_cal);
  if (nvs_get_blob(h, "touchcal", s_cal, &sz) == ESP_OK && sz == sizeof(s_cal)) s_have_cal = true;
  nvs_close(h);
}
static void cal_save(void) {
  nvs_handle_t h;
  if (nvs_open("selftest", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "touchcal", s_cal, sizeof(s_cal));
  nvs_commit(h);
  nvs_close(h);
}

// ---- JD9853 2.01" 240x296 panel init sequence (from hardware_test) ----
static const jd9853_lcd_init_cmd_t JD9853_PANEL_INIT[] = {
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x25}, 1, 0},
    {0xB7, (uint8_t[]){0x00, 0x29, 0x00, 0x51}, 4, 0},
    {0xBB, (uint8_t[]){0x4F, 0x1A, 0x55, 0x73, 0x63, 0xF0}, 6, 0},
    {0xC0, (uint8_t[]){0x44, 0xA4}, 2, 0},
    {0xC1, (uint8_t[]){0x12}, 1, 0},
    {0xC3, (uint8_t[]){0x7D, 0x07, 0x14, 0x06, 0xC8, 0x71, 0x6C, 0x77}, 8, 0},
    {0xC4, (uint8_t[]){0x00, 0x00, 0x94, 0x79, 0x25, 0x0A, 0x16, 0x79, 0x25, 0x0A, 0x16, 0x82}, 12, 0},
    {0xC8, (uint8_t[]){0x3F, 0x34, 0x2D, 0x26, 0x2B, 0x2B, 0x25, 0x24, 0x23, 0x22, 0x20, 0x17, 0x14, 0x0E, 0x06, 0x00,
                       0x3F, 0x34, 0x2D, 0x26, 0x2B, 0x2B, 0x25, 0x24, 0x23, 0x22, 0x20, 0x17, 0x14, 0x0E, 0x06, 0x00}, 32, 0},
    {0xD0, (uint8_t[]){0x04, 0x06, 0x6B, 0x0F, 0x00}, 5, 0},
    {0xD7, (uint8_t[]){0x00, 0x30}, 2, 0},
    {0xE6, (uint8_t[]){0x10}, 1, 0},
    {0xDE, (uint8_t[]){0x01}, 1, 0},
    {0xB7, (uint8_t[]){0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0},
    {0xC1, (uint8_t[]){0x14, 0x15, 0xC0}, 3, 0},
    {0xC2, (uint8_t[]){0x06, 0x3A}, 2, 0},
    {0xC4, (uint8_t[]){0x72, 0x12}, 2, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x05}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x27}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 10},
};

// ---- I2C ----
static i2c_master_dev_handle_t i2c_add(uint8_t addr) {
  i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
  i2c_master_dev_handle_t dev = NULL;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &cfg, &dev));
  return dev;
}
static bool i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
  uint8_t b[2] = {reg, val};
  return i2c_master_transmit(dev, b, 2, 50) == ESP_OK;
}
static bool i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len) {
  return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 50) == ESP_OK;
}
static void i2c_init(void) {
  i2c_master_bus_config_t cfg = {
      .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
      .scl_io_num = PIN_I2C_SCL, .sda_io_num = PIN_I2C_SDA,
      .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
  ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c_bus));
  s_dev_cst810 = i2c_add(CST810_ADDR);
  s_dev_es8311 = i2c_add(ES8311_ADDR);
  s_dev_es7210 = i2c_add(ES7210_ADDR);
  s_dev_accel = i2c_add(LIS2DH12_ADDR);
  const uint8_t want[] = {CST810_ADDR, ES8311_ADDR, LIS2DH12_ADDR, ES7210_ADDR};
  for (size_t i = 0; i < sizeof(want); ++i)
    ESP_LOGI(TAG, "i2c 0x%02X %s", want[i], i2c_master_probe(s_i2c_bus, want[i], 50) == ESP_OK ? "ok" : "MISSING");
#if CHEEKO_DUMP_CODECS_AT_BOOT
  // Read-only snapshot of codec registers as the previous firmware left them
  // (the codecs keep power across an ESP reset/reflash). Bring-up aid.
  for (int dev = 0; dev < 2; ++dev) {
    i2c_master_dev_handle_t h = dev ? s_dev_es8311 : s_dev_es7210;
    const int last = dev ? 0x45 : 0x4f;
    for (int base = 0; base <= last; base += 16) {
      char line[96];
      int o = snprintf(line, sizeof(line), "codec dump %s %02x:", dev ? "es8311" : "es7210", base);
      for (int r = base; r < base + 16 && r <= last; ++r) {
        uint8_t v = 0;
        bool ok = i2c_read_regs(h, (uint8_t)r, &v, 1);
        o += snprintf(line + o, sizeof(line) - o, ok ? " %02x" : " ??", v);
      }
      ESP_LOGI(TAG, "%s", line);
    }
  }
#endif
}
static bool touch_read_raw(int *x, int *y) {
  uint8_t d[5] = {0};
  if (!i2c_read_regs(s_dev_cst810, 0x02, d, sizeof(d))) return false;
  if ((d[0] & 0x0f) == 0) return false;
  *x = ((d[1] & 0x0f) << 8) | d[2];
  *y = ((d[3] & 0x0f) << 8) | d[4];
  return true;
}

// ---- Audio: ES8311 DAC + ES7210 ADC on one I2S bus ----
static bool es8311_init(void) {
  static const uint8_t r[][2] = {
      {0x00, 0x1f}, {0x00, 0x00}, {0x00, 0x80}, {0x01, 0x3f}, {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10},
      {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xff}, {0x09, 0x0c}, {0x0a, 0x0c}, {0x0d, 0x01},
      {0x0e, 0x02}, {0x12, 0x00}, {0x13, 0x10}, {0x1c, 0x6a}, {0x31, 0x00}, {0x32, 0xbf}, {0x37, 0x08}};
  bool ok = true;
  for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); ++i) {
    ok &= i2c_write_reg(s_dev_es8311, r[i][0], r[i][1]);
    if (i == 0) vTaskDelay(pdMS_TO_TICKS(20));
  }
  return ok;
}
static bool es7210_init(void) {
  static const uint8_t r[][2] = {
      {0x00, 0xff}, {0x00, 0x41}, {0x01, 0x3f}, {0x09, 0x30}, {0x0a, 0x30}, {0x23, 0x2a}, {0x22, 0x0a},
      {0x20, 0x0a}, {0x21, 0x2a}, {0x08, 0x00}, {0x40, 0x43}, {0x41, 0x70}, {0x42, 0x70}, {0x07, 0x20},
      {0x02, 0xc1}, {0x11, 0x60}, {0x12, 0x00}, {0x01, 0x34}, {0x06, 0x00}, {0x47, 0x08}, {0x48, 0x08},
      {0x4b, 0x00}, {0x4c, 0xff}, {0x43, 0x1a}, {0x44, 0x1a}, {0x40, 0x43}, {0x00, 0x71}, {0x00, 0x41}};
  bool ok = true;
  for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); ++i) ok &= i2c_write_reg(s_dev_es7210, r[i][0], r[i][1]);
  return ok;
}
static bool audio_init(void) {
  gpio_config_t pa = {.pin_bit_mask = (1ULL << PIN_PA_CTRL), .mode = GPIO_MODE_OUTPUT};
  gpio_config(&pa);
  gpio_set_level(PIN_PA_CTRL, 1);  // idle TX is silenced via auto_clear

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  if (i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan) != ESP_OK) return false;
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CHEEKO_AUDIO_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_LRCK,
                   .dout = PIN_I2S_DOUT, .din = PIN_I2S_DIN, .invert_flags = {0}}};
#if CHEEKO_I2S_TDM
  (void)std;
  i2s_tdm_config_t tdm = {
      .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(CHEEKO_AUDIO_RATE),
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                      I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
      .gpio_cfg = {.mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_LRCK,
                   .dout = PIN_I2S_DOUT, .din = PIN_I2S_DIN, .invert_flags = {0}}};
  tdm.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)CHEEKO_MCLK_MULT;
  tdm.clk_cfg.bclk_div = CHEEKO_MCLK_MULT / 64;       // BCLK = 64 fs
  if (i2s_channel_init_tdm_mode(s_tx_chan, &tdm) != ESP_OK) return false;
  if (i2s_channel_init_tdm_mode(s_rx_chan, &tdm) != ESP_OK) return false;
  ESP_LOGI(TAG, "i2s: TDM 4x16, ws_width %u", (unsigned)tdm.slot_cfg.ws_width);
#else
  if (i2s_channel_init_std_mode(s_tx_chan, &std) != ESP_OK) return false;
  if (i2s_channel_init_std_mode(s_rx_chan, &std) != ESP_OK) return false;
#endif
  if (i2s_channel_enable(s_tx_chan) != ESP_OK) return false;
  if (i2s_channel_enable(s_rx_chan) != ESP_OK) return false;
  bool dac = es8311_init(), adc = es7210_init();
  ESP_LOGI(TAG, "ES8311 %s, ES7210 %s", dac ? "ok" : "FAIL", adc ? "ok" : "FAIL");
  return dac && adc;
}

// ---- Accelerometer ----
static bool accel_init(void) {
  uint8_t who = 0;
  if (!i2c_read_regs(s_dev_accel, 0x0f, &who, 1) || who != LIS2DH12_WHO_AM_I) {
    ESP_LOGW(TAG, "LIS2DH12 not found (WHO_AM_I=0x%02X)", who);
    return false;
  }
  bool ok = i2c_write_reg(s_dev_accel, 0x20, 0x57);  // 100 Hz, XYZ on
  ok &= i2c_write_reg(s_dev_accel, 0x23, 0x88);      // BDU, high-res, +-2 g (1 mg/LSB)
  return ok;
}

// ---- GPIO ----
static void gpio_inputs_init(void) {
  gpio_config_t off = {.pin_bit_mask = (1ULL << PIN_POWER_OFF), .mode = GPIO_MODE_OUTPUT};
  gpio_config(&off);
  gpio_set_level(PIN_POWER_OFF, 0);  // keep unit on

  uint64_t m = (1ULL << PIN_TOUCH_IRQ) | (1ULL << PIN_BOOT_BUTTON) | (1ULL << PIN_VOLUME_DOWN) |
               (1ULL << PIN_VOLUME_UP);
#if !CONFIG_SPIRAM_MODE_OCT
  // GPIO33-37 belong to the in-package octal PSRAM when it is enabled;
  // reconfiguring GPIO37 then crashes the PSRAM bus (WDT boot loop).
  m |= (1ULL << PIN_CHARGE_DET);
#endif
  gpio_config_t in = {.pin_bit_mask = m, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
  gpio_config(&in);
  // POWER_KEY (GPIO3) is active-high with an external pull-down: no internal pull-up.
  gpio_config_t pwr = {.pin_bit_mask = (1ULL << PIN_POWER_KEY), .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE};
  gpio_config(&pwr);
}

// ---- Display ----
static void display_init(void) {
  gpio_config_t bl = {.pin_bit_mask = (1ULL << PIN_LCD_BL), .mode = GPIO_MODE_OUTPUT};
  gpio_config(&bl);
  gpio_set_level(PIN_LCD_BL, 0);

  spi_bus_config_t bus = JD9853_PANEL_BUS_SPI_CONFIG(PIN_LCD_SCLK, PIN_LCD_MOSI, CHEEKO_LCD_W * 40 * 2);
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
  esp_lcd_panel_io_spi_config_t io_cfg = JD9853_PANEL_IO_SPI_CONFIG(PIN_LCD_CS, PIN_LCD_DC, NULL, NULL);
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));

  jd9853_vendor_config_t vendor = {.init_cmds = JD9853_PANEL_INIT,
                                   .init_cmds_size = sizeof(JD9853_PANEL_INIT) / sizeof(JD9853_PANEL_INIT[0])};
  esp_lcd_panel_dev_config_t panel_cfg = {
      .reset_gpio_num = PIN_LCD_RST, .rgb_endian = LCD_RGB_ENDIAN_BGR, .bits_per_pixel = 16, .vendor_config = &vendor};
  ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(s_io, &panel_cfg, &s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  port_cfg.task_priority = 4;
  port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
  port_cfg.task_affinity = 1;
#endif
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
  lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = s_io, .panel_handle = s_panel,
      .buffer_size = CHEEKO_LCD_W * 20, .double_buffer = false,
      .hres = CHEEKO_LCD_W, .vres = CHEEKO_LCD_H, .monochrome = false,
      .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
      .color_format = LV_COLOR_FORMAT_RGB565,
      .flags = {.buff_dma = 1, .swap_bytes = 1},
  };
  s_disp = lvgl_port_add_disp(&disp_cfg);
  if (lvgl_port_lock(0)) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lvgl_port_unlock();
  }
  gpio_set_level(PIN_LCD_BL, 1);
}

// ---- Touch calibration UI (from hardware_test) ----
static void run_calibration(void) {
  const int tgt[3][2] = {{30, 40}, {210, 40}, {120, 256}};
  double rx[3], ry[3], sxv[3], syv[3];
  lv_obj_t *dot = NULL, *info = NULL;
  if (lvgl_port_lock(0)) {
    info = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(info, lv_color_hex(0xFFD166), 0);
    lv_obj_set_pos(info, 24, 130);
    dot = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 20, 20);
    lv_obj_set_style_radius(dot, 10, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x33DD66), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lvgl_port_unlock();
  }
  int rxr, ryr;
  for (int i = 0; i < 3; ++i) {
    if (lvgl_port_lock(0)) {
      lv_obj_set_pos(dot, tgt[i][0] - 10, tgt[i][1] - 10);
      lv_label_set_text_fmt(info, "TOUCH THE DOT  %d/3", i + 1);
      lvgl_port_unlock();
    }
    while (touch_read_raw(&rxr, &ryr)) vTaskDelay(pdMS_TO_TICKS(20));
    while (!touch_read_raw(&rxr, &ryr)) vTaskDelay(pdMS_TO_TICKS(20));
    long ax = 0, ay = 0;
    int cnt = 0;
    for (int k = 0; k < 10; ++k) {
      int xx, yy;
      if (touch_read_raw(&xx, &yy)) { ax += xx; ay += yy; cnt++; }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (cnt == 0) cnt = 1;
    rx[i] = (double)ax / cnt; ry[i] = (double)ay / cnt;
    sxv[i] = tgt[i][0]; syv[i] = tgt[i][1];
    while (touch_read_raw(&rxr, &ryr)) vTaskDelay(pdMS_TO_TICKS(20));
  }
  double cx[3], cy[3];
  if (solve3(rx, ry, sxv, cx) && solve3(rx, ry, syv, cy)) {
    s_cal[0] = cx[0]; s_cal[1] = cx[1]; s_cal[2] = cx[2];
    s_cal[3] = cy[0]; s_cal[4] = cy[1]; s_cal[5] = cy[2];
    s_have_cal = true;
    cal_save();
  } else {
    ESP_LOGW(TAG, "calibration solve failed");
  }
  if (lvgl_port_lock(0)) { lv_obj_delete(dot); lv_obj_delete(info); lvgl_port_unlock(); }
}

// ---- Public ----
void cheeko_board_init(void) {
  esp_err_t nerr = nvs_flash_init();
  if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  gpio_inputs_init();
  i2c_init();
  display_init();

  cal_load();
  bool force_cal = gpio_get_level(PIN_BOOT_BUTTON) == 0;
  ESP_LOGI(TAG, "touch cal %s%s", s_have_cal ? "loaded" : "missing", force_cal ? " (BOOT held: recalibrating)" : "");
  if (!s_have_cal || force_cal) run_calibration();

  s_tx_lock = xSemaphoreCreateMutex();  // before any task can play audio
  s_audio_ready = audio_init();
  s_accel_ready = accel_init();
  ESP_LOGI(TAG, "audio=%d accel=%d", s_audio_ready, s_accel_ready);
}

lv_display_t *cheeko_board_display(void) { return s_disp; }
bool cheeko_board_audio_ready(void) { return s_audio_ready; }
bool cheeko_board_accel_ready(void) { return s_accel_ready; }

volatile uint32_t g_cheeko_touch_i2c_err;  // diagnostics

bool cheeko_board_touch_read(int *x, int *y) {
  int rx, ry;
  uint8_t d[5] = {0};
  if (!i2c_read_regs(s_dev_cst810, 0x02, d, sizeof(d))) { g_cheeko_touch_i2c_err++; return false; }
  if ((d[0] & 0x0f) == 0) return false;
  rx = ((d[1] & 0x0f) << 8) | d[2];
  ry = ((d[3] & 0x0f) << 8) | d[4];
  int sx = (int)(s_cal[0] * rx + s_cal[1] * ry + s_cal[2] + 0.5f);
  int sy = (int)(s_cal[3] * rx + s_cal[4] * ry + s_cal[5] + 0.5f);
  *x = imin(imax(sx, 0), CHEEKO_LCD_W - 1);
  *y = imin(imax(sy, 0), CHEEKO_LCD_H - 1);
  return true;
}

void cheeko_board_buttons_read(bool down[CHEEKO_BTN_COUNT]) {
  down[CHEEKO_BTN_BOOT] = gpio_get_level(PIN_BOOT_BUTTON) == 0;
  down[CHEEKO_BTN_POWER] = gpio_get_level(PIN_POWER_KEY) == 1;
  down[CHEEKO_BTN_VOL_UP] = gpio_get_level(PIN_VOLUME_UP) == 0;
  down[CHEEKO_BTN_VOL_DOWN] = gpio_get_level(PIN_VOLUME_DOWN) == 0;
}

// Tones (main task) and PCM (runtime speaker task) share the TX channel.
static void tx_lock(void) { xSemaphoreTake(s_tx_lock, portMAX_DELAY); }
static void tx_unlock(void) { xSemaphoreGive(s_tx_lock); }

void cheeko_board_tone(float freq, int ms, int amp) {
  if (!s_audio_ready || ms <= 0) return;
  int16_t buf[64 * I2S_SLOTS] = {0};
  size_t w = 0;
  int total = (CHEEKO_AUDIO_RATE * ms) / 1000;
  tx_lock();
  for (int f = 0; f < total; f += 64) {
    int n = imin(64, total - f);
    for (int i = 0; i < n; ++i) {
      float t = (float)(f + i) / CHEEKO_AUDIO_RATE;
      float env = 1.0f - ((float)(f + i) / imax(1, total));
      int16_t v = (int16_t)(sinf(2.0f * (float)M_PI * freq * t) * env * amp);
      buf[i * I2S_SLOTS] = v; buf[i * I2S_SLOTS + I2S_SLOTS / 2] = v;  // L, R
    }
    i2s_channel_write(s_tx_chan, buf, n * I2S_SLOTS * sizeof(int16_t), &w, portMAX_DELAY);
  }
  tx_unlock();
}

void cheeko_board_pcm_write(const int16_t *mono, int frames, int gain_q8) {
  if (!s_audio_ready || frames <= 0) return;
  static int16_t buf[128 * I2S_SLOTS];  // zeroed; only L/R slots written
  size_t w = 0;
  tx_lock();
  for (int f = 0; f < frames; f += 128) {
    int n = imin(128, frames - f);
    for (int i = 0; i < n; ++i) {
      int v = (mono[f + i] * gain_q8) >> 8;
      v = imax(-32768, imin(32767, v));
      buf[i * I2S_SLOTS] = (int16_t)v; buf[i * I2S_SLOTS + I2S_SLOTS / 2] = (int16_t)v;
    }
    i2s_channel_write(s_tx_chan, buf, n * I2S_SLOTS * sizeof(int16_t), &w, portMAX_DELAY);
  }
  tx_unlock();
}

bool cheeko_board_codec_reg_read(int dev, uint8_t reg, uint8_t *val) {
  return i2c_read_regs(dev ? s_dev_es8311 : s_dev_es7210, reg, val, 1);
}
bool cheeko_board_codec_reg_write(int dev, uint8_t reg, uint8_t val) {
  return i2c_write_reg(dev ? s_dev_es8311 : s_dev_es7210, reg, val);
}

// Bring-up: feed I2S0 RX from our own DOUT pad (1) or the real DIN (0).
void cheeko_board_i2s_loopback(bool on) {
  gpio_input_enable(on ? PIN_I2S_DOUT : PIN_I2S_DIN);
  esp_rom_gpio_connect_in_signal(on ? PIN_I2S_DOUT : PIN_I2S_DIN, I2S0I_SD_IN_IDX, false);
  ESP_LOGI(TAG, "i2s rx <- %s", on ? "DOUT loopback" : "DIN (ES7210)");
}

// Which RX slot carries MIC1. Settable at runtime for bring-up (@@MICSLOT).
static int s_mic_slot = 0;
void cheeko_board_mic_slot(int slot) { s_mic_slot = slot < 0 ? 0 : slot >= I2S_SLOTS ? I2S_SLOTS - 1 : slot; }

int cheeko_board_mic_read(int16_t *mono, int max_frames) {
  if (!s_audio_ready) return -1;
  static int16_t st[256 * I2S_SLOTS];  // static: keep it off small task stacks
  int frames = imin(max_frames, 256);
  size_t n = 0;
  esp_err_t err = i2s_channel_read(s_rx_chan, st, frames * I2S_SLOTS * sizeof(int16_t), &n, pdMS_TO_TICKS(40));
  if (err != ESP_OK) {
    static int errs = 0;
    if (errs++ < 3) ESP_LOGW(TAG, "mic read: %s (%u bytes)", esp_err_to_name(err), (unsigned)n);
    return -1;
  }
  int got = (int)(n / (I2S_SLOTS * sizeof(int16_t)));
  static int dumped = 0;
  if (dumped < 2 && got >= 24) {  // raw slots of the first frames, for bring-up
    ++dumped;
    static char line[480];
    int o = snprintf(line, sizeof(line), "mic raw %d slots:", I2S_SLOTS);
    for (int i = 0; i < 24 && o < (int)sizeof(line) - 32; ++i) {
      o += snprintf(line + o, sizeof(line) - o, " ");
      for (int k = 0; k < I2S_SLOTS; ++k)
        o += snprintf(line + o, sizeof(line) - o, k ? "/%d" : "%d", st[i * I2S_SLOTS + k]);
    }
    ESP_LOGI(TAG, "%s", line);
  }
  for (int i = 0; i < got; ++i) mono[i] = st[i * I2S_SLOTS + s_mic_slot];
  return got;
}

bool cheeko_board_accel_read_mg(int *x, int *y, int *z) {
  uint8_t raw[6] = {0};
  if (!s_accel_ready || !i2c_read_regs(s_dev_accel, 0x28 | 0x80, raw, sizeof(raw))) { *x = *y = *z = 0; return false; }
  *x = (int16_t)((raw[1] << 8) | raw[0]) >> 4;
  *y = (int16_t)((raw[3] << 8) | raw[2]) >> 4;
  *z = (int16_t)((raw[5] << 8) | raw[4]) >> 4;
  return true;
}
