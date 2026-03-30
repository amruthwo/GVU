# GVU Universal SpruceOS App — Plan

## Goal
Single zip installable on any SpruceOS device. No per-device builds required by the user.

## SpruceOS Platform Abstraction (Key Finding)
Every app launched via `standard_launch.sh` has these env vars pre-set from `/mnt/SDCARD/spruce/scripts/platform/<PLATFORM>.cfg`:

| Env Var | GVU Use |
|---|---|
| `$PLATFORM` | `Brick`, `A30`, `Flip`, `SmartPro`, `AnbernicRG28XX`, etc. |
| `$PLATFORM_ARCHITECTURE` | `armhf` vs `aarch64` → which binary to exec |
| `$DISPLAY_WIDTH` / `$DISPLAY_HEIGHT` | Screen dimensions at runtime |
| `$DISPLAY_ROTATION` | `270` = A30 (needs CCW rotation), `0` = landscape (no rotation) |
| `$BATTERY` | Battery sysfs dir, e.g. `/sys/class/power_supply/axp2202-battery` |
| `$DEVICE_PYTHON3_PATH` | Python binary for subtitle fetch |
| `$EVENT_PATH_READ_INPUTS_SPRUCE` | Input device path, e.g. `/dev/input/event3` |

Known platforms from `MEDIA/config.json`:
- **32-bit (armhf)**: `MIYOO_A30`, `MIYOO_MINI`, `MIYOO_MINI_V4`, `MIYOO_MINI_PLUS`, `MIYOO_MINI_FLIP`
- **64-bit (aarch64)**: `MIYOO_FLIP`, `TRIMUI_SMART_PRO`, `TRIMUI_SMART_PRO_S`, `TRIMUI_BRICK`, `ANBERNIC_RG34XXSP`, `ANBERNIC_RG28XX`, `ANBERNIC_RGXX640480`
- **Special**: `GKD_PIXEL2` (has its own mpv option)

Platform cfg files live at: `/mnt/SDCARD/spruce/scripts/platform/`
Device function scripts: `/mnt/SDCARD/spruce/scripts/platform/device_functions/<PLATFORM>.sh`

---

## Target Zip Structure
Model after `/mnt/SDCARD/Emu/MEDIA/` exactly:

```
GVU/
  bin32/gvu          ← armhf: A30, Miyoo Mini family
  bin64/gvu          ← aarch64: Brick, Flip, Smart Pro, Anbernic 64-bit
  lib32/             ← shared libs (libz.so.1 etc.)
  lib64/
  config.json        ← Emulator_32 / Emulator_64 device lists (copy MEDIA pattern)
  launch.sh          ← reads SpruceOS env vars, execs correct binary
  gvu.png
  gvu_sel.png
```

---

## config.json Shape
```json
{
  "label": "GVU",
  "launch": "../../spruce/scripts/emu/standard_launch.sh",
  "extlist": "mp4|mkv|avi|mov|flv|ts|mp3|m4a|wav|aac|flac|ogg|opus|mka",
  "menuOptions": {
    "Emulator_32": {
      "display": "Emulator",
      "devices": ["MIYOO_A30","MIYOO_MINI","MIYOO_MINI_V4","MIYOO_MINI_PLUS","MIYOO_MINI_FLIP"],
      "options": ["gvu"],
      "selected": "gvu"
    },
    "Emulator_64": {
      "display": "Emulator",
      "devices": ["MIYOO_FLIP","TRIMUI_SMART_PRO","TRIMUI_SMART_PRO_S","TRIMUI_BRICK",
                  "ANBERNIC_RG34XXSP","ANBERNIC_RG28XX","ANBERNIC_RGXX640480"],
      "options": ["gvu"],
      "selected": "gvu"
    }
  },
  "type": "mediaplayer",
  "default_emulator": "gvu"
}
```

---

## launch.sh Shape
```sh
#!/bin/sh
. /mnt/SDCARD/spruce/scripts/helperFunctions.sh   # gets all platform vars

BIN_DIR="$( [ "$PLATFORM_ARCHITECTURE" = "aarch64" ] && echo bin64 || echo bin32 )"

export GVU_DISPLAY_W="$DISPLAY_WIDTH"
export GVU_DISPLAY_H="$DISPLAY_HEIGHT"
export GVU_DISPLAY_ROTATION="$DISPLAY_ROTATION"
export GVU_BATTERY_PATH="${BATTERY}/capacity"
export GVU_PYTHON="$DEVICE_PYTHON3_PATH"
export GVU_INPUT_DEV="$EVENT_PATH_READ_INPUTS_SPRUCE"
export GVU_PLATFORM="$PLATFORM"

exec "$EMU_DIR/$BIN_DIR/gvu" "$1"
```

---

## C Code Changes Required

### 1. Replace hardcoded paths with getenv() + fallbacks
In `platform.c` / `statusbar.c` / `brick_screen.c` / `a30_screen.c`:
```c
// Battery — was PLATFORM_BRICK ifdef
const char *batt = getenv("GVU_BATTERY_PATH");
if (!batt) batt = "/sys/class/power_supply/axp2202-battery/capacity"; // Brick fallback

// Python — was sub_python_bin()
const char *python = getenv("GVU_PYTHON");
if (!python) python = "/mnt/SDCARD/spruce/flip/bin/python3"; // Brick fallback

// Input device — was hardcoded /dev/input/event3
const char *input = getenv("GVU_INPUT_DEV");
if (!input) input = "/dev/input/event3";
```

### 2. Runtime display geometry
Replace compile-time `BRICK_W/BRICK_H` / `PANEL_W/PANEL_H` constants with values read at startup from `GVU_DISPLAY_W` / `GVU_DISPLAY_H`. Store in a global `g_display_w`, `g_display_h`.

### 3. Runtime rotation decision
```c
// replaces #ifdef GVU_A30 rotation path
int rotation = atoi(getenv("GVU_DISPLAY_ROTATION") ?: "0");
// rotation==270 → use a30-style CCW blit
// rotation==0   → use brick-style direct blit
```

### 4. Platform detection
Extend `get_platform()` to map `$GVU_PLATFORM` string → enum:
```c
const char *p = getenv("GVU_PLATFORM");
if (p) {
    if (!strcmp(p, "Brick") || !strcmp(p, "Flip")) return PLATFORM_BRICK;
    if (!strcmp(p, "A30"))   return PLATFORM_A30;
    // SmartPro, Anbernic → new PLATFORM_TRIMUI_64 / PLATFORM_ANBERNIC entries
}
// fallback: /proc/cpuinfo detection (existing logic)
```

### 5. Keep compile-time split for binary size
`gvu32` still compiled with `GVU_A30` (or a new `GVU_32BIT`) — 32-bit devices all share the portrait/rotation pipeline.
`gvu64` compiled with `GVU_TRIMUI_BRICK` (or a new `GVU_64BIT`) — 64-bit devices all share the landscape-direct pipeline.
The per-device variation within each tier is purely runtime (dimensions, paths).

---

## Display Pipeline by Device Tier

### 32-bit devices
| Device | Resolution | Rotation | Notes |
|---|---|---|---|
| A30 | 480×640 fb0 | 270° CCW | portrait_direct at native res |
| Miyoo Mini | 640×480 | 0° | landscape already — simpler than A30 |
| Miyoo Mini Plus | 640×480 | 0° | same as Mini |
| Miyoo Mini Flip | 640×480 | 0° | clamshell, same pipeline |

A30's rotation blit is the odd one out — all others are native landscape. `$DISPLAY_ROTATION` disambiguates.

### 64-bit devices
| Device | Resolution | Notes |
|---|---|---|
| Brick | 1024×768 | Confirmed working |
| Flip | 1024×768 | Same SoC as Brick, low risk |
| Smart Pro | TBD | Likely landscape, similar pipeline |
| Smart Pro S | TBD | Same |
| Anbernic RG28XX | 640×480 | Landscape, smaller — needs display size runtime |
| Anbernic RG34XXSP | 720×480? | TBD |
| Anbernic RGXX640480 | 640×480 | Landscape |

landscape_direct threshold: `if (fit.w == display_w)` — already uses display width, just needs `display_w` to be runtime not compile-time.

---

## First Universal Build — Test Results (2026-03-30)

Deployed to Brick, A30, Miyoo Flip, Miyoo Mini Flip (V4 screen), Miyoo Mini V3.

### All-device bugs (pending fixes)

| Bug | Root cause | Fix |
|---|---|---|
| Missing GVU icon | `icon.png` not deployed | Copy `icon.png` to app dir |
| Wrong theme / teal default cover | `gvu.conf` not deployed | Deploy `gvu.conf` to `$APPDIR` |
| Subtitle search stuck / fails | `spruce/bin/curl` is wrong-arch binary returning HTTP 000; only SpruceOS `wget` supports HTTPS | Rewrite `fetch_subtitles.py` to use `wget` instead of `curl` |
| Cover scraping stuck | Same wget/curl issue (TVMaze/TMDB HTTPS); also `wget` BusyBox may lack `-T` timeout → hangs | Add `-T 10` timeout to wget calls in `scrape_covers.sh`; confirm wget works |
| posix_spawn failure leaves subtitle/scrape in stuck state | No sentinel written if Python binary not found | Write error sentinel immediately on spawn failure |

**Key network finding on Brick:**
- `/mnt/SDCARD/spruce/bin/curl` reports `x86_64-pc-none` arch — returns HTTP 000 for HTTPS (broken on ARM devices)
- `wget --no-check-certificate` exits 0 for HTTPS — **wget is the correct tool to use**
- Fix: replace `CURL = "/mnt/SDCARD/spruce/bin/curl"` in `fetch_subtitles.py` with wget subprocess calls
- Brick Python: `DEVICE_PYTHON3_PATH=/mnt/SDCARD/spruce/flip/bin/python3.10` (note `.10` suffix)
- Brick PYTHONHOME derivation works: `dirname(dirname(g_python_bin))` → `/mnt/SDCARD/spruce/flip` ✓

### Device-specific bugs (investigate after sdcard swap)

**Miyoo Mini Flip / Mini V4 (armhf, gvu32):**
- Display upside-down — rotation is 180° not 0°. Need to detect and add a 180° blit path.
- Resolution: 752×560 (not 640×480). SpruceOS sets `DISPLAY_WIDTH=752 DISPLAY_HEIGHT=560`; gvu32 needs to handle non-640 landscape sizes.
- Battery indicator not working — different sysfs path for Mini family
- Audio error: "ALSA: Couldn't open audio device" — different ALSA card/device name on Mini
- R1/L1 swapped with R2/L2 — Mini evdev key codes differ from A30

**Miyoo Mini V3 (armhf, gvu32):**
- Display upside-down (same 180° rotation issue)
- Battery indicator not working
- Audio error: "ALSA: Couldn't open audio device"
- R1/L1 swapped with R2/L2

**Miyoo Flip (aarch64, gvu64):**
- Audio error: "SDL_OpenAudioDevice: Couldn't set audio frequency" — ALSA sample rate mismatch
- Everything else (display, input, wifi, battery) working correctly

**Brick, A30:** Working well. All major features functional.

---

## What We Can Ship Without Testing
- **Flip**: identical hardware to Brick. Safe.
- **Smart Pro / S**: 64-bit landscape. Low risk with runtime display sizing.
- **Miyoo Mini family**: 32-bit landscape (no rotation needed). Low risk.
- **Anbernic 64-bit**: Unknown ALSA/input quirks. Ship, let community report issues.

## Unknowns / Risks
- **Miyoo Mini ALSA**: may differ from A30. Check if SpruceOS `asound-setup.sh` normalizes it before launch (likely yes).
- **Anbernic button mapping**: evdev codes may differ. May need runtime keymap via `B_A`/`B_B` env vars from platform cfg.
- **Smart Pro display**: need to check SmartPro.cfg for dimensions and rotation.

---

## Implementation Order
1. Convert battery/python/input paths to `getenv()` with fallbacks — low risk, easy
2. Make `screen_init()` use runtime display dimensions
3. Add `GVU_DISPLAY_ROTATION` dispatch (landscape vs portrait blit path)
4. Build universal zip, test on A30 + Brick
5. Extend `get_platform()` for new device strings
6. Community testing for Anbernic / Smart Pro / Mini

---

## Part 2: Python-Free Subtitle Fetcher (enables MinUI / NextUI)

### Why
`fetch_subtitles.py` requires SpruceOS Python (`/mnt/SDCARD/spruce/…/python3`).
MinUI and NextUI have no Python — subtitles are completely broken there.
Replace it with a compiled C binary that is fully self-contained.

### What the Python Script Does
Two modes, identical CLI — **no changes needed in the GVU C code that calls it**,
only the path to the binary changes:

**`search <video_path> <subdl_key> <lang>`**
1. Parse filename → `(title, season, episode)` by stripping release tags and finding SxxExx patterns
2. Query SubDL REST API (primary, free key from subdl.com)
3. Fall back to Podnapisi REST API (no key needed)
4. Write results to `/tmp/gvu_sub_results.txt`: `provider|download_key|display_name|lang|downloads|hi`
5. Write `ok` or `error: …` to `/tmp/gvu_sub_done`

**`download <provider> <download_key> <srt_dest>`**
1. Download subtitle zip from SubDL or Podnapisi
2. Extract best-matching .srt (prefers SxxExx match; refuses season-pack ambiguity)
3. Write .srt to `<srt_dest>`, write sentinel

Note: the Python script already shells out to `curl` for all HTTPS — the
Python-specific work is only JSON parsing, zip extraction, and string manipulation.

### New Binary: `fetch_subs`
- Single source file: `src/fetch_subs.c` (~700 lines target)
- Built as `build/fetch_subs32` (armhf) and `build/fetch_subs64` (aarch64)
- Shipped alongside `gvu32`/`gvu64` in the zip

### Static Dependencies to Add to Both Dockerfiles

| Library | Purpose | How to add |
|---|---|---|
| **libcurl** + **mbedTLS** | HTTPS GET + file download | Build from source in Dockerfile |
| **jsmn.h** | JSON tokenizer, header-only | Drop into `src/` |
| **miniz.h** | ZIP extraction, single-file | Drop into `src/` |

jsmn and miniz are vendored directly — no build system changes beyond adding
libcurl/mbedTLS to the Dockerfiles.

#### Dockerfile addition (both a30 and brick)
```dockerfile
# mbedTLS (TLS backend for curl)
RUN wget https://github.com/Mbed-TLS/mbedtls/releases/download/v3.5.2/mbedtls-3.5.2.tar.bz2 && \
    tar xf mbedtls-3.5.2.tar.bz2 && cd mbedtls-3.5.2 && \
    cmake -DENABLE_TESTING=Off -DCMAKE_INSTALL_PREFIX=/sysroot && make && make install

# libcurl (static, mbedTLS backend, minimal feature set)
RUN wget https://curl.se/download/curl-8.7.1.tar.gz && tar xf curl-8.7.1.tar.gz && \
    cd curl-8.7.1 && \
    ./configure --host=$CROSS_TRIPLE --prefix=/sysroot --disable-shared \
      --with-mbedtls=/sysroot --without-ssl \
      --disable-ftp --disable-ldap --disable-telnet --disable-dict \
      --disable-file --disable-rtsp --disable-pop3 --disable-imap \
      --disable-smtp --disable-gopher && \
    make && make install
```

### CA Bundle
Ship `resources/cacert.pem` (Mozilla bundle, ~250KB) in the zip.
Devices without a system CA store (MinUI, NextUI) need this for HTTPS.
`fetch_subs` reads `GVU_CACERT_PATH` env var (set by launch.sh):
```c
const char *ca = getenv("GVU_CACERT_PATH");
if (ca) curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
```

### GVU main.c change
Find `fetch_subs` binary relative to `app_dir` (dirname of `/proc/self/exe`):
```c
// Old:
snprintf(cmd, sizeof(cmd), "%s %s search ...", python_bin, py_script, ...);

// New:
char fetch_subs[512];
snprintf(fetch_subs, sizeof(fetch_subs), "%s/fetch_subs%s",
         app_dir, ARCH_SUFFIX);  /* "32" or "64" */
snprintf(cmd, sizeof(cmd), "%s search ...", fetch_subs, ...);
```

### MinUI / NextUI Pak Structure (once complete)
```
.MinUI/Media Player.pak/
  bin32/gvu   bin32/fetch_subs32
  bin64/gvu   bin64/fetch_subs64
  resources/cacert.pem
  resources/font.ttf   resources/font_small.ttf
  launch.sh            config.txt
```
Fully self-contained. No Python, no SpruceOS scripts, no external tools.
`launch.sh` uses `uname -m` to select arch (no `$PLATFORM` vars available on MinUI/NextUI).

### Implementation Order (fetch_subs, ~2-3 sessions)
1. Add mbedTLS + libcurl to both Dockerfiles, verify build
2. Vendor `jsmn.h` and `miniz.h` into `src/`
3. Implement `fetch_subs.c`: HTTP helpers → filename parser → SubDL → Podnapisi → zip extractor
4. Wire into Makefile, test `fetch_subs64` on Brick
5. Test `fetch_subs32` on A30
6. Update `main.c` to use `fetch_subs` binary instead of Python script
7. MinUI/NextUI pak launcher script
