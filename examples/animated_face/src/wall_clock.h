#pragma once

#include <climits>
#include <cstdint>

// Digital clock (12-hour HH:MM, seven-segment digits) drawn with display
// primitives. Redraws only on minute change, and only the digits that changed.
//
// The board has no RTC and Wi-Fi is not wired up yet, so local time is seeded
// from the firmware build time (__DATE__/__TIME__, Mac local time) and saved to
// storage once a minute. A reboot resumes from the last save; time spent
// powered off is lost and can be corrected with Adjust() (VOL+/VOL-).

struct ClockColors {
  uint32_t dial, rim, tick, hand, second, cap, lens;
};

class WallClock {
 public:
  void Start(uint32_t uptime_ms);        // restore/seed time
  void Tick(uint32_t uptime_ms);         // advance; saves on minute change
  void Adjust(int minutes);              // shift time, saved immediately
  int64_t Seconds() const { return ms_ / 1000; }  // local, since 2000-01-01

  // Full face when force, otherwise only digits that changed since last call.
  void Draw(const ClockColors& c, bool force);

 private:
  void Save();
  void DrawLabel(int64_t s);

  int64_t ms_ = 0;
  uint32_t last_uptime_ = 0;
  int64_t drawn_min_ = INT64_MIN, saved_min_ = -1, label_day_ = INT64_MIN;
  bool label_pm_ = false;
  int shown_[4] = {-2, -2, -2, -2};  // digit on screen per position (-1 blank)
};
