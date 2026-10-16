# Memory map and address spaces

This section describes the GPU virtual address spaces on G15 (M3, T8122,
firmware V14_7): the UAT page-table format, the firmware and kernel ranges,
Apple's GART range classes, the PTE protection classes, and the windows
userspace must place objects in. It assumes the G13/G14 Asahi UAT model as
background (16 KiB pages, TTBAT slots, the handoff region, `PROT_*` names in
`pgtable.rs`). Firmware channels and the q22 shared object are covered in
[Firmware interface and submission](20-firmware-and-submission.md).

## Summary of differences from G13/G14

| Topic | G13/G14 | G15 |
|---|---|---|
| Translated input bits per root | 39 | 42 (`uat_ias = 42`, `uat_oas = 42`) [HW] |
| Top-level index | VA[38:36] (3 bits) | VA[41:36] (6 bits) [HW] |
| Root selection | TTB0 (low half) / TTB1 (high half) | Two "banks", selected by VA bit 42. Bank 0 is per-context; bank 1 is one physical root shared by the kernel context and every client [HW] |
| User VM_BIND range | [0x4000, 2^39 - 32 KiB) | Same, **plus** the USC window inside the 1 TiB range-5 aperture [HW] |
| Shader addressing | `usc_exec_base` + 32-bit offset | Fixed 1 TiB base + 32-bit offset; `usc_exec_base` ignored [HW] |
| Render VDM/PPP pointers | Full VAs | Offsets from a fixed render-context base 0x10_0000_0000 [HW] |
| Faults | Hard faults by default (soft optional via `fault_control`) | Soft faults always on; stores to unmapped/RO pages are dropped [HW] |
| Shared-bank map/unmap | Plain PTE writes + TLB flush | Bank-1 range-7/8 changes are also published to the firmware through the q22 mapping ring, and unmaps wait for it to drain [HW] |
| Handoff "no current slot" | 0 | 0xffff_ffff (both accepted) [HW] |
| Context ID | 6-bit slot | Slot plus an 8-bit generation [INFERRED] |

## UAT page tables

### Geometry

| Item | Value | Tag |
|---|---|---|
| Page size | 16 KiB (`UAT_PGBIT = 14`) | [HW] |
| Levels | 3: top (VA[41:36]), middle (VA[35:25], 11 bits), leaf (VA[24:14], 11 bits) | [HW] |
| Middle entry span | 32 MiB | [HW] |
| Bank select | VA bit 42 (0 = bank 0, 1 = bank 1) | [HW] |
| Bank-local address | VA & (2^42 - 1); canonical high VAs (0xffff_fc.., 0xffff_ff..) keep only their low 42 bits inside bank 1 | [HW] |
| Output address | 42 bits; a root above 2^42 is rejected | [INFERRED] |
| Contexts | 64 TTBAT slots, slot 0 = kernel/firmware, 1..63 = clients | [HW] |

Bank 1's root is the bootloader-provided kernel page-table root (the
`pagetables` UAT region, which lies outside Linux RAM; tables there are
mapped with `memremap(WB)`, one 16 KiB page at a time) [HW]. Its top-level
entry 2 points at a firmware-owned middle table located in the page
immediately before the bank-1 root [HW]. Linux owns only the eight leaf
tables it installs under that middle table: six for range 7 (192 MiB) and two
for range 8 (64 MiB). It verifies those middle entries are empty before
installing them and detaches them on teardown [HW].

### TTBAT (GPTBAT) entry format

Each slot still holds two qwords, now one per bank [HW]:

| Qword | Bits | Meaning |
|---|---|---|
| bank 0 | [0] | Valid |
| | [41:14] | Bank-0 root PA (per-context) |
| | [55:48] | Context ID (slot index) |
| bank 1 | [0] | Valid |
| | [41:14] | Shared bank-1 root PA |
| | [63:42] | Bank-0 root PA bits [63:42] (zero in practice) [INFERRED] |
| | [55:48] | Context ID |

The firmware pairs each context ID with an 8-bit generation that Apple only
advances when the ID moves to a different address space; reusing a slot for
the same VM keeps it. The generation is carried in command fields (for
example `RunCompute` +0x85f) [INFERRED].

### Leaf PTE bits

Same field positions as G13/G14 [HW]:

| Bits | Field |
|---|---|
| [1:0] | 3 = valid leaf/table |
| [4:2] | memattr: 0 cached, 1 device, 2 uncached |
| [7:6] | AP: 0 FW+GPU, 1 FW, 2 GPU-only |
| [10] | AF |
| [11] | non-global (set on native user leaves) [NATIVE] |
| [41:14] | Output PA |
| [53] | PXN |
| [54] | UXN |
| [55] | "GPU access" (set on every class below) |

For AP=2 (GPU-only), the (UXN, PXN) pair means (0,0) read-only and
executable, (0,1) write-only, (1,0) read-write, not executable, (1,1) no
access [HW for RO/RW; INFERRED for the others].

Production macOS on G15 maps through the protected UAT-PPL encoder, not
the AGX driver's direct encoder. The two give different PTEs for the same
mapping request: a class the direct encoder writes as AP=0 comes out as
AP=2 from the PPL path [INFERRED, cross-checked HW]. Older Linux comments
that call compact class 0x308 "AP=0 uncached" describe the direct encoder;
the live kernel uses the PPL result (`PROT_GPU_SHARED_RW`).

## PTE protection classes

Masked attribute bits are `PTE & (AP | memattr | [63:52])`; AF and the type
bits are added on top.

| Linux name | AP | memattr | PXN/UXN | Attr bits | Used for on G15 |
|---|---|---|---|---|---|
| `PROT_FW_PRIV_RW` | 1 | cached | 0/1 | 0x00c0..0040 | Kernel-private firmware structures [HW] |
| `PROT_FW_SHARED_RW` | 1 | uncached | 0/1 | 0x00c0..0048 | Kernel/firmware shared structures [HW] |
| `PROT_FW_SHARED_RO` | 1 | uncached | 0/0 | 0x0080..0048 | Read-only firmware data [HW] |
| `PROT_GPU_FW_SHARED_RW` | 0 | uncached | 1/1 | 0x00e0..0008 | FW+GPU shared; range-7 HW metrics and PM page metrics (native leaf ..040b) [HW] |
| `PROT_GPU_FW_PRIV_RW` | 0 | cached | 1/1 | 0x00e0..0000 | TVB block list at its native VA (native leaf 0xe0..0400) [HW] |
| `PROT_GPU_RO_FW_PRIV_RW` | 1 | cached | 1/1 | 0x00e0..0040 | Kernel "GPU RO" heap [HW] |
| `PROT_GPU_SHARED_RO` | 2 | uncached | 0/0 | 0x0080..0088 | VM_BIND READ; code/CDM/statics (Apple compact 0x108) [HW] [NATIVE] |
| `PROT_GPU_SHARED_RW` | 2 | uncached | 0/1 | 0x00c0..0088 | VM_BIND READ\|WRITE; launch entries, binding tables, outputs, render target, UMA pool (Apple compact 0x308) [HW] [NATIVE] |
| `PROT_GPU_SHARED_WO` | 2 | uncached | 1/0 | 0x00a0..0088 | VM_BIND WRITE-only [INFERRED] |
| `PROT_G15_RANGE5_CODE` | 2 | cached | 0/1 | 0x00c0..0080 | Range-5 "cached" arena [HW] |
| `PROT_G15_RANGE5_CACHED` / `_FLIST_LIST` | 0 | cached | 0/0 | 0x0080..0000 | UMA FList page/backup lists (Apple compact 0x300) [INFERRED] |
| `PROT_G15_RANGE5_UNCACHED` | 0 | uncached | 0/0 | 0x0080..0008 | Defined, not used by live arenas (superseded by the PPL result) [INFERRED] |
| `PROT_G15_RANGE7_FW` | 1 | device | 0/1 | 0x00c0..0044 | q22 state/ring, events, stamps, PM resources (native leaf ..0447) [HW] |
| `PROT_G15_RANGE7_QUEUE_STATE` | 1 | uncached | 0/1 | 0x00c0..0048 | Timestamp-queue state, channel memory, scene stats, timing record (native ..044b) [HW] |
| `PROT_G15_RANGE7_FLIST_FW` | 1 | uncached | 0/1 | 0x00c0..0048 | UMA FList firmware-uncached state [HW] |
| `PROT_G15_RANGE8_FW` | 1 | cached | 0/1 | 0x00c0..0040 | Page-pool state, channel state, scheduler state, SKU slots (native ..0443) [HW] |

Two classes encode identically (`RANGE7_QUEUE_STATE`/`RANGE7_FLIST_FW`,
`FW_PRIV_RW`/`RANGE8_FW`); they are kept apart because Apple allocates them
from different pools. A resource the GPU's PM unit reads must have the GPU
access class: mapping PM page metrics firmware-only gave a `NoAccess` fault
[HW].

## Apple GART range classes

Apple's host driver classifies every GPU VA into an "eGartRange" that selects
the mapping path [INFERRED, static analysis]. Linux reproduces the classifier
and uses ranges 5, 7 and 8 directly.

| Range | VA interval | Bank | Known use |
|---|---|---|---|
| 0 | everything not listed | | Linux kernel firmware heaps (0xffff_ffa0..) fall here [HW] |
| 1 | [0x10_0000_0000, 0x6c_0000_0000) | 0 | Per-client render-context aperture (VDM, PPP, TVB, TPC, PM scene) [NATIVE] |
| 2 | [0x6e_0000_0000, 0x6f_0000_0000) | 0 | [UNKNOWN] |
| 3 | [0x6f_0000_0000, 0x6f_ffc0_0000) | 0 | [UNKNOWN] |
| 4 | [0x6f_ffc0_0000, 0x70_0000_0000) | 0 | [UNKNOWN] |
| 5 | [0x100_0000_0000, 0x300_0000_0000) | 0 | Per-client USC aperture, GTP/TPC, parameter-scene allocations, FList lists [HW] |
| 6 | [0xffff_fc1f_ffe0_0000, 0xffff_fc20_0000_0000) | 1 | [UNKNOWN] |
| 7 | [0xffff_fc20_0000_0000, 0xffff_fc20_0c00_0000) (192 MiB) | 1 | Firmware-shared resources (classes above) [HW] |
| 8 | [0xffff_fc20_0c00_0000, 0xffff_fc20_1000_0000) (64 MiB) | 1 | "Special" aperture: page-pool, channel and scheduler state [HW] |
| 9-12 | [0xffff_fc20_1000_0000, 0xffff_fc20_1980_0000) | 1 | [UNKNOWN] (20, 4, 64, 64 MiB) |

Ranges 1-5 lie in each client's bank-0 tables. Ranges 7/8 are in the shared
bank 1, so their mappings are visible to every context.

### Kernel firmware VA ranges

Unchanged from G13/G14: firmware structures live in the kernel context at
[0xffff_ffa0_0000_0000, 0xffff_ffb0_0000_0000) (private, GPU-RO,
shared, shared-RO, GPU-shared, RTKit, timestamp and MMIO sub-ranges). On G15
these sit in bank 1, top-level index 0x3a, under the same root as ranges 7/8
[HW]. G15 GPU VA and firmware VA are identical [INFERRED].

### Range-5 layout (per client VM)

Apple uses one range-5 heap with per-mapping PTE classes. Linux keeps one
class per allocator, so it splits the low part into arenas. The split is a
Linux detail, but two constraints are hardware facts: every
execution-facing object must stay below 0x101_0000_0000 (one top-level slot
and one 32-bit USC offset space), and native launches keep their low 32
address bits small (below 0x23_0000) [HW].

| VA range | Linux arena | Class |
|---|---|---|
| 0x100_0000_0000 - 0x100_0018_0000 | Code | `GPU_SHARED_RO` |
| 0x100_0018_0000 - 0x100_001c_0000 | Prime code (warm-up, fixed placements) | `GPU_SHARED_RO` |
| 0x100_001c_0000 - 0x100_003c_0000 | PPL308 (writable launch data) | `GPU_SHARED_RW` |
| 0x100_003c_0000 - 0x100_033c_0000 | Prime PPL308 (fixed placements) | `GPU_SHARED_RW` |
| 0x100_033c_0000 - 0x100_033d_4000 | TVB block list, native VA, 0x14000 bytes (experimental carve-out) | `GPU_FW_PRIV_RW` |
| 0x100_0340_0000 - 0x100_0380_0000 | "Uncached" | `GPU_SHARED_RW` |
| 0x100_0380_0000 - 0x100_0400_0000 | "Cached" | `G15_RANGE5_CODE` |
| 0x100_0400_0000 - 0x100_0c00_0000 | Render UMA pool backing (25 x 4 MiB extents + guards), created on first render | `GPU_SHARED_RW` |
| 0x100_0c00_0000 - 0x100_1000_0000 | Unused gap | |
| 0x100_1000_0000 - 0x101_0000_0000 | **USC window (userspace, VM_BIND)** | from bind flags |
| 0x101_0000_0000 - 0x300_0000_0000 | Unused | |

## User-visible windows

### Ordinary VM_BIND range

`vm_start` = 0x4000, `vm_end` = 2^39 - 32 KiB (0x7f_ffff_8000),
`vm_kernel_min_size` = 512 MiB, all as on G13/G14 [HW]. Binds are 16 KiB
aligned. READ maps `PROT_GPU_SHARED_RO`, READ|WRITE maps
`PROT_GPU_SHARED_RW`, WRITE maps `PROT_GPU_SHARED_WO` [HW]. A bind may not
overlap the VM's kernel range or its kernel render windows (below).

### USC window: [0x100_1000_0000, 0x101_0000_0000)

- Every USC fetch (shader code, entry programs, binding tables, statics,
  helper programs) resolves as **0x100_0000_0000 + (32-bit offset)** [HW].
  Natively the scissor and depth-bias tables are in this aperture too [NATIVE]. The queue's `usc_exec_base` is
  ignored: a binding table bound at 0x101_0001_0100 faulted at
  0x100_0001_0100 with either base [HW].
- VM_BIND accepts this window on G15 VMs only, outside [`vm_start`,
  `vm_end`) [HW].
- The lower 256 MiB of the 4 GiB aperture is kept for the kernel's range-5
  arenas, hence the 0x100_1000_0000 start [HW].
- Code may be bound RO or RW; both execute [HW]. Natively, code, CDM and
  statics are RO and launch entries, bindings and outputs RW [NATIVE].
- A Mesa-style client sets `shader_base = 1 << 40` and puts its USC heap at
  [`usc_window_start`, `usc_window_end`) [HW].

### Render-context window: [0x10_0400_0000, 0x11_0000_0000)

- The render-context aperture is [0x10_0000_0000, 0x11_0000_0000), in Apple
  range 1 and inside the ordinary VM_BIND range [HW].
- TA register 0x1c880 (VDM control stream base) and the VDM stream's PPP
  State pointers hold **offsets from 0x10_0000_0000**, not VAs. Offsets are
  32-bit, so the aperture is 4 GiB [HW].
- The kernel owns the low 64 MiB, [0x10_0000_0000, 0x10_0400_0000), for its
  per-VM render windows. Natively these hold fixed sections, the TVB, the
  TVB page list (0x10_0022_8000), the TPC (0x10_0028_0000), the context-switch
  buffer and the PM scene (offset 0x22_0000); Linux creates them per VM at
  the native VAs, currently spanning 0x10_0000_0000-0x10_0028_8000 [HW] [NATIVE].
- User VDM streams and PPP state go in [0x10_0400_0000, 0x11_0000_0000). A
  VDM base below 0x10_0400_0000 or not 4-byte aligned is EINVAL; its first
  16 KiB must be in a READ mapping [HW]. RO is enough for both VDM and PPP
  [HW].
- The render target and other attachments may be anywhere in a READ|WRITE
  VM_BIND mapping (ordinary range or USC window) [HW].

### GET_PARAMS fields

Appended to `drm_asahi_params_global`; older userspace reads a prefix [HW].

| Field | G15 value | Elsewhere |
|---|---|---|
| `usc_window_start` / `usc_window_end` | 0x100_1000_0000 / 0x101_0000_0000 | 0 |
| `render_context_start` / `render_context_end` | 0x10_0400_0000 / 0x11_0000_0000 | 0 |
| `features` | always includes `DRM_ASAHI_FEATURE_SOFT_FAULTS` | only with `fault_control=0xb` |

The UAPI header also defines `DRM_ASAHI_G15_RENDER_CONTEXT_START`
(0x10_0000_0000), the base that offsets are taken from.

## Faults

- **Soft faults are always enabled.** The firmware init field
  `soft_fault_settings` (G15 q4 config +0xdf9) is 3, as traced on macOS; the
  `fault_control` parameter does not reach this path [HW] [NATIVE].
- A **store** to a read-only page, to an unmapped page, or to a page unbound
  while the job was in flight is dropped. The job completes normally (fence
  signalled, no error) [HW]. Verify results byte for byte; a missing write is
  the only symptom.
- A **load** from an unmapped address is still reported as an MMU fault (for
  example `UL1C read, Unmapped, level 2`) and is recoverable without a reboot
  [HW]. A load from a page without GPU access reports `NoAccess` [HW].
- Fault addresses in bank 1 are reported in a 43-bit form: bank bit 42 plus
  the bank-local address (for example 0x420_000b_0000 for range-7 VA
  0xffff_fc20_000b_0000) [HW]. The TA register for the PM page-metrics pointer
  (0x1c910) holds that VA relative to 0xffff_fc00_0000_0000, masked with
  0x7f_ffff_fffe [NATIVE] [HW].

## q22 mapping notification (bank 1)

Changes to range-7/8 mappings must be published to the firmware through the
q22 mapping ring. Bank-0 (per-client) maps are plain PTE writes plus TLB
invalidation, as on G13/G14 [HW].

| Structure | Layout |
|---|---|
| State (0x20 bytes) | `read_idx` u32 at +0x00 (firmware), `write_idx` u32 at +0x10 (host), indices modulo 256 |
| Backing | State at +0x00, ring at +0x40, 256 entries, 0x1840 bytes total, mapped in range 7 |
| Entry (0x18 bytes) | +0x00 u64 GPU VA; +0x08 u32 PA >> 12; +0x0c u32 context (0 map, 0xffff_ffff unmap); +0x10 u16 page count in firmware pages; +0x12 u16 flags; +0x14 u32 zero |
| Flags | bit0 map, bit1 special 64 MiB aperture (range 8), bit2 property (unused). Range 7: map 1, unmap 0. Range 8: map 3, unmap 2 |

Rules [HW unless noted]:

1. Mappings made before the firmware reports ready are not published, and
   are not replayed later. Publication is armed once, after firmware-ready
   and before the render node is registered.
2. Map: write the PTEs first, then publish the map record.
3. When more than 0xc0 entries are pending, send the firmware-control kick
   (endpoint FWCTL message); the firmware then advances `read_idx`.
4. **Unmap: publish the unmap record, send the kick, then busy-poll until
   `read_idx == write_idx`, and only then clear the PTEs and free the VA.**
   Apple's timeout is 150 ms and it does not sleep while polling [INFERRED].
   If the drain times out, keep the PTEs and VA reserved (leak) rather than
   risk the firmware using a freed page.
5. Linux also drains the ring once before publishing each submit, so every
   mapping the job uses has been consumed.

## Other memory facts

- The handoff region keeps the G13/G14 layout and PPL magic; G15 writes
  0xffff_ffff as "no current slot" [HW].
- Render UMA pool: a ring of page VAs in range 5 shared by TA and 3D; each job
  advances both cursors [NATIVE] [HW].
- A VM's render windows and UMA heap keep freed allocations until collected;
  collect them before each render, or a VM's second render gets ENOSPC [HW].
- GEM objects are shmem, CPU-mapped write-combined unless `WRITEBACK`. A
  `WRITEBACK` BO behind an uncached GPU PTE is untested [UNKNOWN].
- Some firmware pointer fields mask VAs to 40 bits (context-switch buffer:
  mask 0xff_ffff_ffe0) [INFERRED].

## Open questions

- What Apple ranges 2, 3, 4, 6 and 9-12 hold, and whether any later feature
  (TVB growth, partial renders) needs them.
- The TVB heap/growth address path: partial renders do not run on Linux yet,
  and the block list at its native VA and class did not change that.
- Whether the `PROT_GPU_SHARED_WO` and (1,1) "no access" AP=2 encodings
  behave as the table says on G15.
- Whether `usc_window` can grow past 0x101_0000_0000 (it cannot while USC
  offsets are 32-bit), and whether the kernel arenas can shrink so the window
  starts at 0x100_0000_0000 as on macOS.
- Whether the 8-bit context generation matters for correctness when a slot is
  reassigned to a different VM.
- Whether user render-context objects need RW binds in any case (RO has been
  enough so far).
- Whether bank-0 root bits [63:42] in the bank-1 TTBAT qword are ever nonzero.
