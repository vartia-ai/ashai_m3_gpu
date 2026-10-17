# G15 kernel

The Linux kernel with the G15 GPU driver: a port of the Asahi AGX Rust driver to
the M3 GPU (G15G), plus the display (DCP) support it runs with.

| File | What it is |
|---|---|
| `base-patches/` | 8 patches on top of AsahiLinux/linux `asahi-wip-7.2` @ `236788cd2602`: DCP display device-tree nodes for t8122/t603x (AsahiLinux PR 608) and the DCP 14.7 tiling fix (PR 607). Authorship as in the PRs |
| `g15.patch` | Everything else, one diff: the G15 GPU driver (`drivers/gpu/drm/asahi`), its UAPI, the Rust kernel-crate support it needs, the DCP soft power-off for 14.7 firmware, and two small fixes (`of_reserved_mem.c`, `soc/apple`) |
| `config` | The tested kernel config (release suffix `-display-driver-g15`) |
| `build.sh` | Fetch, patch, check the source tree hashes and build `Image` + modules |
| `g15-load` | Load the GPU driver after boot, with the tested options |
| `modprobe-g15.conf` | Stops udev from autoloading the driver without those options |

## When it works

The driver starts only on hardware and firmware that match its built-in
defaults, and refuses (`ENODEV`, nothing touched) otherwise. `g15_dt_overlay`
checks, before changing the device tree:

1. the machine is a MacBook Air 13" M3, `apple,j613`;
2. `/proc/device-tree/chosen/asahi,os-fw-version` is `14.7`;
3. `/proc/device-tree/chosen/asahi,iboot2-version` is `iBoot-10151.140.19.700.2`
   (the iBoot that loads the macOS 14.7 GPU firmware);
4. the GPU power domain and interrupt controller sit where the J613 device tree
   puts them, and the GPU's page-table regions are outside Linux RAM.

Values 2 and 3 come from the macOS system firmware of your Linux install (the
"stub" macOS the Asahi installer sets up), not from the macOS you use day to
day. Check them before building:

```
tr -d '\0' < /proc/device-tree/chosen/asahi,os-fw-version; echo
tr -d '\0' < /proc/device-tree/chosen/asahi,iboot2-version; echo
```

## Where the built-in values come from

Several tables in the driver hold values observed on one J613 while macOS
drove the GPU with the 14.7 firmware: the built-in render profile
(`g15_builtin_profile.rs`), the static and power words of the firmware's
hardware data (`j613_hwdata_a_static.rs`, `j613_hwdata_b_power.rs`), part of
the firmware init data (`initdata.rs`), and the device-tree overlays generated
from that machine's device tree and ADT (`g15_j613_overlay.h`,
`g15_j613_pmp_overlay.h`). Each file says so at the top. No Apple code or
firmware is included.

These values are correct for a J613 with the 14.7 firmware. A different
machine (MacBook Air 15", Pro, iMac, other M3 variants) or a different GPU
firmware version needs its own values, taken from that machine. That means
tracing macOS under the m1n1 hypervisor, which is a research task, not an
install step. The kernel refuses to start rather than run with wrong values.

## Build

On the laptop (needs the usual kernel build tools plus rustc,
bindgen and clang; tested with gcc 16.1.1, rustc 1.98.1, bindgen 0.73.2):

```
kernel/build.sh ~/g15-kernel
```

The script stops if either source tree hash differs from the tested one.
Installing and booting it is described in [../AGENTS.md](../AGENTS.md).
