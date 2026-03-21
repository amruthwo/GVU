#!/bin/sh
# Runs INSIDE the Docker container.
# Compiles gvu32 and collects all .so dependencies into /gvu/build/libs32/
set -e

ARMHF_LIB=/usr/lib/arm-linux-gnueabihf
BUILD=/gvu/build

mkdir -p "$BUILD/libs32"

echo "=== Compiling gvu32 ==="
make -C /gvu miyoo-a30-build
cp /gvu/gvu32 "$BUILD/gvu32"

echo "=== Patching GLIBC version symbols ==="
python3 /gvu/cross-compile/miyoo-a30/patch_verneed.py "$BUILD/gvu32"

echo "=== Collecting shared library dependencies ==="
# Collect .so files needed by gvu32, resolving symlinks to get real files.
# We skip the libs the A30 already provides: libc, libm, libpthread, libdl,
# librt, libgcc_s, ld-linux.
SKIP="libc.so libm.so libpthread.so libdl.so librt.so libgcc_s.so ld-linux"

collect_libs() {
    BINARY="$1"
    # List NEEDED entries from the ELF dynamic section
    arm-linux-gnueabihf-objdump -p "$BINARY" 2>/dev/null \
        | awk '/NEEDED/{print $2}'
}

copy_lib() {
    SONAME="$1"
    # Skip system libs
    for skip in $SKIP; do
        case "$SONAME" in *$skip*) return ;; esac
    done
    # Already copied?
    [ -f "$BUILD/libs32/$SONAME" ] && return
    # Find the real file
    REAL=$(readlink -f "$ARMHF_LIB/$SONAME" 2>/dev/null)
    if [ -z "$REAL" ] || [ ! -f "$REAL" ]; then
        echo "  WARNING: $SONAME not found in $ARMHF_LIB"
        return
    fi
    echo "  $SONAME -> $(basename $REAL)"
    cp "$REAL" "$BUILD/libs32/$SONAME"
    # Recurse into this lib's deps
    for dep in $(collect_libs "$BUILD/libs32/$SONAME"); do
        copy_lib "$dep"
    done
}

for soname in $(collect_libs "$BUILD/gvu32"); do
    copy_lib "$soname"
done

echo "=== libs32 contents ==="
ls -lh "$BUILD/libs32/"
echo "=== Done ==="
