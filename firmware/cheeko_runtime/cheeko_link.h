// cheeko_link — line protocol to the host voice bridge over USB-Serial/JTAG.
//
// Device -> host (stdout, shares the console with ESP_LOG lines):
//   @@HELLO <sdk>            link up (also re-sent when the host says @@HI)
//   @@BEGIN                  push-to-talk started
//   @@MIC <base64 s16le>     16 kHz mono mic audio
//   @@END                    push-to-talk released; host should reply
// Host -> device (stdin):
//   @@HI                     host heartbeat (~2 s); Linked() lapses after 6 s
//   @@STATE <idle|listening|thinking|researching|speaking|error>
//   @@TEXT <utf-8>           transcript / reply text for OnCloudText()
//   @@PCM <base64 s16le>     16 kHz mono speech, played via the speaker queue
//   @@PCMEND                 flush any buffered PCM
//   @@REC <ms>               mic self-test: stream mic for ms, then @@END
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "cheeko.h"

namespace cheeko {
namespace link {

struct Event {
  enum class Kind { State, Text } kind;
  VoiceState state;
  std::string text;
};

void Start();                 // idempotent; installs the USB-Serial/JTAG driver
bool Linked();                // host heartbeat seen in the last 6 s
void Begin();                 // start streaming mic frames
void End();                   // stop streaming, ask host to reply
bool Streaming();
// Mic frames go out from the link's own task while streaming; OnMicAudio pauses.
void SendMic(const int16_t* samples, size_t count);
bool Poll(Event* out);        // app-thread dispatch of host events

}  // namespace link
}  // namespace cheeko
