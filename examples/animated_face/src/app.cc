#include <chrono>

#include "cheeko.h"
#include "duck_voice.h"
#include "wall_clock.h"

using namespace cheeko;

// Three screens drawn with display primitives:
//   - Clock: digital HH:MM clock under a Microduck camera eye. Default.
//   - Microduck: side-profile tribute to Pollen Robotics' Microduck biped
//     (capsule head, flat bill, camera-ring eye, servo neck). 
//   - Cheeko: the original round face.
//
// Microduck speaks with its own synthesized voice (duck_voice.cc, ported from
// pollen-robotics/microduck). Each colourway is a different duck (seed).
//
// Controls: tap = random duck call (Cheeko: talk on/off), hold = talk to the
// voice agent (tools/voice_agent on the host, over USB), BOOT = next screen,
// VOL+/VOL- = clock: +/-1 min (hold to repeat), Microduck: colourway,
// shake = dizzy. Choices persist in storage.

namespace {

struct Duckway {
  uint32_t shell, face, bill, bill_dark, ring, ring_dark, torso, plate, foot, foot_side;
};

// Colours sampled from Pollen Robotics store/README photos (lit surfaces).
// Cream is from microduck-cream-sitting-three-quarter; the rest from the hero.
constexpr Duckway kDuckways[] = {
    // shell     face      bill      bill_dk   ring      ring_dk   torso     plate     foot      foot_side
    {0xf2f1eb, 0x948a7e, 0xec5410, 0xb0300f, 0xf8b81f, 0xc98a0c, 0xd4c3a9, 0xe3dbc8, 0xf3a418, 0xc74705},  // cream
    {0xa6dee5, 0x6f9ea6, 0xee5a14, 0xb03a10, 0xf5a000, 0xc27c00, 0x9fd6de, 0xa6dee5, 0xf89209, 0xf17112},  // blue
    {0x56585a, 0x3a3b3d, 0xf6cd1a, 0xc9a00f, 0x9176bf, 0x6c5598, 0x545658, 0x5e6062, 0xf6cd1a, 0x987b94},  // grey
    {0xb198c3, 0x8a74a0, 0xf4cd28, 0xc4a018, 0x7fc7d6, 0x5a9fae, 0xa18ab3, 0xb198c3, 0xf4cd28, 0x9a7fc0},  // lilac
};
constexpr int kNumDuckways = sizeof(kDuckways) / sizeof(kDuckways[0]);

// One voice seed per colourway, so every duck sounds like itself.
constexpr uint32_t kDuckSeeds[kNumDuckways] = {100, 101, 7, 42};

constexpr uint32_t kBackground = 0x101820;
constexpr uint32_t kServo = 0x2b2b2e;
constexpr uint32_t kServoHorn = 0x8a8a90;

enum class Character { Clock = 0, Microduck = 1, Cheeko = 2 };
constexpr int kNumCharacters = 3;

// Everything a frame depends on; redraw only when it changes.
struct Frame {
  int character, colorway, voice;
  int64_t clock_s;
  bool blink, talking, dizzy;
  int mouth, bob, look_x, look_y;
  bool operator!=(const Frame& o) const {
    return character != o.character || colorway != o.colorway || voice != o.voice || clock_s != o.clock_s || blink != o.blink ||
           talking != o.talking || dizzy != o.dizzy || mouth != o.mouth || bob != o.bob ||
           look_x != o.look_x || look_y != o.look_y;
  }
};

void RoundRect(int x, int y, int w, int h, int r, uint32_t c) {
  auto& d = Cheeko().display();
  d.FillRect(x + r, y, w - 2 * r, h, c);
  d.FillRect(x, y + r, w, h - 2 * r, c);
  d.FillCircle(x + r, y + r, r, c);
  d.FillCircle(x + w - r - 1, y + r, r, c);
  d.FillCircle(x + r, y + h - r - 1, r, c);
  d.FillCircle(x + w - r - 1, y + h - r - 1, r, c);
}

uint32_t Darken(uint32_t rgb) {
  return ((rgb >> 1) & 0x7f7f7f) + ((rgb >> 2) & 0x3f3f3f);  // ~75%
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

class AnimatedFaceApp : public CheekoApp {
 public:
  void OnStart() override {
    auto& st = Cheeko().storage();
    character_ = static_cast<Character>(Clamp(st.GetInt("screen", 0), 0, kNumCharacters - 1));
    colorway_ = Clamp(st.GetInt("duck_color", 0), 0, kNumDuckways - 1);
    duck_ = duckvoice::Personality::FromSeed(kDuckSeeds[colorway_]);
    clock_.Start(0);
    Cheeko().cloud().Connect();
    Cheeko().log().Info("animated_face: tap=random call, BOOT=screen, VOL+/-=clock/duck, shake=dizzy");
    Render(MakeFrame(0), /*force=*/true);
  }

  void OnTick(uint32_t uptime_ms) override {
    now_ms_ = uptime_ms;
    clock_.Tick(uptime_ms);
    RepeatClockSet(uptime_ms);
    // Push-to-talk: a hold past kHoldMs opens a voice turn.
    if (touching_ && !voice_turn_ && !hold_used_ && uptime_ms - press_ms_ >= kHoldMs) {
      hold_used_ = true;
      if (!Cheeko().cloud().IsLinked()) {  // no bridge on the host: say so now
        Cheeko().log().Warn("voice: no host bridge (run tools/voice_agent/cheeko_voice.py)");
        error_until_ms_ = uptime_ms + 2500;
        Cheeko().speaker().Tone(220, 120);
        return;
      }
      voice_turn_ = true;
      voice_since_ms_ = uptime_ms;
      voice_ = VoiceState::Listening;
      Cheeko().speaker().Tone(1320, 35);
      Cheeko().cloud().StartVoiceSession();
    }
    // Host went quiet mid-turn (bridge died, agent hung): give up.
    const bool waiting = voice_ == VoiceState::Thinking || voice_ == VoiceState::Researching;
    if (waiting && uptime_ms - voice_since_ms_ > kReplyTimeoutMs) OnVoiceState(VoiceState::Error);
    Render(MakeFrame(uptime_ms), /*force=*/false);
  }

  void OnTouch(const TouchEvent& event) override {
    const bool down = event.pressed && !touching_;
    const bool up = !event.pressed && touching_;
    touching_ = event.pressed;
    if (down) {
      press_ms_ = now_ms_;
      hold_used_ = false;
      // Camera eye glances toward the tap (relative to the eye position).
      look_x_ = Clamp((event.x - 92) / 16, -4, 4);
      look_y_ = Clamp((event.y - 80) / 16, -4, 4);
      return;
    }
    if (!up) return;
    if (voice_turn_) {  // released after a hold: send the turn
      voice_turn_ = false;
      voice_ = VoiceState::Thinking;
      voice_since_ms_ = now_ms_;
      Cheeko().cloud().EndVoiceTurn();
      return;
    }
    if (hold_used_) return;                  // long press already handled
    if (voice_ != VoiceState::Idle) return;  // agent busy: ignore taps
    if (character_ != Character::Cheeko) {
      Speak(RandomCall());
    } else {
      talking_ = !talking_;
      Cheeko().speaker().Tone(talking_ ? 880 : 440, 70);
    }
  }

  void OnButton(const ButtonEvent& event) override {
    const bool vol = event.button == ButtonEvent::Button::VolumeUp ||
                     event.button == ButtonEvent::Button::VolumeDown;
    if (vol && !event.pressed) held_dir_ = 0;
    if (!event.pressed) return;
    auto& st = Cheeko().storage();
    switch (event.button) {
      case ButtonEvent::Button::Boot:
        character_ = static_cast<Character>((static_cast<int>(character_) + 1) % kNumCharacters);
        st.PutInt("screen", static_cast<int>(character_));
        talking_ = false;
        if (character_ != Character::Cheeko) Speak(duckvoice::Call::Chirp);
        else Cheeko().speaker().Tone(660, 40);
        break;
      case ButtonEvent::Button::VolumeUp:
      case ButtonEvent::Button::VolumeDown: {
        const bool up = event.button == ButtonEvent::Button::VolumeUp;
        if (character_ == Character::Clock) {
          held_dir_ = up ? 1 : -1;
          held_since_ms_ = repeat_at_ms_ = now_ms_;
          SetClock(held_dir_);
          break;
        }
        const int step = up ? 1 : kNumDuckways - 1;
        colorway_ = (colorway_ + step) % kNumDuckways;
        st.PutInt("duck_color", colorway_);
        duck_ = duckvoice::Personality::FromSeed(kDuckSeeds[colorway_]);
        if (character_ == Character::Microduck) Speak(duckvoice::Call::Peck);
        break;
      }
      default:
        break;
    }
  }

  void OnVoiceState(VoiceState state) override {
    // While the finger is still down we're listening, whatever the host says.
    if (voice_turn_ && state != VoiceState::Error) return;
    if (state == VoiceState::Error) {
      voice_turn_ = false;
      voice_ = VoiceState::Idle;
      error_until_ms_ = now_ms_ + 2000;  // label shows "no reply" briefly
      Cheeko().speaker().Tone(220, 120);
      return;
    }
    voice_ = state;
    voice_since_ms_ = now_ms_;
    error_until_ms_ = 0;
  }

  void OnCloudText(const std::string& text) override { Cheeko().log().Info("agent: " + text); }

  void OnShake() override {
    dizzy_until_ms_ = now_ms_ + 1600;
    if (character_ != Character::Cheeko) {
      Speak(duckvoice::Call::Alarm);
    } else {
      Cheeko().speaker().Tone(300, 50);
      Cheeko().speaker().Tone(240, 70);
    }
  }

 private:
  Frame MakeFrame(uint32_t t) const {
    Frame f{};
    // A voice turn on the clock screen hands over to the duck.
    const bool voice_active = voice_turn_ || voice_ != VoiceState::Idle;
    const Character shown =
        voice_active && character_ == Character::Clock ? Character::Microduck : character_;
    f.character = static_cast<int>(shown);
    f.voice = t < error_until_ms_ ? 99 : static_cast<int>(voice_);
    f.colorway = colorway_;
    f.clock_s = shown == Character::Clock ? clock_.Seconds() / 60 : 0;  // minute resolution
    const bool agent_speaking = voice_ == VoiceState::Speaking;
    const bool duck_speaking = t < speaking_until_ms_ || (agent_speaking && (t / 160) % 2 == 0);
    f.talking = shown == Character::Microduck ? duck_speaking : talking_ || agent_speaking;
    f.dizzy = t < dizzy_until_ms_;
    // Idle frames change only on 2 s boundaries (bob), plus a 150 ms blink
    // every 8 s. Events (quack, shake) redraw immediately: beak open for the
    // whole call, closed when it ends.
    f.blink = !f.dizzy && !f.talking && t % 8000 < 150 && t > 1000;
    f.mouth = f.talking ? 9 : 0;
    f.bob = f.talking ? 2 : (int)((t / 2000) % 2);
    f.look_x = look_x_;
    f.look_y = look_y_;
    return f;
  }

  void Render(const Frame& f, bool force) {
    if (!force && !(f != last_)) return;
    const bool scene_changed = force || f.character != last_.character || f.colorway != last_.colorway;
    last_ = f;
    if (f.character == static_cast<int>(Character::Clock)) {
      const Duckway& c = kDuckways[f.colorway];
      clock_.Draw({0x16202b, c.shell, 0x9aa4b0, 0xf5f8fb, c.bill, c.ring, 0x171311}, scene_changed);
    } else if (f.character == static_cast<int>(Character::Microduck)) {
      // Legs, body and neck are drawn once; each frame only redraws the head.
      if (scene_changed) {
        DrawDuckBody(f);
        label_.clear();
      }
      DrawDuckHead(f);
      DrawDuckLabel(f);
    } else {
      DrawCheeko(f.blink, f.mouth > 0);
    }
  }

  // Hold VOL+/-: first repeat after 500 ms, 1 min steps, 10 min steps after 3 s.
  void RepeatClockSet(uint32_t t) {
    if (!held_dir_ || character_ != Character::Clock) return;
    if (t - held_since_ms_ < 500 || t - repeat_at_ms_ < 120) return;
    repeat_at_ms_ = t;
    SetClock(held_dir_ * (t - held_since_ms_ > 3000 ? 10 : 1));
  }

  void SetClock(int minutes) {
    clock_.Adjust(minutes);
    Cheeko().speaker().Tone(minutes > 0 ? 1320 : 990, 12);
  }

  // Any of the six calls, never the same one twice in a row. xorshift32
  // seeded by tap time; the SDK has no RNG.
  duckvoice::Call RandomCall() {
    rand_ ^= now_ms_ * 2654435761u + 1;
    int pick;
    do {
      rand_ ^= rand_ << 13;
      rand_ ^= rand_ >> 17;
      rand_ ^= rand_ << 5;
      pick = (int)(rand_ % duckvoice::kNumCalls);
    } while (pick == last_call_);
    last_call_ = pick;
    return static_cast<duckvoice::Call>(pick);
  }

  // Synthesize a call (~20-60 ms on the S3) and queue it; the beak flaps
  // until it has finished playing.
  void Speak(duckvoice::Call call) {
    static const char* kNames[] = {"greet", "chirp", "peck", "alarm", "inquire", "coo"};
    const uint32_t variant = variant_++;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<int16_t> pcm = duckvoice::Render(duck_, call, variant);
    const long render_ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
    const uint32_t dur_ms = (uint32_t)(pcm.size() * 1000 / duckvoice::kSampleRate);
    if (Cheeko().speaker().PlayPcm(pcm.data(), pcm.size(), duckvoice::kSampleRate))
      speaking_until_ms_ = now_ms_ + dur_ms + 60;
    Cheeko().log().Info(std::string("duck ") + std::to_string(kDuckSeeds[colorway_]) + " " +
                        kNames[static_cast<int>(call)] + " v" + std::to_string(variant) + " " +
                        std::to_string(dur_ms) + " ms (render " + std::to_string(render_ms) + " ms)");
  }

  // Static part of the duck (sitting, three-quarter view): background,
  // shadow, feet blocks, hip servos, side plate, torso, neck.
  void DrawDuckBody(const Frame& f) {
    auto& d = Cheeko().display();
    const Duckway& c = kDuckways[f.colorway];
    d.Clear(kBackground);
    RoundRect(40, 262, 180, 8, 4, 0x1a2532);     // ground shadow
    d.FillRect(70, 226, 112, 26, kServo);         // hip servos
    d.FillCircle(98, 238, 8, kServoHorn);
    d.FillCircle(98, 238, 3, kServo);
    RoundRect(164, 220, 52, 44, 10, c.plate);     // side plate (thigh)
    d.FillCircle(176, 230, 2, kServoHorn);
    d.FillCircle(204, 254, 2, kServoHorn);
    RoundRect(44, 228, 48, 38, 10, c.foot);       // left foot block
    RoundRect(108, 232, 50, 36, 10, c.foot);      // right foot block
    d.FillRect(150, 236, 8, 28, c.foot_side);     // foot side face
    d.FillRect(84, 232, 8, 30, c.foot_side);
    RoundRect(96, 188, 104, 46, 14, c.torso);     // torso
    d.Line(104, 226, 192, 226, Darken(c.torso));
    DrawNeck(c);
  }

  void DrawNeck(const Duckway& c) {
    auto& d = Cheeko().display();
    d.FillRect(116, 150, 30, 46, kServo);
    RoundRect(102, 184, 28, 22, 8, c.torso);      // servo horn plate
    d.FillCircle(110, 190, 2, kServo);
    d.FillCircle(122, 190, 2, kServo);
    d.FillCircle(116, 199, 2, kServo);
    d.FillRect(140, 158, 6, 10, 0xe8e8e8);        // connector
  }

  // Head box: x 22..219, y 16..163 covers every bob/jaw position.
  void DrawDuckHead(const Frame& f) {
    auto& d = Cheeko().display();
    const Duckway& c = kDuckways[f.colorway];
    const int dy = f.bob;
    d.FillRect(22, 16, 198, 148, kBackground);
    DrawNeck(c);  // the head box overlaps the top of the neck

    // Hood shell: big rounded back + flatter front.
    d.FillCircle(160, 86 + dy, 56, c.shell);
    RoundRect(40, 30 + dy, 150, 106, 28, c.shell);
    // Recessed front face panel, cream rim above and wall to the right.
    RoundRect(48, 40 + dy, 112, 90, 22, c.face);

    // Rear bracket with screws.
    RoundRect(184, 110 + dy, 32, 30, 7, c.bill);
    d.FillCircle(196, 118 + dy, 2, kServo);
    d.FillCircle(206, 122 + dy, 2, kServo);
    d.FillCircle(196, 130 + dy, 2, kServo);
    d.FillCircle(206, 134 + dy, 2, kServo);

    // Bill: thin upper plate + lower jaw plate, protruding at the front.
    if (f.mouth) d.FillRect(40, 136 + dy, 140, f.mouth + 3, 0x2a1a1a);
    RoundRect(28, 126 + dy, 162, 11, 5, c.bill);
    RoundRect(32, 138 + dy + f.mouth, 154, 9, 4, c.bill_dark);

    // Camera eye: thick ring, dark lens; pill sensor to its right.
    const int ex = 92, ey = 80 + dy;
    RoundRect(126, 86 + dy, 22, 10, 5, 0x080807);
    d.FillCircle(ex, ey, 26, c.ring);
    d.FillCircle(ex, ey, 14, c.ring_dark);  // inner bevel
    if (f.blink) {
      d.FillCircle(ex, ey, 13, c.ring);
      d.FillRect(ex - 12, ey - 2, 24, 4, 0x16181c);
    } else if (f.dizzy) {
      d.FillCircle(ex, ey, 13, Color::White);
      d.Circle(ex, ey, 10, Color::Black);
      d.Circle(ex + 1, ey, 5, Color::Black);
      d.FillCircle(ex + 2, ey, 2, Color::Black);
    } else {
      const int lx = ex + f.look_x, ly = ey + f.look_y;
      d.FillCircle(lx, ly, 11, 0x171311);
      d.FillCircle(lx, ly, 6, 0x2c3440);
      d.FillCircle(lx + 4, ly - 4, 3, Color::White);
    }
  }

  void DrawDuckLabel(const Frame& f) {
    const char* text = f.voice == 99                                        ? (Cheeko().cloud().IsLinked() ? "no reply - try again" : "no bridge on mac")
                       : f.voice == static_cast<int>(VoiceState::Listening)   ? "listening..."
                       : f.voice == static_cast<int>(VoiceState::Thinking)    ? "thinking..."
                       : f.voice == static_cast<int>(VoiceState::Researching) ? "looking it up..."
                       : f.voice == static_cast<int>(VoiceState::Speaking)    ? "quack quack"
                       : f.dizzy                                              ? "whoa..."
                       : f.talking                                            ? "quack!"
                       : Cheeko().cloud().IsLinked()                          ? "tap quack, hold talk"
                                                                              : "tap to quack";
    if (label_ == text) return;
    label_ = text;
    Cheeko().display().FillRect(0, 270, 240, 26, kBackground);
    Cheeko().display().CenterText(272, text);
  }

  void DrawCheeko(bool blink, bool mouth_open) {
    auto& display = Cheeko().display();
    display.Clear(kBackground);

    display.FillCircle(120, 120, 78, Color::Amber);
    display.FillCircle(86, 110, 38, Color::White);
    display.FillCircle(154, 110, 38, Color::White);
    display.FillCircle(120, 164, 46, 0xfff4cf);

    if (blink) {
      display.Line(74, 108, 98, 108, Color::Black);
      display.Line(142, 108, 166, 108, Color::Black);
    } else {
      display.FillCircle(86, 108, 10, Color::Black);
      display.FillCircle(154, 108, 10, Color::Black);
      display.FillCircle(90, 104, 3, Color::White);
      display.FillCircle(158, 104, 3, Color::White);
    }

    display.FillCircle(120, 146, 7, Color::Black);
    if (mouth_open) {
      display.FillCircle(120, 176, 14, Color::Black);
      display.FillRect(108, 166, 24, 10, 0xfff4cf);
    } else {
      display.Line(106, 174, 120, 184, Color::Black);
      display.Line(120, 184, 134, 174, Color::Black);
    }

    display.CenterText(264, talking_ ? "talking..." : "tap to talk");
  }

  Character character_ = Character::Clock;
  WallClock clock_;
  int held_dir_ = 0;
  uint32_t held_since_ms_ = 0, repeat_at_ms_ = 0;
  int colorway_ = 0;
  bool talking_ = false;
  bool touching_ = false;
  static constexpr uint32_t kHoldMs = 450;
  uint32_t press_ms_ = 0;
  bool voice_turn_ = false;  // finger down, mic streaming
  bool hold_used_ = false;   // this press already became a voice turn / error
  static constexpr uint32_t kReplyTimeoutMs = 45000;
  uint32_t voice_since_ms_ = 0;
  VoiceState voice_ = VoiceState::Idle;
  uint32_t error_until_ms_ = 0;
  int look_x_ = 0, look_y_ = 0;
  uint32_t now_ms_ = 0;
  uint32_t dizzy_until_ms_ = 0;
  uint32_t speaking_until_ms_ = 0;
  uint32_t variant_ = 0;
  uint32_t rand_ = 0x9e3779b9u;
  int last_call_ = -1;
  duckvoice::Personality duck_ = duckvoice::Personality::FromSeed(kDuckSeeds[0]);
  Frame last_{};
  std::string label_;
};

CHEEKO_APP(AnimatedFaceApp);
