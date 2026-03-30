#!/bin/sh
# Host-side packaging script for Trimui Brick.
# Assembles the SpruceOS app package from:
#   build/gvu64       — compiled binary (from build_inside_docker.sh)
#   build/libs64/     — collected .so files (from build_inside_docker.sh)
#   gvu_base/         — SpruceOS infrastructure (launch.sh, config.json)
#   resources/        — fonts, default cover
#
# Usage: ./package_gvu_brick.sh [version]
set -e

VERSION=${1:-test}
SCRIPT_DIR=$(dirname "$0")
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BASE_DIR="$SCRIPT_DIR/gvu_base"
BUILD="$REPO_ROOT/build"

if [ ! -f "$BUILD/gvu64" ]; then
    echo "ERROR: $BUILD/gvu64 not found. Run 'make trimui-brick-docker' first."
    exit 1
fi

STAGE=$(mktemp -d)
trap "rm -rf $STAGE" EXIT

APP="$STAGE/spruce_gvu_pkg/App/GVU"
mkdir -p "$APP/libs64"
mkdir -p "$APP/resources/fonts"

# SpruceOS infrastructure
cp -v "$BASE_DIR/launch.sh"   "$APP/"
cp -v "$BASE_DIR/config.json" "$APP/"
chmod +x "$APP/launch.sh"

# Icon
if [ -f "$BASE_DIR/icon.png" ]; then
    cp -v "$BASE_DIR/icon.png" "$APP/"
fi

# Binary
cp -v "$BUILD/gvu64" "$APP/"
chmod +x "$APP/gvu64"

# Bundled .so files from Docker build
if [ -d "$BUILD/libs64" ]; then
    cp -v "$BUILD/libs64/"* "$APP/libs64/" 2>/dev/null || true
fi

# Resources
cp -v "$REPO_ROOT/resources/fonts/DejaVuSans.ttf"      "$APP/resources/fonts/"
cp -v "$REPO_ROOT/resources/default_cover.png"          "$APP/resources/"
cp -v "$REPO_ROOT/resources/default_cover.svg"          "$APP/resources/"
cp -v "$REPO_ROOT/resources/app_icon.svg"               "$APP/resources/"
cp -v "$REPO_ROOT/resources/scrape_covers.sh"           "$APP/resources/"
cp -v "$REPO_ROOT/resources/clear_covers.sh"            "$APP/resources/"
cp -v "$REPO_ROOT/resources/fetch_subtitles.py"         "$APP/resources/"
cp -v "$REPO_ROOT/resources/clear_subtitle_pref.sh"     "$APP/resources/"
chmod +x "$APP/resources/scrape_covers.sh" \
         "$APP/resources/clear_covers.sh" \
         "$APP/resources/clear_subtitle_pref.sh"

# gvu.conf
printf 'theme = SPRUCE\n' > "$APP/gvu.conf"
TMDB_KEY_FILE="$REPO_ROOT/.tmdb_key"
if [ -f "$TMDB_KEY_FILE" ]; then
    KEY=$(tr -d '[:space:]' < "$TMDB_KEY_FILE")
    if [ -n "$KEY" ]; then
        printf 'tmdb_key = %s\n' "$KEY" >> "$APP/gvu.conf"
        echo "TMDB key: included from .tmdb_key"
    fi
else
    echo "TMDB key: not found (.tmdb_key missing) — TVMaze-only package"
fi
SUBDL_KEY_FILE="$REPO_ROOT/.subdl_key"
if [ -f "$SUBDL_KEY_FILE" ]; then
    KEY=$(tr -d '[:space:]' < "$SUBDL_KEY_FILE")
    if [ -n "$KEY" ]; then
        printf 'subdl_key = %s\n' "$KEY" >> "$APP/gvu.conf"
        echo "SubDL key: included from .subdl_key"
    fi
else
    echo "SubDL key: not found (.subdl_key missing) — Podnapisi-only subtitle search"
fi

OUTFILE="$REPO_ROOT/build/gvu_spruce_brick_v${VERSION}.zip"
(cd "$STAGE" && zip -r "$OUTFILE" spruce_gvu_pkg)
echo ""
echo "Package: $OUTFILE"
echo "Contents:"
unzip -l "$OUTFILE" | tail -n +4 | head -40
