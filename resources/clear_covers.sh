#!/bin/sh
# clear_covers.sh — remove all cover art from GVU's media folder
#
# Deletes every cover.jpg and cover.png found anywhere under
# /mnt/SDCARD/Roms/MEDIA/ (GVU's video root only).
#
# Safe to run from DinguxCommander or SSH.
# Does NOT touch /mnt/SDCARD/Media/ (used by music apps).

count=0

clear_dir() {
    dir="$1"
    skip="$2"
    [ -d "$dir" ] || return
    for f in $(find "$dir" -name "cover.jpg" -o -name "cover.png"); do
        case "$f" in
            "$skip"/*) continue ;;
        esac
        rm -f "$f"
        echo "Removed: $f"
        count=$((count + 1))
    done
}

clear_dir "/mnt/SDCARD/Roms/MEDIA" ""
clear_dir "/mnt/SDCARD/Media"      "/mnt/SDCARD/Media/Music"

echo "Done. Removed $count cover file(s)."
