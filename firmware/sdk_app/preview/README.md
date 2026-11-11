# Host preview

Rasterizes an app's display calls on the Mac (no device, no AA) and writes
`idle.ppm`, `talk.ppm`, `blink.ppm` (frames for animated_face). Arg 1 = colourway.

```bash
APP=../../../examples/animated_face/src
uvx --from ziglang python -m ziglang c++ -std=c++17 -I../../../sdk/include -I$APP \
  mock_runtime.cc $APP/*.cc -o preview && ./preview 0
sips -s format png idle.ppm --out idle.png
```
