# Front USB-C display on the MacBook Air M3

The display patch adds DisplayPort alt mode to the front USB-C port on the
13-inch M3 MacBook Air, J613, with the macOS 14.7 system firmware. It builds
with the G15 kernel but stays inactive until you install and enable the
display startup service.

The tested setup is a front-port USB-C HDMI adapter and an HP Pavilion32 at
2560x1440 and about 60 Hz, using HBR2 and normal plug orientation. The
internal panel remains available. Other adapters, rates, plug orientation,
the rear port, Thunderbolt and Studio Display are not established by this
release's test. Unsupported link configurations can fail rather than train.

The clean kernel and the public-source m1n1 patch below passed a boot-time
picture check with the adapter attached, followed by unplug/replug.

## Build the kernel

Follow the [GPU installation guide](../AGENTS.md) for the machine and
firmware checks, build dependencies and kernel build. The default build
applies [kernel.patch](kernel.patch) after the existing G15 patch and checks
both source tree hashes. The config enables the USB-C controller and
crossbar drivers as modules.

Use the kernel release reported by your build:

```sh
kernel_release=$(make -s -C "$HOME/g15-kernel/linux" \
    O="$HOME/g15-kernel/build" LOCALVERSION= kernelrelease)
printf '%s\n' "$kernel_release"
```

This release updates the existing G15 kernel name. Keep the stock Asahi
kernel and its boot entry as your fallback. Before replacing an existing
G15 kernel, boot the stock kernel and back up its G15 Image, initramfs and
module directory. Do not overwrite your only working kernel.

Install the complete built kernel and its modules using the GPU guide.
The display installer below then installs the matching six display modules
under `updates/j613-display` and adds the startup service and initramfs
configuration. It does not install the Image or edit GRUB.

## Reserve the external firmware before enabling it

The optional display feature needs a stage-2 m1n1 change. The base GPU
instructions intentionally leave m1n1 alone. Choose this boot-chain change
separately and keep the existing recovery route available.

[The source patch](m1n1.patch) applies to the public m1n1 revision
`4e06076e390f005f8368b272c445b932c4aa2d28`. It adds the T8122 internal
display setup and reserves the external DCP's live firmware regions from
the machine's own ADT. It leaves the external controller and its IOMMUs
disabled. The startup service verifies the reservation marker and every
region's non-overlap with Linux System RAM before enabling the controller.

Build m1n1 locally using its native ARM64 or cross-compilation dependencies,
including the Rust `aarch64-unknown-none-softfloat` target. If your current
m1n1 includes other required platform changes, preserve and review those
changes too. No assembled m1n1 image, firmware, ADT or device tree is
distributed here. See [the m1n1 attribution](NOTICE.md).

On Arch Linux ARM the `rust` package has only the host target, so a plain
`make` fails at `build/librust.a`. Do not install the `rustup` package: it
replaces `rust`, which the kernel build uses. Install rustup for your user
only, without changing your PATH, and add the target:

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
    sh -s -- -y --no-modify-path --profile minimal
~/.cargo/bin/rustup target add aarch64-unknown-none-softfloat
```

Then put `~/.cargo/bin` first on the PATH for the m1n1 build only, as below.

```sh
release_dir=$PWD
git init m1n1-display
git -C m1n1-display fetch --depth=1 https://github.com/AsahiLinux/m1n1.git \
    4e06076e390f005f8368b272c445b932c4aa2d28
git -C m1n1-display checkout --detach FETCH_HEAD
git -C m1n1-display apply --index "$release_dir/display/m1n1.patch"
git -C m1n1-display write-tree
PATH=$HOME/.cargo/bin:$PATH make -C m1n1-display -j"$(nproc)"
```

With only the released patch applied, the source tree hash is
`2b294caa5b0b6c164ff052762562b3f1d9fbc4f5`. This source builds both
m1n1.bin and m1n1.macho. A local build with additional required changes has
a different tree hash and needs its own review.

Locate the standalone m1n1 binary used to assemble your current
`/boot/m1n1/boot.bin`. A distribution may keep it at
`/usr/lib/asahi-boot/m1n1.bin`. A custom boot image needs its matching
custom binary. The assembler refuses an unknown prefix. It copies the
existing appended device trees and U-Boot payload without changing them.

```sh
python3 display/boot-image.py assemble \
    --old-m1n1 /path/to/the/current/standalone/m1n1.bin \
    --new-m1n1 m1n1-display/build/m1n1.bin \
    --output boot.bin.display
```

Keep the printed original and candidate SHA-256 values locally. Use them
in the following commands. `check` changes nothing:

```sh
original_sha=THE_PRINTED_ORIGINAL_SHA256
candidate_sha=THE_PRINTED_CANDIDATE_SHA256
python3 display/boot-image.py check --candidate boot.bin.display \
    --original-sha256 "$original_sha" --candidate-sha256 "$candidate_sha"
sudo python3 display/boot-image.py install --candidate boot.bin.display \
    --original-sha256 "$original_sha" --candidate-sha256 "$candidate_sha"
```

Installation first creates `/boot/m1n1/boot.bin.before-j613-display`, then
copies, syncs and atomically renames the candidate to `boot.bin`. It refuses
to replace a newer active image or an unrelated backup. A
`boot.bin.before-j613-display` left by an earlier install counts as
unrelated: rename it (for example with a date suffix) rather than deleting
it. It does not reboot or change the boot selection.

**Keep it across updates.** Package updates that touch m1n1 or the kernel's
device trees run `update-m1n1`, which rebuilds `boot.bin` from
`/etc/default/update-m1n1` and would put the previous m1n1 back. Install the
new m1n1 to a system path and point `M1N1=` at it, keeping the `DTBS=`
setting your image was built with:

```sh
sudo install -D -m 644 m1n1-display/build/m1n1.bin /usr/local/lib/j613-display/m1n1.bin
sudoedit /etc/default/update-m1n1   # set M1N1=/usr/local/lib/j613-display/m1n1.bin
```

If the file does not exist yet, create it with only the `M1N1=` line:
`update-m1n1` then keeps its default device trees (those of the newest stock
kernel), which are the ones a stock image carries. With the right settings,
`sudo env LC_ALL=C update-m1n1` reproduces the candidate image exactly
(compare `sha256sum /boot/m1n1/boot.bin` with the candidate hash).
The glob order of `DTBS=` depends on the locale, and the package hook runs
with the C locale, so use `LC_ALL=C` when you run it by hand.

Reboot (into either kernel), then verify the live reservations:

```sh
sudo python3 display/check-reservations.py
```

The expected result is four reserved segments outside System RAM. Stop if
this check fails. The display startup service repeats it on every start.

## Install display startup

These helpers target Arch Linux ARM with systemd and mkinitcpio. The
initramfs inclusion path uses mkinitcpio 42.2's `--nopost` and `-I` options.
Use your existing GPU kernel entry and the actual initramfs path below.
Do not change the stock fallback kernel or its entry.

If you used the earlier local test package, restore it with that package's
own rollback script first. The installer refuses the old
`j613-external-display.service` or its test config so two startup units
cannot race. If updating a previous installation of these helpers, run
its rollback before replacing the kernel, then archive
`/var/lib/j613-display` for the new installation.

A check verifies the six module names, kernel release and live firmware
reservations before changing system files:

```sh
sudo python3 display/install.py check \
    --kernel-release "$kernel_release" \
    --modules-dir "$HOME/g15-kernel/build" \
    --initramfs /boot/g15/initramfs.img
```

To opt in for the next boot of that kernel:

```sh
sudo python3 display/install.py install \
    --kernel-release "$kernel_release" \
    --modules-dir "$HOME/g15-kernel/build" \
    --initramfs /boot/g15/initramfs.img
```

The installer records each overwritten file and its mode in the root-only
`/var/lib/j613-display` directory. It builds an initramfs outside the boot
partition, verifies the modules and config are included, then replaces the
initramfs atomically. It enables `j613-display.service` for the recorded
kernel release. It does not reboot, unload the live graphics driver, edit
session settings, change GRUB, or touch m1n1.

A failed install restores the recorded files. Repeating the same install
reuses its original backups. An interrupted install can be retried; an
unrelated file change or a different bundle is refused rather than replaced.
Keep the state directory for rollback.

The service defers `sn201202x` autoload until the front-port display route
exists. The supported stock fallback does not build this module. If another
custom kernel builds it, the installed blacklist also affects its autoload.
Display setup attempts to load the controller even on failure, so a display
error does not intentionally leave USB-C data disabled.

## Check the first display boot

Choose the G15 kernel for one boot without changing the default. Leave the
HDMI adapter connected for the boot-time picture check. Check the internal
panel, external picture, unplug and replug. A boot with no adapter is also
valid; the service reports the controller ready and waits for future plugs.

Successful replug runs still log an `atcphy_mux_set` warning when the USB
pipehandler remains active during the mode switch. That mode-transition
issue remains open.

Check service state and the saved logs:

```sh
systemctl status j613-display.service
journalctl -b -u j613-display.service
```

Kernel logs saved under `/var/log/j613-display` redact disk-unlock command
line values. Do not print unfiltered command lines or logs. Do not reload
the live DRM driver to retry a failed setup; reboot when ready.

## Roll back

From Linux, restore the display install's recorded files and service
selection without changing the current session or rebooting:

```sh
sudo python3 display/rollback.py
```

Rollback preserves unrelated modules and refuses to overwrite files changed
outside the installer. It does not remove your separately installed kernel
or change the boot entry. Return to the stock kernel through your existing
fallback entry.

To restore m1n1 separately:

```sh
sudo python3 display/boot-image.py rollback \
    --original-sha256 "$original_sha" --candidate-sha256 "$candidate_sha"
```

If Linux cannot boot, use your existing macOS or Recovery route to mount the
Linux EFI partition. In its `m1n1` directory, restore the neighboring
known-good image with copy, sync and rename:

```sh
cp boot.bin.before-j613-display boot.bin.restore-new
sync
mv boot.bin.restore-new boot.bin
sync
```

Keep the backup. A distribution's later m1n1 update can replace the custom
reservation helper; recheck live reservations before enabling the display
again. The startup service refuses missing reservations.

## Check the package without hardware

```sh
python3 -B -m unittest discover -s display -p 'test_*.py'
bash -n display/start
```

These checks use synthetic images, fake system tools and temporary file
systems. They exercise interrupted install recovery, failed initramfs
builds, file ownership boundaries, rollback, USB-controller fallback and
log redaction. They do not establish a hardware display result.
