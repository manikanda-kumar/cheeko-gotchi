// cheeko_link — see cheeko_link.h for the wire protocol.
#include "cheeko_link.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cheeko_board.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace cheeko {
namespace link {
namespace {

const char* TAG = "link";
constexpr size_t kLineMax = 4096;
constexpr size_t kPcmChunk = CHEEKO_AUDIO_RATE / 2;  // play in 0.5 s slices

std::atomic<bool> g_started{false};
std::atomic<int64_t> g_last_hi_ms{-1};  // host sends @@HI every ~2 s
constexpr int64_t kLinkTimeoutMs = 6000;
std::atomic<bool> g_streaming{false};
std::atomic<int64_t> g_rec_until_ms{0};  // host-requested test recording (@@REC)
QueueHandle_t g_events = nullptr;  // Event*
int16_t* g_pcm = nullptr;
size_t g_pcm_n = 0;

// One fwrite per line: newlib's stdout lock keeps it whole against ESP_LOG.
void WriteLine(const char* line, size_t len) {
  fwrite(line, 1, len, stdout);
  fflush(stdout);
}

void SendHello() {
  char line[48];
  int n = snprintf(line, sizeof(line), "@@HELLO %s\n", CHEEKO_SDK_VERSION_STRING);
  WriteLine(line, (size_t)n);
}

void PushEvent(Event* ev) {
  if (xQueueSend(g_events, &ev, pdMS_TO_TICKS(50)) != pdTRUE) {
    ESP_LOGW(TAG, "event queue full");
    delete ev;
  }
}

void FlushPcm() {
  if (g_pcm_n == 0) return;
  // The speaker queue holds a few slices; wait for room rather than drop.
  // Blocking here back-pressures the host through USB flow control.
  for (int tries = 0; tries < 400; ++tries) {
    if (Cheeko().speaker().PlayPcm(g_pcm, g_pcm_n, CHEEKO_AUDIO_RATE)) break;
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  g_pcm_n = 0;
}

void OnPcm(const char* b64, size_t len) {
  static uint8_t raw[kLineMax];
  size_t out = 0;
  if (mbedtls_base64_decode(raw, sizeof(raw), &out, (const unsigned char*)b64, len) != 0) {
    ESP_LOGW(TAG, "bad PCM base64 (%u chars)", (unsigned)len);
    return;
  }
  const int16_t* s = reinterpret_cast<const int16_t*>(raw);
  size_t n = out / 2;
  while (n > 0) {
    size_t take = std::min(n, kPcmChunk - g_pcm_n);
    memcpy(g_pcm + g_pcm_n, s, take * sizeof(int16_t));
    g_pcm_n += take;
    s += take;
    n -= take;
    if (g_pcm_n == kPcmChunk) FlushPcm();
  }
}

bool ParseState(const char* s, VoiceState* out) {
  static const struct { const char* name; VoiceState st; } kStates[] = {
      {"idle", VoiceState::Idle},         {"listening", VoiceState::Listening},
      {"thinking", VoiceState::Thinking}, {"researching", VoiceState::Researching},
      {"speaking", VoiceState::Speaking}, {"error", VoiceState::Error},
  };
  for (const auto& k : kStates)
    if (strcmp(s, k.name) == 0) { *out = k.st; return true; }
  return false;
}

void HandleLine(char* line, size_t len) {
  while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
  if (len < 4 || line[0] != '@' || line[1] != '@') return;
  const char* cmd = line + 2;
  if (strncmp(cmd, "PCM ", 4) == 0) {
    OnPcm(cmd + 4, len - 6);
  } else if (strcmp(cmd, "PCMEND") == 0) {
    FlushPcm();
  } else if (strncmp(cmd, "STATE ", 6) == 0) {
    VoiceState st;
    if (ParseState(cmd + 6, &st)) PushEvent(new Event{Event::Kind::State, st, {}});
  } else if (strncmp(cmd, "TEXT ", 5) == 0) {
    PushEvent(new Event{Event::Kind::Text, VoiceState::Idle, std::string(cmd + 5)});
  } else if (strncmp(cmd, "REC ", 4) == 0) {
    // Mic self-test from the host: stream for N ms, no touch needed.
    const int ms = std::max(100, std::min(atoi(cmd + 4), 10000));
    ESP_LOGI(TAG, "host mic test: %d ms", ms);
    g_rec_until_ms = esp_timer_get_time() / 1000 + ms;
    Begin();
  } else if (strncmp(cmd, "LOOP ", 5) == 0) {
    cheeko_board_i2s_loopback(atoi(cmd + 5) != 0);
  } else if (strncmp(cmd, "TONE ", 5) == 0) {
    int hz = 0, ms = 0;
    if (sscanf(cmd + 5, "%d %d", &hz, &ms) == 2) cheeko_board_tone((float)hz, ms, 7000);
  } else if (strncmp(cmd, "MICSLOT ", 8) == 0) {
    cheeko_board_mic_slot(atoi(cmd + 8));
    ESP_LOGI(TAG, "mic slot %d", atoi(cmd + 8));
  } else if (strncmp(cmd, "REG ", 4) == 0) {
    // Debug: "@@REG <dev> <reg> [val]" (hex). dev 0 = ES7210, 1 = ES8311.
    // No val: dump 16 regs from <reg>. With val: write then read back.
    unsigned dev = 0, reg = 0, val = 0;
    const int k = sscanf(cmd + 4, "%x %x %x", &dev, &reg, &val);
    if (k == 3) {
      bool ok = cheeko_board_codec_reg_write((int)dev, (uint8_t)reg, (uint8_t)val);
      uint8_t rb = 0;
      cheeko_board_codec_reg_read((int)dev, (uint8_t)reg, &rb);
      ESP_LOGI(TAG, "reg dev%u %02x <- %02x %s, reads %02x", dev, reg, val, ok ? "ok" : "FAIL", rb);
    } else if (k == 2) {
      char out[128];
      int o = snprintf(out, sizeof(out), "regs dev%u %02x:", dev, reg);
      for (unsigned r = reg; r < reg + 16 && r < 0x100; ++r) {
        uint8_t v = 0;
        bool ok = cheeko_board_codec_reg_read((int)dev, (uint8_t)r, &v);
        o += snprintf(out + o, sizeof(out) - o, ok ? " %02x" : " ??", v);
      }
      ESP_LOGI(TAG, "%s", out);
    }
  } else if (strcmp(cmd, "HI") == 0) {
    const int64_t now = esp_timer_get_time() / 1000;
    const int64_t last = g_last_hi_ms.exchange(now);
    if (last < 0 || now - last > kLinkTimeoutMs) {
      ESP_LOGI(TAG, "host bridge attached");
      SendHello();
    }
  }
}

void RxTask(void*) {
  auto* line = static_cast<char*>(heap_caps_malloc(kLineMax + 1, MALLOC_CAP_DEFAULT));
  size_t len = 0;
  bool overflow = false;
  uint8_t buf[512];
  for (;;) {
    int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
    for (int i = 0; i < n; ++i) {
      const char c = (char)buf[i];
      if (c == '\n') {
        if (!overflow) { line[len] = 0; HandleLine(line, len); }
        len = 0;
        overflow = false;
      } else if (len < kLineMax) {
        line[len++] = c;
      } else {
        overflow = true;
      }
    }
  }
}

// Own task so slow app redraws in the main loop can't overrun the I2S DMA.
void MicTask(void*) {
  int16_t pcm[256];
  for (;;) {
    if (!g_streaming) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    const int64_t rec_until = g_rec_until_ms;
    if (rec_until && esp_timer_get_time() / 1000 >= rec_until) {
      g_rec_until_ms = 0;
      End();
      continue;
    }
    int n = cheeko_board_mic_read(pcm, 256);
    if (n > 0) SendMic(pcm, (size_t)n);
    else vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void Start() {
  if (g_started.exchange(true)) return;
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  cfg.rx_buffer_size = 8192;
  cfg.tx_buffer_size = 4096;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
    ESP_LOGE(TAG, "usb_serial_jtag driver install failed");
    return;
  }
  usb_serial_jtag_vfs_use_driver();  // console writes now go through the driver
  g_events = xQueueCreate(16, sizeof(Event*));
  g_pcm = static_cast<int16_t*>(heap_caps_malloc(kPcmChunk * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (!g_pcm) g_pcm = static_cast<int16_t*>(heap_caps_malloc(kPcmChunk * sizeof(int16_t), MALLOC_CAP_DEFAULT));
  xTaskCreatePinnedToCore(RxTask, "cheeko_link", 8192, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(MicTask, "cheeko_mic", 8192, nullptr, 7, nullptr, 1);
  SendHello();
  ESP_LOGI(TAG, "serial link up; run tools/voice_agent on the host");
}

bool Linked() {
  const int64_t last = g_last_hi_ms;
  return last >= 0 && esp_timer_get_time() / 1000 - last <= kLinkTimeoutMs;
}

void Begin() {
  if (!g_started) return;
  g_streaming = true;
  WriteLine("@@BEGIN\n", 8);
}

void End() {
  if (!g_streaming.exchange(false)) return;
  WriteLine("@@END\n", 6);
}

bool Streaming() { return g_streaming; }

void SendMic(const int16_t* samples, size_t count) {
  if (!g_streaming || count == 0) return;
  constexpr size_t kMaxSamples = 512;  // 1024 bytes -> 1368 base64 chars
  static unsigned char line[6 + 4 * ((kMaxSamples * 2 + 2) / 3) + 2];
  count = std::min(count, kMaxSamples);
  size_t olen = 0;
  memcpy(line, "@@MIC ", 6);
  if (mbedtls_base64_encode(line + 6, sizeof(line) - 7, &olen, (const unsigned char*)samples,
                            count * sizeof(int16_t)) != 0)
    return;
  line[6 + olen] = '\n';
  WriteLine((const char*)line, 7 + olen);
}

bool Poll(Event* out) {
  if (!g_events) return false;
  Event* ev = nullptr;
  if (xQueueReceive(g_events, &ev, 0) != pdTRUE) return false;
  *out = std::move(*ev);
  delete ev;
  return true;
}

}  // namespace link
}  // namespace cheeko
