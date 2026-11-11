# Cheeko voice agent (OpenCode Go)

Hold the Cheeko's screen, talk, release. The Mac transcribes, asks an OpenCode
Go model, and streams the spoken reply back to the duck's speaker.

```
device mic ──@@MIC──▶ cheeko_voice.py ──▶ Whisper (local, mlx)
   (USB serial)              │
                             ├──▶ opencode serve ── cheeko-voice     (beam-free, no tools)
                             │                    └ cheeko-research  (beam-free + websearch/webfetch)
device speaker ◀─@@PCM── macOS `say` ◀──┘
```

## Run

```bash
export OPENCODE_GO_API_KEY=...            # OpenCode Go key
# flash firmware/sdk_app with CHEEKO_APP=../../examples/animated_face, then:
uv run tools/voice_agent/cheeko_voice.py  # hold screen ≥0.45 s to talk

uv run tools/voice_agent/cheeko_voice.py --text "what's the weather in Chennai?"   # no device
uv run tools/voice_agent/cheeko_voice.py --wav question.wav                        # 16 kHz mono
```

Debug: `--say "text"` plays text through the device speaker. `--mic-test` records
the device mic for 4 s while the Mac speaks a phrase out loud, then saves the WAV
to `.ocdata/turns/` and transcribes it. Every turn's mic audio is saved there
too, and `.ocdata/bridge.log` records each turn's audio length, peak, RMS and
fraction of zero samples.

Options: `--voice Samantha` (any `say -v '?'` voice, or `CHEEKO_VOICE`),
`--rate 195`, `--port /dev/cu.usbmodemX`. Only one program can own the serial
port, so close `idf.py monitor` and other serial loggers first. Device logs go to `.ocdata/device.log`.

The bridge runs a private `opencode serve` with `XDG_DATA_HOME=.ocdata`, so only
the env key is used. Otherwise an expired key in
`~/.local/share/opencode/auth.json` overrides `opencode.json`.

## Model choice

Benchmarked 2026-11-11 through `opencode run`. Model time is measured from
step start to first text. A turn through a warm `opencode serve` adds ~0.6 s.

| Role | Pick | Why | Fallbacks |
| --- | --- | --- | --- |
| Voice | `beam-free` (free) | ~0.4 s to first text, short natural replies, correctly emits `RESEARCH:` for fresh-info questions | `longcat-2.5-free` (free, 1–3 s), `gpt-6-luna` (paid, fastest) |
| Research | `beam-free` (free) | 3–7 s with web tools, right answers on T20 WC 2026 / opencode version / weather | `longcat-2.5-free` (free, 8–18 s), `glm-5.3-flash` (paid, reliable tools) |
| STT | local `whisper-large-v3-turbo` (mlx) | 0.5 s warm. opencode drops audio attachments, so the Go audio models (mimo-v2.x, gemini-4-argon) can't be used through it | — |
| TTS | macOS `say` | No Go model outputs audio | Kokoro later |

Rejected: `gpt-6-luna` and `kimi-k3` gave up on every websearch/webfetch call
in research. `cat-jam-free`, `crouton-alpha-free`, `minimax-m3.1-flash-free`
and `step-5-preview-free` returned "Model unavailable". `iceman-contributor-free`
needs "Allow paid endpoints that train on request data". `gemini-3.8-flash`
accepts audio but is Zen-only, not Go.

## Wire protocol

See `firmware/cheeko_runtime/cheeko_link.h`. Line-based, sharing the console
with ESP_LOG output: `@@BEGIN`, `@@MIC <b64>`, `@@END` up; `@@HI`, `@@STATE`,
`@@TEXT`, `@@PCM <b64>`, `@@PCMEND` down. Audio is 16 kHz s16le mono.

Debug commands (host → device): `@@REC <ms>` streams the mic for ms without a
touch. `@@REG <dev> <reg> [val]` (hex; dev 0 = ES7210, 1 = ES8311) dumps 16
registers, or writes one and reads it back. The result goes to the device log.

## Mic status (2026-11-11): not usable yet

The pipeline (mic stream, Whisper, agent, reply, speaker) works end to end.
The mic audio does not: the ES7210 drives data in only **2 of every 16 LRCK
frames**, and the other 14 are exact zeros (about 88%), so Whisper hears noise.

What's been ruled out (`@@REG`/`@@REC`/`@@LOOP` + `--mic-test`, see `~/snippets/esp32-cheeko-codec-probe.py`):

| Tried | Effect on 2-of-16 |
| --- | --- |
| ES7210 TDM mode reg 0x12 = 00/01/02, format reg 0x11, 4 mics on | Period stays 16 (01 gives 3/16) |
| ES7210 clocks: reg 0x02 (DLL/doubler/div, 10 values), 0x03, LRCK div 0x04/05, OSR 0x07, with reset | No change (bad doubler/OSR = silence) |
| ESP BCLK 32 fs vs 64 fs, MCLK 256 vs 512 fs, fs 16 vs 32 kHz | No change: it's tied to the LRCK count |
| ESP TX+RX both TDM (4×16, `CHEEKO_I2S_TDM=1`) | No change |
| RX TDM with std TX (xiaozhi style) | `i2s_channel_read` times out |
| ES8311 ADC mute, ES7210 DMIC/misc regs 0x0D/0x0E | No change / read-only |
| ESP RX from its own DOUT pad (GPIO matrix loopback) | **Clean, continuous.** The ESP RX path is fine |
| ES7210 MIC1/2 power-down (0x4B=FF) | Data disappears, so the ES7210 is the source |

Conclusion: the ES7210 itself frames its output as 2 of every 16 LRCK frames.
Software can't move that, so it's most likely board-level: how its
LRCK/SCLK/SDOUT pins are wired, or a mode strap. `firmware/hardware_test`
behaves the same; its MIC bar only shows the peak level, so it never caught this.
The "factory backup" (`.flash/factory-backup.bin`) turned out to be
`cheeko_gotchi_hardware_test` v1.0.0-4 (its only app partition), not a
vendor voice firmware. Booting it and then dumping the codec registers
(`CHEEKO_DUMP_CODECS_AT_BOOT=1`) gives ES7210/ES8311 registers byte-for-byte
identical to ours, so there's no known-good config on this device to copy.
Next: the board schematic or vendor voice firmware/sample code, or a logic
analyzer on BCLK/LRCK/DIN.
