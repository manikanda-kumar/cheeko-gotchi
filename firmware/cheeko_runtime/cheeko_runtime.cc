// cheeko_runtime — implementation of sdk/include/cheeko.h on the Cheeko Gotchi
// board layer (cheeko_board.c). Display calls draw immediate-mode into a
// full-screen LVGL canvas (PSRAM when available); LVGL flushes it to the LCD.
//
// Implemented: display, speaker tones/volume/PCM (async task), mic PCM frames, touch, buttons,
// motion + shake, NVS storage, log, lifecycle (OnStart/OnTick/...).
// Cloud = USB serial link to tools/voice_agent (cheeko_link.cc).
// Stubbed (log a warning): Display::Image, Speaker::Play, Wifi.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "cheeko_board.h"
#include "cheeko_host.h"
#include "cheeko_link.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"

namespace cheeko {
namespace {

const char* TAG = "cheeko";
const char* APP_TAG = "app";

lv_obj_t* g_canvas = nullptr;
int g_volume = 50;
bool g_mic_on = false;
TouchEvent g_touch;
MotionSample g_motion;
bool g_shake_pending = false;

const lv_font_t* Font() {
#if CONFIG_LV_FONT_MONTSERRAT_20
  return &lv_font_montserrat_20;
#else
  return LV_FONT_DEFAULT;
#endif
}

lv_color_t C(uint32_t rgb) { return lv_color_hex(rgb); }

// Run fn(layer) on the canvas under the LVGL lock, then invalidate only
// `dirty` (screen coords) so LVGL re-sends just that region to the LCD.
// Same dispatch loop as lv_canvas_finish_layer(), minus its full invalidate.
template <typename F>
void Draw(lv_area_t dirty, F&& fn) {
  if (!g_canvas || !lvgl_port_lock(0)) return;
  lv_layer_t layer;
  lv_canvas_init_layer(g_canvas, &layer);
  fn(layer);
  while (layer.draw_task_head) {
    lv_draw_dispatch_wait_for_request();
    if (!lv_draw_dispatch_layer(lv_obj_get_display(g_canvas), &layer)) {
      lv_draw_wait_for_finish();
      lv_draw_dispatch_request();
    }
  }
  if (dirty.x1 < 0) dirty.x1 = 0;
  if (dirty.y1 < 0) dirty.y1 = 0;
  if (dirty.x2 > CHEEKO_LCD_W - 1) dirty.x2 = CHEEKO_LCD_W - 1;
  if (dirty.y2 > CHEEKO_LCD_H - 1) dirty.y2 = CHEEKO_LCD_H - 1;
  if (dirty.x1 <= dirty.x2 && dirty.y1 <= dirty.y2) lv_obj_invalidate_area(g_canvas, &dirty);
  lvgl_port_unlock();
}

void DrawText(int x, int y, const std::string& text) {
  lv_point_t sz;
  lv_text_get_size(&sz, text.c_str(), Font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  Draw({x, y, x + sz.x, y + sz.y}, [&](lv_layer_t& layer) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = C(Color::Ink);
    d.font = Font();
    d.text = text.c_str();
    lv_area_t a = {x, y, x + sz.x, y + sz.y};
    lv_draw_label(&layer, &d, &a);
  });
}

int TextWidth(const std::string& text) {
  lv_point_t sz;
  lv_text_get_size(&sz, text.c_str(), Font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return sz.x;
}

void CreateCanvas() {
  const size_t bytes = CHEEKO_LCD_W * CHEEKO_LCD_H * 2;
  void* buf = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes, MALLOC_CAP_SPIRAM);
  if (!buf) buf = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) {
    ESP_LOGE(TAG, "no memory for %u-byte canvas; display disabled", (unsigned)bytes);
    return;
  }
  if (lvgl_port_lock(0)) {
    g_canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(g_canvas, buf, CHEEKO_LCD_W, CHEEKO_LCD_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(g_canvas, 0, 0);
    lv_canvas_fill_bg(g_canvas, lv_color_black(), LV_OPA_COVER);
    lvgl_port_unlock();
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void Display::Clear(uint32_t rgb) {
  if (!g_canvas || !lvgl_port_lock(0)) return;
  lv_canvas_fill_bg(g_canvas, C(rgb), LV_OPA_COVER);
  lv_obj_invalidate(g_canvas);
  lvgl_port_unlock();
}

void Display::Text(int x, int y, const std::string& text) { DrawText(x, y, text); }

void Display::CenterText(int y, const std::string& text) {
  DrawText((CHEEKO_LCD_W - TextWidth(text)) / 2, y, text);
}

void Display::Rect(int x, int y, int w, int h, uint32_t rgb) {
  Draw({x, y, x + w - 1, y + h - 1}, [&](lv_layer_t& layer) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = C(rgb);
    d.border_width = 1;
    d.border_opa = LV_OPA_COVER;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(&layer, &d, &a);
  });
}

void Display::FillRect(int x, int y, int w, int h, uint32_t rgb) {
  Draw({x, y, x + w - 1, y + h - 1}, [&](lv_layer_t& layer) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = C(rgb);
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(&layer, &d, &a);
  });
}

void Display::Circle(int x, int y, int radius, uint32_t rgb) {
  Draw({x - radius - 2, y - radius - 2, x + radius + 2, y + radius + 2}, [&](lv_layer_t& layer) {
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = C(rgb);
    d.width = 2;
    d.center.x = x;
    d.center.y = y;
    d.radius = radius;
    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(&layer, &d);
  });
}

void Display::FillCircle(int x, int y, int radius, uint32_t rgb) {
  Draw({x - radius, y - radius, x + radius, y + radius}, [&](lv_layer_t& layer) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = C(rgb);
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {x - radius, y - radius, x + radius, y + radius};
    lv_draw_rect(&layer, &d, &a);
  });
}

void Display::Line(int x1, int y1, int x2, int y2, uint32_t rgb) {
  lv_area_t a = {std::min(x1, x2) - 2, std::min(y1, y2) - 2, std::max(x1, x2) + 2, std::max(y1, y2) + 2};
  Draw(a, [&](lv_layer_t& layer) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = C(rgb);
    d.width = 2;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(&layer, &d);
  });
}

void Display::Image(const std::string& path) {
  ESP_LOGW(TAG, "Display::Image(%s) not supported yet", path.c_str());
}

// ---------------------------------------------------------------------------
// Speaker / Microphone
// ---------------------------------------------------------------------------
void Speaker::Play(const std::string& path) {
  ESP_LOGW(TAG, "Speaker::Play(%s) not supported yet", path.c_str());
}
void Speaker::Tone(int frequency_hz, int duration_ms) {
  // volume 50 == the hardware_test tone level (amp 7000).
  cheeko_board_tone((float)frequency_hz, duration_ms, g_volume * 140);
}
void Speaker::SetVolume(int volume) { g_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume; }

namespace {
struct PcmJob {
  int16_t* samples;
  size_t count;
};
QueueHandle_t g_pcm_queue = nullptr;
std::atomic<int> g_pcm_pending{0};

void SpeakerTask(void*) {
  PcmJob job;
  for (;;) {
    if (xQueueReceive(g_pcm_queue, &job, portMAX_DELAY) != pdTRUE) continue;
    // volume 50 == unity gain (matches the tone level scale).
    cheeko_board_pcm_write(job.samples, (int)job.count, g_volume * 256 / 50);
    heap_caps_free(job.samples);
    g_pcm_pending.fetch_sub(1);
  }
}

void StartSpeakerTask() {
  g_pcm_queue = xQueueCreate(4, sizeof(PcmJob));
  xTaskCreatePinnedToCore(SpeakerTask, "cheeko_spk", 4096, nullptr, 5, nullptr, 0);
}
}  // namespace

bool Speaker::PlayPcm(const int16_t* samples, size_t count, int rate) {
  if (!g_pcm_queue || !samples || count == 0 || rate <= 0) return false;
  // Linear resample to the native rate.
  const size_t out_n = (size_t)((uint64_t)count * CHEEKO_AUDIO_RATE / (uint64_t)rate);
  if (out_n == 0) return false;
  auto* buf = static_cast<int16_t*>(heap_caps_malloc(out_n * sizeof(int16_t), MALLOC_CAP_DEFAULT));
  if (!buf) return false;
  if (rate == CHEEKO_AUDIO_RATE) {
    memcpy(buf, samples, out_n * sizeof(int16_t));
  } else {
    const double step = (double)rate / CHEEKO_AUDIO_RATE;
    for (size_t i = 0; i < out_n; ++i) {
      double src = i * step;
      size_t a = (size_t)src;
      size_t b = a + 1 < count ? a + 1 : a;
      double fr = src - a;
      buf[i] = (int16_t)(samples[a] * (1.0 - fr) + samples[b] * fr);
    }
  }
  PcmJob job{buf, out_n};
  g_pcm_pending.fetch_add(1);
  if (xQueueSend(g_pcm_queue, &job, 0) != pdTRUE) {
    g_pcm_pending.fetch_sub(1);
    heap_caps_free(buf);
    return false;
  }
  return true;
}

bool Speaker::IsPlaying() { return g_pcm_pending > 0; }

void Microphone::Start(int sample_rate_hz) {
  if (sample_rate_hz != CHEEKO_AUDIO_RATE)
    ESP_LOGW(TAG, "mic: %d Hz requested, delivering %d Hz", sample_rate_hz, CHEEKO_AUDIO_RATE);
  g_mic_on = true;
}
void Microphone::Stop() { g_mic_on = false; }

// ---------------------------------------------------------------------------
// Touch / Motion
// ---------------------------------------------------------------------------
TouchEvent Touch::Get() { return g_touch; }
bool Touch::IsPressed() { return g_touch.pressed; }

MotionSample Motion::Read() { return g_motion; }
bool Motion::IsShaken() {
  bool s = g_shake_pending;
  g_shake_pending = false;
  return s;
}

// ---------------------------------------------------------------------------
// Storage (NVS namespace "cheeko_app"; keys max 15 chars)
// ---------------------------------------------------------------------------
namespace {
bool OpenStore(nvs_open_mode_t mode, nvs_handle_t* h) { return nvs_open("cheeko_app", mode, h) == ESP_OK; }
}  // namespace

void Storage::PutInt(const std::string& key, int value) {
  nvs_handle_t h;
  if (!OpenStore(NVS_READWRITE, &h)) return;
  nvs_set_i32(h, key.c_str(), value);
  nvs_commit(h);
  nvs_close(h);
}
int Storage::GetInt(const std::string& key, int fallback) {
  nvs_handle_t h;
  int32_t v = fallback;
  if (!OpenStore(NVS_READONLY, &h)) return fallback;
  if (nvs_get_i32(h, key.c_str(), &v) != ESP_OK) v = fallback;
  nvs_close(h);
  return v;
}
void Storage::PutString(const std::string& key, const std::string& value) {
  nvs_handle_t h;
  if (!OpenStore(NVS_READWRITE, &h)) return;
  nvs_set_str(h, key.c_str(), value.c_str());
  nvs_commit(h);
  nvs_close(h);
}
std::string Storage::GetString(const std::string& key, const std::string& fallback) {
  nvs_handle_t h;
  if (!OpenStore(NVS_READONLY, &h)) return fallback;
  size_t len = 0;
  std::string out = fallback;
  if (nvs_get_str(h, key.c_str(), nullptr, &len) == ESP_OK && len > 0) {
    out.resize(len);
    nvs_get_str(h, key.c_str(), &out[0], &len);
    out.resize(len - 1);
  }
  nvs_close(h);
  return out;
}
bool Storage::Has(const std::string& key) {
  nvs_handle_t h;
  if (!OpenStore(NVS_READONLY, &h)) return false;
  nvs_type_t t;
  bool found = nvs_find_key(h, key.c_str(), &t) == ESP_OK;
  nvs_close(h);
  return found;
}
void Storage::Remove(const std::string& key) {
  nvs_handle_t h;
  if (!OpenStore(NVS_READWRITE, &h)) return;
  nvs_erase_key(h, key.c_str());
  nvs_commit(h);
  nvs_close(h);
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
void Log::Info(const std::string& m) { ESP_LOGI(APP_TAG, "%s", m.c_str()); }
void Log::Warn(const std::string& m) { ESP_LOGW(APP_TAG, "%s", m.c_str()); }
void Log::Error(const std::string& m) { ESP_LOGE(APP_TAG, "%s", m.c_str()); }

// ---------------------------------------------------------------------------
// Cloud — USB serial link to the host voice bridge. Wi-Fi not wired yet.
// ---------------------------------------------------------------------------
void Cloud::Connect() { link::Start(); }
void Cloud::StartVoiceSession() {
  link::Start();
  link::Begin();
}
void Cloud::EndVoiceTurn() { link::End(); }
bool Cloud::IsLinked() { return link::Linked(); }
void Cloud::SendText(const std::string&) { ESP_LOGW(TAG, "Cloud::SendText not implemented"); }
void Cloud::GetJson(const std::string& url) { ESP_LOGW(TAG, "Cloud::GetJson(%s) not implemented", url.c_str()); }
void Cloud::PostJson(const std::string& url, const std::string&) {
  ESP_LOGW(TAG, "Cloud::PostJson(%s) not implemented", url.c_str());
}
void Wifi::Connect() { ESP_LOGW(TAG, "Wifi::Connect not implemented"); }
bool Wifi::IsConnected() const { return false; }

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
Display& CheekoRuntime::display() { static Display d; return d; }
Speaker& CheekoRuntime::speaker() { static Speaker s; return s; }
Microphone& CheekoRuntime::mic() { static Microphone m; return m; }
Touch& CheekoRuntime::touch() { static Touch t; return t; }
Motion& CheekoRuntime::motion() { static Motion m; return m; }
Storage& CheekoRuntime::storage() { static Storage s; return s; }
Log& CheekoRuntime::log() { static Log l; return l; }
Cloud& CheekoRuntime::cloud() { static Cloud c; return c; }
Wifi& CheekoRuntime::wifi() { static Wifi w; return w; }

CheekoRuntime& Cheeko() {
  static CheekoRuntime rt;
  return rt;
}

extern "C" volatile uint32_t g_cheeko_touch_i2c_err;

namespace {
// Touch is sampled by its own task so a slow OnTick/draw can't swallow a tap:
// every press edge, move and release is queued and delivered in order.
QueueHandle_t g_touch_queue = nullptr;

void TouchTask(void*) {
  bool was = false;
  int lx = -1, ly = -1;
  for (;;) {
    int x, y;
    const bool now = cheeko_board_touch_read(&x, &y);
    TouchEvent ev;
    if (now && (!was || x != lx || y != ly)) {
      ev.x = x; ev.y = y; ev.pressed = true;
      if (!was) {
        ESP_LOGI(TAG, "touch down %d,%d", x, y);
        if (xQueueSend(g_touch_queue, &ev, pdMS_TO_TICKS(5)) != pdTRUE) ESP_LOGW(TAG, "touch queue full");
      }
      else xQueueSend(g_touch_queue, &ev, 0);                           // moves may drop
      lx = x; ly = y;
    } else if (!now && was) {
      ev.x = lx; ev.y = ly; ev.pressed = false;
      ESP_LOGI(TAG, "touch up");
      xQueueSend(g_touch_queue, &ev, pdMS_TO_TICKS(5));
    }
    was = now;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void StartTouchTask() {
  g_touch_queue = xQueueCreate(32, sizeof(TouchEvent));
  xTaskCreatePinnedToCore(TouchTask, "cheeko_touch", 3072, nullptr, 6, nullptr, 0);
}
}  // namespace

namespace host {

void Run(CheekoApp* app) {
  ESP_LOGI(TAG, "cheeko_runtime, SDK %s", CHEEKO_SDK_VERSION_STRING);
  cheeko_board_init();
  CreateCanvas();
  StartSpeakerTask();
  StartTouchTask();

  app->OnStart();

  constexpr ButtonEvent::Button kButtons[CHEEKO_BTN_COUNT] = {
      ButtonEvent::Button::Boot, ButtonEvent::Button::Power, ButtonEvent::Button::VolumeUp,
      ButtonEvent::Button::VolumeDown};
  bool prev_btn[CHEEKO_BTN_COUNT] = {};
  int64_t last_shake_ms = 0;
  int16_t pcm[256];
  int64_t last_stats_ms = 0, max_tick_us = 0;
  int ticks = 0;

  for (;;) {
    const int64_t now_ms = esp_timer_get_time() / 1000;

    bool down[CHEEKO_BTN_COUNT];
    cheeko_board_buttons_read(down);
    for (int i = 0; i < CHEEKO_BTN_COUNT; ++i) {
      if (down[i] != prev_btn[i]) {
        ButtonEvent ev;
        ev.button = kButtons[i];
        ev.pressed = down[i];
        app->OnButton(ev);
        prev_btn[i] = down[i];
      }
    }

    TouchEvent tev;
    while (xQueueReceive(g_touch_queue, &tev, 0) == pdTRUE) {
      g_touch = tev;
      app->OnTouch(tev);
    }

    int ax, ay, az;
    if (cheeko_board_accel_read_mg(&ax, &ay, &az)) {
      g_motion.x = ax / 1000.0f;
      g_motion.y = ay / 1000.0f;
      g_motion.z = az / 1000.0f;
      float mag = std::sqrt((float)ax * ax + (float)ay * ay + (float)az * az);
      if (std::fabs(mag - 1000.0f) > 800.0f && now_ms - last_shake_ms > 700) {
        last_shake_ms = now_ms;
        g_shake_pending = true;
        app->OnShake();
      }
    }

    const int64_t tick_start_us = esp_timer_get_time();
    app->OnTick((uint32_t)now_ms);
    const int64_t tick_us = esp_timer_get_time() - tick_start_us;
    if (tick_us > max_tick_us) max_tick_us = tick_us;
    ++ticks;
    if (now_ms - last_stats_ms >= 5000) {
      ESP_LOGI(TAG, "loop: %d ticks/5s, slowest OnTick %lld ms, touch i2c errors %u", ticks,
               max_tick_us / 1000, (unsigned)g_cheeko_touch_i2c_err);
      ticks = 0;
      max_tick_us = 0;
      last_stats_ms = now_ms;
    }

    link::Event lev;
    while (link::Poll(&lev)) {
      if (lev.kind == link::Event::Kind::State) app->OnVoiceState(lev.state);
      else app->OnCloudText(lev.text);
    }

    // While a voice turn streams, the link's mic task owns the I2S RX channel.
    if (g_mic_on && !link::Streaming()) {
      // Blocking I2S reads (~16 ms each) pace the loop while the mic is on.
      int n = cheeko_board_mic_read(pcm, 256);
      if (n > 0) {
        AudioFrame f;
        f.samples = pcm;
        f.sample_count = (size_t)n;
        f.sample_rate_hz = CHEEKO_AUDIO_RATE;
        f.channels = 1;
        app->OnMicAudio(f);
      }
      if (n <= 0) vTaskDelay(pdMS_TO_TICKS(20));
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

}  // namespace host
}  // namespace cheeko
