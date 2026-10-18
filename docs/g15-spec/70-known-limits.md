# Known limits and open problems

What the driver does not do yet, why, and what is known about the cause.
Facts are tagged as in the [README](README.md).

## What works (for context)

| Measure (MacBook Air M3, 2560x1664) | G15 | llvmpipe (CPU) | Tag |
|---|---|---|---|
| glmark2 full run (offscreen), score | 443 | | [HW] |
| glmark2 in a Weston window, 7 scenes, score | 343 | 151 | [HW] |
| glmark2 Gouraud-shaded mesh (offscreen run) | 479 fps | | [HW] |
| Desktop GL piglit list | 306 / 306 | | [HW] |
| One-hour Hyprland soak (compositor + clients) | 676,080 jobs, 0 errors | | [HW] |
| Fresh GL processes in a row (one test each) | 1,000 without an error | | [HW] |

## 1. The per-tile list limit ("64-byte cliff", G2) — fixed

**Symptom (before the fix).** On Linux, a tile whose primitive list ran past
byte 0x40 of a 128-byte chunk hung the fragment pass: the firmware parked in
its wait on unit 1, no pixel of that tile was shaded, and it reported
`Timeout`. One 128x128 tile passed with 160 stacked triangles and hung with
168; macOS ran the same work, with byte-identical lists, up to x1000 [HW]
[NATIVE].

**Cause.** Bit 0 of the Fragment ISP control word `0x15021`. The driver's
built-in render profile used `0xc001`, the value macOS sends for small
renders; macOS's long-list renders use `0xc000`. Moving only that bit moves
the failure both ways: macOS's own long-list job hangs on Linux with
`0xc001`, and Mesa's renders pass with `0xc000` [HW] [NATIVE]. What the bit
selects inside the fragment pass is not known [UNKNOWN].

**Fix.** The kernel clears bit 0 in every render
(`g15_frag_15021_bit0`, default 0). Mesa's per-render primitive and draw caps
and the kernel's list relinker, both workarounds for this limit, are off by
default; the relinker remains as `g15_relink=1|2` for diagnostics [HW].

## 2. Partial renders — working

When the tiler fills its buffer in the middle of a render, the firmware runs a
partial render: it stores the tiles so far with the partial end-of-tile
program and reloads them with the partial background program. Mesa's own
reload and store programs serve as both; the kernel places them in
`RunFragment`'s partial slots [HW].

| Test | Result | Tag |
|---|---|---|
| One render of 262,144 triangles over a 128x128 D32F grid | 4 partial renders, colour and depth exact (macOS also runs 4 for this job) | [HW] [NATIVE] |
| 5,000 full-panel triangles in one draw, 3 frames | 72 partial renders, every pixel exact | [HW] |
| glmark2 with no per-render cap | completes; 1,700+ renders ran partials, 0 errors | [HW] |

`ASAHI_G15_MESA_PARTIAL=0` selects the captured macOS partial programs'
addresses instead, as a diagnostic; no program payload is included, so with
the built-in profile those addresses are unbacked and fault [HW].

## 3. Hangs the firmware does not report

A job that never finishes and that the firmware does not time out (an
infinite-loop shader is the usual example) is caught by the kernel's
watchdog (`g15_job_timeout_ms`, 5000 in the loader). The watchdog fails every
pending job and refuses new submits, so nothing hangs, but **the GPU then
needs a reboot** [HW]. Faults and the timeouts the firmware does report
recover without one [HW].

macOS recovers from this case. Its captures show no host message before the
firmware's timeout, but competing work on another 3D queue published shortly
before it [NATIVE]. The best-supported reading is that the firmware's
context-switch and kill limits (40 and 50 ms, which Linux sets to the same
values) kill a context that does not yield to that work [STATIC]
[INFERRED]. Admitting one competing compute job while a compute spin runs did
not reproduce it: the firmware ran the competing job while the spin stayed
live, and killed nothing [HW]. A fragment-side spin with competing 3D work, the shape macOS
shows, is the next test [UNKNOWN].

## 4. A short wait after each render

One G15 job runs at a time device-wide, and the next render waits until about
300 us after the previous one completed (`g15_reseed_min_age_us`) before it
prepares its firmware event state. The firmware is still retiring the
finished 3D job's event control for 26-187 us after the completion callback;
seeding the channel's next event state inside that window hangs the next job
(TA or Fragment two commands behind, no firmware timeout) [HW]. The wait
sleeps for most of the interval and spins the last 50 us
(`g15_retire_spin_us`). Without it, glmark2 scores about 15% higher on the
same build, but back-to-back small renders hang [HW]. No firmware completion
field that marks the end of that retirement in every workload has been found
[UNKNOWN].

## 5. Other limits

| Limit | Status | Tag |
|---|---|---|
| MSAA, framebuffer compression | Not implemented | — |
| Desktop GL version | GL 2.1 level plus extensions (GLES 3.0); apps needing GL 3.x or 4.x (Ghostty 1.3 needs 4.3 core) run on the CPU renderer inside the G15 session. GL 3.0 needs 4x MSAA, GL 3.2 geometry shaders (compute-emulated in Asahi's Mesa, and G15 GL contexts cannot run compute jobs yet), GL 3.3 timer queries | [HW] |
| One GPU job at a time device-wide | Kept: overlapping two VMs' jobs crashed the firmware | [HW] |
| GPU suspend | Not supported | — |
