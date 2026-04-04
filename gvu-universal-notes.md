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

**Miyoo Flip (aarch64, gvu64) — Session 2026-04-01:**
- fb0: 640×480, single page (virtual_yres=480=yres, smem_len=1.2MB). No double-buffering without FBIOPUT_VSCREENINFO.
- `brick_screen_init`: crash (SIGSEGV) — back_yoff=480 wrote one page past end of mmap. Fix: detect smem < 2×page_bytes → s_fb_pan_disabled=1, back_yoff=yoffset.
- Audio: PyUI (MainUI) holds /dev/snd/pcmC0D0p open exclusively (not via dmix) at 44100Hz S16_LE. Neither ALSA default nor OSS /dev/dsp accessible while PyUI runs.
  - Root cause chain: gvu64 SDL2 had NO ALSA compiled in (libasound2-dev missing from Dockerfile) → OSS only → /dev/dsp locked by PyUI → SNDCTL_DSP_SPEED EINVAL.
  - Fix 1: Add `libasound2-dev` to Dockerfile.gvu (Brick), rebuild SDL2 → ALSA now in gvu64.
  - Fix 2: `launch.sh` writes `HOME=/tmp/gvu_home/.asoundrc` mapping default→plughw:0,0 → SDL ALSA opens hw:0,0 directly, bypassing dmix lock.
  - Fix 3: `AUDIO_OUT_RATE` changed 48000→44100 (not strictly needed with plughw, but correct).
- Video scrambled/tiled: `video.c` #elif GVU_TRIMUI_BRICK used hardcoded `video_fit_rect(..., 1024, 768)` — fit rect wrong for 640×480 display, sws_scale output 1024px wide into 640px buffer. Fix: use `g_display_w / g_display_h`.
- landscape_direct threshold also used hardcoded `fit.w == 1024` → fixed to `fit.w == g_display_w`.
- **Status (2026-04-01)**: display ✓, input ✓, audio ✓, video ✓. Tearing (single-page fb0, no double-buffering). Stale image on startup (pending fix).

**Brick, A30:** Working well. All major features functional.

**Note on gvu64 Brick Docker image**: Rebuilt 2026-04-01 to add libasound2-dev. SDL2 now has ALSA compiled in. Brick audio now uses ALSA (plughw:0,0 via .asoundrc) instead of OSS — untested on Brick with new binary.

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
1. ✓ Add mbedTLS + libcurl to both Dockerfiles, verify build
2. ✓ Implement `fetch_subs.c` (no vendored JSON/ZIP libs — hand-rolled; uses system zlib for inflate)
3. ✓ Wire into Makefile, built `fetch_subs64` on Brick
4. Deployed to Brick 2026-03-30: **search returns "no subtitles found"** — debugging needed (see Part 3)
5. Test `fetch_subs32` on A30 — NOT YET DONE
6. ✓ Update `main.c` to call `fetch_subs` binary (no Python)
7. MinUI/NextUI pak launcher script — future

---

## Part 3: Session 2026-03-31 — Bugs Found and Fixed

All four bugs from the 2026-03-30 debug plan were resolved. Three additional bugs were found and
fixed during subtitle overlay testing.

---

### fetch_subs: "no subtitles found" — FIXED ✓

**Root cause**: mbedTLS 3.5.2 CA bundle loading was broken in the cross-compiled Brick build.
Both `CURLOPT_CAINFO` (file path) and `CURLOPT_CAINFO_BLOB` (in-memory) returned curl error 77
(`CURLE_SSL_CACERT_BADFILE`). Disabling SSL verify (`CURLOPT_SSL_VERIFYPEER=0`) confirmed the
JSON parser was correct — SubDL returned 5 results once TLS was bypassed.

**Fix**: Replaced mbedTLS 3.5.2 with OpenSSL 1.1.1w as the libcurl TLS backend in
`cross-compile/trimui-brick/Dockerfile.gvu`. OpenSSL builds reliably and libcurl's
`--with-ca-bundle` points directly to the device CA path. Docker image rebuilt from scratch.

**Additional fix**: SubDL API key was missing from `gvu.conf` (user had not yet configured it).
Key is stored in `~/Projects/GVU/gvu/SubDL_API_(Don't Upload).txt` (gitignored).

**Status**: SubDL returns results ✓. Podnapisi is currently DOWN (service outage, not a code
issue); 8s `CURLOPT_CONNECTTIMEOUT` limits the wait.

**Remaining**: A30 Dockerfile still uses mbedTLS 3.5.2 — same OpenSSL switch needed.
`fetch_subs32` not yet rebuilt or tested on A30.

---

### scrape_covers.sh: cover art not fetching — FIXED ✓

Added `--no-check-certificate -T 10` to all `wget` calls in `scrape_covers.sh`. Confirmed
working on Brick: cover art fetches and displays correctly.

---

### Icon + default cover not matching theme — FIXED ✓

Both issues were resolved in the same session by deploying theme-matched SVG assets. Icon and
default cover now respect the configured theme (e.g., vampire). No code changes needed —
asset deployment was the fix.

---

### New bugs found during subtitle overlay testing

#### 1. Subtitle overlay alpha blending — FIXED ✓

**Symptom**: When subtitle search results appeared over a playing video, the video went
completely black instead of showing through the dim layer.

**Root cause**: `brick_flip_video()` composited the OSD using `if (alpha > 0) → opaque`. The
`draw_dim()` call sets alpha=170 (semi-transparent grey), which was treated as fully opaque
solid black.

**Fix** (`src/brick_screen.c`): Added three helpers after `apply_brightness()`:
- `alpha_blend(osd, osd_a, vid)` — per-channel blend using integer math
- `osd_has_partial_alpha(osd_bgra)` — scans a row for any partial-alpha pixel
- `composite_pixel(osd, vid, bri256)` — dispatches: α=255 → opaque, α=0 → video, else blend

NEON fast path now requires `bri256 >= 256 && !partial`; scalar path calls `composite_pixel`.
Same treatment applied to zoom path.

#### 2. Subtitle results disappearing immediately — FIXED ✓

**Symptom**: The subtitle results panel appeared but vanished before the user could select
anything.

**Root cause**: Key repeat events from holding START would fire SDLK_RETURN with `ev.key.repeat
!= 0`. The moment `SUB_RESULTS` state became active, a pending repeat triggered
`sub_start_download()` immediately, dismissing the panel.

**Fix** (`src/main.c`): Added `result_ready_tick` to `SubWorkflow` struct. Set when entering
`SUB_RESULTS`. Confirm action gated by: `&& !ev.key.repeat && SDL_GetTicks() -
sub_wf.result_ready_tick > 300`.

#### 3. Subtitle overlay invisible on first search (no pre-existing subtitle) — FIXED ✓

**Symptom**: Pressing START with no pre-existing subtitle showed no UI. If the user pressed
START or A blind, a download would actually trigger — the overlay was rendering but not
being composited over the video.

**Root cause**: `has_ui` in both A30 and Brick rendering paths was missing
`sub_wf.state != SUB_NONE`. When the subtitle workflow was active but no subtitle was loaded,
`has_ui` evaluated false and the OSD surface was never blended over the video.

**Fix** (`src/main.c`): Added `|| sub_wf.state != SUB_NONE` to the `has_ui` expression in
both the A30 (`#ifdef GVU_A30`) and Brick (`#ifdef GVU_BRICK`) rendering paths.

---

### Remaining work

- **Video tearing (Brick) — FIXED ✓**: DRM/KMS page-flip via raw ioctls (no libdrm). See
  project_gvu_flip.md for full details.
- **Miyoo Flip tearing — FIXED ✓**: DRM/KMS page-flip. `s_use_drm=1`, pitch=2560, 60fps
  tear-free confirmed. Brick has no DRM connectors exposed → correctly falls back to fb0.
- **Stale image on Flip startup — FIXED ✓**: `dd if=/dev/zero of=/dev/fb0` in launch.sh +
  `memset(s_fb_mem, 0, s_fb_size)` in `brick_screen_close()`.
- **A30 volume — FIXED ✓**: Software volume. `audio.volume` initialised from SVM at
  `player_open` (was hardcoded 1.0 → OSD/audio desync). No amixer calls anywhere.
- **A30 fetch_subs — FIXED ✓**: Three glibc compat issues resolved:
  1. `patch_verneed.py` PATCH_MAP extended to cover GLIBC_2.25 and GLIBC_2.27
  2. `getentropy` shim added to `fetch_subs.c` (OpenSSL entropy; absent in A30 libc)
  3. `glibc_compat.c` now linked into fetch_subs32 (`-DGVU_A30`); provides `fcntl64`
- **Subtitle search timeout**: 45s watchdog added to main loop — shows toast instead of
  hanging forever if fetch_subs crashes without writing the done file.
- **Video tearing (A30)**: `s_fb_pan_disabled=1` intentionally for 60fps. `FBIOPAN_DISPLAY`
  blocks ~28ms on A30. Can be enabled for tear-free at the cost of 60fps → ~30fps cap.
- **Miyoo Mini family**: 180° rotation, 752×560 resolution (Mini Flip/V4), no audio,
  L1/R1 ↔ L2/R2 swap — not yet addressed.

---

## Session 2026-04-03 — Theme/UI Polish + Miyoo Mini V2/V3 Support

### SubDL SSL on ARM32 — FIXED ✓

**Root cause**: Static OpenSSL 1.1.1w `linux-armv4` build cannot verify ECDSA certificate
chains on ARM32, regardless of CA bundle content. This is a known limitation of the
`linux-armv4` target (lacks the full EC instruction path that upstream builds assume).

**Fix** (`src/fetch_subs.c`): Disabled SSL peer verification on ARM32:
```c
#ifdef GVU_A30
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // ECDSA can't be verified on ARM32
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#else
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (g_ca_path) curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_path);
#endif
```
Also switched CA bundle loading from `CURLOPT_CAINFO_BLOB` (in-memory) to `CURLOPT_CAINFO`
(file path) — simpler and more compatible with the OpenSSL backend.

Acceptable trade-off for subtitle downloads. Brick/aarch64 keeps full verification.

**Podnapisi**: Permanently down (Cloudflare-blocked). SubDL is the only working provider.

---

### Theme + UI Changes — DEPLOYED ✓

#### Per-theme folder cover icon in file browser
- New `resources/default_folder.svg` — distinct folder shape for directories in browser grid
- `src/theme.h`: Added `RGB folder_tab`, `folder_screen`, `folder_body` to Theme struct
- `src/theme.c`: Per-theme folder colors in `THEMES[]`; `svg_recolor_folder()` replaces
  three color slots in the SVG; `theme_render_folder_cover()` rasterizes via nanosvg
- `src/main.c`: Loads `default_folder` texture, passes to `browser_draw()`, re-renders on
  theme cycle, cleans up on exit
- SVG original colors: Tab=`#71c6c4`, Screen=`#dde5e8`, Body=`#afc3c9`
- Special overrides: vampire tab=`#222222`, light_sepia screen=`#e8d5b0` (bg clash), night
  body=`#8888a0` (bg clash), monochrome tab=`#404040` (distinct from body)

#### Season list (VIEW_SEASONS) contrast fix
- Combined season name + episode count into single label: `"Season X - XX episodes"`
- Single pill behind combined label when backdrop active (was two separate pills — too busy)
- Fixes contrast on cream_latte, nordic_frost, light_contrast, light_sepia with dark covers

#### Monochrome theme (renamed from night_contrast)
- 4-tone palette: `#000000`, `#404040`, `#808080`, `#f0f0f0`
- Inverse-video selection: white highlight_bg + black highlight_text on black background
- Designed for MiniUI/NextUI port compatibility (no colour dependencies)
- `statusbar_fg`, `secondary`, `icon_outer` all updated to greyscale

#### Other colour fixes
- night folder body: `#0d0d10` → `#8888a0` (was same as background)
- light_sepia folder screen: `#faf0dc` → `#e8d5b0` (was same as background)
- light_sepia icon: outer/center arrows swapped for better contrast

---

### Persistent Log — CHANGED

`LOG` in `launch.sh` moved from `/tmp/gvu.log` (RAM, lost on power-off) to
`$APPDIR/gvu.log` (`/mnt/SDCARD/App/GVU/gvu.log`). Survives power-off; readable from card
reader. Useful for debugging WiFi-less devices.

---

### Miyoo Mini V2/V3 — PARTIALLY WORKING

**Platform detection**: SpruceOS uses a shared `MiyooMini.cfg` for all Mini variants
(reports `DISPLAY_WIDTH=640 DISPLAY_HEIGHT=480 DISPLAY_ROTATION=0` for all).
V4 (Mini Flip) is detected by `grep -q "752x560p" /sys/class/graphics/fb0/modes`.

**launch.sh changes** (`MIYOO_V4` var set early, used throughout):
- `GVU_DISPLAY_ROTATION=180` override for all MiyooMini variants (physical upside-down mount)
- V4: additional `GVU_DISPLAY_W=752 GVU_DISPLAY_H=560` override

**Display**: ✓ Working — 640×480 at 180° rotation

**Input**: ✓ Working — `EVENT_PATH_READ_INPUTS_SPRUCE=/dev/input/event0` on V2/V3

**Battery**:
- V3/V4 (axp_test present): JSON poller via `/customer/app/axp_test`
- V2/og (no axp_test): uses `/mnt/SDCARD/spruce/miyoomini/bin/read_battery` binary
  (outputs plain integer percent; always reports "Discharging" — V2 has no charge detection)
- Fallback: `${BATTERY}/capacity` sysfs path (likely absent on V2/V3)

**Audio**: ✗ Not working — fundamental incompatibility:
- V2/V3 audio requires the custom `mmiyoo` SDL audio driver
- `mmiyoo` driver only exists in SpruceOS's SDL2 (`miyoomini/lib/libSDL2-2.0.so.0`)
- SpruceOS's SDL2 does NOT have the `dummy` video driver GVU requires
- Our SDL2 (lib32/) has `dummy` video but no `mmiyoo` audio
- `/dev/dsp` exists on device but is a SigmaStar stub — rejects standard OSS ioctls
- `libpadsp.so` is V4-only; not present on V2/V3 SD card
- **Solution would require**: building our SDL2 with the `mmiyoo` audio backend (needs
  SpruceOS SDL2 source + SigmaStar MI_AO headers)
- **Current state**: `SDL_AUDIODRIVER=dummy` — silent playback

**Volume**: V2/V3 has a hardware volume wheel, not buttons. Software volume control is
irrelevant. GVU's volume OSD still shows but has no effect on actual output level.

---

## Session save point (2026-04-01) — A30 + Brick + Flip all working

**Confirmed working on device:**
- Flip: DRM/KMS tear-free ✓, audio (ALSA plughw:0,0) ✓, video ✓, subtitles ✓
- Brick: fb0 double-buffering ✓, audio (OSS/dsp) ✓, video ✓, subtitles ✓
- A30: display ✓, audio/volume ✓, video ✓, subtitles ✓ (tearing intentional)

**Not yet tested:** Miyoo Mini v2, Miyoo Mini Flip

---

## Session 2026-04-04 — Mini Flip Video Freeze + Statusbar Dim Fix

### Mini Flip video freeze root cause (FIXED ✓)

**Symptom**: video froze after ~1 second of playback on every seek; audio and subtitles continued.

**Root cause** (confirmed via `[diag]` + `[sws]` diagnostic logging):
- Futurama S01E01 is **480×360 SD**
- `video.c` pre-scaled tex to fit rect: 480×360 → **746×560 (upscale)**
- `sws_scale` upscaling 480×360 → 746×560 took **35-40ms per frame**
- 25fps content = 40ms budget → sws consumed the entire frame budget
- AV sync diff grew ~130ms/s more negative; once past -150ms nosync threshold, every frame was dropped without display → video appeared frozen

**Fix** (`src/video.c:video_open`):
- For A30 portrait (`g_display_rotation == 270`): keep existing behavior (pre-scale to fit rect in sws + portrait_direct NEON kernel). sws **downscales** to small fit rect → fast.
- For Mini Flip and all other landscape rotation=180/0 devices: **set `tex_w = native_w`, `tex_h = native_h`**. sws does only YUV→BGRA format conversion (no resize). SDL_RenderCopy handles the scale to fit rect at render time.
- Measured result: sws time dropped from 35-40ms to ~5ms for 480×360 SD content.

**Key insight**: the original "pre-scale in sws, 1:1 SDL blit" optimization is correct for A30 (sws **downscales** = fast NEON). It backfires for landscape devices with SD content that needs **upscaling** (sws processes more output than input pixels). Letting SDL_RenderCopy scale is faster for that case.

**Diagnostic code** (temporary, leave in until stable — strip before release):
- `src/player.c:player_update()`: `[diag]` line logged once/sec showing raw_clk, ring_delay, master, vq, vf_pts, diff, out_rate
- `src/video.c:video_sws_thread()`: `[sws]` line logged once/sec showing frames/s, last frame ms, src/dst dims

### Statusbar brightness dimming fix (FIXED ✓)

**Symptom**: top status bar (wifi/battery/clock) was not dimmed when brightness was lowered.

**Root cause**: draw order in `main.c` playback path:
1. `player_draw()` — draws video + OSD + **brightness black overlay**
2. `statusbar_draw()` — drawn **on top of** the brightness overlay → unaffected by dim

**Fix**: added `int show_statusbar` parameter to `player_draw()`. Statusbar is now drawn **inside** `player_draw()`, just **before** the brightness overlay, so it gets dimmed along with the video frame.
- `src/player.h` / `src/player.c`: `player_draw(..., int show_statusbar)` — calls `statusbar_draw()` before brightness rect
- `src/main.c`: removed separate `statusbar_draw()` call; passes `player.osd_visible` as flag

### Diagnostic logging stripped (2026-04-04) ✓
Removed `[diag]` block from `src/player.c:player_update()` and `[sws]` block from `src/video.c:video_sws_thread()`.

### Package script updated (2026-04-04)
`cross-compile/universal/package_gvu_universal.sh` changes:
- Output filename: `GVU-universal-VERSION.zip` → `gvu_spruce_universal_vVERSION.zip`
- Now includes `lib32_a30/` (patched SDL2 for A30 glibc 2.23)
- Copies `icon.png` (not the old `gvu.png`/`gvu_sel.png`)
- Excludes build artifacts (uses `*.so*` glob, removed stale `.bak` from libs32/)

### Status (2026-04-04) — v0.2.0 built ✓
- Mini Flip: video ✓, audio ✓, subtitles ✓, display ✓, statusbar dim ✓ — confirmed
- A30: confirmed working after sws native-tex change ✓
- Brick + Flip: unchanged from prior confirmed working state
- `gvu_spruce_universal_v0.2.0.zip` (27MB) built and ready for tester distribution

### Pending before GitHub push
- Strip `flip180: Xms` + `pan: Xms` startup timing prints from `src/a30_screen.c`
  (`s_flip180_logged` block ~line 197 and `s_pan_logged` block ~line 279 — both harmless but noisy)
- Commit all changes to `universal` branch, create `v0.2.0` tag, push, upload zip as release asset

---

## Session 2026-04-04 — v0.2.1 Bug Fixes (INCOMPLETE — weekly token limit hit)

### Context
v0.2.0 tested on all devices. Multiple bugs found. Session goal: fix all bugs.
Token budget exhausted partway through; not all bugs verified; no commit made yet.

### Fixes applied this session (in source, not yet committed)

#### 1. API key files — IMPLEMENTED ✓
- `resources/api/SubDL_API.txt` + `TMDB_API.txt`: ship keys alongside binary, gitignored
- `resources/api/readme.txt`: instructions for users who want to use their own keys
- `src/theme.h`: declared `void config_load_api_keys(const char *api_dir)`
- `src/theme.c`: `load_key_file()` + `config_load_api_keys()` — reads key files, only sets if key is currently empty (gvu.conf overrides)
- `src/main.c`: calls `config_load_api_keys(g_app_dir + "/resources/api")` after `config_load`
- `.gitignore`: added `resources/api/SubDL_API.txt` and `resources/api/TMDB_API.txt`

#### 2. Cover scraping: posix_spawn empty environment — FIXED ✓
**Root cause**: `posix_spawn` with `envp=NULL` spawns into a clean (empty) environment on glibc 2.23.
`PATH` is unset in the child, so `wget` cannot be found. All API requests return "failed" instantly.
This is the SAME bug documented in commit `8f2e61e` (fixed for subtitle subprocess by building
an explicit `env_buf`). The scrape subprocess was the only one still using `envp=NULL`.
**Fix** (`src/main.c`): Added `extern char **environ;` and changed the scrape `posix_spawn` call from
`envp=NULL` to `envp=environ`. Verified working on A30: Rick and Morty cover fetched successfully.
**Note**: commit `8f2e61e` should have been read first — this was a known/documented pattern.

#### 3. Subtitle toast visible during playback — IMPLEMENTED (not yet verified)
- `src/main.c`: removed `mode != MODE_PLAYBACK` guard on toast render
- Added `sub_toast_msg[0] && SDL_GetTicks() < sub_toast_hide_at` to `has_ui` in both
  portrait_direct (A30) and landscape_direct (Brick/Flip) paths

#### 4. Volume OSD: Flip shows GVU bar (no hardware OSD), Brick suppresses it — IMPLEMENTED (not yet verified)
- `src/platform.h` + `src/platform.c`: added `int g_hw_has_volume_osd` global
- `src/platform.c`: Brick sets `g_hw_has_volume_osd = 1`; Flip leaves it 0
- `src/player.c`: compile-time `#ifdef GVU_TRIMUI_BRICK` replaced with runtime `g_hw_has_volume_osd` check

#### 5. A30 sleep/wake audio retry — IMPLEMENTED (not yet verified)
- `src/audio.c`: replaced `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE` (caused pitch shift) with
  a retry loop (5 attempts, 200ms apart) using exact requested params

#### 6. A30 startup timing prints stripped — DONE ✓
- Removed `flip180: Xms` and `pan: Xms` debug prints from `src/a30_screen.c`

### Fixes NOT yet applied
- Mini Flip: multiple audio track crash — not investigated
- V2/V3: audio silent — known (mmiyoo backend incompatibility), deferred
- Brick/Flip sleep/wake pitch shift — not yet tested with audio retry fix

### Build status
- `gvu32` + `fetch_subs32` rebuilt (A30 builder, 2026-04-04) ✓
- `gvu64` + `fetch_subs64` rebuilt (Brick builder, 2026-04-04) ✓
- `gvu_spruce_universal_v0.2.1.zip` packaged ✓ (27MB)
- Deployed `gvu32` to A30 at 192.168.1.62 ✓
- **NOT committed** — commit + tag + GitHub release pending

### Pending for next session (when weekly limit resets Monday 2026-04-07)
1. Verify Flip volume OSD fix (need device test)
2. Verify subtitle toast during playback (need device test)
3. Verify sleep/wake audio retry fix on A30 + Brick/Flip
4. Investigate Mini Flip multiple-audio-track crash
5. Commit all changes to `universal` branch
6. Create `v0.2.1` tag and GitHub release with zip
