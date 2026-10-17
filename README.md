# Linux GPU rendering on the Apple M3 (G15)

> [!WARNING]
> **Not ready for use yet.** This is an early research release. It documents
> how the M3 GPU works and shows a working demo, but you cannot install it on
> your Mac from here yet: the kernel driver is not published, there are no
> prebuilt packages or install instructions, and it has only been tested on one
> MacBook Air 13" M3 (J613) with macOS 14.7 system firmware. Expect hangs that
> need a reboot, slow mesh-heavy apps, and no support. This notice will be
> removed once there is something people can install and use.

A working GPU-accelerated Linux desktop on a MacBook Air M3 (J613, T8122,
GPU G15G, firmware 14.7): Hyprland and GL/GLES clients rendered by the M3 GPU
through Mesa, using a G15 port of the Asahi AGX kernel driver.

![Hyprland on the M3 GPU](media/g15-hyprland-demo.png)

Video: [media/g15-hyprland-demo.mp4](media/g15-hyprland-demo.mp4) (36 s,
2560x1664: foot, weston-simple-egl, simple-dmabuf-egl and the glmark2 texture
scene, all on the G15 GPU).

## Status

| | G15 | CPU renderer (llvmpipe) |
|---|---|---|
| Hyprland + weston-simple-egl | 57-60 fps | 59-60 fps |
| glmark2 texture scene | 113 fps | 112 fps |
| glmark2 Gouraud mesh | **2 fps** | 95 fps |
| Whole-system CPU busy, Hyprland + simple-egl | 3% | 15% |
| Whole-system CPU busy, Hyprland + glmark2 | 11% | 47% |

Raw numbers: [media/g15-hyprland-demo-numbers.tsv](media/g15-hyprland-demo-numbers.tsv).

Also: 305 of 306 tests in our desktop-GL piglit list, a complete glmark2 run
(score 91; its terrain scene's compute stage is refused, as expected), and a
one-hour soak with 873,352 GPU jobs and no GPU errors or recoveries.

**Known limits.** Mesh-heavy scenes are slow: renders are kept small
because of an unresolved per-tile primitive-list limit on Linux (the
"64-byte cliff"), and each render costs a fixed ~0.7 ms. Desktop GL is at the
2.1 level plus extensions (GLES 3.0), with no MSAA or framebuffer compression.
See [docs/g15-spec/70-known-limits.md](docs/g15-spec/70-known-limits.md) for
the full list and everything known about the open problem.

## What is here

| Path | Contents |
|---|---|
| [docs/g15-spec/](docs/g15-spec/README.md) | How the G15 GPU is driven: memory map, firmware interface and submission, render and compute jobs, shaders, display, known limits. Written for developers who know the M1/M2 (G13/G14) Asahi driver |
| [mesa/](mesa/README.md) | The Mesa G15 render patch, one diff against Gravity Mesa `87ac51d` |
| [tools/gl-test/](tools/gl-test/README.md) | GLES test client with CPU-oracle and llvmpipe checks |
| [tools/desktop/](tools/desktop/README.md) | Example scripts for running a Hyprland session on the G15 GPU |
| [media/](media/) | Demo video, screenshot and numbers |

**Not included yet: the kernel driver.** Parts of it still carry values
taken from macOS captures (a built-in render profile, power tables and
initialization data). Those will be replaced with documented derivations or
local generators before the kernel source is published.

## How this was made

This is an independent project, not affiliated with or endorsed by Asahi
Linux, and nothing here has been submitted to Asahi. It builds on the Asahi
Linux kernel driver and Mesa work for M1/M2 and on Gravity Mesa's Apple9
compiler. The G15 port, the reverse engineering and these documents were
produced largely by AI coding agents (Claude Code and Codex) working on real
hardware under the project owner's direction. No Apple binaries, raw
captures or captured payloads are included; the documents and the test
oracle contain facts and hashes observed in macOS captures.

## License

MIT (see [LICENSE](LICENSE)). The Mesa patch is a change to Mesa and is
under Mesa's MIT license.
