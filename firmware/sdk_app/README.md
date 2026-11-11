# SDK App Host

Builds one SDK app (`examples/<name>/src/*.cc`) against `firmware/cheeko_runtime`,
an implementation of `sdk/include/cheeko.h` on the Cheeko Gotchi board drivers
(adapted from `firmware/hardware_test`).

```bash
export PATH=~/esp/py312bin:$PATH && . ~/esp/v5.5.2/esp-idf/export.sh
cd firmware/sdk_app
idf.py build                                         # examples/hello_display (default)
idf.py -DCHEEKO_APP=../../examples/self_test build   # any other app dir
idf.py -p /dev/cu.usbmodem101 flash                  # then unplug/replug USB
```

## What works

| API | Status |
| --- | --- |
| `display()` Clear/Text/CenterText/Rect/FillRect/Circle/FillCircle/Line | yes — LVGL canvas in PSRAM, Montserrat 20 |
| `speaker()` Tone, SetVolume | yes (16 kHz I2S, ES8311) |
| `speaker()` PlayPcm, IsPlaying | yes — **added to cheeko.h** ([Experimental]); async speaker task, copies + resamples to 16 kHz |
| `mic()` Start/Stop → `OnMicAudio` | yes, 16 kHz mono (MIC1); requested rate ignored |
| `touch()`, `OnTouch` | yes, uses the calibration saved by hardware_test; hold BOOT at power-up to recalibrate |
| `OnButton` | yes, all four keys |
| `motion()`, `OnShake` | yes (LIS2DH12, g units) |
| `storage()` | yes, NVS namespace `cheeko_app` (keys ≤ 15 chars) |
| `log()` | yes, USB serial 115200 |
| `display().Image`, `speaker().Play`, `wifi()`, `cloud()` | stubs — log a warning |

## Notes

- Octal PSRAM is enabled for the 240×296 canvas. It occupies GPIO33–37, so the
  charge-detect pin (GPIO37) is not configured in this build.
- After `flash`, power-cycle the board (or open the serial port, which resets it).
