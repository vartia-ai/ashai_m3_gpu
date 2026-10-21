#!/bin/bash
# Install the G15 desktop session pieces (see README.md), so the Omarchy/Hyprland
# session runs on the G15 GPU from the first boot of the G15 kernel. Run as the
# desktop user (it uses sudo for the system files):  tools/desktop/install.sh
#
# Needs the G15 kernel and g15-load installed (AGENTS.md section 5) and the G15
# Mesa built under $G15_MESA (default ~/g15-mesa, AGENTS.md section 4). Changes
# nothing in the boot setup; on any other kernel every piece is inactive.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
kernel=$here/../../kernel
stamp=$(date +%Y%m%d-%H%M%S)

[ "$(id -u)" != 0 ] || { echo "run as the desktop user, not root"; exit 1; }
[ -x /usr/local/sbin/g15-load ] || { echo "install /usr/local/sbin/g15-load first (AGENTS.md section 5)"; exit 1; }
[ -e "${G15_MESA:-$HOME/g15-mesa}/50_g15_mesa.json" ] \
    || echo "note: no G15 Mesa at ${G15_MESA:-$HOME/g15-mesa} yet; the session stays on the CPU renderer until it is built"

# Keep a copy of anything this replaces that differs from the new file.
put() { # mode source target [sudo]
    local mode=$1 src=$2 dst=$3 run=${4:-}
    if [ -e "$dst" ] && ! cmp -s "$src" "$dst"; then
        $run cp -p "$dst" "$dst.bak-$stamp"; echo "kept $dst.bak-$stamp"
    fi
    $run install -D -m "$mode" "$src" "$dst"; echo "installed $dst"
}

# Load the driver at boot, before the login screen.
put 644 "$kernel/g15-load.service" /etc/systemd/system/g15-load.service sudo
sudo systemctl daemon-reload
sudo systemctl enable g15-load.service

# Apps that need more than the G15 driver offers run on the CPU renderer.
for f in sw quickshell ghostty g15top g15-demo-apps; do
    put 755 "$here/$f" "/usr/local/bin/$f" sudo
done
put 644 "$here/ghostty-systemd-dropin.conf" \
    "$HOME/.config/systemd/user/app-com.mitchellh.ghostty.service.d/m3gpu-g15-wrapper.conf"
systemctl --user daemon-reload

# Point the Hyprland session at the G15 Mesa.
put 644 "$here/uwsm-env-hyprland" "$HOME/.config/uwsm/env-hyprland"

# The login screen must stay on the CPU renderer (Omarchy's SDDM config does this).
if ! grep -rqs 'kms_swrast' /etc/sddm.conf /etc/sddm.conf.d; then
    echo "warning: SDDM's CompositorCommand does not force the CPU renderer; see README.md"
fi
echo "done: reboot into the G15 kernel (sudo grub-reboot g15 && sudo reboot)"
