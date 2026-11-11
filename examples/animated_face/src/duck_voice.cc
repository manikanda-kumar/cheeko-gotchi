// Microduck voice synthesis — C++ port of sounds/src/{rng,synth,personality,voices}.rs
// from https://github.com/pollen-robotics/microduck
//
//   Copyright Pollen Robotics. Licensed under the Apache License, Version 2.0
//   (http://www.apache.org/licenses/LICENSE-2.0).
//
// Changes from the original (Apache-2.0 §4b):
//   - Ported from Rust to C++; greet, chirp, peck, alarm, inquire and coo
//     recipes (not wheee or the chorale/theremin streams).
//   - Renders at 16 kHz instead of 48 kHz (the Cheeko Gotchi I2S rate). The two
//     rate-dependent constants (jitter window, pink-noise pole) are rescaled the
//     same way the original rescales them from 22.05 kHz.
//   - Harmonics above Nyquist are skipped per sample to avoid aliasing at 16 kHz.
//   - Output is int16 PCM; the per-duck seed is chosen by the app (the robot
//     derives it from its SoC serial).
// The RNG (xoshiro256++ via splitmix64, Box-Muller normals) and CRC-32 tag hash
// are kept bit-for-bit so the per-seed personalities match the robot's.

#include "duck_voice.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace duckvoice {
namespace {

constexpr double kTau = 6.283185307179586;
constexpr double kTunedSr = 22050.0;
constexpr double kSr = kSampleRate;

// ---- rng.rs ----
class Rng {
 public:
  explicit Rng(uint32_t seed) {
    uint64_t x = seed;
    for (auto& w : s_) {
      x += 0x9E3779B97F4A7C15ull;
      uint64_t z = x;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      w = z ^ (z >> 31);
    }
  }
  double Random() { return (double)(Next() >> 11) * (1.0 / (double)(1ull << 53)); }
  double Uniform(double lo, double hi) { return lo + (hi - lo) * Random(); }
  int64_t Integers(int64_t lo, int64_t hi) { return lo + (int64_t)std::floor(Random() * (double)(hi - lo)); }
  double Choice(const double* c, int n) { return c[Integers(0, n)]; }
  float Normal() {
    if (has_spare_) {
      has_spare_ = false;
      return spare_;
    }
    // Float math: the S3 has no double FPU and this runs per sample.
    const float r = (float)(1.0 - Random());
    const float theta = (float)(kTau * Random());
    const float mag = std::sqrt(-2.0f * std::log(std::max(r, 1e-30f)));
    spare_ = mag * std::sin(theta);
    has_spare_ = true;
    return mag * std::cos(theta);
  }

 private:
  static uint64_t Rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t Next() {
    uint64_t result = Rotl(s_[0] + s_[3], 23) + s_[0];
    uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = Rotl(s_[3], 45);
    return result;
  }
  uint64_t s_[4];
  bool has_spare_ = false;
  float spare_ = 0;
};

uint32_t Crc32(const std::string& data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char b : data) {
    crc ^= b;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

Rng VariantRng(const Personality& p, const char* tag, uint32_t variant) {
  uint64_t h = ((uint64_t)p.seed * 1000003ull) ^ (uint64_t)Crc32(tag) ^ ((uint64_t)variant * 2654435761ull);
  return Rng((uint32_t)h);
}

std::vector<double> Harmonics(const Personality& p) {
  constexpr int N = 7;
  std::vector<double> w(N);
  for (int n = 1; n <= N; ++n) {
    double base = 1.0 / std::pow((double)n, p.tilt);
    double high_lift = p.brightness * std::pow((double)n / N, 1.5);
    double nasal = p.nasal * ((n == 2 || n == 3) ? 0.6 : 0.0);
    double skew = p.harmonic_skew >= 0 ? p.harmonic_skew * (n % 2 == 0 ? 0.4 : -0.2)
                                       : -p.harmonic_skew * (n % 2 == 0 ? -0.3 : 0.4);
    double formant = n == p.formant_n ? p.formant_gain : 0.0;
    w[n - 1] = std::max(0.0, base + high_lift + nasal + skew + formant * base * 1.5);
  }
  w[0] = std::max(w[0], 0.7);
  return w;
}

// ---- synth.rs ----
using Buf = std::vector<float>;
using Points = std::vector<std::pair<double, double>>;

size_t NumSamples(double dur) { return std::max<size_t>(1, (size_t)std::llround(dur * kSr)); }
float T(size_t i) { return (float)i / (float)kSr; }

Buf Lerp(size_t n, const Points& pts) {
  Buf out(n);
  for (size_t i = 0; i < n; ++i) {
    double x = T(i);
    double v = pts.back().second;
    if (x <= pts[0].first) {
      v = pts[0].second;
    } else {
      for (size_t k = 0; k + 1 < pts.size(); ++k) {
        auto [x0, y0] = pts[k];
        auto [x1, y1] = pts[k + 1];
        if (x <= x1) {
          v = x1 <= x0 ? y1 : y0 + (y1 - y0) * (x - x0) / (x1 - x0);
          break;
        }
      }
    }
    out[i] = (float)v;
  }
  return out;
}

Buf ExpDecay(size_t n, double attack_s, double decay_s) {
  float a = (float)std::max(attack_s, 1e-4), d = (float)std::max(decay_s, 1e-4);
  Buf out(n);
  for (size_t i = 0; i < n; ++i) {
    float x = T(i);
    out[i] = std::clamp(x / a, 0.0f, 1.0f) * std::exp(-std::max(x - a, 0.0f) / d);
  }
  return out;
}

// Soft attack, plateau, soft release.
Buf Bell(size_t n, double attack_s, double release_s) {
  const double total = T(n - 1);
  const double rel_start = std::max(total - release_s, 0.0);
  const double attack = std::max(attack_s, 1e-4), release = std::max(release_s, 1e-4);
  Buf out(n);
  for (size_t i = 0; i < n; ++i) {
    const double x = T(i);
    double v = 1.0;
    if (x < attack_s) v = x / attack;
    if (x > rel_start) v = std::max(1.0 - (x - rel_start) / release, 0.0);
    out[i] = (float)std::clamp(v, 0.0, 1.0);
  }
  return out;
}

Buf Vibrato(size_t n, double rate, double depth, double phase) {
  Buf out(n, 1.0f);
  if (rate <= 0 || depth <= 0) return out;
  const float w = (float)(kTau * rate / kSr), d = (float)(depth / 12.0), ph0 = (float)phase;
  for (size_t i = 0; i < n; ++i) out[i] = std::exp2(d * std::sin(w * (float)i + ph0));
  return out;
}

Buf Jitter(size_t n, double depth, Rng& rng) {
  Buf out(n, 1.0f);
  if (depth <= 0) return out;
  Buf raw(n);
  for (auto& v : raw) v = rng.Normal();
  const size_t k = std::max<size_t>(1, (size_t)std::llround(64.0 * kSr / kTunedSr));
  const size_t off = (k - 1) / 2;
  // numpy convolve(mode="same") with ones(k)/k, via prefix sums.
  std::vector<double> pre(n + 1, 0.0);
  for (size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + raw[i];
  for (size_t i = 0; i < n; ++i) {
    size_t hi = std::min(i + off + 1, n);
    size_t lo = (i + off + 1 > k) ? i + off + 1 - k : 0;
    double avg = (pre[hi] - pre[lo]) / (double)k;
    out[i] = std::exp2((float)(depth * avg / 12.0));
  }
  return out;
}

Buf PinkNoise(size_t n, Rng& rng) {
  const float a = (float)std::pow(0.985, kTunedSr / kSr);
  Buf out(n);
  float leak = 0, peak = 1e-9f;
  for (auto& v : out) {
    leak = a * leak + rng.Normal();
    v = leak;
    peak = std::max(peak, std::fabs(v));
  }
  for (auto& v : out) v /= peak;
  return out;
}

Buf Click(size_t n, Rng& rng, size_t len) {
  Buf out(n, 0.0f);
  size_t l = std::min(len, n);
  for (size_t i = 0; i < l; ++i) {
    float fade = 1.0f - (float)i / (float)l;
    out[i] = (float)rng.Uniform(-1.0, 1.0) * fade * fade;
  }
  return out;
}

void Normalise(Buf& x, double peak_dbfs) {
  float peak = 1e-9f;
  for (float v : x) peak = std::max(peak, std::fabs(v));
  float gain = (float)std::pow(10.0, peak_dbfs / 20.0) / peak;
  for (auto& v : x) v *= gain;
}

// ---- voices.rs ----
double Attack(const Personality& p, double dur, double snappy) {
  double soft = 0.04 * dur, sharp = 0.003 * dur;
  return std::max(soft + (sharp - soft) * p.attack_sharpness * snappy, 0.001);
}

// Harmonic osc + vibrato + jitter + AM buzz + breath.
Buf Voice(const Personality& p, const Buf& freq, Rng& rng, double am_scale, double breath_scale) {
  const size_t n = freq.size();
  Buf vib = Vibrato(n, p.vibrato_rate_hz, p.vibrato_depth, rng.Uniform(0.0, kTau));
  Buf jit = Jitter(n, p.jitter_depth, rng);
  const std::vector<double> w = Harmonics(p);
  Buf body(n);
  // Phase kept in cycles and wrapped, then harmonics by the angle-addition
  // recurrence: 2 float trig calls per sample instead of one double sin() per
  // harmonic on an ever-growing argument (no double FPU on the S3; that took
  // seconds per call and starved the watchdog).
  std::vector<float> wf(w.begin(), w.end());
  double cyc = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double f = (double)freq[i] * vib[i] * jit[i];
    cyc += f / kSr;
    cyc -= std::floor(cyc);
    const float ph = (float)(kTau * cyc);
    const float s1 = std::sin(ph), c1 = std::cos(ph);
    float sh = s1, ch = c1, s = 0.0f;
    for (size_t h = 0; h < wf.size(); ++h) {
      if (wf[h] != 0.0f && f * (double)(h + 1) < kSr * 0.5) s += wf[h] * sh;
      const float ns = sh * c1 + ch * s1;  // sin((h+2)ph)
      ch = ch * c1 - sh * s1;              // cos((h+2)ph)
      sh = ns;
    }
    body[i] = s;
  }
  const double am_d = p.am_depth * am_scale * p.quackiness;
  const float am_w = (float)(kTau * p.am_rate_hz / kSr);
  if (am_d > 0.01) {
    for (size_t i = 0; i < n; ++i)
      body[i] *= 1.0f - (float)am_d * (0.5f + 0.5f * std::sin(am_w * (float)i));
  }
  const double breath = p.breath * breath_scale;
  if (breath > 0.0) {
    Buf noise = PinkNoise(n, rng);
    for (size_t i = 0; i < n; ++i) body[i] += (float)breath * noise[i];
  }
  return body;
}

void Apply(Buf& sig, const Buf& env) {
  for (size_t i = 0; i < sig.size(); ++i) sig[i] *= env[i];
}

void AppendSilence(Buf& sig, double secs) { sig.insert(sig.end(), (size_t)(secs * kSr), 0.0f); }

Buf GreetSyllable(const Personality& p, Rng& rng, double dur_scale, double f0_scale) {
  const double dur = (0.32 + 0.25 * rng.Random()) * dur_scale / p.speed;
  const size_t n = NumSamples(dur);
  const double f0 = p.pitch_center_hz * (0.9 + 0.15 * rng.Random()) * f0_scale;
  const double bias = p.glide_bias, bend = 0.10 + 0.15 * p.pitch_spread;
  const double start = f0 * (1.0 - bias * bend * 0.5);
  const double mid = f0 * (1.0 + bias * bend);
  const double end = f0 * (1.0 - bias * bend * 0.3) * (0.92 + 0.08 * rng.Random());
  Buf freq = Lerp(n, {{0.0, start}, {0.18 * dur, mid}, {dur, end}});
  Buf env = ExpDecay(n, Attack(p, dur, 0.5), dur * 0.7);
  Buf sig = Voice(p, freq, rng, 1.0, 1.0);
  Apply(sig, env);
  return sig;
}

Buf Greet(const Personality& p, uint32_t variant) {
  Rng rng = VariantRng(p, "greet", variant);
  Buf sig = GreetSyllable(p, rng, 1.0, 1.0);
  if (rng.Random() < 0.4) {  // "wak-wak" double quack
    AppendSilence(sig, (0.05 + 0.06 * rng.Random()) / p.speed);
    const double f0_scale = 0.95 + 0.06 * rng.Random();
    Buf b = GreetSyllable(p, rng, 0.8, f0_scale);
    sig.insert(sig.end(), b.begin(), b.end());
  }
  Normalise(sig, -3.0);
  return sig;
}

Buf ChirpSyllable(const Personality& p, Rng& rng, double f0, double dur, const Points& contour) {
  const size_t n = NumSamples(dur);
  Buf warble = Vibrato(n, p.warble_hz, p.warble_depth, rng.Uniform(0.0, kTau));
  Buf shape = Lerp(n, contour);
  Buf freq(n);
  for (size_t i = 0; i < n; ++i) freq[i] = (float)f0 * shape[i] * warble[i];
  Buf env = ExpDecay(n, Attack(p, dur, 0.7), dur * 0.55);
  Personality soft = p;
  soft.quackiness *= 0.2;
  soft.brightness *= 0.5;
  soft.formant_gain *= 0.5;
  Buf sig = Voice(soft, freq, rng, 1.0, 0.4);
  Apply(sig, env);
  return sig;
}

Buf Chirp(const Personality& p, uint32_t variant) {
  Rng rng = VariantRng(p, "chirp", variant);
  const double f0 = p.pitch_center_hz * (0.95 + 0.75 * rng.Random());
  Buf sig;
  switch (variant % 4) {
    case 0: {  // rising blip
      double dur = (0.10 + 0.10 * rng.Random()) / p.speed;
      double peak = 1.12 + 0.10 * rng.Random();
      sig = ChirpSyllable(p, rng, f0, dur, {{0.0, 0.88}, {0.5 * dur, peak}, {dur, 1.05}});
      break;
    }
    case 1: {  // falling blip
      double dur = (0.10 + 0.10 * rng.Random()) / p.speed;
      double start = 1.12 + 0.10 * rng.Random();
      sig = ChirpSyllable(p, rng, f0, dur, {{0.0, start}, {0.3 * dur, 1.0}, {dur, 0.78}});
      break;
    }
    case 2: {  // trill
      double dur = (0.22 + 0.14 * rng.Random()) / p.speed;
      Personality trill = p;
      trill.warble_depth = std::max(p.warble_depth, 0.7) * 1.6;
      sig = ChirpSyllable(trill, rng, f0, dur, {{0.0, 1.0}, {0.5 * dur, 1.06}, {dur, 0.94}});
      break;
    }
    default: {  // double "wek-wek"
      double dur = (0.08 + 0.05 * rng.Random()) / p.speed;
      sig = ChirpSyllable(p, rng, f0, dur, {{0.0, 0.95}, {0.4 * dur, 1.10}, {dur, 0.88}});
      AppendSilence(sig, (0.03 + 0.03 * rng.Random()) / p.speed);
      double dur_b = dur * 0.9;
      double f0_b = f0 * (0.92 + 0.10 * rng.Random());
      Buf b = ChirpSyllable(p, rng, f0_b, dur_b, {{0.0, 0.95}, {0.4 * dur_b, 1.10}, {dur_b, 0.88}});
      sig.insert(sig.end(), b.begin(), b.end());
      break;
    }
  }
  Normalise(sig, -6.0);
  return sig;
}

Buf Peck(const Personality& p, uint32_t variant) {
  Rng rng = VariantRng(p, "peck", variant);
  const double dur = (0.16 + 0.12 * rng.Random()) / p.speed;
  const size_t n = NumSamples(dur);
  const double f0 = p.pitch_center_hz * (0.45 + 0.20 * rng.Random());
  Buf freq = Lerp(n, {{0.0, f0 * 1.5}, {0.04 * dur, f0}, {dur, f0 * 0.80}});
  Buf env = ExpDecay(n, Attack(p, dur, 1.0), dur * 0.35);
  Buf body = Voice(p, freq, rng, 0.3, 0.5);
  Apply(body, env);
  const size_t click_len = (size_t)((0.003 + 0.006 * p.attack_sharpness) * kSr);
  const float click_gain = (float)(0.4 + 0.4 * p.attack_sharpness);
  Buf c = Click(n, rng, click_len);
  for (size_t i = 0; i < n; ++i) body[i] += click_gain * c[i];
  Normalise(body, -3.0);
  return body;
}

Buf Alarm(const Personality& p, uint32_t variant) {
  Rng rng = VariantRng(p, "alarm", variant);
  const double dur = (0.20 + 0.12 * rng.Random()) / p.speed;
  const size_t n = NumSamples(dur);
  const double f0 = p.pitch_center_hz * (1.25 + 0.35 * p.pitch_spread) * (0.94 + 0.12 * rng.Random());
  const double peak_mul = 1.15 + 0.25 * p.pitch_spread + 0.10 * rng.Random();
  const double fall_mul = 0.75 + 0.20 * (1.0 - p.pitch_spread);
  Buf freq = Lerp(n, {{0.0, f0}, {0.05 * dur, f0 * peak_mul}, {dur, f0 * fall_mul}});
  Buf env = ExpDecay(n, Attack(p, dur, 1.0), dur * (0.40 + 0.20 * rng.Random()));
  Buf sig = Voice(p, freq, rng, 0.5, 1.0);
  Apply(sig, env);
  const float crackle = (float)(0.04 + 0.10 * p.brightness);
  for (size_t i = 0; i < n; ++i) sig[i] += crackle * rng.Normal() * env[i];
  Normalise(sig, -3.0);
  return sig;
}

// Rising "huh?": always climbs (it's a question).
Buf Inquire(const Personality& p, uint32_t variant) {
  Rng rng = VariantRng(p, "inquire", variant);
  const double dur = (0.42 + 0.25 * rng.Random()) / p.speed;
  const size_t n = NumSamples(dur);
  const double f0 = p.pitch_center_hz * (0.88 + 0.10 * rng.Random());
  const double rise = 1.15 + 0.50 * p.pitch_spread + 0.20 * std::max(p.glide_bias, 0.0) + 0.10 * rng.Random();
  Buf freq = Lerp(n, {{0.0, f0 * 0.92}, {0.30 * dur, f0 * 0.95}, {dur, f0 * rise}});
  Buf env = Bell(n, dur * (0.06 + 0.10 * (1.0 - p.attack_sharpness)), dur * 0.32);
  Buf sig = Voice(p, freq, rng, 0.6, 1.0);
  Apply(sig, env);
  Normalise(sig, -3.0);
  return sig;
}

// Slow, low, breathy, sleepy.
Buf Coo(const Personality& p, uint32_t variant) {
  Rng rng = VariantRng(p, "coo", variant);
  const double dur = (0.85 + 0.55 * rng.Random()) / p.speed;
  const size_t n = NumSamples(dur);
  const double f0 = p.pitch_center_hz * (0.42 + 0.15 * (1.0 - p.attack_sharpness)) * (0.94 + 0.12 * rng.Random());
  const double drift_a = 1.0 + 0.05 * rng.Random() + 0.04 * p.glide_bias;
  Buf freq = Lerp(n, {{0.0, f0 * 0.94}, {dur * 0.5, f0 * drift_a}, {dur, f0 * 0.90}});
  Buf env = Bell(n, dur * (0.18 + 0.10 * (1.0 - p.attack_sharpness)), dur * 0.30);
  Personality soft = p;
  soft.breath = std::max(p.breath, 0.12) + 0.10;
  soft.quackiness = p.quackiness * 0.25;
  soft.am_rate_hz = p.am_rate_hz * 0.30;
  soft.vibrato_rate_hz = p.vibrato_rate_hz * 0.45;
  soft.vibrato_depth = p.vibrato_depth * 0.7;
  Buf sig = Voice(soft, freq, rng, 1.0, 1.0);
  Apply(sig, env);
  Normalise(sig, -5.0);
  return sig;
}

}  // namespace

Personality Personality::FromSeed(uint32_t seed) {
  Rng rng(seed);
  static const double kRegisters[] = {-1.0, 0.0, 0.0, 1.0};
  Personality p{};
  p.seed = seed;
  p.reg = rng.Choice(kRegisters, 4) + rng.Uniform(-0.4, 0.4);
  const double base = rng.Uniform(160.0, 380.0);
  p.pitch_center_hz = std::clamp(base * std::pow(2.0, p.reg * 0.45), 110.0, 620.0);
  p.pitch_spread = rng.Uniform(0.4, 1.2);
  p.glide_bias = rng.Uniform(-1.0, 1.0);
  p.brightness = rng.Uniform(0.05, 0.55);
  p.tilt = rng.Uniform(1.4, 2.8);
  p.nasal = rng.Uniform(0.1, 1.0);
  p.harmonic_skew = rng.Uniform(-1.0, 1.0);
  p.formant_n = (int)rng.Integers(1, 6);
  p.formant_gain = rng.Uniform(0.0, 1.4);
  p.vibrato_rate_hz = rng.Uniform(3.5, 9.5);
  p.vibrato_depth = rng.Uniform(0.0, 0.7);
  p.jitter_depth = rng.Uniform(0.03, 0.35);
  p.breath = rng.Uniform(0.0, 0.30);
  p.quackiness = rng.Uniform(0.2, 1.0);
  p.am_rate_hz = rng.Uniform(18.0, 55.0);
  p.am_depth = rng.Uniform(0.15, 0.70);
  p.warble_hz = rng.Uniform(7.0, 18.0);
  p.warble_depth = rng.Uniform(0.0, 1.4);
  p.attack_sharpness = rng.Uniform(0.0, 1.0);
  p.speed = rng.Uniform(0.82, 1.22);
  return p;
}

std::vector<int16_t> Render(const Personality& p, Call call, uint32_t variant) {
  Buf sig;
  switch (call) {
    case Call::Greet: sig = Greet(p, variant % 12); break;
    case Call::Chirp: sig = Chirp(p, variant % 12); break;
    case Call::Peck: sig = Peck(p, variant % 10); break;
    case Call::Alarm: sig = Alarm(p, variant % 10); break;
    case Call::Inquire: sig = Inquire(p, variant % 10); break;
    case Call::Coo: sig = Coo(p, variant % 10); break;
  }
  std::vector<int16_t> out(sig.size());
  for (size_t i = 0; i < sig.size(); ++i)
    out[i] = (int16_t)std::clamp(sig[i] * 32767.0f, -32768.0f, 32767.0f);
  return out;
}

}  // namespace duckvoice
