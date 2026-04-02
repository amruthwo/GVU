#!/bin/sh

APPDIR=/mnt/SDCARD/App/GVU
LOG=/tmp/gvu.log

echo "launch.sh start" > "$LOG"

. /mnt/SDCARD/spruce/scripts/helperFunctions.sh
echo "helperFunctions sourced, PLATFORM=$PLATFORM ARCH=$PLATFORM_ARCHITECTURE" >> "$LOG"

# Select binary and library directory based on CPU architecture.
if [ "$PLATFORM_ARCHITECTURE" = "aarch64" ]; then
    BIN="$APPDIR/bin64/gvu"
    LIBDIR="$APPDIR/lib64"
    echo "using bin64" >> "$LOG"
else
    BIN="$APPDIR/bin32/gvu"
    LIBDIR="$APPDIR/lib32"
    echo "using bin32" >> "$LOG"
fi

export LD_LIBRARY_PATH="$LIBDIR:/usr/lib:$LD_LIBRARY_PATH"
export SDL_VIDEODRIVER=dummy

# On some devices (e.g. Miyoo Flip) the system .asoundrc uses dmix which
# locks to 44100Hz, causing SDL audio init to fail.  Override with plughw
# which performs automatic rate/format conversion.
export HOME=/tmp/gvu_home
mkdir -p "$HOME"
cat > "$HOME/.asoundrc" << 'ASOUND_EOF'
pcm.!default {
    type plug
    slave.pcm "hw:0,0"
}
ctl.!default {
    type hw
    card 0
}
ASOUND_EOF

# Forward SpruceOS platform vars to GVU runtime.
export GVU_PLATFORM="$PLATFORM"
export GVU_DISPLAY_W="$DISPLAY_WIDTH"
export GVU_DISPLAY_H="$DISPLAY_HEIGHT"
export GVU_DISPLAY_ROTATION="$DISPLAY_ROTATION"
export GVU_INPUT_DEV="$EVENT_PATH_READ_INPUTS_SPRUCE"
export GVU_PYTHON="$DEVICE_PYTHON3_PATH"
export GVU_BATTERY_PATH="${BATTERY}/capacity"
export GVU_CACERT_PATH="$APPDIR/resources/cacert.pem"

cd "$APPDIR"
echo "launching $BIN" >> "$LOG"

sleep 0.5

# Clear fb0 to black before GVU starts so no stale content is visible
# during the ~500ms before brick_screen_init runs.
dd if=/dev/zero of=/dev/fb0 2>/dev/null || true

"$BIN" >> "$LOG" 2>&1
echo "gvu exited: $?" >> "$LOG"
