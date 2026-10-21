# Installing the G15 GPU driver: instructions for a coding agent

You are helping a person turn on GPU acceleration on their MacBook Air M3
under Linux, using this repository. Read this whole file before doing
anything. Work step by step, show the person each command before you run
anything that needs root, and stop at the first failed check.

## 1. Check that this machine can use it

This release works on exactly one configuration. All of these must hold;
if any does not, stop and tell the person why (section 8).

| Check | Command | Must be |
|---|---|---|
| Machine | `tr -d '\0' < /proc/device-tree/compatible` | contains `apple,j613` (MacBook Air 13", M3) |
| Linux already runs | | The person boots Linux on this Mac today (Asahi-style install: m1n1, U-Boot, GRUB), with working storage, network and display |
| Stub firmware | `tr -d '\0' < /proc/device-tree/chosen/asahi,os-fw-version` | `14.7` |
| GPU firmware loader | `tr -d '\0' < /proc/device-tree/chosen/asahi,iboot2-version` | `iBoot-10151.140.19.700.2` |
| Architecture | `uname -m` | `aarch64` |
| Free space | `df -h ~ /boot` | ~12 GB for the kernel build, a few GB for Mesa, ~100 MB in `/boot` |

The firmware values describe the macOS system firmware of the Linux install
(the stub macOS the Asahi installer created), not the macOS the person uses.
They cannot be changed from Linux.

The driver itself refuses to start when the machine or firmware differ, so
a mismatch cannot damage anything; it just will not work.

## 2. Ground rules

- **Do not change the existing boot setup.** Add one new GRUB entry next to
  the person's working one, and do not make the new entry the default. Do not
  touch m1n1, U-Boot, the EFI partition layout, macOS, the stub firmware, or
  disk encryption settings. If anything in the steps below would need that,
  stop and ask. The one exception is the optional external display
  ([display/README.md](display/README.md)): it needs an m1n1 change, made
  only when the person explicitly asks for that feature, following that
  guide, with their current boot image kept beside the new one.
- **Never print secrets.** Kernel command lines can hold disk-unlock material
  (for example `cryptkey=`). Redact such values whenever you show GRUB
  entries, `/proc/cmdline` or logs.
- **Only use the values in this repository.** Do not edit the driver's tables
  or the overlay to "fit" another machine or firmware. Section 8 explains
  what other configurations need.
- **One driver load per boot.** The GPU firmware cannot be reset, so the
  driver cannot be unloaded or reloaded. To retry, reboot.
- **Expect hangs.** This is early work (see the README). A GPU hang the
  firmware does not report freezes graphics until a reboot. Make sure the
  person has saved their work before testing.

## 3. Build the kernel

```
git clone https://github.com/vartia-ai/m3_gpu.git ~/g15
~/g15/kernel/build.sh ~/g15-kernel
```

The script fetches AsahiLinux/linux at the tested commit, applies the
patches, and stops if the source differs from what was tested. Build
dependencies (Arch Linux ARM names): `base-devel bc cpio pahole rust
rust-bindgen rust-src clang llvm`. The config expects recent toolchains
(tested: gcc 16.1.1, rustc 1.98.1, bindgen 0.73.2); if `olddefconfig` reports
a config change, tell the person, but the build can continue.

## 4. Install the kernel next to the existing one

Release name: `7.2.2-display-driver-g15` (print it with
`make -s -C ~/g15-kernel/linux O=~/g15-kernel/build LOCALVERSION= kernelrelease`).

```
sudo make -C ~/g15-kernel/linux O=~/g15-kernel/build LOCALVERSION= modules_install
sudo mkdir /boot/g15
sudo cp ~/g15-kernel/build/arch/arm64/boot/Image /boot/g15/Image
sudo mkinitcpio -k 7.2.2-display-driver-g15 -g /boot/g15/initramfs.img
sudo install -m 644 ~/g15/kernel/modprobe-g15.conf /etc/modprobe.d/g15.conf
sudo install -m 755 ~/g15/kernel/g15-load /usr/local/sbin/g15-load
```

Use the initramfs tool the system already uses (`mkinitcpio` on Arch Linux
ARM; `dracut` elsewhere). If `/lib/modules/7.2.2-display-driver-g15` already
exists, stop and ask: something else installed that release.

Then add a GRUB entry: copy the person's current Linux menu entry (find it
in `/boot/grub/grub.cfg` or the file their setup uses for custom entries,
such as `/boot/grub/custom.cfg` or `/etc/grub.d/40_custom`). Change only the
title, `--id g15`, the `linux` path to `/g15/Image` (keep every kernel
argument as is), and the `initrd` path to `/g15/initramfs.img`. Paths are
relative to the partition GRUB searches, as in the copied entry. Regenerate
`grub.cfg` the way the system does it (`grub-mkconfig -o /boot/grub/grub.cfg`)
if the entry lives in `/etc/grub.d`. Show the person the new entry (with
secrets redacted) before you write it.

## 5. Boot it once and load the driver

Boot the new entry for one boot only, so the next reboot returns to the
known-good kernel:

```
sudo grub-reboot g15 && sudo reboot
```

After the reboot, check that `uname -r` is `7.2.2-display-driver-g15`, that
the display, keyboard and network work, then:

```
sudo g15-load
```

Success prints `crashed=0`, `submissions=0` and `G15 driver loaded`. If
`modprobe g15_dt_overlay` fails with "No such device", one of the checks in
section 1 failed: `sudo dmesg | grep g15_dt_overlay` says which.

## 6. Build Mesa and test rendering

Install Mesa's build dependencies first. These steps were run end to end on
the project's laptop (up-to-date Omarchy, LLVM 23), where the first six
packages were missing; a fresh install may lack others too:

```
sudo pacman -S --needed meson ninja python-mako python-pycparser libclc \
    spirv-llvm-translator clang llvm libdrm wayland wayland-protocols \
    libglvnd glslang spirv-tools bison flex python-packaging python-yaml
```

If meson still reports a missing dependency or option, report it rather
than working around it with different options.

```
git clone https://github.com/GravityLinux/mesa.git ~/g15-mesa-src
cd ~/g15-mesa-src
git checkout 87ac51d411f0e58a9433567ec2666f457be89e1d
git apply ~/g15/mesa/g15-render.patch
meson setup build --prefix="$HOME/g15-mesa" --libdir=lib -Dbuildtype=debugoptimized \
    -Dgallium-drivers=asahi -Dvulkan-drivers= -Dplatforms=wayland \
    -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Degl=enabled -Dgbm=enabled \
    -Dglx=disabled -Dglvnd=enabled -Dllvm=enabled -Dgallium-rusticl=false \
    -Dvideo-codecs= -Dtools=
ninja -C build install
cat > ~/g15-mesa/50_g15_mesa.json <<EOF
{"file_format_version": "1.0.0", "ICD": {"library_path": "$HOME/g15-mesa/lib/libEGL_mesa.so.0"}}
EOF
```

Test with the client in `tools/gl-test` (needs the driver loaded):

```
cd ~/g15/tools/gl-test && make
ASAHI_G15_GRAPHICS=1 LD_LIBRARY_PATH=$HOME/g15-mesa/lib \
    __EGL_VENDOR_LIBRARY_FILENAMES=$HOME/g15-mesa/50_g15_mesa.json \
    ./g15-gl-triangle --mode const
```

It must print `G15_GL_RESULT PASS mismatches=0 ...`. Then
`sudo cat /sys/kernel/debug/asahi/g15_accounting` should show
`completion_errors=0` and `recoveries=0`.

## 7. Optional: a desktop session on the GPU

`tools/desktop/README.md` describes running Hyprland (Omarchy) on the G15
GPU: a session hook that loads the driver at login and points the compositor
and apps at `~/g15-mesa`, plus wrappers for apps that need features the
driver does not have yet. Keep the login screen on the CPU renderer as
described there. Logging out and back in works; apps that need more than
GLES 3.0 or desktop GL 2.1 (Chromium's GPU path, Ghostty) run on the CPU
renderer.

## 8. When it does not apply, and how to undo it

**Other machines or firmware.** The driver's built-in values (render
profile, firmware hardware tables, init data, device-tree overlays) were
observed on one J613 with the 14.7 firmware; `kernel/README.md` lists them.
Another model, or another stub firmware version, needs its own values from
that machine. Today that means tracing macOS under the m1n1 hypervisor and
regenerating those tables, which is research work, not an install step.
Tell the person their configuration is not supported yet, rather than
changing values.

**Undo.** Reboot (the one-time boot returns to the old kernel), then remove
the GRUB entry, `/boot/g15`, `/lib/modules/7.2.2-display-driver-g15`,
`/etc/modprobe.d/g15.conf`, `/usr/local/sbin/g15-load`, `~/g15-mesa` and any
desktop hooks from section 7.

**Making it the default** is the person's decision, once the G15 kernel has
worked for them. Only then set GRUB's default to the `g15` entry.

## 9. Known limits

See `docs/g15-spec/70-known-limits.md`. In short: one GPU job runs at a
time with a short wait after each render, desktop GL is 2.1 (GLES 3.0), no
MSAA, no suspend support for the GPU, and a GPU hang the firmware does not
report (for example an infinite-loop shader) needs a reboot.
