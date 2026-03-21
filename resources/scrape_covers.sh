#!/bin/sh
# scrape_covers.sh — fetch cover art for a GVU media folder
#
# Usage: scrape_covers.sh <folder_path> [tmdb_api_key]
#
# Saves cover.jpg to <folder_path>/cover.jpg.
# Season folder detection: if basename matches "Season N" / "S01" patterns,
# the parent directory name is used as the show/search name.
#
# Sources (in order):
#   1. TMDB (The Movie Database) — requires API key
#   2. TVMaze — no key needed, TV shows only
#
# Writes progress messages to stdout.
# Exit 0 = success, non-zero = failure.
#
# On-device dependencies: wget, sed, tr, grep, basename, dirname
#   (all standard BusyBox utilities on SpruceOS)

set -e

FOLDER="$1"
TMDB_KEY="${2:-}"

TMDB_SEARCH="https://api.themoviedb.org/3/search/multi"
TMDB_IMG_BASE="https://image.tmdb.org/t/p/w500"
TVMAZE_SEARCH="https://api.tvmaze.com/singlesearch/shows"

if [ -z "$FOLDER" ]; then
    echo "Usage: scrape_covers.sh <folder_path> [tmdb_key]" >&2
    exit 1
fi

# -------------------------------------------------------------------------
# Determine search name
# Season folder: "Season 1", "Season 01", "season 2", "S01", "s02", etc.
# -------------------------------------------------------------------------
name=$(basename "$FOLDER")
if echo "$name" | grep -qiE '^(season[ _-]*[0-9]+|s[0-9]{1,2})$'; then
    parent=$(dirname "$FOLDER")
    show=$(basename "$parent")
    echo "Season folder detected: '$name' — using parent name: '$show'"
    name="$show"
fi
echo "Searching for: $name"

# -------------------------------------------------------------------------
# URL-encode the query (spaces and common punctuation)
# -------------------------------------------------------------------------
urlencode() {
    printf '%s' "$1" | sed \
        's/ /%20/g; s/!/%21/g; s/"/%22/g; s/#/%23/g; s/&/%26/g;
         s/(/%28/g; s/)/%29/g; s/+/%2B/g; s/,/%2C/g; s/:/%3A/g;
         s/;/%3B/g; s/=/%3D/g; s/?/%3F/g; s/@/%40/g'
}
query=$(urlencode "$name")

# -------------------------------------------------------------------------
# Temp file for API responses
# -------------------------------------------------------------------------
tmpfile="/tmp/gvu_scrape_$$.json"
tmpimg="/tmp/gvu_cover_$$.jpg"
# On any exit (normal or abnormal), clean up temps and ensure the sentinel
# is always written so GVU never gets stuck showing the progress overlay.
trap 'rm -f "$tmpfile" "$tmpimg"; [ -f /tmp/gvu_scrape_done ] || echo "error" > /tmp/gvu_scrape_done' EXIT

cover_url=""

# -------------------------------------------------------------------------
# 1. TMDB (primary — requires API key)
# -------------------------------------------------------------------------
if [ -n "$TMDB_KEY" ]; then
    echo "Trying TMDB..."
    url="${TMDB_SEARCH}?api_key=${TMDB_KEY}&query=${query}&page=1"
    if wget -q --timeout=20 --no-check-certificate -O "$tmpfile" "$url" 2>/dev/null; then
        # Split on commas so each JSON field is on its own line, then extract
        # the first "poster_path" value (skips "null" entries).
        poster=$(tr ',' '\n' < "$tmpfile" \
                 | grep '"poster_path"' \
                 | head -1 \
                 | sed 's/.*"poster_path":"\([^"]*\)".*/\1/' || true)
        if [ -n "$poster" ] && [ "$poster" != "null" ]; then
            cover_url="${TMDB_IMG_BASE}${poster}"
            echo "TMDB: found poster $poster"
        else
            echo "TMDB: no poster in results"
        fi
    else
        echo "TMDB: request failed (check API key and network)"
    fi
fi

# -------------------------------------------------------------------------
# 2. TVMaze (fallback — no key, TV shows only)
# -------------------------------------------------------------------------
if [ -z "$cover_url" ]; then
    echo "Trying TVMaze..."
    url="${TVMAZE_SEARCH}?q=${query}"
    if wget -q --timeout=20 --no-check-certificate -O "$tmpfile" "$url" 2>/dev/null; then
        # TVMaze JSON: {...,"image":{"medium":"url","original":"url"},...}
        orig=$(tr ',' '\n' < "$tmpfile" \
               | grep '"original"' \
               | head -1 \
               | sed 's/.*"original":"\([^"]*\)".*/\1/' || true)
        if [ -n "$orig" ] && [ "$orig" != "null" ]; then
            cover_url="$orig"
            echo "TVMaze: found image"
        else
            echo "TVMaze: no image in results"
        fi
    else
        echo "TVMaze: request failed"
    fi
fi

# -------------------------------------------------------------------------
# Download cover image
# -------------------------------------------------------------------------
if [ -z "$cover_url" ]; then
    echo "ERROR: No cover art found for: $name" >&2
    echo "error" > /tmp/gvu_scrape_done
    exit 1
fi

echo "Downloading: $cover_url"
dest="${FOLDER}/cover.jpg"
if wget -q --timeout=30 --no-check-certificate -O "$tmpimg" "$cover_url" 2>/dev/null; then
    mv "$tmpimg" "$dest"
    echo "Saved: $dest"
    # Signal GVU that the scrape is complete
    echo "ok" > /tmp/gvu_scrape_done
    exit 0
else
    echo "ERROR: Download failed: $cover_url" >&2
    echo "error" > /tmp/gvu_scrape_done
    exit 1
fi
