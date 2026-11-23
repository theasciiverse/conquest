#!/bin/sh
# Records the catalog preview, asciiverse/conquest.cast: a game against
# the robots at 100x30 (hub/RECORDING.md).  The menu runs inside
# asciinema in a detached tmux session and the keys are typed with
# tmux send-keys, in a throwaway HOME and state folder, as USER=player.
#   sh asciiverse/stage.sh && sh asciiverse/record-preview.sh
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
out="$root/asciiverse/conquest.cast"
tmp=$(mktemp -d)
session=conquest-preview-$$
trap 'tmux kill-session -t "$session" 2>/dev/null; rm -rf "$tmp"' EXIT

mkdir -p "$tmp/home" "$tmp/state"
rm -f "$out"
tmux new-session -d -s "$session" -x 100 -y 30 \
  "cd '$root/stage' && env -i PATH='$root/stage:/usr/bin:/bin:/snap/bin' HOME='$tmp/home' \
     XDG_STATE_HOME='$tmp/state' USER=player LOGNAME=player \
     TERM=xterm-256color COLORTERM=truecolor LANG=C.UTF-8 \
     asciinema rec --quiet --window-size 100x30 --idle-time-limit 2 \
       --capture-env TERM,COLORTERM,LANG --title Conquest \
       --command conquest '$out'"

k() { tmux send-keys -t "$session" "$@"; }
w() { sleep "$1"; }

w 1.5; k p                       # Play here
w 1.5; k player Enter; w 0.5; k y Enter; w 0.5
k secret Enter; w 0.5; k secret Enter
w 4; k e                         # enter the game
w 2; k 5                         # warp 5
w 2; k M                         # the long range map
w 2.5; k M
w 4.5; k /                       # who is flying: the robots are up
w 2.5; k q
w 1; k i nes Enter               # information on the nearest enemy ship
w 2; k k nes Enter               # and set course for it
w 1; k 9
w 1.5; k m; w 0.5; k all Enter; w 0.5; k 'Anyone out there?' Enter
w 2.5; k p Enter                 # a torpedo along the course
w 1; k P Enter                   # a burst of three
w 1.5; k f Enter                 # phasers
w 1.5; k Q; w 0.5; k Tab         # self-destruct, the way out of a ship
w 19; k Space
w 1.5; k q                       # leave the client
w 1.5; k q                       # leave the menu

# wait for the recording to finish; if the game is still running,
# hang it up the way a closed terminal would
i=0
while tmux has-session -t "$session" 2>/dev/null && [ $i -lt 20 ]; do
  sleep 0.5; i=$((i + 1))
done
for p in $(pgrep -x conquest); do kill -HUP "$p"; done
sleep 4
tmux kill-session -t "$session" 2>/dev/null || true
ls -l "$out"
