#!/bin/bash
# Install the G15 GPU driver on a MacBook Air M3 (J613) next to the existing
# kernel, with the desktop on the GPU from the first boot. See AGENTS.md.
#
#   ./install.sh check            change nothing; say what is missing
#                                 (with sudo it also previews the GRUB entry)
#   sudo ./install.sh packages    install the build packages check lists
#   ./install.sh build            build the kernel and Mesa as the user (about an hour)
#   sudo ./install.sh install     install the kernel, its GRUB entry and the desktop session
#   sudo ./install.sh all         all of the above, building as the user who ran sudo
#
# Then boot the G15 kernel once:  sudo grub-reboot g15 && sudo reboot
# Every step can be rerun. Nothing here changes the existing GRUB entries or
# default, m1n1, or the stock kernel. The external display is a separate,
# optional step (display/README.md).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
release=7.2.2-display-driver-g15
packages=(base-devel bc cpio git pahole rust rust-bindgen rust-src clang llvm
    meson ninja python-mako python-pycparser libclc spirv-llvm-translator libdrm
    wayland wayland-protocols libglvnd glslang spirv-tools bison flex
    python-packaging python-yaml)
state=/var/lib/g15-install
step=${1:-}

if [ "$(id -u)" = 0 ]; then
    user=${SUDO_USER:-}
    [ -n "$user" ] && [ "$user" != root ] || { echo "run with sudo from the desktop user"; exit 1; }
    home=$(getent passwd "$user" | cut -d: -f6)
else
    user=$(id -un) home=$HOME
fi
kwork=$home/g15-kernel
msrc=$home/g15-mesa-src
mesa=$home/g15-mesa
image=$kwork/build/arch/arm64/boot/Image

stop() { echo "STOP    $*"; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || stop "run this step with sudo: sudo $0 $step"; }
missing_packages() { pacman -T "${packages[@]}" || true; }
sha() { sha256sum < "$1" | cut -c1-64; }
free_mb() { df -BM --output=avail "$1" | tail -1 | tr -dc 0-9; }
# What a kernel was built from: the checked source tree and the config. Rebuilds
# of the same source differ only in their build timestamp.
source_id() {
    echo "$(git -c safe.directory="$kwork/linux" -C "$kwork/linux" rev-parse 'HEAD^{tree}')" \
        "$(sha "$kwork/build/.config")"
}
# The installed G15 kernel is the one this script installed (state file below),
# or was copied from this very build.
ours() {
    [ -e /boot/g15/Image ] || return 1
    [ "$(sed -n 's/^image=//p' "$state/installed" 2>/dev/null)" = "$(sha /boot/g15/Image)" ] \
        || { [ -e "$image" ] && cmp -s "$image" /boot/g15/Image; }
}
installed_from_this_source() {
    ours && [ "$(sed -n 's/^source=//p' "$state/installed" 2>/dev/null)" = "$(source_id)" ] \
        && [ -e /boot/g15/initramfs.img ] && [ -d "/lib/modules/$release" ]
}
# Builds run as the desktop user, at low priority so the desktop stays responsive.
as_user() {
    if [ "$(id -u)" = 0 ]; then
        runuser -u "$user" -- env -i HOME="$home" USER="$user" LOGNAME="$user" \
            PATH=/usr/local/bin:/usr/bin LANG="${LANG:-C.UTF-8}" nice -n 19 "$@"
    else
        nice -n 19 "$@"
    fi
}

check() {
    local bad=0 v
    ok() { echo "ok      $*"; }
    no() { echo "STOP    $*"; bad=1; }
    note() { echo "note    $*"; }
    v=$(tr -d '\0' < /proc/device-tree/compatible 2>/dev/null || true)
    [[ $v == *apple,j613* ]] && ok "MacBook Air 13\" M3 (apple,j613)" \
        || no "not a MacBook Air 13\" M3 (apple,j613): not supported yet (AGENTS.md section 8)"
    v=$(tr -d '\0' < /proc/device-tree/chosen/asahi,os-fw-version 2>/dev/null || true)
    [ "$v" = 14.7 ] && ok "stub firmware 14.7" \
        || no "stub firmware is '${v:-unknown}', not 14.7 (AGENTS.md sections 1 and 8)"
    v=$(tr -d '\0' < /proc/device-tree/chosen/asahi,iboot2-version 2>/dev/null || true)
    [ "$v" = iBoot-10151.140.19.700.2 ] && ok "GPU firmware loader $v" \
        || no "iBoot2 is '${v:-unknown}', not iBoot-10151.140.19.700.2 (AGENTS.md section 1)"
    [ "$(uname -m)" = aarch64 ] || no "not aarch64"
    command -v pacman >/dev/null || no "no pacman: this script is for Arch Linux ARM; follow AGENTS.md by hand"
    command -v mkinitcpio >/dev/null || no "no mkinitcpio: install the kernel by hand (AGENTS.md section 5)"

    v=$(missing_packages)
    [ -z "$v" ] && ok "build packages installed" \
        || note "missing build packages:" $v "(sudo $0 packages)"
    v=$(rustc --version 2>/dev/null | cut -d' ' -f2 || true)
    [ -z "$v" ] || [ "$v" = 1.98.1 ] \
        || note "rustc $v, tested 1.98.1: build with the tested release from rustup (AGENTS.md section 3, Rust version)"
    grep -Eqs '^[[:space:]]*IgnorePkg[[:space:]]*=.*[[:space:]]rust([[:space:]]|$)' /etc/pacman.conf \
        || note "to keep rust at a working release across updates: IgnorePkg = rust rust-src in /etc/pacman.conf"

    v=0
    [ -e "$image" ] && ok "kernel built in $kwork" || v=$((v + 12000))
    [ -e "$mesa/50_g15_mesa.json" ] && ok "Mesa built in $mesa" || v=$((v + 3000))
    [ "$v" = 0 ] || [ "$(free_mb "$home")" -ge "$v" ] \
        || no "the builds need about $((v / 1000)) GB free in $home"
    if [ -e "/lib/modules/$release" ] || [ -e /boot/g15 ]; then
        ours && ok "G15 kernel installed by this script or from this build" \
            || no "/lib/modules/$release or /boot/g15 exists but did not come from this build: stop and ask (AGENTS.md section 5)"
    else
        [ "$(free_mb /boot)" -ge 100 ] || no "the G15 kernel needs about 100 MB free in /boot"
        [ "$(free_mb /usr/lib/modules)" -ge 2500 ] || no "the G15 modules need about 2.5 GB free in /usr/lib/modules"
    fi
    [ ! -e /etc/systemd/system/j613-display.service ] || ours \
        || no "the external display is installed for another G15 kernel: update it with display/README.md"

    if [ -r /boot/grub/grub.cfg ]; then
        python3 -B "$here/kernel/grub-entry.py" show | sed 's/^/        /' || no "GRUB entry (above)"
        grep -q next_entry /boot/grub/grub.cfg \
            || note "grub.cfg has no next_entry support: grub-reboot will not work; pick the G15 entry in the menu"
    else
        note "GRUB entry: run sudo $0 check to preview it"
    fi
    [ -d "$home/.config/uwsm" ] \
        || note "no ~/.config/uwsm: the GPU session hook is for Omarchy (uwsm) Hyprland sessions"
    [ "$(uname -r)" != "$release" ] || note "running the G15 kernel now"
    [ "$bad" = 0 ] || { echo "Stopped: fix the STOP lines first. Nothing was changed."; return 1; }
}

packages() {
    need_root
    local missing
    missing=$(missing_packages)
    [ -n "$missing" ] || { echo "all build packages are installed"; return; }
    echo "installing:" $missing
    # shellcheck disable=SC2086
    pacman -S --needed --noconfirm $missing
}

build() {
    [ -z "$(missing_packages)" ] || stop "install the build packages first: sudo $0 packages"
    as_user "$here/kernel/build.sh" "$kwork"
    as_user "$here/mesa/build.sh" "$msrc" "$mesa"
}

install_all() {
    need_root
    [ -e "$image" ] && [ -e "$mesa/50_g15_mesa.json" ] || stop "build first: $0 build"
    [ "$(cat "$kwork/build/include/config/kernel.release")" = "$release" ] \
        || stop "the build in $kwork is not release $release"
    if installed_from_this_source || { cmp -s "$image" /boot/g15/Image 2>/dev/null \
            && [ -e /boot/g15/initramfs.img ] && [ -d "/lib/modules/$release" ]; }; then
        echo "the G15 kernel from this source is already installed"
    else
        [ "$(uname -r)" != "$release" ] || stop "the G15 kernel is running: boot the stock kernel to replace it"
        [ ! -e /etc/systemd/system/j613-display.service ] \
            || stop "the external display is installed: follow display/README.md before replacing the kernel"
        make -s -C "$kwork/linux" O="$kwork/build" LOCALVERSION= modules_install
        mkdir -p /boot/g15 "$state"
        cp "$image" /boot/g15/Image.new && sync && mv -f /boot/g15/Image.new /boot/g15/Image
        mkinitcpio -k "$release" -g /boot/g15/initramfs.img
        install -m 644 "$here/kernel/modprobe-g15.conf" /etc/modprobe.d/g15.conf
        install -m 755 "$here/kernel/g15-load" /usr/local/sbin/g15-load
        printf 'source=%s\nimage=%s\n' "$(source_id)" "$(sha /boot/g15/Image)" > "$state/installed"
        sync
        echo "installed the G15 kernel in /boot/g15 and /lib/modules/$release"
    fi
    python3 -B "$here/kernel/grub-entry.py" install
    "$here/tools/desktop/install.sh"
    echo
    echo "Installed. Save your work, then boot the G15 kernel once:"
    echo "    sudo grub-reboot g15 && sudo reboot"
    echo "The reboot after that returns to your current default kernel."
}

case $step in
    check) check ;;
    packages) packages ;;
    build) check && build ;;
    install) check && install_all ;;
    all) need_root; check; packages; build; install_all ;;
    *) sed -n '2,15s/^# \{0,1\}//p' "$0"; exit 2 ;;
esac
