#!/bin/sh
# package_gvu_universal.sh — assemble a universal GVU SpruceOS zip
#
# Usage:  sh cross-compile/universal/package_gvu_universal.sh [VERSION]
# Output: GVU-universal-<VERSION>.zip  (in the project root)
#
# Requires: build/gvu32, build/gvu64, build/libs32/, build/libs64/
#           cross-compile/universal/launch.sh, config.json
#           resources/ directory

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

VERSION="${1:-dev}"
OUT_ZIP="$REPO_ROOT/GVU-universal-${VERSION}.zip"
STAGE="$REPO_ROOT/build/universal-stage"

echo "=== Packaging GVU universal ${VERSION} ==="

# Clean and recreate staging area
rm -rf "$STAGE"
mkdir -p "$STAGE/GVU/bin32"
mkdir -p "$STAGE/GVU/bin64"
mkdir -p "$STAGE/GVU/lib32"
mkdir -p "$STAGE/GVU/lib64"
mkdir -p "$STAGE/GVU/resources"

# Binaries
cp "$REPO_ROOT/build/gvu32"  "$STAGE/GVU/bin32/gvu"
cp "$REPO_ROOT/build/gvu64"  "$STAGE/GVU/bin64/gvu"
chmod +x "$STAGE/GVU/bin32/gvu" "$STAGE/GVU/bin64/gvu"

# Shared libraries
if [ -d "$REPO_ROOT/build/libs32" ]; then
    cp "$REPO_ROOT/build/libs32/"* "$STAGE/GVU/lib32/" 2>/dev/null || true
fi
if [ -d "$REPO_ROOT/build/libs64" ]; then
    cp "$REPO_ROOT/build/libs64/"* "$STAGE/GVU/lib64/" 2>/dev/null || true
fi

# Launch script and config
cp "$SCRIPT_DIR/launch.sh"   "$STAGE/GVU/launch.sh"
cp "$SCRIPT_DIR/config.json" "$STAGE/GVU/config.json"
chmod +x "$STAGE/GVU/launch.sh"

# Resources (fonts, icons, fetch_subtitles.py, etc.)
cp -r "$REPO_ROOT/resources/." "$STAGE/GVU/resources/"

# Icons (if present)
for icon in gvu.png gvu_sel.png; do
    if [ -f "$REPO_ROOT/$icon" ]; then
        cp "$REPO_ROOT/$icon" "$STAGE/GVU/"
    fi
done

# Create zip
cd "$STAGE"
rm -f "$OUT_ZIP"
zip -r "$OUT_ZIP" GVU/
echo "=== Created: $OUT_ZIP ==="
ls -lh "$OUT_ZIP"
