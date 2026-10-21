# G15 desktop session helpers

Scripts that run an Omarchy/Hyprland desktop on the G15 GPU on a MacBook Air
M3 (J613), from the first boot of the G15 kernel. They assume the G15 kernel
from [../../kernel](../../kernel/README.md), its loader installed as
`/usr/local/sbin/g15-load`, and a G15 Mesa build installed under `$G15_MESA`
(default `~/g15-mesa`), as in [../../AGENTS.md](../../AGENTS.md).

Install everything below with one command, as the desktop user or with sudo
from it (it uses sudo for the system files and keeps a `.bak-<date>` copy of
any file it replaces; `../../install.sh install` runs it for you):

```
tools/desktop/install.sh
```

| File | Install as | Purpose |
|---|---|---|
| `../../kernel/g15-load.service` | `/etc/systemd/system/g15-load.service`, enabled | Loads the driver at boot, before the login screen, on the G15 kernel only |
| `uwsm-env-hyprland` | `~/.config/uwsm/env-hyprland` | On the G15 kernel, points Hyprland and its clients at the G15 Mesa, uses GTK4's cairo renderer, and lists every display controller for Hyprland (internal panel and external port). Falls back to the CPU renderer if the driver is not loaded or the Mesa is not built. Without `g15-load.service` it tries `sudo -n g15-load`, which needs a sudoers rule |
| `sw` | `/usr/local/bin/sw` | Run one app on the CPU renderer (llvmpipe) for comparison: `sw glmark2-es2-wayland` |
| `ghostty` | `/usr/local/bin/ghostty` | Ghostty 1.3 needs desktop GL 4.3 core, which the G15 driver does not offer yet; runs it on the CPU renderer |
| `ghostty-systemd-dropin.conf` | `~/.config/systemd/user/app-com.mitchellh.ghostty.service.d/m3gpu-g15-wrapper.conf` | Ghostty is started by its systemd user unit (`ExecStart=/usr/bin/ghostty`), which bypasses the wrapper; this drop-in points it at `/usr/local/bin/ghostty` |
| `quickshell` | `/usr/local/bin/quickshell` | Omarchy's shell uses render-target formats the G15 Mesa does not support yet; runs it on the CPU renderer |
| `g15-demo-apps` | `/usr/local/bin/g15-demo-apps` | Opens the demo clients (`weston-simple-egl`, glmark2 texture scene); `--cpu` runs them on the CPU renderer |
| `g15top` | `/usr/local/bin/g15top` | Live GPU panel for a terminal beside btop, which cannot see the G15 (the driver publishes no per-client GPU time yet): GPU jobs/s with a history graph, partial renders/s, errors, recoveries, system and SoC power. Reads the driver's debugfs accounting through `sudo -n`, so run it as `sudo g15top` unless you have a sudoers rule for it |

The login screen's compositor must stay on the CPU renderer (for SDDM:
`CompositorCommand` with `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast`), so it stays
usable whether or not the G15 driver is loaded. Omarchy's SDDM configuration
(`/etc/sddm.conf.d/10-wayland.conf`) already does this; `install.sh` warns if
it does not find it.

A GPU hang the firmware does not report freezes the session (reboot).
