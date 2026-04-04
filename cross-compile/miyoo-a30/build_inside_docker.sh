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

echo "=== Compiling fetch_subs32 ==="
arm-linux-gnueabihf-gcc -Wall -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -DGVU_A30 \
    -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
    $(pkg-config --cflags libcurl) \
    -o "$BUILD/fetch_subs32" /gvu/src/fetch_subs.c /gvu/src/glibc_compat.c \
    $(pkg-config --static --libs libcurl) \
    -lz -lm -static-libgcc
echo "Built: fetch_subs32"

echo "=== Patching GLIBC version symbols ==="
python3 /gvu/cross-compile/miyoo-a30/patch_verneed.py "$BUILD/gvu32"
python3 /gvu/cross-compile/miyoo-a30/patch_verneed.py "$BUILD/fetch_subs32"

echo "=== Copying SDL2 shared library ==="
# lib32/libSDL2-2.0.so.0   — unpatched, for MiyooMini (V2/V3/V4, glibc 2.28+)
# lib32_a30/libSDL2-2.0.so.0 — patched GLIBC_2.27/2.28/2.29→2.4, for A30 (glibc 2.23)
# launch.sh prepends lib32_a30 to LD_LIBRARY_PATH only when PLATFORM=A30.
SDL2_SRC="$(readlink -f /opt/a30/lib/libSDL2-2.0.so.0)"
mkdir -p "$BUILD/libs32" "$BUILD/libs32_a30"
cp "$SDL2_SRC" "$BUILD/libs32/libSDL2-2.0.so.0"
cp "$SDL2_SRC" "$BUILD/libs32_a30/libSDL2-2.0.so.0"
python3 /gvu/cross-compile/miyoo-a30/patch_verneed.py "$BUILD/libs32_a30/libSDL2-2.0.so.0"
echo "Copied: lib32/libSDL2-2.0.so.0 (unpatched)"
echo "Copied: lib32_a30/libSDL2-2.0.so.0 (patched for glibc 2.23)"

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
    # Find the real file — try versioned name first, then unversioned base
    REAL=$(readlink -f "$ARMHF_LIB/$SONAME" 2>/dev/null)
    if [ -z "$REAL" ] || [ ! -f "$REAL" ]; then
        # e.g. libz.so.1 → try libz.so
        BASE=$(echo "$SONAME" | sed 's/\.so\..*/\.so/')
        REAL=$(readlink -f "$ARMHF_LIB/$BASE" 2>/dev/null)
    fi
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
