#!/bin/bash
# The native aquarium (aquarium-native: the WebGL Aquarium as a GL 2.1
# program) in the G15 Hyprland session, with g15top and btop beside it.
# Run on the laptop, from a desktop terminal or over SSH.
#   g15-aquarium-demo.sh [--cpu] [--record FILE.mp4] [--secs N] [--fish LIST] [--view N]
#   g15-aquarium-demo.sh --stop
# --cpu runs it on the CPU renderer (llvmpipe) for comparison. The scene
# renders at 1440x1440 with vsync off (as on macOS); aquarium.csv gets a line per second.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
AQ=${AQ_DIR:-$DIR/aquarium-native}
LOG=$HOME/.cache/g15-aquarium-demo
cpu=0 record="" secs=25 fish=500,1000,5000,10000 view=0
while [ $# -gt 0 ]; do
  case $1 in
    --cpu) cpu=1 ;;
    --record) record=$2; shift ;;
    --secs) secs=$2; shift ;;
    --fish) fish=$2; shift ;;
    --view) view=$2; shift ;;
    --stop)
      pkill -INT -x wf-recorder || true
      pkill -TERM -f "[a]quarium-native/aquarium " || true
      pkill -f "[f]oot --title g15-demo-" || true
      exit 0 ;;
    *) echo "usage: $0 [--cpu] [--record FILE.mp4] [--secs N] [--fish LIST] [--view N] | --stop" >&2; exit 2 ;;
  esac
  shift
done
[ -x "$AQ/aquarium" ] && [ -f "$AQ/pack/manifest.txt" ] ||
  { echo "build $AQ first (build.sh, convert.py)" >&2; exit 1; }

# The Hyprland session's environment (G15 Mesa paths, Wayland socket).
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
export LANG=${LANG:-C.UTF-8}
eval "$(systemctl --user show-environment |
  grep -E '^(WAYLAND_DISPLAY|HYPRLAND_INSTANCE_SIGNATURE|ASAHI_G15_GRAPHICS|LD_LIBRARY_PATH|__EGL_VENDOR_LIBRARY_FILENAMES|GBM_BACKENDS_PATH|XDG_[A-Z_]+|LANG|LC_[A-Z]+|PATH)=' |
  sed 's/^/export /')"

"$0" --stop
sleep 2
mkdir -p "$LOG"
if [ $cpu = 1 ]; then
  label="Linux · CPU renderer (llvmpipe)"
  run=/usr/local/bin/sw
else
  label="Linux · Apple M3 GPU (G15 driver + Mesa)"
  run=""
fi
setsid nohup $run "$AQ/aquarium" "$AQ/pack" --tour "$fish" --secs "$secs" --view "$view" --size 1440x1440 \
  --label "$label" > "$LOG/aquarium.csv" 2> "$LOG/aquarium.err" < /dev/null &
sleep 3
setsid nohup foot --title g15-demo-g15top g15top > /dev/null 2>&1 < /dev/null &
sleep 1
setsid nohup foot --title g15-demo-btop --font="JetBrainsMono Nerd Font:size=7" btop \
  > /dev/null 2>&1 < /dev/null &
sleep 1
# Give the aquarium most of the width: narrow the focused right column.
hyprctl dispatch 'hl.dsp.window.resize({ x = 120, y = 0, relative = true })' > /dev/null || true

if [ -n "$record" ]; then
  sleep 2
  setsid nohup wf-recorder -f "$record" -c libx264 -p preset=veryfast -p crf=23 -r 30 \
    > "$LOG/wf-recorder.log" 2>&1 < /dev/null &
  echo "recording to $record (stop with: $0 --stop)"
fi
echo "demo running: $label (log $LOG/aquarium.csv)"
