#include "wall_clock.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "cheeko.h"

using namespace cheeko;

namespace {

// Digital face layout (240 x 296 screen).
constexpr int kDigitW = 44, kDigitH = 88, kSeg = 10;  // seven-segment digit, bar thickness
constexpr int kDigitY = 104;
constexpr int kDigitX[4] = {14, 64, 132, 182};       // HH : MM, colon centred at x=120
constexpr int kEyeX = 120, kEyeY = 54;
constexpr int kDateY = 222;
constexpr uint32_t kBackground = 0x101820;

// Days since 2000-01-01 for a civil date (Howard Hinnant's algorithm).
int64_t DaysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468 - 10957;
}

void CivilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468 + 10957;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int doe = (int)(z - era * 146097);
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}

// Build time of this file, local to the machine that compiled it.
int64_t BuildSeconds() {
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* date = __DATE__;  // "Nov  7 2026"
  const char* time = __TIME__;  // "14:05:33"
  const int month = (int)(strstr(kMonths, std::string(date, 3).c_str()) - kMonths) / 3 + 1;
  const int day = atoi(date + 4), year = atoi(date + 7);
  const int hh = atoi(time), mm = atoi(time + 3), ss = atoi(time + 6);
  return DaysFromCivil(year, month, day) * 86400 + hh * 3600 + mm * 60 + ss;
}

// Segment bits: a b c d e f g (top, top-right, bottom-right, bottom,
// bottom-left, top-left, middle). Bit 7 unused; 0 = blank digit.
constexpr uint8_t kGlyph[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};

// Paint every segment straight to its final colour (lit or ghost). Nothing is
// cleared first, so a changing digit never flashes.
void DrawDigit(int x, int digit, uint32_t on, uint32_t off) {
  const uint8_t bits = digit < 0 ? 0 : kGlyph[digit];
  const int y = kDigitY, w = kDigitW, h = kDigitH, t = kSeg, half = kDigitH / 2;
  const int v = half - t - t / 2;  // vertical bar length
  const int seg[7][4] = {
      {x + t, y, w - 2 * t, t},                    // a
      {x + w - t, y + t, t, v},                    // b
      {x + w - t, y + half + t / 2, t, v},         // c
      {x + t, y + h - t, w - 2 * t, t},            // d
      {x, y + half + t / 2, t, v},                 // e
      {x, y + t, t, v},                            // f
      {x + t, y + half - t / 2, w - 2 * t, t},     // g
  };
  auto& d = Cheeko().display();
  for (int i = 0; i < 7; ++i) d.FillRect(seg[i][0], seg[i][1], seg[i][2], seg[i][3], (bits >> i) & 1 ? on : off);
}
}  // namespace

void WallClock::Start(uint32_t uptime_ms) {
  auto& st = Cheeko().storage();
  const int64_t build = BuildSeconds();
  const int64_t saved = st.GetInt("clk_s", 0);
  // A fresh build resets to its own timestamp; otherwise resume the last save.
  const bool same_build = st.GetInt("clk_build", 0) == (int)build;
  ms_ = (same_build && saved > build ? saved : build) * 1000;
  st.PutInt("clk_build", (int)build);
  last_uptime_ = uptime_ms;
  saved_min_ = ms_ / 60000;
}

void WallClock::Tick(uint32_t uptime_ms) {
  ms_ += (uint32_t)(uptime_ms - last_uptime_);  // wrap-safe
  last_uptime_ = uptime_ms;
  if (ms_ / 60000 != saved_min_) Save();
}

void WallClock::Adjust(int minutes) {
  ms_ += (int64_t)minutes * 60000;
  ms_ -= ms_ % 60000;  // setting the time zeroes the seconds
  Save();
}

void WallClock::Save() {
  saved_min_ = ms_ / 60000;
  Cheeko().storage().PutInt("clk_s", (int)(ms_ / 1000));
}

void WallClock::Draw(const ClockColors& c, bool force) {
  // Minute resolution: nothing on screen changes between minutes.
  const int64_t s = Seconds();
  const int64_t minute = (s >= 0 ? s : s - 59) / 60;
  if (!force && minute == drawn_min_) return;
  drawn_min_ = minute;
  auto& d = Cheeko().display();
  const uint32_t ghost = 0x1e2935;  // unlit segment, a shade above the panel

  if (force) {
    d.Clear(kBackground);
    // Microduck camera eye above the time.
    d.FillCircle(kEyeX, kEyeY, 24, c.rim);
    d.FillCircle(kEyeX, kEyeY, 17, c.cap);
    d.FillCircle(kEyeX, kEyeY, 10, c.lens);
    d.FillCircle(kEyeX + 4, kEyeY - 4, 3, 0xffffff);
    d.FillRect(6, kDigitY - 12, 228, kDigitH + 24, c.dial);  // panel
    // Static colon (no blink: a per-second redraw is what caused flicker).
    d.FillRect(116, kDigitY + 24, 8, 8, c.second);
    d.FillRect(116, kDigitY + kDigitH - 32, 8, 8, c.second);
    for (int& v : shown_) v = -2;  // force every digit
  }

  const int day_s = (int)(((s % 86400) + 86400) % 86400);
  const int h24 = day_s / 3600, mins = day_s / 60 % 60;
  const int h12 = h24 % 12 == 0 ? 12 : h24 % 12;
  const int digits[4] = {h12 >= 10 ? 1 : -1, h12 % 10, mins / 10, mins % 10};  // blank leading 0
  for (int i = 0; i < 4; ++i) {
    if (digits[i] == shown_[i]) continue;
    shown_[i] = digits[i];
    DrawDigit(kDigitX[i], digits[i], c.hand, ghost);
  }
  DrawLabel(s);
}

void WallClock::DrawLabel(int64_t s) {
  const int64_t days = s >= 0 ? s / 86400 : (s - 86399) / 86400;
  if (days == label_day_ && (s - days * 86400 >= 43200) == label_pm_) return;
  label_day_ = days;
  label_pm_ = s - days * 86400 >= 43200;
  static const char* kDays[] = {"Sat", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri"};  // 2000-01-01 = Sat
  static const char* kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int y, m, dd;
  CivilFromDays(days, y, m, dd);
  char buf[40];
  snprintf(buf, sizeof buf, "%s %d %s  %s", kDays[((days % 7) + 7) % 7], dd, kMon[m - 1], label_pm_ ? "PM" : "AM");
  auto& d = Cheeko().display();
  d.FillRect(0, kDateY - 4, 240, 30, kBackground);
  d.CenterText(kDateY, buf);
}
