# G15 desktop session helpers

Example scripts used to run an Omarchy/Hyprland desktop on the G15 GPU on a
MacBook Air M3 (J613). They assume the G15 kernel from [../../kernel](../../kernel/README.md),
its loader installed as `/usr/local/sbin/g15-load` (with a sudoers rule letting
the session run it without a password), and a G15 Mesa build installed under
`$G15_MESA` (default `~/g15-mesa`), as in [../../AGENTS.md](../../AGENTS.md).

| File | Install as | Purpose |
|---|---|---|
| `uwsm-env-hyprland` | `~/.config/uwsm/env-hyprland` | On the G15 kernel, loads the driver and points Hyprland and its clients at the G15 Mesa. Falls back to the CPU renderer if the driver is not loaded |
| `sw` | `/usr/local/bin/sw` | Run one app on the CPU renderer (llvmpipe) for comparison: `sw glmark2-es2-wayland` |
| `ghostty` | `/usr/local/bin/ghostty` | Ghostty 1.3 needs desktop GL 4.3 core, which the G15 driver does not offer yet; runs it on the CPU renderer |
| `ghostty-systemd-dropin.conf` | `~/.config/systemd/user/app-com.mitchellh.ghostty.service.d/m3gpu-g15-wrapper.conf` | Ghostty is started by its systemd user unit (`ExecStart=/usr/bin/ghostty`), which bypasses the wrapper; this drop-in points it at `/usr/local/bin/ghostty`. Then `systemctl --user daemon-reload` |
| `quickshell` | `/usr/local/bin/quickshell` | Omarchy's shell uses render-target formats the G15 Mesa does not support yet; runs it on the CPU renderer |
| `g15-demo-apps` | `/usr/local/bin/g15-demo-apps` | Opens the demo clients (`weston-simple-egl`, glmark2 texture scene); `--cpu` runs them on the CPU renderer |
| `g15top` | `/usr/local/bin/g15top` | Live GPU panel for a terminal beside btop, which cannot see the G15 (the driver publishes no per-client GPU time yet): GPU jobs/s with a history graph, partial renders/s, errors, recoveries, system and SoC power. Reads the driver's debugfs accounting through `sudo -n` |

Keep the login screen's compositor on the CPU renderer (for SDDM:
`CompositorCommand` with `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast`), so it stays
usable whether or not the G15 driver is loaded.

A GPU hang the firmware does not report freezes the session (reboot).
