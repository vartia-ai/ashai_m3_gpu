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
# Tree after the 8 base patches, and after the G15 patch. A mismatch means
# the source is not what was tested.
base_tree=383716fbbb1d469efd91069729c06f55e3e327bf
g15_tree=3871177782557fb76586f034ac8c41d2d48dbcc5

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

mkdir -p "$obj"
cp "$here/config" "$obj/.config"
make -C "$src" O="$obj" LOCALVERSION= olddefconfig
cmp -s "$here/config" "$obj/.config" \
    || echo "note: olddefconfig changed the config (different compiler or rustc version?)"
make -C "$src" O="$obj" LOCALVERSION= -j"$(nproc)" Image modules
echo "built: $obj/arch/arm64/boot/Image, release $(make -s -C "$src" O="$obj" LOCALVERSION= kernelrelease)"
