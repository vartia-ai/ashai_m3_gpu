#!/bin/bash
# Build the G15 kernel (Linux 7.2.2, Asahi asahi-wip-7.2 + DCP display
# patches + the G15 GPU driver) on the MacBook Air M3 itself.
#
#   kernel/build.sh WORKDIR
#
# Produces WORKDIR/linux (source) and WORKDIR/build (objects, Image, modules).
# Installs nothing; see AGENTS.md for installing and booting it.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
work=${1:?usage: $0 WORKDIR}
upstream=${UPSTREAM_REPO:-https://github.com/AsahiLinux/linux.git}
base=236788cd2602a24c703fe7bdaddaf73ef77d2027  # AsahiLinux/linux asahi-wip-7.2, 2026-08-31
# Tree after the 8 base patches, after the G15 patch, and after the J613
# external display patch. A mismatch means the source is not what was tested.
base_tree=383716fbbb1d469efd91069729c06f55e3e327bf
g15_tree=bd8e4f7dcc987fb5dbf2e3245501fcaaa27c7f10
display_tree=205560425730dd1aeb48bab8eb8bfd9bf8c7203f

mkdir -p "$work"
src=$work/linux
obj=$work/build
[ ! -e "$src" ] || { echo "$src exists: use a new WORKDIR"; exit 1; }
git init -q "$src"
git -C "$src" fetch --depth=1 "$upstream" "$base"
git -C "$src" checkout -q FETCH_HEAD
git -C "$src" -c user.name=g15-build -c user.email=g15-build@localhost \
    am -q "$here"/base-patches/*.patch
[ "$(git -C "$src" rev-parse HEAD^{tree})" = "$base_tree" ] \
    || { echo "base tree mismatch"; exit 1; }
git -C "$src" apply --index "$here/g15.patch"
[ "$(git -C "$src" write-tree)" = "$g15_tree" ] \
    || { echo "G15 tree mismatch"; exit 1; }
git -C "$src" -c user.name=g15-build -c user.email=g15-build@localhost \
    commit -q -m "G15 GPU driver (kernel/g15.patch)"
# External display (display/README.md). Inert unless enabled: its overlay
# module does nothing without stage=2.
git -C "$src" apply --index "$here/../display/kernel.patch"
[ "$(git -C "$src" write-tree)" = "$display_tree" ] \
    || { echo "display tree mismatch"; exit 1; }
git -C "$src" -c user.name=g15-build -c user.email=g15-build@localhost \
    commit -q -m "J613 external display (display/kernel.patch)"
# Host build tools only: linux-api-headers 7.3 and later use __u128 in the arm64
# uapi headers, which this tree's tools/include does not define. Applied after
# the tree checks; the kernel and modules are unchanged.
git -C "$src" apply "$here/host-tools-u128.patch"

mkdir -p "$obj"
cp "$here/config" "$obj/.config"
make -C "$src" O="$obj" LOCALVERSION= olddefconfig
cmp -s "$here/config" "$obj/.config" \
    || echo "note: olddefconfig changed the config (different compiler or rustc version?)"
make -C "$src" O="$obj" LOCALVERSION= -j"$(nproc)" Image modules
echo "built: $obj/arch/arm64/boot/Image, release $(make -s -C "$src" O="$obj" LOCALVERSION= kernelrelease)"
