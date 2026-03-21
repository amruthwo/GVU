#!/bin/sh
# Host-side packaging script.
# Assembles the SpruceOS app package from:
#   build/gvu32       — compiled binary (from build_inside_docker.sh)
#   build/libs32/     — collected .so files (from build_inside_docker.sh)
#   gvu_base/         — SpruceOS infrastructure (launch.sh, config.json)
#   resources/        — fonts, default cover
#
# Usage: ./package_gvu_a30.sh [version]
set -e

VERSION=${1:-test}
SCRIPT_DIR=$(dirname "$0")
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BASE_DIR="$SCRIPT_DIR/gvu_base"
BUILD="$REPO_ROOT/build"

if [ ! -f "$BUILD/gvu32" ]; then
    echo "ERROR: $BUILD/gvu32 not found. Run 'make miyoo-a30-docker' first."
    exit 1
fi

STAGE=$(mktemp -d)
trap "rm -rf $STAGE" EXIT

APP="$STAGE/spruce_gvu_pkg/App/GVU"
mkdir -p "$APP/libs32"
mkdir -p "$APP/resources/fonts"

# SpruceOS infrastructure
cp -v "$BASE_DIR/launch.sh"   "$APP/"
cp -v "$BASE_DIR/config.json" "$APP/"
chmod +x "$APP/launch.sh"

# Icon — use a placeholder if none exists yet
if [ -f "$BASE_DIR/icon.png" ]; then
    cp -v "$BASE_DIR/icon.png" "$APP/"
fi

# Binary
cp -v "$BUILD/gvu32" "$APP/"
chmod +x "$APP/gvu32"

# Bundled .so files
cp -v "$BUILD/libs32/"* "$APP/libs32/"

# Resources
cp -v "$REPO_ROOT/resources/fonts/DejaVuSans.ttf"  "$APP/resources/fonts/"
cp -v "$REPO_ROOT/resources/default_cover.png"      "$APP/resources/"
cp -v "$REPO_ROOT/resources/scrape_covers.sh"       "$APP/resources/"
chmod +x "$APP/resources/scrape_covers.sh"

OUTFILE="$REPO_ROOT/build/gvu_spruce_a30_v${VERSION}.zip"
(cd "$STAGE" && zip -r "$OUTFILE" spruce_gvu_pkg)
echo ""
echo "Package: $OUTFILE"
echo "Contents:"
unzip -l "$OUTFILE" | tail -n +4 | head -40
