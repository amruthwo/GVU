#!/bin/sh

APPDIR=/mnt/SDCARD/App/GVU
LOG=/tmp/gvu.log

echo "launch.sh start" > "$LOG"

. /mnt/SDCARD/spruce/scripts/helperFunctions.sh
echo "helperFunctions sourced" >> "$LOG"

export LD_LIBRARY_PATH="$APPDIR/libs32:/mnt/SDCARD/spruce/bin:$LD_LIBRARY_PATH"
export SDL_VIDEODRIVER=dummy

cd "$APPDIR"
echo "cwd: $(pwd)" >> "$LOG"

sleep 0.5

echo "launching gvu32" >> "$LOG"
"$APPDIR/gvu32" >> "$LOG" 2>&1
echo "gvu32 exited: $?" >> "$LOG"
