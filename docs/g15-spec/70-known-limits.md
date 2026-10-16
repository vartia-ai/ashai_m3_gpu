# Known limits and open problems

What the demo driver does not do yet, why, and what is known about the cause.
Facts are tagged as in the [README](README.md).

## What works (for context)

| Measure (MacBook Air M3, 2560x1664) | G15 | llvmpipe (CPU) | Tag |
|---|---|---|---|
| Hyprland + `weston-simple-egl` | 57-60 fps | 59-60 fps | [HW] |
| glmark2 texture scene | 113 | 112 | [HW] |
| glmark2 full run (offscreen), score | 91 | | [HW] |
| Whole-system CPU busy, Hyprland + simple-egl | 3% | 15% | [HW] |
| Whole-system CPU busy, Hyprland + glmark2 windowed | 11% | 47% | [HW] |
| Desktop GL piglit list | 305 / 306 | | [HW] |
| One-hour soak (compositor + clients) | 873,352 jobs, 0 errors | | [HW] |

## 1. Mesh-heavy scenes are slow

| Measure | G15 | llvmpipe | Tag |
|---|---|---|---|
| glmark2 Gouraud-shaded mesh (cat/horse models) | 2-3 fps | 82-95 fps | [HW] |

**Why.** Mesa splits every render into small pieces: at most 32 primitives
and 8 draws (`ASAHI_G15_BATCH_PRIMS`, `ASAHI_G15_BATCH_DRAWS`). A
few-thousand-triangle mesh becomes 100+ GPU renders per frame, each with a
fixed per-job cost of roughly 0.7-0.8 ms on the kernel/firmware side. Larger
renders are not safe because of the per-tile list limit (section 2).
Raising the cap alone does not help: at 64 primitives mesh renders are clean
but barely faster; from 128 upward they start to time out (glmark2 shading
and build, caps 32/64/128/256/1024: errors 0/0/4/116/189) [HW].

**What fails above the cap.** Renders whose busiest tile accumulates more than
64 bytes of primitive list. A deterministic reproducer is three overlapping
256-triangle grid draws in one render (60 of 60 frames time out) [HW]. Each
failure is a recovered firmware timeout; the client continues.

## 2. The per-tile list limit ("64-byte cliff", G2) — open

**Symptom.** The tile accelerator (TA) writes each tile's primitive list into
128-byte chunks. On Linux, when a tile's list runs past byte 0x40 of a chunk
without a link, the fragment pass never finishes that tile and the firmware
reports a timeout. One 128x128 tile passes with 160 stacked triangles (list
ends at 0x3f) and hangs with 168 (ends at 0x43); full-panel overdraw passes
at 50 quads and hangs at 56 [HW]. macOS runs the same work, with
byte-identical lists, up to x1000 [NATIVE]. Details: [Render jobs,
section 9](30-render.md#9-tile-lists-and-the-per-tile-list-limit-g2).

**Workaround in the driver (relinker).** Mesa marks renders that may produce
long lists. For those, the kernel waits for the TA, then rewrites every list
that runs past 0x40 into chunks that end by +0x38 with a link to a fresh
chunk. The fragment pass consumes those exactly (hotspot x168/x192/x1000 and
overdraw x56/x200 pass in one render) [HW].

**Why the workaround does not lift the mesh cap.** The relinker reads the
TA's output from the CPU. After a queue's first render, that output is
usually not in memory when the TA completes: the GPU keeps the tilemap, TPC
and even the list chunks in a write-back cache that the CPU cannot see. The
GPU's page-table entry and the CPU view resolve to the same physical page,
and both a cached and a non-cacheable CPU read return zeros, while the
fragment pass still draws the correct image [HW]. How much reaches memory
varies between identical runs. A full GPU TLB invalidate after the TA,
another GPU context running in between, and the firmware's resource-release
command (DeviceControl 0x11) do not write it back [HW] [STATIC]. So the
relinker cannot see later renders' long lists, leaves them alone, and those
renders hang. No GPU cache write-back primitive is known.

**What has been excluded as the macOS/Linux difference** (each tested on
hardware or proven equal to macOS):

- Every per-render input: TA/3D register lists, full command bodies,
  VDM/PPP words, shaders (macOS's compiled fragment shader swapped in), tile
  programs, coefficient tables, TVB/tilemap/TPC/heapmeta layout and VAs,
  block IDs, PTE classes, context-switch buffers (all-zero on both).
- Device setup: InitData, HwDataA/HwDataB, the boot-patch table, firmware
  image, the DeviceControl startup stream, PMP mailbox transcript and
  registry values, DCS/fabric/power setup, memory-cache (MCC) data streams
  for the GPU (macOS requests size 0), and CPU writes into the GPU's MMIO
  (traced from macOS's first instruction: only the 24 fabric words Linux
  also writes). The macOS GPU kext has no G15 hardware-workaround (BRN) code.
- Timing, power and placement: serialization, delays to 100 ms, idle-off,
  performance state, physical address and page colour of the list memory,
  concurrent display and memory traffic.
- History on Linux: a broad warm-up (214 jobs across every supported render
  mode, compute, large targets, relinked long lists) and repeating the same
  long list on the same pages (30 of 30 hang) do not lift it.
- Content-level controls: padding experiments show some content past 0x40 is
  consumed, so position alone is not the rule; the TPC tail value is not the
  cause.

**What is left.** The remaining difference lies in state not visible from
Linux: the GPU firmware's internal runtime state or hardware state that
macOS establishes before the measured render [UNKNOWN]. Two next steps need
macOS-side captures: a diff of firmware memory (Linux at the hang against
macOS during the same render), and a replay of the exact macOS producer
(all command, shader and state bytes) through the Linux driver to separate
"input" from "hidden global state".

## 3. Other limits

| Limit | Status | Tag |
|---|---|---|
| Partial renders (TVB overflow mid-render) | Never triggered successfully; the list limit is hit first | [HW] |
| MSAA, framebuffer compression | Not implemented | — |
| Desktop GL version | GL 2.1 level plus extensions; apps needing GL 3.3 (e.g. Ghostty) run on the CPU renderer inside the G15 session | [HW] |
| One GPU job at a time device-wide | Kept: overlapping two VMs' jobs crashes the firmware | [HW] |
| Hangs the firmware does not report | Fail-stop cleanly via the kernel watchdog; the GPU then needs a reboot | [HW] |
