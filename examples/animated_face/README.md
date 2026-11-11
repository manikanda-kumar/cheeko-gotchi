# Animated Face

The first app every Cheeko developer should understand: draw a face, blink the
eyes, and make the mouth animate while the device is "talking".

This example is intentionally simple. It teaches the mental model:

- `OnStart()` paints the first frame.
- `OnTick()` updates animation state.
- Touch toggles talking mode.
- The face is drawn with display primitives, so developers can replace it with
  their own character later.

## Try Next

- Change the colors to match your character.
- Swap touch toggling for `OnCloudText()` so the face talks during cloud replies.
- Add emotions such as sleepy, happy, confused, and thinking.

## Voice agent

Run `tools/voice_agent/cheeko_voice.py` on the Mac (see its README). Then hold
the screen for half a second, talk, and release. A short tap still quacks. On
the clock screen the duck takes over during a turn. The label shows
listening / thinking / looking it up, and the beak flaps while the reply plays.
The app only uses `Cloud::StartVoiceSession()`, `EndVoiceTurn()` and
`OnVoiceState()`. The runtime handles mic streaming and speaker playback.

## Clock (default screen)

A digital 12-hour clock (`src/wall_clock.cc`): HH:MM in seven-segment digits
with dim unlit segments, an orange colon, Microduck's camera eye above, and the
date with AM/PM underneath. The screen changes only when the minute changes,
and then only the digits that changed. Each segment is painted straight to its
lit or unlit colour, never cleared first, so nothing flickers. (The earlier
analog face repainted the dial every second and flickered.)

The board has no RTC and Wi-Fi isn't wired up yet, so the time starts at the
firmware's build time (the Mac's local time) and is saved to storage every
minute. After a reboot it resumes from the last save, so time spent powered
off is lost. Set it with VOL+/VOL-: press = 1 minute, hold to repeat (10-minute
steps after 3 s).

## Microduck

The default character is a tribute to Pollen Robotics'
[Microduck](https://github.com/pollen-robotics/microduck): capsule head, flat
bill, camera-ring eye, servo neck, in the four launch colourways.

It speaks with Microduck's own procedural voice. `src/duck_voice.cc` is a C++
port of the robot's `sounds` crate (Apache-2.0, Pollen Robotics; changes are
listed in the file header), rendered at 16 kHz and played with
`speaker().PlayPcm()`. Each colourway uses a different voice seed, so each duck
sounds like itself.

| Input | Clock | Microduck | Cheeko |
| --- | --- | --- | --- |
| Tap | random duck call | random call: `greet`, `chirp`, `peck`, `alarm`, `inquire` or `coo` (never the same twice in a row); beak open while it plays | talk on/off |
| BOOT | next screen | next screen (`chirp`) | next screen |
| VOL+ / VOL- | +/- 1 min (hold to repeat) | next/previous duck (`peck`) | — |
| Shake | `alarm` | dizzy eye + `alarm` | dizzy tone |

BOOT cycles Clock → Microduck → Cheeko. Screen, colourway and clock time
persist in storage.
