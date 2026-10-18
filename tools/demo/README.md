# Aquarium demo: the same 3D program on macOS and on the G15 driver

`aquarium-native/` is the [WebGL Aquarium](https://webglsamples.org/aquarium/aquarium.html)
ported to a native C program on desktop OpenGL 2.1 / GLSL 1.20, which both
macOS and the G15 Mesa driver run. It uses the aquarium's own models,
textures and shaders, camera presets, fish motion and draw order; bubbles and
lasers are left out. [raylib](https://www.raylib.com) 5.5 provides the window,
GL context, timing and overlay text.

The scene renders into a fixed-size offscreen target with vsync off. The
overlay shows fps, frame time, fish, draw calls and triangles per frame, and
the program prints one CSV line per second (`t,fish,fps,frame_ms,draws,triangles`).

## Build

```sh
cd aquarium-native
./convert.py .        # downloads the aquarium at a pinned revision, writes ./pack (about 60 MB); needs Pillow
./build.sh            # downloads raylib 5.5 (checksum-verified), builds ./aquarium
./aquarium pack --fish 1000          # V cycles the camera presets
```

Linux builds use raylib's SDL2 backend (`sdl2` or `sdl2-compat` headers), so
on Wayland the program gets its GL context from EGL, i.e. from the G15 Mesa in
the session environment. macOS builds need the Xcode command-line tools and
use raylib's bundled GLFW (Cocoa/NSOpenGL).

## Run the demo

| Script | Where | What it shows |
|---|---|---|
| `g15-aquarium-demo.sh [--cpu]` | the G15 Hyprland session ([../desktop/](../desktop/README.md)) | the aquarium beside `g15top` (GPU job rate, partial renders, errors, power) and btop; `--cpu` runs the same scene on llvmpipe |
| `macos-aquarium-demo.sh` | Terminal on macOS | the aquarium beside `powermetrics` (GPU residency, frequency, power) |

Both step through 500, 1,000, 5,000 and 10,000 fish, 25 s each, inside the
globe (view 0), with a 1440x1440 scene. `--stop` ends either one.

## Results (MacBook Air M3, Linux, 1440x1440, median fps over 20 s)

| Fish | G15 GPU | llvmpipe (CPU) |
|---|---|---|
| 1,000 | 59.1 | 16.2 |
| 5,000 | 38.5 | 10.3 |
| 10,000 | 18.0 | 7.2 |

Zero GPU errors in every run. At these counts the G15 does about one partial
render per frame (the tiler's buffer fills mid-frame), which is a known
performance lever.

## Licenses

The aquarium is Copyright 2009 Google Inc., BSD 3-Clause; see
[aquarium-native/NOTICE](aquarium-native/NOTICE). Its assets are downloaded
by `convert.py`, not kept here. raylib is zlib-licensed and downloaded by
`build.sh`.
