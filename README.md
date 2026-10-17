# Linux GPU rendering on the Apple M3 (G15)

> [!WARNING]
> **Early research release, one machine only.** It installs and runs on a
> MacBook Air 13" M3 (J613) whose Linux install uses the macOS 14.7 stub
> firmware, and nowhere else: the driver refuses to start on other models or
> firmware. It has been tested on one such laptop. Expect GPU hangs that need a
> reboot, slow mesh-heavy apps, missing GL features, and no support. Do not use
> it on a machine you cannot afford to reboot.

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

## Can I use it?

Only if all of these hold (details and the commands to check them:
[AGENTS.md](AGENTS.md), section 1):

- a **MacBook Air 13" M3** (model J613);
- it **already runs Linux** through an Asahi-style install (m1n1, U-Boot,
  GRUB);
- the Linux install's **stub macOS firmware is 14.7**
  (`/proc/device-tree/chosen/asahi,os-fw-version` reads `14.7`).

Other M3 machines, other M-series chips and other firmware versions are not
supported yet. The driver needs a set of machine- and firmware-specific values
(render profile, firmware tables, device-tree overlay) that so far exist only
for this configuration; see [kernel/README.md](kernel/README.md).

## Installing

Clone this repository on the laptop and have a coding agent (Claude Code,
Codex or similar) follow [AGENTS.md](AGENTS.md); it is also readable as a
manual guide. It checks the machine, builds the kernel and Mesa from source,
adds the kernel as a **one-time, non-default** boot entry next to your
existing one, loads the driver and runs a rendering test. Your existing boot
entry, m1n1, macOS and disk encryption are left alone, and undoing it is
removing a few files. Building takes a while (a full kernel plus Mesa, on
the laptop).

## What is here

| Path | Contents |
|---|---|
| [AGENTS.md](AGENTS.md) | Install guide, written for a coding agent |
| [kernel/](kernel/README.md) | The kernel: base patches, the G15 driver patch, config, build script, driver loader |
| [docs/g15-spec/](docs/g15-spec/README.md) | How the G15 GPU is driven: memory map, firmware interface and submission, render and compute jobs, shaders, display, known limits. Written for developers who know the M1/M2 (G13/G14) Asahi driver |
| [mesa/](mesa/README.md) | The Mesa G15 render patch, one diff against Gravity Mesa `87ac51d` |
| [tools/gl-test/](tools/gl-test/README.md) | GLES test client with CPU-oracle and llvmpipe checks |
| [tools/desktop/](tools/desktop/README.md) | Example scripts for running a Hyprland session on the G15 GPU |
| [media/](media/) | Demo video, screenshot and numbers |

**Built-in values.** Some tables in the kernel driver hold numbers observed
on one J613 while macOS drove the GPU with the 14.7 firmware (a render
profile, firmware hardware and power words, init data, and device-tree
overlays generated from that machine). They are what makes it work without
any macOS step on that configuration, and the reason it works only there.
Each such file says so at the top.

## How this was made

This is an independent project, not affiliated with or endorsed by Asahi
Linux, and nothing here has been submitted to Asahi. It builds on the Asahi
Linux kernel driver and Mesa work for M1/M2 and on Gravity Mesa's Apple9
compiler. The G15 port, the reverse engineering and these documents were
produced largely by AI coding agents (Claude Code and Codex) working on real
hardware under the project owner's direction. No Apple code, firmware or raw
captures are included; the driver's built-in tables and the documents contain
values observed in captures of macOS on the test laptop.

## License

MIT (see [LICENSE](LICENSE)). The Mesa patch is a change to Mesa and is
under Mesa's MIT license.
