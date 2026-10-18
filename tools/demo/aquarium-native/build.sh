#!/bin/sh
# Build aquarium-native against raylib 5.5 (downloaded next to this script
# on first use, checksum-verified). Linux uses raylib's SDL2 backend (EGL on
# Wayland, so the G15 Mesa in the session environment); macOS uses its
# bundled GLFW (Cocoa/NSOpenGL). Both request an OpenGL 2.1 context.
#   ./build.sh            -> ./aquarium
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
RL=$DIR/raylib-5.5
SHA=aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
if [ ! -f "$RL/src/raylib.h" ]; then
  tmp=$DIR/raylib-5.5.tar.gz
  curl -sSL -o "$tmp" https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
  got=$( (sha256sum "$tmp" 2>/dev/null || shasum -a 256 "$tmp") | cut -d' ' -f1)
  [ "$got" = "$SHA" ] || { echo "raylib checksum mismatch: $got" >&2; exit 1; }
  tar xzf "$tmp" -C "$DIR" && rm -f "$tmp"
fi
S=$RL/src
SRCS="$S/rcore.c $S/rshapes.c $S/rtextures.c $S/rtext.c $S/rmodels.c $S/utils.c"
case $(uname) in
  Darwin)
    clang -O2 -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_21 -I"$S" -I"$S/external/glfw/include" \
      -x objective-c "$S/rglfw.c" -x c $SRCS "$DIR/aquarium.c" \
      -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL \
      -o "$DIR/aquarium" ;;
  Linux)
    cc -O2 -DPLATFORM_DESKTOP_SDL -DGRAPHICS_API_OPENGL_21 -I"$S" $(pkg-config --cflags sdl2) \
      $SRCS "$DIR/aquarium.c" $(pkg-config --libs sdl2) -lOpenGL -lm -ldl -lpthread \
      -o "$DIR/aquarium" ;;
  *) echo "unsupported OS" >&2; exit 1 ;;
esac
echo "built $DIR/aquarium"
