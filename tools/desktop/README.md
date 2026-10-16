# G15 desktop session helpers

Example scripts used to run an Omarchy/Hyprland desktop on the G15 GPU on a
MacBook Air M3 (J613). They assume the G15 kernel driver is loaded by a
site-specific `/usr/local/sbin/g15-load` (the kernel driver is not part of
this release yet; see the top-level README) and a G15 Mesa build installed
under `$G15_MESA` (default `~/g15-mesa`).

| File | Install as | Purpose |
|---|---|---|
| `uwsm-env-hyprland` | `~/.config/uwsm/env-hyprland` | On the G15 kernel, loads the driver and points Hyprland and its clients at the G15 Mesa. Falls back to the CPU renderer if the driver is not loaded |
| `sw` | `/usr/local/bin/sw` | Run one app on the CPU renderer (llvmpipe) for comparison: `sw glmark2-es2-wayland` |
| `ghostty` | `/usr/local/bin/ghostty` | Ghostty needs desktop GL 3.3, which the G15 driver does not offer yet; runs it on the CPU renderer |
| `quickshell` | `/usr/local/bin/quickshell` | Omarchy's shell uses render-target formats the G15 Mesa does not support yet; runs it on the CPU renderer |
| `g15-demo-apps` | `/usr/local/bin/g15-demo-apps` | Opens the demo clients (`weston-simple-egl`, glmark2 texture scene); `--cpu` runs them on the CPU renderer |

Keep the login screen's compositor on the CPU renderer (for SDDM:
`CompositorCommand` with `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast`), so it stays
usable whether or not the G15 driver is loaded.

Known: logging out of the G15 session while the driver is loaded can leave the
display firmware (DCP) unstable; reboot to leave the session. A GPU hang the
firmware does not report freezes the session (reboot).
