# Compute jobs, faults and recovery

This section covers how a compute job reaches the G15 firmware, how faults and
timeouts are reported, and how the host recovers from them. It assumes the
G13/G14 Asahi driver as background: `RunCompute`, the work-queue/pipe model,
`FwStatus` halt/resume and `mark_pending_events`. Channel construction, the q22
shared region and the VA windows are described in
[Firmware interface and submission](20-firmware-and-submission.md) and
[Memory map](10-memory-map.md). CDM stream contents and launch records are in
[Shaders](50-shaders.md).

## Part 1: compute jobs

### Submission path at a glance

| Step | G13/G14 | G15 |
|---|---|---|
| Pipe message | `RunWorkQueue` pipe-ring entry with pipe type, QueueInfo pointer, wptr, event slot, new flag | A compact 0x18-byte scheduler entry: timestamp (+0x00), work-queue FWVA (+0x08), pipe type (+0x10, Compute = 2), u16 wptr (+0x14), u8 event slot (+0x16), u8 "new" flag (+0x17) [HW] |
| Channel | Per-queue compute pipe | One firmware-selected Compute ("CL") channel per client, built lazily on the first compute submit, with event-control blocks, an SKU stream slot pool, a channel command resource and a UMA (FList) pool lease [HW] |
| Command | `RunCompute` with a host-built microsequence (StartCompute / EndCompute ops) | `RunCompute` (0x880 bytes) whose microsequence pointer is replaced by an **SKU stream**: an 0x2c0-byte stream in a 0x300-byte slot, 0xf0 slots per channel [HW] |
| Job parameters | `JobParameters1` (G13) or a register array (G14X) | Register array only, with a different, fixed register set (below) [HW] |
| Concurrency | Many jobs in flight | One G15 job runs at a time, device-wide [HW] |

The SKU stream holds type-0xb setup (pointing at the command's register array
and `JobParameters2`), start timestamp, compute wait-for-idle, end timestamp,
a type-0xc retirement packet and a finish word. After normalising pointers and
per-command identity, the host-built stream is byte-identical to the one macOS
publishes for the same job [NATIVE] [HW]. There is no separate "stock" and
"user" variant of it [NATIVE].

### RunCompute layout (G15, V14_7)

Offsets inside the 0x880-byte command. Fields not listed are zero or are
ordinary work-queue bookkeeping shared with G14.

| Offset | Field | Value / note |
|---|---|---|
| +0x04 | counter | as G14 [HW] |
| +0x10 | context ID (u32) | the VM's firmware context slot [HW] |
| +0x14 | event-control FWVA | filled when the command is finalised for its channel [HW] |
| +0x20 | register array | see below [HW] |
| +0x740, +0x748, +0x750 | raw compute-encoder state | zero for every job tested [HW] |
| +0x760 | SKU stream FWVA | the selected SKU slot [HW] |
| +0x768 | SKU stream size | 0x2c0 [HW] |
| +0x76c | `JobParameters2` (0x60 bytes) | preempt_buf1 at +0x28, `cdm_ctrl_stream_end` at +0x30, a copy of the 0x1a440 value at +0x58 [HW] |
| +0x7cc..+0x7e3 | compact encoder metadata | +0x7d8 = 0x1e; +0x7e0 = 0xffffffff on every traced job [NATIVE] [HW] |
| +0x7e4 | job meta (G15 layout) | stamp, fw_stamp (+0x7f0), stamp value (+0x7f8), slot, UUID (+0x808), event sequence (+0x80c) [HW] |
| +0x810..+0x837 | command time, timestamp and user-timestamp pointers | as G14 [HW] |
| +0x83e..+0x85e | UMA pool state FWVA, "prepared" byte (1), min/ideal pool size, HWMetrics FWVA (unaligned fields) [HW] |
| +0x85f | context-ID generation byte | incremented each time the context slot is newly allocated [INFERRED] |
| +0x860 / +0x870 | context-store request / completion timestamps | zero on a healthy job [HW] |
| +0x878 | recovery marker (u32) | the firmware sets it to 1 in its recovery callback before reconciling this command's UMA pool state [INFERRED] |

`sampler_heap`/`sampler_count` have no place in the G15 command: G13/G14's
`EncoderParams` block is gone [HW]. Where samplers go on G15 compute is
[UNKNOWN].

### Register array

G15 uses the 12-byte `RegisterArray` entries of G14X. Register IDs are written
with bit 0 set (0x12090 is emitted as 0x12091) [HW]. The kernel writes this
fixed set for every compute job. All values match a macOS application compute
job [NATIVE] and are verified to run shaders [HW]:

| Register | Value | Meaning |
|---|---|---|
| 0x1a510 | preempt buffer base | preempt_buf1 |
| 0x1a420 | CDM control stream base | user stream (ordinary path) or kernel stream |
| 0x1a4d0, 0x1a4d8, 0x1a4e0, 0x1a4e8 | preempt base +0x1480, +0x1488, +0x1490, +0x1498 | preempt_buf2..5 |
| 0x1a440 | 0x154024201 | G14X writes 0x24201; also mirrored into `JobParameters2` +0x58 |
| 0x1a458 | 0x10c08860 | same on the macOS system and application jobs |
| 0x12091, 0x101d9 | 0x1c | 0 on the macOS system (context 1) job; 0x1c on application jobs [NATIVE]. Likely a resource/uniform limit [INFERRED] |
| 0x107a1 | 0xff0000 | Apple's zero-resource fallback |
| 0x1a089, 0x1a091, 0x1a059, 0x1a061, 0x1a0b9, 0x1a0c1, 0x101d1, 0x0d479, 0x1a0e9 | 0 | written explicitly |

Differences from G14X:

- **No `USC_EXEC_BASE_CP` (0x10071).** G15 USC fetches are a fixed 1 TiB base
  (0x100_0000_0000) plus a 32-bit offset. The queue's `usc_exec_base` is
  ignored [HW]. Setting 0x10071 alone did not change this [HW].
- **No helper registers (0x11841, 0x11849, 0x11f81).** G15 has no known
  helper/scratch program interface yet. The kernel rejects nonzero
  `helper.*` with ENODEV [HW]. The native equivalent is [UNKNOWN].
- **0x1a440** has the extra high bits (0x154 above 0x24201) [NATIVE].

### Preempt buffer and launch arguments

- The primary compute preempt buffer is 0x1480 bytes on T8122 (G13: 0x7f80;
  T600x: 0x3bd00) [NATIVE] [HW].
- preempt_buf2..5 are four 8-byte slots directly after it, at +0x1480..+0x1498.
  The block ends at +0x14a0 [HW].
- macOS places the shader's **launch-argument (buffer-binding) qword** at
  preempt base +0x14a0, in the same mapped page. The entry program loads its
  binding table from there [NATIVE]. The kernel allocates the preempt block as
  0x14a0 + 0x100 bytes and writes the qword (up to 0x40 bytes of bindings for
  kernel-built launches) at +0x14a0 [HW].
- The preempt page is mapped GPU read-write, shared, uncached, in the USC
  window, like macOS [NATIVE] [HW].
- For an ordinary (user-stream) job the kernel still builds this block. The
  user's CDM and entry program need not reference it: an ordinary job with its
  binding table in a user buffer runs [HW].

### Three ways a compute job is formed

| Mode | How it is selected | Stream and programs |
|---|---|---|
| Ordinary | `flags == 0`, `cdm_ctrl_stream_base != 0` (opt-in module parameter `g15_ordinary_compute=1`) | The firmware runs the user's CDM stream from a user VM_BIND buffer. 0x1a420 = base, `JobParameters2.cdm_ctrl_stream_end` = end. Results go to user buffers. This is the path Mesa uses [HW] |
| User launch (diagnostic) | `flags = DRM_ASAHI_COMPUTE_G15_USER_LAUNCH` (bit 31), base = user pointer to a launch description | The kernel copies CDM, entry, body, statics and params into kernel buffers in the USC window, applies relocations and returns results through a readback ioctl [HW] |
| Configured / stock-empty | `flags == 0`, base == 0 | The kernel runs a 4-byte terminate-only stream, or a launch described by a firmware-node property if present. Used for bring-up only [HW] |

The ordinary path checks the stream before it publishes anything [HW]:

- end > base;
- both 4-byte aligned;
- end − base ≤ 64 KiB;
- `[base, end + 4)` lies inside a VM_BIND READ mapping of this VM, outside
  kernel windows.

A bad stream pointer is then EINVAL instead of a GPU fault.
`cdm_ctrl_stream_end` is the address of the final terminate word, not one past
it [HW]. `sampler_*`, `helper.*` and `ts.*` must be zero (ENODEV) [HW].

The kernel also allocates and maps inert copies of the kernel launch objects
for ordinary jobs, to keep allocation order identical to the proven path.
Whether the firmware needs them is [UNKNOWN]. Removing them is untested.

### G15 CDM and address rules (summary)

- The G15 CDM grammar differs from G13/G14. The launch carries a full-VA
  entry-program pointer packed into one qword. It runs a small entry/state
  program, which LoadShaders the body by VA. The 48-byte native stream is
  launch word, entry qword, a state word, six grid/group words, a barrier-like
  word and a terminate [NATIVE] [HW]. Details are in [Shaders](50-shaders.md).
- Everything the USC fetches (CDM-referenced programs, entry, body, binding
  table, statics) must lie in [0x100_1000_0000, 0x101_0000_0000). That is the
  1 TiB fixed base plus 32 bits, above the kernel's own arenas [HW].
- A terminate-only stream in a user buffer retires and signals its fence. This
  is a cheap transport check [HW].
- Shaders do not start until the firmware has a valid power/DVFS state. With
  that state missing, the job sits in a context-store/retry loop before the
  shader starts and never retires [HW].

### Submission rules visible to userspace

- One command per submit. A compute and a render in one submit returns ENODEV
  [HW].
- A submit waits (interruptibly) for the device-wide job gate instead of
  failing with EBUSY. Several clients and pipelined submits queue correctly
  [HW].
- Mesa's shape works: header barriers 0, input syncs, and binary plus timeline
  out-syncs [HW]. A barrier against earlier work on the same queue never needs
  a firmware Barrier command, because the previous job has already retired
  [INFERRED].
- Stores to unmapped or read-only memory are dropped silently (soft fault).
  Check results byte for byte [HW].

## Part 2: faults and recovery

### How the firmware reports trouble

| Event-channel tag | G13/G14 | G15 |
|---|---|---|
| 0 (`Fault`) | `handle_fault` and recovery | Ignored. Apple's G15 handler for this tag does nothing, and Linux matches it [INFERRED] |
| 4 (`Timeout`: counter, unk, event slot) | Timeout handling | Delivered for **both** MMU faults and firmware-detected timeouts [HW] |

On a tag-4 event the host reads the FAULT_INFO registers. The registers are
unchanged from G14 and decode correctly on G15 [HW]. Valid fault info means
the error is `Fault` (fence EIO), otherwise `Timeout` (fence ETIMEDOUT) [HW].
Observed reported faults are fetch/read faults, for example an unmapped
USC-L1C read with level and address [HW].

### The q22 recovery window

G15 has no `FwStatus` object. The legacy halt/resume words, and the
pending-stamp table that lived in globals, now sit in the q22 shared region
[HW]:

| q22 offset | Field | Who writes | Status |
|---|---|---|---|
| +0x40a0 | pending stamps, 128 × {u32 info, u32 wait_value} | firmware sets, host clears | [HW] Encoding as V13.5+: valid = info & 1, flags = info & 0xf, slot = (info >> 4) & 0x7f |
| +0x44a0..+0x4567 | fault/restart information block | firmware | [HW] About 20 nonzero words after a fault; meaning [UNKNOWN] |
| +0x4580 | recovery (halt) count, u64 | firmware, +1 per halt | [HW] |
| +0x4590 | halted, u32 | firmware sets; host clears | [HW] |
| +0x45a0 | resume, u32 | host sets to 1; **the firmware clears it after resuming** | [HW] Reads 0 at idle after many recoveries |
| +0x45b0 | "stopped for host recovery" | firmware | [INFERRED] from Apple's host driver; stayed 0 in every Linux run, including faults and timeouts [HW] |
| +0x45c0 | completion counter, u32 | firmware | [HW] It increments with work. In one compute-heavy session it equalled successful completions exactly; in render-heavy sessions it was lower. What it counts is [UNKNOWN] |

### Recovery sequence

This is the legacy `recover()` moved onto q22. Apple's host driver does the
same: halted = 0, then resume = 1 [INFERRED].

1. Tag-4 event arrives. Read FAULT_INFO and pick `Fault` or `Timeout`.
2. **Mark pending events.** Walk the 128 q22 pending stamps. For each valid
   entry, mark that event slot's work up to `wait_value` with the error. Use
   `Killed` (ECANCELED) when the event named a different culprit slot. Then
   zero the entry [HW].
3. **Wait for halted.** Poll +0x4590 until nonzero (bounded wait) [HW].
4. **Clear and resume.** Store halted = 0, then resume = 1 [HW].
5. The firmware resumes within about 0.3 ms. The faulted job then completes
   through the normal Flag event → work-queue signal path, carrying the error,
   and the host frees its slots, leases and UMA reference as for any completed
   job [HW].

There is no firmware reset. Apple's host driver has none either: its "reset
firmware for recovery" hook is empty [INFERRED]. Halt/resume is the whole
native mechanism.

Linux gates this behind `g15_recovery=1`. With it off, the firmware stays
halted, the job never completes and the GPU needs a reboot [HW].

### Accounting that shows nothing leaks

A read-only debugfs file (`asahi/g15_accounting`) reports host counters next to
the firmware words. After a session with recoveries, a clean state is [HW]:

- `submissions == releases == completions`, `reservations_armed = 0`;
- `completion_errors == recoveries == q22_halt_count`;
- `q4_pending_submissions = 0`, `q4_command_gate = 1`;
- `event_slots_owned = 0`;
- `q22_halted = 0`, `q22_resume = 0`, `q22_pending_stamps = 0`;
- `crashed = 0`, job gate idle.

Typical verified sessions:

- 23 submissions with 5 recoveries;
- 142 submissions with 14 recoveries;
- 149 submissions with 3 forced timeouts;

all with these invariants [HW].

### What recovers and what does not

| Failure | Reported? | Outcome |
|---|---|---|
| MMU fault (bad VA in shader or stream) | Yes, tag 4 with fault info | Recovers. Repeats on one boot (fault, good, fault, compute, fault, fault, good…) with all later jobs exact [HW] |
| Firmware render timeout (fragment work hangs on heavy single-tile geometry) | Yes, tag 4 without fault info, about 80 ms into the job | Recovers. Later GL, render and compute jobs pass on the same boot [HW] |
| Compute infinite loop | **No.** No event, ever | Never recovers. The firmware keeps running the job [HW] |
| Some render hangs (a heavy job after earlier timeouts) | **No** | Never recovers. q22 stops draining for good [HW] |

The firmware init data carries progress-check intervals (3D = 40, TA = 10,
CL = 250) with the same values as earlier generations [HW]. Despite CL = 250,
a compute spin gets no timeout. The units and what arms the compute watchdog
are [UNKNOWN]. The 3D value may relate to the ~80 ms render timeout
[INFERRED].

### Teardown hazard

If a client exits while an unreported hang is still running, the queue
teardown sends release commands to a firmware that is still executing the job.
The firmware then crashes (RTKit data abort) [HW]. Without a host watchdog this
happened about 150 s after submit, just after the client gave up waiting [HW].
The DRM scheduler's own timeout (100 s by default) does not prevent it: the
G15 `timed_out` callback returns `ENODEV`, so the scheduler neither frees the
job nor re-arms its timer [HW]. Returning `NO_HANG` would only re-arm the
timer and would not stop the job, so it does not help [INFERRED].

### Host watchdog: fail-stop

Linux adds the module parameter `g15_job_timeout_ms`. 0 (the default) means
off. When set, it becomes the G15 queue's DRM scheduler timeout. On expiry
`timed_out` fail-stops the device exactly like a firmware crash [HW]:

- marks the GPU crashed and fails every pending job with `Timeout`, so the
  hung fence reports ETIMEDOUT;
- makes later GET_PARAMS and submits return ENODEV within about 1 ms. New GL
  clients fail at once instead of hanging;
- makes teardown skip all firmware commands and keep firmware-visible memory
  mapped ("retaining firmware-visible resources");
- leaves the firmware alive: no crash in the following 180 s.

With 3000 ms the watchdog never fires on normal work or on a firmware-reported
timeout, which recovers first [HW]. An unreported hang now costs a clean
fail-stop and a reboot, instead of a hung client and a firmware crash.

## Open questions

- How to stop a running G15 job the firmware does not report, so that
  unreported hangs can recover without a reboot. This needs a native capture
  of a long-running macOS compute shader and of what Apple does on its
  watchdog.
- Why the firmware never times out a compute spin, given the CL
  progress-check interval, and what the interval units are.
- Why some heavy render jobs hang without a timeout event after earlier
  recovered timeouts.
- What q22 +0x45c0 counts, what +0x45b0 signals, and the layout of the
  +0x44a0 fault/restart block.
- Whether the firmware sets the +0x878 recovery marker on the faulted command,
  and whether the host must act on it.
- Native sampler-heap, helper/scratch and timestamp registers for G15 compute.
- Whether the inert kernel launch objects allocated for ordinary jobs can be
  dropped.
- Culprit attribution with two clients (`Killed` vs `Fault`) is implemented
  but untested.
- The G15 firmware Barrier command, needed for compute and render in one
  submit, is untested.
