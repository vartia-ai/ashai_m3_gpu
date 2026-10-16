# Firmware interface and submission

How the host boots the G15 firmware (RTKit-2419, interface `V14_7`), what it
shares with it, and how work reaches it. Background assumed: the G13/G14 Asahi
driver. See also [Memory map](10-memory-map.md),
[Render jobs](30-render.md) and
[Compute jobs, faults and recovery](40-compute-and-recovery.md).

## Summary of what changed

| Area | G13/G14 | G15 |
|---|---|---|
| InitData root | Mixed struct: version info, RegionA/B/C pointers, FwStatus, UAT info | 24 consecutive qwords (0xc0 bytes), each a pointer or constant [HW] |
| Firmware status | `FwStatus` with FWCtl channel and halt flags | No FwStatus or FWCtl. Halt/resume words moved into root object q22 [HW] |
| Cache flush / mapping notes | FWCtl channel, DeviceControl 0x17 trick | q22 256-entry mapping ring, kicked with an EP21 0x84 message. No standalone cache-flush command is known [HW] |
| Boot | `Initialize` DeviceControl + MSG_INIT + DEVCTRL doorbell + KICKFW | DeviceControl 0x13 pre-queued, MSG_INIT, wait for q21 ready. No doorbell and no kick at boot [HW] |
| Work channels | One device pipe ring per (type, priority) carries QueueInfo pointers | Same device pipe rings, but every client queue also gets **selected** per-engine firmware channels with their own command ring [HW] |
| Submission gating | none | q4 command gate, q4 pending-submission counter, host submission flag [HW] |
| Concurrency | Many jobs in flight | One job device-wide [HW] |

## Firmware boot

### RTKit endpoints and messages

| Endpoint | Use on G15 |
|---|---|
| EP 0x20 (firmware) | MSG_INIT `0x81 << 48 \| (InitData VA & (2^44 - 1))`; RX doorbells (`0x42 << 48`) for the event/log/stats rings [HW] |
| EP 0x21 (doorbell) | TX doorbell `0x83 << 48 \| doorbell number`; firmware-control note `0x84 << 48` (q22 kick) [HW] |

Doorbell numbers are as on G14: work pipe `pipe_type | (priority << 2)`
(Vertex 0, Fragment 1, Compute 2), DeviceControl 0x11 [HW]. KICKFW (0x10) is
not used at boot or since [HW]. Mailbox, `gfx-handoff` and ASC base: as G14 [HW].

### Boot sequence

1. Build the whole InitData graph (below), with q22 already holding its state
   and ring pointers and the ring empty [HW].
2. Queue one DeviceControl record with opcode 0x13 and a zero body. Do not ring
   the DEVCTRL doorbell. macOS does this at every boot; the firmware consumes
   it while handling MSG_INIT [NATIVE] [HW].
3. Boot RTKit, start EP 0x20 and EP 0x21, send MSG_INIT on EP 0x20 [HW].
4. Poll q21 +0x14 (`firmware_ready`) until it is 1. 1 s is ample [HW].
5. Arm q22 mapping notifications. Bank-1 mappings made before this point are
   not announced; every later map or unmap in ranges 7/8 must be [HW].
6. Set q4 +0x070 (command gate) to 1 and set the host's own
   "command submission enabled" flag. Both stay on for the life of the
   firmware [HW].

The firmware cannot be re-initialised: a new driver instance needs a reboot [HW].

## InitData

### Root (0xc0 bytes)

| Offset | Qword | Contents |
|---|---|---|
| +0x00 | q0 | Fixed interface signature `0x0c08_e21e_8380_0490` [HW] |
| +0x08 | q1 | Init-sequence page (0x4000). Records are 0x18 bytes `{value u64, reg_offset u32, shift u32, kind u32, rsvd u32}`, kind 0 = end, 1 = u32, 2 = u64, 3 = u64 >> shift. On G15/G15G the host fills nothing: the page holds only the end record [HW] |
| +0x10 | q2 | Zero [HW] |
| +0x18 | q3 | Runtime-pointer wrapper (0x490 bytes, below) [HW] |
| +0x20 | q4 | "Globals" configuration block (0xe00 bytes, below) [HW] |
| +0x28 | q5 | Upper dword 1 enables host-mapped firmware allocations; value `1 << 32` [HW] |
| +0x30 | q6..q20 | UAT configuration (0x78 bytes): level info at +0x04; 42-bit GPU VA, 16 KiB pages [HW] |
| +0xa8 | q21 | Shared status (0x20 bytes) [HW] |
| +0xb0 | q22 | Firmware-control / recovery object (0xc3d0 bytes) [HW] |
| +0xb8 | q23 | Runtime/tuning object (0x238 bytes) [HW] |

### q3 runtime-pointer wrapper (replaces RegionB)

| Offset | Field |
|---|---|
| +0x000 | HwDataB pointer (0x1860 bytes) [HW] |
| +0x008 | firmware table pointer [INFERRED] |
| +0x010 | persistent time state (0x88 bytes; firmware sets +0x00 to 1 on first use) [HW] |
| +0x018 | 4 pipes x {Vertex, Fragment, Compute} TX ring descriptors, 0x20 bytes each [HW] |
| +0x198 | DeviceControl TX ring descriptor [HW] |
| +0x1b8..+0x1e8 | Event, FWLog (six sub-rings, payload buffer at +0x1f8), KTrace, Stats RX rings [HW] |
| +0x234/+0x23c/+0x244 | Vertex / Fragment / Compute stats blocks (0xc10 / 0x1248 / 0xe10) [HW] |
| +0x24c | firmware control state (0x60) [HW] |
| +0x2a8 | HWDS-ID counters, 256 x 8 bytes [HW] |
| +0x2b0/+0x2c0 | PB (256 x 0x10) and UMA page-pool (256 x 0x20) descriptor tables; each followed by its firmware-VA copy [HW] |
| +0x2d0 | default USC max threadgroup memory (4) [HW] |
| +0x2d8..+0x3af | firmware-owned runtime state; firmware writes a 0xabcdabcd marker at +0x314 [HW] |
| +0x441 | HwDataA pointer (0x4360 bytes), unaligned [HW] |
| +0x449..+0x48f | zero [HW] |

A G15 TX ring descriptor is four qwords `{read_ptr, write_ptr_shadow,
write_ptr, ring}`. The driver points the first three at +0x00, +0x10 and +0x20
of one legacy 0x30-byte channel state, so legacy ring code works unchanged
[HW]. Each ring has 0x100 entries [HW]. Element sizes: DeviceControl 0x38
(4-byte opcode + 0x34), pipe 0x18, event 0x38, FWLog 0x38 (payload 0xd8),
KTrace 0x38, Stats 0x40 [HW].

### q4 Globals (0xe00 bytes): fields that matter

| Offset | Field | Notes |
|---|---|---|
| +0x070 | **command gate** | 1 after boot, never toggled per job [HW] |
| +0x074 | **pending-submission counter** | see below [HW] |
| +0x078..+0x0a4 | max power, power-interface targets, perf-state cap, performance controller targets and boost parameters | from the ADT [HW] |
| +0x7a8..+0x7dc | smart-idle and keep-alive tuning | [INFERRED] |
| +0x7e4..+0x96b | zeroed by the host; +0x964 is the count of 0x18-byte register-patch records at +0x7f4, 0 at boot | [HW] |
| +0x97c/+0x980/+0x984 | progress-check intervals 3D / TA / CL | [INFERRED] |
| +0x9a0..+0x9bc | idle-off delays, early-wake timeout, CL context-switch and kill timeouts, CDM backoff timeout | [INFERRED] |
| +0xdf9 | soft-fault settings = 3 (as macOS); G15 always soft-faults | [HW] |

### Power and clock data

Power and clock words live in q4 (+0x078..+0x0a4, curve tables near +0x26c),
HwDataA (0x4360 bytes: DPE PPT and leakage-update configs) and HwDataB (0x1860
bytes: per-level frequencies, core/SRAM voltages, relative max powers,
low-perf states). All derive from the machine's ADT performance states,
leakage fuse words and a few fixed T8122 constants; a generator from those
inputs reproduced every power word of a native capture [NATIVE] [HW]. Unlike
G13/G14, firmware indexes **distinct core-voltage levels**, not ADT states:
merge states that share a voltage and set `max_pstate` to the last level [HW].

### HwDataB +0xa74 and the power-down SError

HwDataB +0xa74 must be 0, as in every native J613 image [NATIVE] [HW]. The
G13/G14 HwDataB layout, applied to G15, leaves a 1 there. Firmware copies
the word to its internal configuration and uses it as a divisor: when it is
nonzero, every count that divides evenly raises an event for the firmware's
power task, `agx_power`. That event's handler reads the 64-bit RGX counter at
+0x1138/+0x113c. If the GPU has just powered down (Smart Idle standby, about
0.7 ms after a job), the read takes an L2 access fault and the firmware dies
with an SError roughly 1.1 ms after the last completion. Large renders that
load a whole surface make that timing far more likely; the bug reaches every
workload eventually (about one in 500,000 small renders with waits between
them). With +0xa74 = 0: 100,000 renders of the fastest reproducer, which
otherwise crashed within a few hundred, completed cleanly [HW].

How it was found: the crash dump's PC is the exception handler and the FAR
looks invalid for an SError, but the RTKit crashlog's `CasC` section holds
the coprocessor's L2 error registers (`L2C_ERR_STS` with ACCESS_FAULT,
`L2C_ERR_ADR` = the faulting physical address 0x29000113c), and the `Crtk`
section names the crashing task. m1n1's ASC crash decoder reads both. The
Linux RTKit parser prints only "Unknown crashlog section" for them, so the
G15 driver hex-dumps every undecoded section on a firmware crash.

### q21 shared status (0x20 bytes)

+0x00 host flags (0), +0x04 banner guard (1 before boot), +0x14
`firmware_ready` (firmware sets 1) [HW]; +0x0c busy flag and +0x18
power-state index [INFERRED]. This is not the legacy FwStatus layout.

### q22 firmware-control object (0xc3d0 bytes)

q22 replaces FwStatus and the FWCtl channel.

| Offset | Field |
|---|---|
| +0x4030..+0x4054 | trace enable, descriptor pointer and count [INFERRED] |
| +0x40a0 | pending-stamp table, 128 x 8 bytes, legacy V13.5+ encoding (`valid = info & 1`, `slot = (info >> 4) & 0x7f`) [HW] |
| +0x44a0..+0x4567 | fault/restart information, firmware-written [NATIVE] semantics [UNKNOWN] |
| +0x4568 / +0x4570 | mapping-ring state and ring pointers [HW] |
| +0x4580 | halt count [HW] |
| +0x4590 | halted [HW] |
| +0x45a0 | resume (host writes 1) [HW] |
| +0x45b0 | "stopped for host recovery" state [INFERRED] |
| +0x45c0 | counter that increments with completed work; what exactly it counts is [UNKNOWN] (see [recovery](40-compute-and-recovery.md)) |
| +0x45c4 | host flag, 1 at boot; +0x0000, +0x0008, +0xc3c8, +0xc3cc are 0 [HW] |

**Mapping ring.** One 0x1840-byte allocation in range 7: a 0x20-byte state
(`read_idx` +0x00, `write_idx` +0x10) and, at +0x40, 256 entries of 0x18 bytes
[HW]:

| Offset | Field |
|---|---|
| +0x00 | GPU VA (firmware VA is identical) |
| +0x08 | physical address >> 12 |
| +0x0c | secure context ID: 0 for kernel bank-1 mappings, map and unmap |
| +0x10 | firmware page count, `1 << (GART page shift - 14)` |
| +0x12 | flags: bit 0 map, bit 1 special 64 MiB aperture (range 8), bit 2 property |
| +0x14 | zero |

Range-7 records use flags 1/0 (map/unmap), range-8 records 3/2. Per-client
(bank-0) mappings are not announced: page-table write plus TLBI only [HW].

Producer rules [HW]:
- Write the entry, `dmb`, then advance `write_idx` modulo 256.
- When occupancy (`write - read` mod 256) has both bits 0x80 and 0x40 set,
  send the EP21 0x84 note before inserting. If the ring is full, sleep 10 ms
  and recheck.
- **Drain** before an unmap tears down a PTE: send the 0x84 note, then spin
  until `read_idx == write_idx`, 150 ms timeout.
- The driver also drains q22 before publishing each job, once all its mappings
  exist (a first job drains about 19 entries). Getting q22 wrong crashes the
  firmware in the drain ("UAT bad flush") [HW].

### q23 runtime/tuning object (0x238 bytes)

Firmware imports host update blocks (clearing each "update" word) and writes
idle counters and an epoch here [INFERRED]. The host zeroes +0x1e8, which gates
a pre-ready branch, and seeds boot defaults in the +0x158 block [HW].

## The channel model

G15 keeps the device-wide pipe rings (4 priorities x {Vertex, Fragment,
Compute}) but uses them only as scheduler kicks. The command itself goes into
a **selected channel** owned by the client queue: one Compute (CL) channel,
built on the queue's first compute submit, and one TA plus one 3D channel,
built on its first render [HW]. Each selected channel leases elements from device-global firmware resource
stacks, allocated in bank-1 apertures [HW]:

| Resource | Size | Aperture |
|---|---|---|
| Channel state (`QueueInfo` 0xb0 at the start; `gpu_buf` = state + 0xb0) | 0x24c0, 3 per 0x8000 block | range 8 |
| Uncached channel memory (ring control) | 0x2860, 3 per 0x8000 block | range 8 |
| Cached channel memory (command ring) | 0x2860, 3 per 0x8000 block | range 7 |
| Scheduler state (one per queue, shared by its channels) | 0x40 (host clears 0x38) | range 8 |
| Timestamp-queue state (pointed to by `QueueInfo` +0x10) | 0x18 | range 7 |
| Event stamps, 36 x 4 bytes / event controls, 36 x 0xc0 | 0x90 / 0x1b00 | range 7 / range 8 |
| SKU stream slots (Compute: 0xf0 x 0x300) | page-rounded | range 8 |
| HWMetrics ring, 256 x 0x40 records | 0x4000 | range 7 |

**Ring.** Uncached channel memory holds the control words: +0x00 `doneptr`
(firmware), +0x30 GPU read pointer, +0x40 host `wptr`, +0x50 ring size. The
cached channel memory holds the ring itself: one 8-byte command FWVA per entry.
The ring size is 0x500 (1280) entries on every channel: `min(0x50, 0x80) << 4`
for Compute, and the same value in the captured TA/3D records [HW] [NATIVE].
Resetting a channel zeroes +0x00..+0x40 and writes +0x50 [HW].

Writing a command [HW]: refuse if `(wptr + 1) % size == doneptr`; store the
command FWVA at ring[`wptr`], `dmb ish`, then store the new `wptr` at +0x40.
The scheduler entry (below) carries the new `wptr`.

The job is complete on the transport when `doneptr == wptr` [HW]. QueueInfo
priority words (+0x30..+0x4b) follow Apple's non-foreground priority/QoS branch
[INFERRED].

### Scheduler entry (device pipe ring, 0x18 bytes)

| Offset | Field |
|---|---|
| +0x00 | submission time, `CNTPCT_EL0` (macOS writes `mach_absolute_time`) [HW] |
| +0x08 | FWVA of the selected channel's `QueueInfo` [HW] |
| +0x10 | pipe type (u32): Vertex 0, Fragment 1, Compute 2 [HW] |
| +0x14 | new `wptr` (u16) [HW] |
| +0x16 | `EventManager` slot (u8) [HW] |
| +0x17 | "new": 1 on this channel's first submission only [HW] |

### Stamps and event controls

- `EventManager` keeps 128 global slots. On G15 each slot's counter starts at
  0, not `slot << 24` [HW].
- Each channel has 36 command-buffer states (stamp *i* in range 7, event
  control *i* in range 8), selected round-robin: increment, then modulo 36 [HW].
- Event control: +0x00 stamp FWVA, +0x08 state sequence, +0x10 effective record
  count (0x50 for Compute), +0xa8 sentinel `u64::MAX`, rest zero. At
  construction only +0x00 is set; before reuse, clear stamp and block and
  rewrite the four fields [HW].
- The command points at its event control (RunCompute +0x14), and the
  `G15JobMeta` tail keeps the legacy stamp, firmware-stamp, value, slot,
  event-control index, UUID (+0x24) and event sequence (+0x28). Its first
  dword is engine-specific state, not the legacy flags [HW].
- Completion arrives as the usual `Flag` event on EP 0x20 [HW].

### Publishing one job

Per publication, in this order [HW]:

1. Check that the GPU has not crashed, EP 0x21 exists and both gates are on:
   q4 +0x070 and a host-only flag (Apple's accelerator +0x622, checked again
   before the doorbell [INFERRED]).
2. Raise q4 +0x074 (pending submissions) from 0 to 1. Native captures show
   it at 1 at every publication and 0 between jobs [NATIVE]; publishing with it
   already at 1 loses the job and wedges the GPU, so any other starting value
   is refused (EIO) and the increment undone.
3. For a render or shared UMA pool: write the command's UMA coherency epoch
   (below).
4. Write the command(s) into the selected ring and update `wptr`.
5. Put the 0x18-byte entry on the device pipe ring for (type, priority).
6. Recheck the gates, then ring the EP 0x21 doorbell.
7. **Scheduler acknowledgement:** spin until that pipe ring's `read_ptr`
   reaches the entry (1 s timeout). This proves the scheduler consumed and
   returned from the entry. Engine completion is later and asynchronous.
8. On completion (normal or error), lower q4 +0x074 to 0 exactly once.

Any failure after step 4 leaves a firmware-visible entry. The driver then
fail-stops: it marks the GPU crashed and refuses further submits [HW].

**Render pass.** A render is one publication of two channels under one q4
raise: [Barrier, Fragment] on the 3D channel, then [InitBuffer, Vertex] on the
TA channel. macOS publishes the 3D channel first [NATIVE]; the Barrier waits on
the TA stamp, so the order only decides which channel firmware sees first. The
count stays 1 across both publications and returns to 0 after the last of the
two completes [NATIVE] [HW].

### UMA coherency epoch

Each channel holding a UMA (FList) lease has a page-pool state. Its uncached
state +0x48 is the coherency value; the pool state's +0x50 is a mirror that
firmware updates when a command completes [HW] [NATIVE]. Before each
publication the host writes `mirror + 1` (1/0, 2/1, 3/2 in captures)
[NATIVE] [HW]. A render pool shared by TA and 3D writes
`max(mirror, current) + 1` per command, immediately before that command's ring
write: 1 before 3D and 2 before TA from a zero mirror [NATIVE] [HW]. Undo the
write if the command never reaches the firmware [HW].

### One job at a time

Only one G15 job (one compute command or one TA/3D pair) may be in flight on
the device [HW]. The driver takes a device-wide slot before building a command
and frees it in the completion callback, before the fence signals; other
submitters sleep instead of getting EBUSY, so Mesa can still pipeline [HW].

### DeviceControl on G15

Records are 0x38 bytes; V13.3+ G14's 0x0d is gone, shifting later opcodes
[INFERRED]: 0x0d GrowTVBAck, 0x0e RecoverChannel, 0x0f IdlePowerOff, 0x11
ReleaseResource, 0x13 startup, 0x17 DestroyContext, 0x19 Initialize. Only
0x13 and 0x11 are used [HW]. 0x11 is queue teardown: bytes +0x08..+0x0b come
from scheduler state +0x27/+0x00/+0x01/+0x04, the scheduler-state FWVA is at
+0x0c [HW].

### Firmware cache flush

G13/G14 flush the firmware cache with a DestroyContext carrying invalid
context IDs. G15 does not, and no standalone flush command is known
[INFERRED]. The G15 flush therefore always fails: "Failed to flush FW cache" is
logged whenever freed firmware-private memory passes the garbage threshold,
and that memory is kept rather than reused. The message is routine [HW].

### Events and faults

- Event tag 0 (Fault) is a no-op on G15 (macOS's handler is empty) [HW].
- MMU faults and firmware timeouts both arrive as tag 4 (Timeout). Recovery
  walks the q22 pending stamps, clears +0x4590 and writes 1 to +0x45a0; the
  firmware resumes in about 0.3 ms [HW]. See
  [Compute jobs, faults and recovery](40-compute-and-recovery.md).
- A hang the firmware never times out (shader infinite loop, some render hangs)
  never completes, and queue-teardown commands sent meanwhile crash the
  firmware. Fail-stop on a host timeout instead [HW].

## MMIO access rules on J613

| Fact | Tag |
|---|---|
| The J613 `/soc` bus is `nonposted-mmio`: every hand-mapped register range must use `ioremap_np`. Posted writes to PMGR, DCS, fabric or the AGX report bank reset the SoC; the same writes non-posted are harmless | [HW] |
| Reading the RGX status page (sgx +0xc000..+0xd000) with the GPU idle, or during passing work, resets the SoC | [HW] |
| One read of c020/c028/c038/c120 (and the init registers 0x16420/0x1c058/0x1c858/0x1ec78) while the fragment unit is parked, or at the firmware-timeout event, is safe; reading the same set every few microseconds during a hang is not | [HW] |
| Firmware scheduler words (ready 0xfffffc00000a8c78, wait 0xa8d70, unit-1 active 0xa8f48) read the same through a held WC mapping, a per-read mapping or a WB mapping with DC CIVAC. A passing render never sets them: it finishes before the firmware parks (about 0.1 ms) | [HW] |

## Userspace interface additions

All of the following apply to G15 only; other generations return ENODEV.

| Item | Meaning |
|---|---|
| `DRM_IOCTL_ASAHI_G15_OPT_IN` (ioctl 13, `W`) | `struct { u32 version = 1; u32 flags = 0; }`. Until a file calls it, every SUBMIT on that file is ENODEV and reaches no queue. Lasts for the file [HW] |
| GET_PARAMS `usc_window_start/end` | [0x100_1000_0000, 0x101_0000_0000). Shaders and everything the USC fetches must be bound here. USC fetch uses a fixed 1 TiB base plus a 32-bit offset; `usc_exec_base` is ignored [HW] |
| GET_PARAMS `render_context_start/end` | [0x10_0400_0000, 0x11_0000_0000). VDM streams and PPP state go here; their pointers are offsets from 0x10_0000_0000. The kernel keeps the 64 MiB below for its own windows; a user VDM stream below the floor is EINVAL [HW] |
| GET_PARAMS limits | `max_commands_per_submission = 1`, `max_attachments = 0`, `FEATURE_SOFT_FAULTS` always set [HW] |
| VM_BIND | Ordinary user range, plus the USC window (G15 VMs only). READ maps as GPU shared read-only, READ\|WRITE as GPU shared read-write, uncached [HW] |
| SUBMIT `flags = 0` | Mesa's plain compute (header + full `cmd_compute`, flags 0, real CDM base) and Mesa's render (optional SET_FRAGMENT_ATTACHMENTS with 1..16 entries, then one render). Up to 256 in-syncs and 256 out-syncs, timelines allowed, barriers NONE or 0 [HW] |
| Compute + render in one submit | ENODEV. Compute and render use separate channels, and ordering them needs the untested G15 Barrier [HW] |
| `DRM_IOCTL_ASAHI_G15_READBACK` (ioctl 11) | Debug: copies the output of the last kernel-owned launch or render target [HW] |
| `DRM_IOCTL_ASAHI_G15_BRIDGE` (ioctl 12, root only) | `struct { u64 render; u32 flags = 0; u32 pad = 0; }`. Installs (or, with `render = 0`, removes) a render bundle [HW] |

**The bridge** is transitional. Some render-pass setup is not yet derived from
the command, so root installs a natively captured render bundle once per
device, and each plain Mesa render runs on a per-submit copy of it. The
render's VDM stream, first attachment (the render target), register fields and,
optionally, its background and end-of-tile programs replace the bundle's;
scissor, depth bias and partial-render programs still come from the bundle
[HW].

## Open questions

All [UNKNOWN]:
- How TA/3D channel-state values (priority words, process identity, SKU slot
  geometry, UMA pool sizes) are chosen in general. Today they are copied from
  one native capture.
- The q22 fault/restart block (+0x44a0..+0x4567) and +0x45b0.
- Whether more than one job can be in flight (q4 +0x074 > 1), and how macOS
  allows concurrency.
- Whether a q22 drain is needed before every publication or only before unmaps.
- Whether a standalone firmware cache-flush command exists.
- Why the firmware watchdog misses compute spins and some render hangs, and how
  macOS stops a running job.
- The other DeviceControl opcodes, stats tag 0x0f, the q23 update blocks, the
  G15 Barrier (needed for compute + render in one submit) and the foreground
  priority/QoS branch.
