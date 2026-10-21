#!/bin/bash
# Build the G15 Mesa (Gravity Mesa + g15-render.patch) on the MacBook Air M3
# itself and install it under PREFIX, with no root:
#
#   mesa/build.sh SRCDIR PREFIX        (AGENTS.md uses ~/g15-mesa-src ~/g15-mesa)
#
# Rerunning with the same SRCDIR resumes an interrupted build.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=${1:?usage: $0 SRCDIR PREFIX}
prefix=${2:?usage: $0 SRCDIR PREFIX}
upstream=${UPSTREAM_REPO:-https://github.com/GravityLinux/mesa.git}
base=87ac51d411f0e58a9433567ec2666f457be89e1d
patch=$here/g15-render.patch

if [ ! -e "$src" ]; then
    git init -q "$src"
    git -C "$src" fetch --depth=1 "$upstream" "$base"
    git -C "$src" checkout -q FETCH_HEAD
fi
[ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" = "$base" ] \
    || { echo "$src exists and is not Mesa $base: use a new SRCDIR"; exit 1; }
if ! git -C "$src" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$src" apply "$patch"
fi

if [ ! -e "$src/build/build.ninja" ]; then
    meson setup "$src/build" "$src" --prefix="$prefix" --libdir=lib -Dbuildtype=debugoptimized \
        -Dgallium-drivers=asahi -Dvulkan-drivers= -Dplatforms=wayland \
        -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Degl=enabled -Dgbm=enabled \
        -Dglx=disabled -Dglvnd=enabled -Dllvm=enabled -Dgallium-rusticl=false \
        -Dvideo-codecs= -Dtools=
fi
ninja -C "$src/build" install
cat > "$prefix/50_g15_mesa.json" <<EOF
{"file_format_version": "1.0.0", "ICD": {"library_path": "$prefix/lib/libEGL_mesa.so.0"}}
EOF
echo "installed: $prefix"
