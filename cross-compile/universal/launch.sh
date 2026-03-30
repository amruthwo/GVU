#!/bin/sh
# GVU universal launcher — SpruceOS
# Reads platform env vars exported by standard_launch.sh / helperFunctions.sh
# and forwards them as GVU_ prefixed vars before exec-ing the correct binary.

# Resolve the directory this script lives in.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Select 32-bit or 64-bit binary based on PLATFORM_ARCHITECTURE.
if [ "$PLATFORM_ARCHITECTURE" = "aarch64" ]; then
    BIN_DIR="$SCRIPT_DIR/bin64"
else
    BIN_DIR="$SCRIPT_DIR/bin32"
fi

# Forward SpruceOS platform vars to GVU.
export GVU_PLATFORM="$PLATFORM"
export GVU_DISPLAY_W="$DISPLAY_WIDTH"
export GVU_DISPLAY_H="$DISPLAY_HEIGHT"
export GVU_DISPLAY_ROTATION="$DISPLAY_ROTATION"
export GVU_INPUT_DEV="$EVENT_PATH_READ_INPUTS_SPRUCE"
export GVU_PYTHON="$DEVICE_PYTHON3_PATH"
export GVU_BATTERY_PATH="${BATTERY}/capacity"

exec "$BIN_DIR/gvu" "$1"
