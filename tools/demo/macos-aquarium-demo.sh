#!/bin/bash
# macOS half of the aquarium demo: the same native aquarium as
# g15-aquarium-demo.sh on Linux (same tour, view and 1440x1440 scene), plus a
# Terminal window with the GPU and CPU numbers from powermetrics. Run it in
# Terminal on the laptop's macOS, with aquarium-native built there:
#   ./macos-aquarium-demo.sh [--secs N] [--fish LIST] [--view N]
#   ./macos-aquarium-demo.sh --stop
# Activity Monitor's GPU History (Window > GPU History, Cmd-4) can sit
# beside it. powermetrics needs sudo (passwordless on this machine).
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
AQ=${AQ_DIR:-$DIR/aquarium-native}
LOG=$HOME/Library/Logs/g15-aquarium-demo
secs=25 fish=500,1000,5000,10000 view=0
while [ $# -gt 0 ]; do
  case $1 in
    --secs) secs=$2; shift ;;
    --fish) fish=$2; shift ;;
    --view) view=$2; shift ;;
    --stop)
      sudo -n pkill -x powermetrics || true
      pkill -TERM -f "[a]quarium-native/aquarium " || true
      exit 0 ;;
    *) echo "usage: $0 [--secs N] [--fish LIST] [--view N] | --stop" >&2; exit 2 ;;
  esac
  shift
done
[ -x "$AQ/aquarium" ] && [ -f "$AQ/pack/manifest.txt" ] ||
  { echo "build $AQ first (build.sh, convert.py)" >&2; exit 1; }
mkdir -p "$LOG"

label="macOS $(sw_vers -productVersion) · Apple M3 GPU (Apple OpenGL)"
# Screen size in points; the aquarium takes the left three quarters.
read -r _ _ W H <<< "$(osascript -e 'tell application "Finder" to get bounds of window of desktop' | tr -d ',')"
split=$((W * 3 / 4))

nohup "$AQ/aquarium" "$AQ/pack" --tour "$fish" --secs "$secs" --view "$view" --size 1440x1440 \
  --window "0,25,$split,$((H - 25))" --label "$label" \
  > "$LOG/aquarium.csv" 2> "$LOG/aquarium.err" < /dev/null &

stats="sudo -n powermetrics --samplers cpu_power,gpu_power -i 1000 | grep --line-buffered -E 'GPU HW active (frequency|residency)|GPU Power|CPU Power|Combined Power'"
osascript - "$stats" "$split" "$W" "$H" <<'OSA'
on run argv
  set cmd to item 1 of argv
  set x to (item 2 of argv) as integer
  set w to (item 3 of argv) as integer
  set h to (item 4 of argv) as integer
  tell application "Terminal"
    do script "clear; echo 'macOS GPU/CPU (powermetrics, 1 s)'; " & cmd
    set bounds of front window to {x, 25, w, h}
  end tell
end run
OSA
echo "demo running: $label (log $LOG/aquarium.csv)"
echo "optional: open Activity Monitor and press Cmd-4 for GPU History"
