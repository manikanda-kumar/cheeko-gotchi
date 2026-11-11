// Microduck voice synthesis — C++ port of the `sounds` crate from
// https://github.com/pollen-robotics/microduck (Apache-2.0, Pollen Robotics).
// See duck_voice.cc for the list of changes from the original.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckvoice {

// Rendered natively at the speaker's rate (the original renders at 48 kHz).
constexpr int kSampleRate = 16000;

// Per-duck vocal traits, a stable function of the seed (personality.rs).
struct Personality {
  uint32_t seed;
  double pitch_center_hz, reg, pitch_spread, glide_bias;
  double brightness, tilt, nasal, harmonic_skew;
  int formant_n;
  double formant_gain;
  double vibrato_rate_hz, vibrato_depth, jitter_depth, breath, quackiness;
  double am_rate_hz, am_depth, warble_hz, warble_depth;
  double attack_sharpness, speed;

  static Personality FromSeed(uint32_t seed);
};

enum class Call { Greet, Chirp, Peck, Alarm, Inquire, Coo };
constexpr int kNumCalls = 6;

// Render one call as 16-bit mono PCM at kSampleRate. `variant` re-rolls the
// call's sub-randomisation so repeated calls don't sound like a recording.
std::vector<int16_t> Render(const Personality& p, Call call, uint32_t variant);

}  // namespace duckvoice
