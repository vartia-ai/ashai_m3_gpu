#!/bin/bash
# Install the G15 desktop session pieces (see README.md), so the Omarchy/Hyprland
# session runs on the G15 GPU from the first boot of the G15 kernel. Run as the
# desktop user (it uses sudo for the system files), or with sudo from that user:
#   tools/desktop/install.sh
#
# Needs the G15 kernel and g15-load installed (AGENTS.md section 5) and the G15
# Mesa built under $G15_MESA (default ~/g15-mesa, AGENTS.md section 4). Changes
# nothing in the boot setup; on any other kernel every piece is inactive.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
kernel=$here/../../kernel
stamp=$(date +%Y%m%d-%H%M%S)

if [ "$(id -u)" = 0 ]; then
    user=${SUDO_USER:-}
    [ -n "$user" ] && [ "$user" != root ] \
        || { echo "run as the desktop user, or with sudo from it"; exit 1; }
    home=$(getent passwd "$user" | cut -d: -f6)
    as_root= as_user="runuser -u $user --"
else
    user=$(id -un) home=$HOME as_root=sudo as_user=
fi
mesa=${G15_MESA:-$home/g15-mesa}

[ -x /usr/local/sbin/g15-load ] || { echo "install /usr/local/sbin/g15-load first (AGENTS.md section 5)"; exit 1; }
[ -e "$mesa/50_g15_mesa.json" ] \
    || echo "note: no G15 Mesa at $mesa yet; the session stays on the CPU renderer until it is built"

# Keep a copy of anything this replaces that differs from the new file.
put() { # mode source target runner
    local mode=$1 src=$2 dst=$3 run=$4
    if [ -e "$dst" ] && ! cmp -s "$src" "$dst"; then
        $run cp -p "$dst" "$dst.bak-$stamp"; echo "kept $dst.bak-$stamp"
    fi
    $run install -D -m "$mode" "$src" "$dst"; echo "installed $dst"
}

# Load the driver at boot, before the login screen.
put 644 "$kernel/g15-load.service" /etc/systemd/system/g15-load.service "$as_root"
$as_root systemctl daemon-reload
$as_root systemctl enable g15-load.service

# Apps that need more than the G15 driver offers run on the CPU renderer.
for f in sw quickshell ghostty g15top g15-demo-apps; do
    put 755 "$here/$f" "/usr/local/bin/$f" "$as_root"
done
put 644 "$here/ghostty-systemd-dropin.conf" \
    "$home/.config/systemd/user/app-com.mitchellh.ghostty.service.d/m3gpu-g15-wrapper.conf" "$as_user"
runtime=/run/user/$(id -u "$user")
if [ -d "$runtime" ]; then
    $as_user env XDG_RUNTIME_DIR="$runtime" systemctl --user daemon-reload \
        || echo "note: could not reload the user units; the next login picks up the drop-in"
fi

# Point the Hyprland session at the G15 Mesa.
put 644 "$here/uwsm-env-hyprland" "$home/.config/uwsm/env-hyprland" "$as_user"

# The login screen must stay on the CPU renderer (Omarchy's SDDM config does this).
if ! grep -rqs 'kms_swrast' /etc/sddm.conf /etc/sddm.conf.d; then
    echo "warning: SDDM's CompositorCommand does not force the CPU renderer; see README.md"
fi
echo "done: reboot into the G15 kernel (sudo grub-reboot g15 && sudo reboot)"
