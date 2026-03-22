# GVU — Video Player for SpruceOS Devices: Handoff Document

> **Status as of 2026-03-21:** A30 build working on-device. Video playback, file browser, OSD,
> seek, A/V sync, audio, theme cycling, history, resume, help overlay, volume keys, screen
> tearing, dynamic UI scaling, themed launcher icon, B-to-exit, D-pad hold-to-scroll,
> Plex-style 3-level media browser (shows → seasons → files), cover art scraping with live
> season art refresh (Y button, TMDB primary / TVMaze fallback), independent per-view layout
> preferences (persisted to `gvu.conf`), Clear History (SELECT), and status bar (clock /
> title / WiFi + battery — shown in browser and during OSD) all confirmed working.
> **Sleep/wake audio recovery is partially working but not yet solved** — see the sleep/wake
> section below for full analysis.

---

## Project Goal

Build **GVU**, a standalone video player for all SpruceOS v4.0.0 supported devices. It should
feel like a native app — file browser, cover art, clean fullscreen playback, OSD — not a wrapper
around an existing player. Built from scratch in C around FFmpeg + SDL2 (not SDL1), because no
existing open-source handheld player has A/V sync and a frame-accurate seek model that works
cleanly on these underpowered ARM devices.

---

## Why Not Fork GMU?

GMU's decoder plugin interface returns PCM audio buffers. There is no frame type, no frame
timing, no A/V sync concept anywhere in the codebase. The SDL frontend is hardcoded around a
music player HUD. Adapting it to video would mean gutting everything except the file browser and
key mapper — at which point you're writing a new app. Better to start clean.

---

## Supported Devices (SpruceOS v4.0.0)

| Device | `/proc/cpuinfo` token | CPU | Resolution | Display Notes |
|---|---|---|---|---|
| Miyoo A30 | `sun8i` | Allwinner H700, Cortex-A53 | 640×480 logical | **Special**: fb0 is **480×640 portrait**, ARGB8888. App renders 640×480 landscape surface; `a30_flip()` rotates 90° CCW into fb0. `SDL_VIDEODRIVER=dummy` required. |
| Miyoo Flip (V1/V2) | `0xd05` | RK3566, Cortex-A55, 1.8GHz | 640×480 | SDL2, normal landscape |
| GKD Pixel2 | `0xd05` | RK3566, Cortex-A55 | 640×480 | SDL2, normal landscape |
| Miyoo Mini Flip | (falls through to MiyooMini) | Cortex-A7, 1.2GHz | 750×560 | SDL2 |
| TrimUI Brick (+ Hammer) | `TG3040` | Allwinner A133P, Cortex-A53, 1.8GHz | 1024×768 | SDL2, 4:3 |
| TrimUI Smart Pro | `TG5040` | Allwinner A133P, Cortex-A53, 1.8GHz | 1280×720 | SDL2, 16:9 |
| TrimUI Smart Pro S | `TG5050` | Upgraded, Mali-G57 GPU | 1280×720 | SDL2, same as Smart Pro |

**Architecture split:** A30 is 32-bit ARMv7 hard-float. All other supported devices are 64-bit
ARM. A single binary cannot serve both — the universal package ships two binaries.

---

## Actual Source Layout (as built)

```
gvu/
├── src/
│   ├── main.c            # Entry point, SDL2 event loop, all mode dispatch
│   ├── platform.c/.h     # /proc/cpuinfo detection → PLATFORM enum
│   ├── decoder.c/.h      # FFmpeg: demux thread, packet queues, seek
│   ├── video.c/.h        # Video decode thread, frame queue, swscale, blit
│   ├── audio.c/.h        # Audio decode thread, resample, SDL audio callback
│   ├── player.c/.h       # High-level player: open/play/pause/seek/zoom/volume
│   ├── browser.c/.h      # File browser: folder grid, season list, file list, cover cache
│   ├── filebrowser.c/.h  # Library scan: 3-level show/season/file detection, MediaLibrary
│   ├── history.c/.h      # Watch history page (in-progress + completed)
│   ├── hintbar.c/.h      # Button hint bar: glyphs, pills, circle buttons
│   ├── overlay.c/.h      # Help overlay, tutorial slides, resume prompt, up-next, error
│   ├── theme.c/.h        # Color themes, SVG recolor, nanosvg rasterizer
│   ├── resume.c/.h       # Resume position save/load, completed-file tracking, clear-all
│   ├── statusbar.c/.h    # Top status bar: clock (left), "GVU" (center), WiFi+battery (right)
│   ├── a30_screen.c/.h   # A30: fb0 mmap + 90° CCW rotation; evdev input injection
│   └── glibc_compat.c    # fcntl64@GLIBC_2.28 shim for glibc 2.23 compatibility
├── resources/
│   ├── fonts/DejaVuSans.ttf
│   ├── default_cover.svg
│   ├── default_cover.png
│   ├── scrape_covers.sh  # On-device cover art fetch script (TMDB + TVMaze)
│   └── clear_covers.sh   # Manual utility: remove all cover art from media roots
├── cross-compile/miyoo-a30/
│   ├── Dockerfile.gvu           # Builds SDL2/FFmpeg from source for ARMv7 static linking
│   ├── build_inside_docker.sh   # Called inside container: make miyoo-a30-build + patch
│   ├── patch_verneed.py         # Patches GLIBC_2.28/2.29 → GLIBC_2.4 in ELF VERNEED
│   ├── package_gvu_a30.sh       # Assembles SpruceOS zip
│   └── gvu_base/
│       ├── launch.sh
│       └── config.json
├── Makefile
└── gvu.conf         # Theme, first-run flag, optional tmdb_key
```

---

## Threading Model

```
[Main Thread]
  a30_poll_events() [A30 only] → inject SDL events from /dev/input/event3
  SDL2 event loop → key dispatch → player / browser commands
  player_update() → A/V sync, upload YUV texture
  Render (browser_draw / player_draw / overlays)
  SDL_RenderPresent() → a30_flip(a30_surf) [A30 only]

[Demux Thread]  (decoder.c)
  av_read_frame() → route packets to video_queue or audio_queue
  Handles seek: av_seek_frame, flush queues, send sentinel packets

[Video Decode Thread]  (video.c)
  avcodec_receive_frame() → swscale YUV→YUV420P → fq_push(frame_queue)
  On sentinel: avcodec_flush_buffers, flush frame_queue

[Audio Decode Thread]  (audio.c)
  avcodec_receive_frame() → swresample → ring buffer
  SDL audio callback drains ring buffer (hardware thread)
```

---

## A30 Display Path

**Never use SDL's fbcon driver on the A30.** It calls `FBIOPUT_VSCREENINFO` which permanently
disables the sunxi display controller overlay until reboot. Set `SDL_VIDEODRIVER=dummy` before
`SDL_Init()` (in `launch.sh`). SDL renders into a CPU-side `SDL_Surface`; `a30_flip()` writes
that surface to fb0 directly.

**Do NOT call `FBIOPUT_VSCREENINFO`.** It resets the display mode and permanently breaks the
overlay layer until reboot. `FBIOPAN_DISPLAY` (pan only, no mode change) is safe and is used
by `a30_flip()` for double-buffering during normal operation. After sleep/wake,
`a30_screen_wake()` switches `a30_flip()` to a back→front `memcpy` path permanently, avoiding
`FBIOPAN_DISPLAY` which can block for 100+ seconds after the display controller reinitialises.

### fb0 facts (confirmed on-device)

| Property | Value |
|----------|-------|
| Resolution | 480×640 portrait |
| Pixel format | ARGB8888, no R/B swap needed |
| `line_length` | 1920 bytes (stride = 480 pixels) |
| `smem_len` | 3,686,400 bytes (3 pages: 480×640×4×3) |
| `yoffset` when PyUI launches app | **640** (page 1 is displayed, not page 0) |
| Overlay transparency | alpha=0 → transparent; **must force alpha=0xFF on every pixel** |

### Rotation formula

GVU renders a 640×480 landscape surface. fb0 expects 480×640 portrait.
90° CCW rotation: app surface pixel `(x, y)` → fb0 pixel index `(GVU_W-1-x) * stride + y`
i.e. `(639-x) * 480 + y`.

### Actual `a30_screen.c` API (SDL2, not SDL1)

```c
int  a30_screen_init(void);          // open fb0, mmap, cache yoffset+stride, open event3
void a30_flip(SDL_Surface *surface); // rotate 90° CCW, force alpha, write to fb page
void a30_poll_events(void);          // drain /dev/input/event3, inject SDL_KEYDOWN/UP events
void a30_screen_wake(void);          // call on sleep/wake: switches a30_flip to memcpy path
void a30_screen_close(void);         // munmap, close fds
```

All functions guarded by `#ifdef GVU_A30`. The surface passed to `a30_flip` must be
640×480 ARGB8888 (masks: `0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000`).

**`a30_screen_wake()` detail:** `FBIOPAN_DISPLAY` blocks waiting for vsync, which can stall
for 100–700+ seconds after the display controller reinitialises post-wake. Calling
`a30_screen_wake()` sets a permanent bool that switches `a30_flip()` from the
`FBIOPAN_DISPLAY` double-buffer path to a direct back→front `memcpy` path. The flag is never
cleared — after the first wake the memcpy path is used for the remainder of the session.
Called in `main.c` alongside `audio_wake()` inside the gap-detection block.

### SDL2 setup for A30

```c
// In main.c, #ifdef GVU_A30 branch:
// win_w = 640, win_h = 480 (runtime variables, not macros)
a30_screen_init();
SDL_Surface *a30_surf = SDL_CreateRGBSurface(0, win_w, win_h, 32,
    0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
renderer = SDL_CreateSoftwareRenderer(a30_surf);

// Per-frame:
a30_poll_events();       // before SDL_PollEvent
// ... render to renderer ...
SDL_RenderPresent(renderer);
a30_flip(a30_surf);      // after SDL_RenderPresent
```

---

## A30 Input Path (evdev, confirmed mappings)

`/dev/input/event3`, `O_RDONLY | O_NONBLOCK`. Linux keycode → SDL2 keysym mapping:

| Button | Linux keycode | SDL2 keysym |
|--------|--------------|-------------|
| A | KEY_SPACE (57) | SDLK_SPACE |
| B | KEY_LEFTCTRL (29) | SDLK_LCTRL |
| X | KEY_LEFTSHIFT (42) | SDLK_LSHIFT |
| Y | KEY_LEFTALT (56) | SDLK_LALT |
| L1 | KEY_TAB (15) | SDLK_PAGEUP |
| R1 | KEY_BACKSPACE (14) | SDLK_PAGEDOWN |
| L2 | KEY_E (18) | SDLK_COMMA |
| R2 | KEY_T (20) | SDLK_PERIOD |
| SELECT | KEY_RIGHTCTRL (97) | SDLK_RCTRL |
| START | KEY_ENTER (28) | SDLK_RETURN |
| MENU | KEY_ESC (1) | SDLK_ESCAPE |
| D-pad UP | KEY_UP (103) | SDLK_UP |
| D-pad DOWN | KEY_DOWN (108) | SDLK_DOWN |
| D-pad LEFT | KEY_LEFT (105) | SDLK_LEFT |
| D-pad RIGHT | KEY_RIGHT (106) | SDLK_RIGHT |

ev.value: 1=down, 0=up, 2=repeat. `gpio-keys-polled` never generates value=2 (no EV_REP
capability). Software key-repeat is implemented in `a30_poll_events()`: 300ms initial delay,
then one synthetic SDL_KEYDOWN (repeat=1) every 80ms while held. Only navigation keys repeat
(UP/DOWN/LEFT/RIGHT). All other buttons fire once per physical press.

---

## Button Actions (A30, as implemented)

### During Playback

| Button | SDL keysym | Action |
|--------|-----------|--------|
| A | SDLK_SPACE | Pause / Resume |
| START | SDLK_RETURN | Pause / Resume |
| B | SDLK_LCTRL | Stop, save position, return to browser |
| D-pad LEFT | SDLK_LEFT | Seek −10 s |
| D-pad RIGHT | SDLK_RIGHT | Seek +10 s |
| L1 | SDLK_PAGEUP | Seek −60 s |
| R1 | SDLK_PAGEDOWN | Seek +60 s |
| L2 | SDLK_COMMA | Previous file in current season (or folder if flat) |
| R2 | SDLK_PERIOD | Next file in current season (or folder if flat) |
| Y | SDLK_LALT | Cycle zoom mode (Fit → Wide → Fill) |
| X | SDLK_LSHIFT | Cycle audio track |
| SELECT | SDLK_RCTRL | Mute / unmute |
| D-pad UP | SDLK_UP | Brightness up |
| D-pad DOWN | SDLK_DOWN | Brightness down |
| MENU (tap) | SDLK_ESCAPE | Stop, return to browser |
| MENU (hold 1s) | SDLK_ESCAPE | Open controls reference overlay |

Volume UP/DOWN keys (KEY_VOLUMEUP=115, KEY_VOLUMEDOWN=114) come through `/dev/input/event3` and
are mapped to SDLK_EQUALS / SDLK_MINUS → ±10% software volume. SpruceOS may also adjust ALSA
system volume on these presses — that's fine, both layers work independently.

### In the File Browser — Folder Grid (top level)

| Button | Action |
|--------|--------|
| A / START | Open folder; if folder is a multi-season show, goes to Season List |
| B | First press: show "press B again to exit" toast. Second press within 3s: quit to SpruceOS |
| D-pad | Navigate |
| SELECT | Cycle tile layout (Large → Small → List) |
| X | Open History page |
| R1 | Cycle color theme |
| Y | Fetch cover art for selected folder (TMDB / TVMaze — see Cover Art Scraping) |
| MENU (tap) | Quit to SpruceOS (functional but not shown in hint bar — slow/inconsistent on A30) |
| MENU (hold 1s) | Controls reference overlay |

### In the File Browser — Season List (multi-season shows only)

| Button | Action |
|--------|--------|
| A / START | Open selected season → File List |
| B | Back to Folder Grid |
| D-pad | Navigate seasons (UP/DOWN = row, LEFT/RIGHT = item within row) |
| SELECT | Cycle tile layout (Large → Small → List) — **independent of folder grid layout** |
| X | Open History page |
| R1 | Cycle color theme |
| MENU (tap) | Quit to SpruceOS |
| MENU (hold 1s) | Controls reference overlay |

### In the File Browser — File List (inside a folder or season)

| Button | Action |
|--------|--------|
| A / START | Play selected file (shows resume prompt if applicable) |
| B | Back to Season List (if show); back to Folder Grid (if flat folder) |
| D-pad UP/DOWN | Navigate |
| L2 / R2 | Jump to previous / next season (if show); previous / next folder (if flat) |
| X | Open History page |
| SELECT | Cycle tile layout |
| MENU (tap) | Quit to SpruceOS |
| MENU (hold 1s) | Controls reference overlay |

### In the History Page

| Button | Action |
|--------|--------|
| A / START | Play selected entry (shows resume prompt if a saved position exists) |
| B / X | Back to browser |
| D-pad UP/DOWN | Navigate entries |
| SELECT | Clear all history (truncates `resume.dat` + `history.dat`, reloads in place) |
| MENU (hold 1s) | Controls reference overlay |

### Resume Prompt

| Button | Action |
|--------|--------|
| A | Resume from saved position |
| X | Play from beginning |
| B | Cancel |

### Up Next Countdown

| Button | Action |
|--------|--------|
| A | Play next immediately |
| B | Cancel, return to browser |

---

## A/V Sync Model

Audio clock is master. `audio_get_clock()` returns current playback position in seconds
(updated by the SDL audio callback). `player_update()` (called every frame) pops video frames
from the frame queue when `vf.pts - master_clock <= AV_SYNC_THRESHOLD_SEC (40ms)`. Frames more
than 10s behind are dropped without display.

### Seek implementation — critical notes

`player_seek()` and `player_seek_to()` both call `video_flush_frames()` **before**
`demux_request_seek()`. This is essential: without the flush, old pre-seek frames (with PTS
far ahead of the new clock) sit in the frame queue passing the "too early" check, blocking
`fq_push` in the video decode thread. The result is video permanently frozen while audio
continues — looks like video is missing but is actually deadlocked. The flush unblocks the
decode thread so it can reach and process the seek sentinel.

`player_cycle_audio()` does the same flush for the same reason.

`demux_request_seek()` sets an interrupt flag on both packet queues so the demux thread wakes
immediately from any blocked `pq_enqueue` call and processes the seek within one loop
iteration rather than waiting for the queues to drain naturally.

---

## A30 Build System

### Docker image: `gvu-a30`

Built from `cross-compile/miyoo-a30/Dockerfile.gvu`. Based on Debian Bullseye. Builds all
dependencies from source for `arm-linux-gnueabihf`:

| Library | Version | Notes |
|---------|---------|-------|
| SDL2 | 2.26.5 | `--enable-static --disable-shared`, ALSA enabled (dlopen at runtime) |
| libjpeg-turbo | 2.1.5.1 | Static, for SDL2_image |
| libpng | 1.6.39 | Static |
| FreeType | 2.13.0 | Static, `--without-harfbuzz` |
| SDL2_image | 2.6.3 | Static, JPEG+PNG only |
| SDL2_ttf | 2.20.2 | Static |
| FFmpeg | 5.1.6 | Static, `--disable-everything` + selective decoders/demuxers |

Output: `/opt/a30/` with static `.a` libraries and pkg-config files.

### Build commands

```bash
# Build Docker image (one-time, ~15 min):
make miyoo-a30-docker

# Cross-compile + patch (working form — build_inside_docker.sh has a path issue via make):
docker run --rm -v "$(pwd):/gvu:z" gvu-a30 \
    bash -c "make -C /gvu miyoo-a30-build && cp /gvu/gvu32 /gvu/build/gvu32"
docker run --rm -v "$(pwd):/gvu:z" gvu-a30 \
    python3 /gvu/cross-compile/miyoo-a30/patch_verneed.py /gvu/build/gvu32

# Deploy to device:
scp build/gvu32 spruce@192.168.1.62:/mnt/SDCARD/App/GVU/

# Output: build/gvu32 (patched), build/libs32/libz.so.1
```

**IMPORTANT:** Always run `patch_verneed.py` after every build. Without it, gvu32 requires
`GLIBC_2.28`/`GLIBC_2.29` which the A30's glibc 2.23 doesn't have — the binary exits
immediately with "version not found" errors. The patch step is fast (~1 second) and safe to
run multiple times.

`build_inside_docker.sh` runs `make miyoo-a30-build` then calls `patch_verneed.py` on the
result and collects shared library dependencies into `build/libs32/`. It is the canonical
all-in-one build path, but currently fails when invoked via `make miyoo-a30-docker` due to
a SELinux/podman volume mount issue with relative script paths. Use the two-step form above
until that is fixed.

### glibc compatibility

A30 ships glibc **2.23**. Debian Bullseye cross-compiler targets glibc 2.31, which emits
calls to `fcntl64@GLIBC_2.28` and `pow@GLIBC_2.29`.

Two-pronged fix:
1. **`src/glibc_compat.c`** — provides a local `fcntl64()` that forwards to `fcntl@GLIBC_2.4`.
   This eliminates the dynamic NEEDED reference entirely so the linker never looks for it.
2. **`patch_verneed.py`** — patches the ELF VERNEED section in the compiled binary to downgrade
   `GLIBC_2.28 → GLIBC_2.4` and `GLIBC_2.29 → GLIBC_2.4` for any remaining versioned symbols.

After patching, `gvu32` requires only: `libc.so.6`, `libm.so.6`, `libpthread.so.0`,
`libdl.so.2`, `libz.so.1`. Maximum glibc version required: **GLIBC_2.7**.

### Runtime dependencies

Only `libz.so.1` is not present on a stock A30. It's bundled in `libs32/` (74KB, from Debian
Bullseye armhf, requires only GLIBC_2.4 — safe). Everything else is statically linked.
`libs32/` does **not** need SDL2, FFmpeg, or any other heavy libraries.

---

## SpruceOS Integration

### Current app folder structure (A30)

```
/mnt/SDCARD/App/GVU/
├── gvu32            # 32-bit ARMv7 binary
├── launch.sh        # SpruceOS launcher
├── config.json      # SpruceOS metadata
├── icon.png         # Launcher tile icon — generated/updated by GVU on startup + theme cycle
├── libs32/
│   └── libz.so.1    # Only bundled runtime dep
├── resources/
│   ├── fonts/DejaVuSans.ttf
│   ├── default_cover.svg
│   ├── default_cover.png
│   ├── scrape_covers.sh  # Cover art fetch script (chmod +x, called via posix_spawn)
│   └── clear_covers.sh   # Manual utility: remove cover art (runnable from DinguxCommander)
└── gvu.conf         # Theme, first-run flag, optional tmdb_key
```

State (resume positions, history) lives at the device level — currently stored as flat files
in the app directory (not yet moved to `/mnt/SDCARD/Saves/CurrentProfile/states/GVU/`).

### `launch.sh` (current A30 version)

```sh
#!/bin/sh
APPDIR=/mnt/SDCARD/App/GVU
LOG=/tmp/gvu.log

echo "launch.sh start" > "$LOG"
. /mnt/SDCARD/spruce/scripts/helperFunctions.sh
echo "helperFunctions sourced" >> "$LOG"

export LD_LIBRARY_PATH="$APPDIR/libs32:/mnt/SDCARD/spruce/bin:$LD_LIBRARY_PATH"
export SDL_VIDEODRIVER=dummy

cd "$APPDIR"
echo "cwd: $(pwd)" >> "$LOG"
sleep 0.5
echo "launching gvu32" >> "$LOG"
"$APPDIR/gvu32" >> "$LOG" 2>&1
echo "gvu32 exited: $?" >> "$LOG"
```

The log at `/tmp/gvu.log` is invaluable for debugging crashes. The `sleep 0.5` gives
SpruceOS time to finish its launch animation before gvu32 writes to fb0.

### Universal `launch.sh` (planned, for multi-device release)

```sh
#!/bin/sh
. /mnt/SDCARD/spruce/scripts/helperFunctions.sh
cd "$(dirname "$0")"

if [ "$PLATFORM" = "A30" ]; then
    export SDL_VIDEODRIVER=dummy
    export LD_LIBRARY_PATH="$(dirname "$0")/libs32:/mnt/SDCARD/spruce/bin:$LD_LIBRARY_PATH"
    ./gvu32 "$@"
else
    export LD_LIBRARY_PATH="$(dirname "$0")/libs:/mnt/SDCARD/spruce/bin64:$LD_LIBRARY_PATH"
    ./gvu "$@"
fi
```

### `config.json`

```json
{
  "label": "GVU",
  "icon": "icon.png",
  "launch": "launch.sh",
  "description": "Video player for SpruceOS",
  "devices": ["MIYOO_A30"],
  "hideInSimpleMode": false
}
```

---

## Color Themes (implemented)

GVU uses the same 10 themes as PixelReader for SpruceOS visual consistency.
Each theme defines 5 UI colors (`background`, `text`, `secondary`, `highlight_bg`,
`highlight_text`) plus 5 dedicated cover icon colors (`cover_body`, `cover_tab`,
`cover_shadow`, `cover_screen`, `cover_play`).

The cover fields are separate from the UI fields so each theme can style the default cover
art independently without affecting browser text colors. All cover colors are drawn from the
same 5 UI colors — just remapped to different elements per theme.

| Theme | Background | Text |
|-------|-----------|------|
| `SPRUCE` *(default)* | `#14231e` | `#d2e1d2` |
| `night_contrast` | `#000000` | `#f0f0f0` |
| `light_contrast` | `#ffffff` | `#000000` |
| `light_sepia` | `#faf0dc` | `#000000` |
| `vampire` | `#000000` | `#c00000` |
| `coffee_dark` | `#2b1f16` | `#f5e6d3` |
| `cream_latte` | `#f5e6d3` | `#2b1f16` |
| `nautical` | `#0f192d` | `#d4af37` |
| `nordic_frost` | `#eceff4` | `#2e3440` |
| `night` | `#0d0d10` | `#f2f2f2` |

Theme is saved to `gvu.conf`. Cycling with R1 in the browser takes effect immediately,
regenerates the default cover texture, and calls `theme_save_icon()` to overwrite `icon.png`
so the launcher tile in PyUI matches the active theme. The help overlay texture cache is also
invalidated on theme change so the overlay re-renders in the new theme.

### Default cover icon elements

`default_cover.svg` is a document/video card icon with five recolorable elements:

| Element | SVG original | Theme field | Notes |
|---------|-------------|-------------|-------|
| Body | `#dde5e8` | `cover_body` | Large card shape |
| Tab | `#71c6c4` | `cover_tab` | Folded top-right corner |
| Tab shadow | `#17292d` | `cover_shadow` | Dark overlay on fold edge, 6% opacity |
| Screen | `#afc3c9` | `cover_screen` | Rounded rect representing the video area |
| Play button | `#ffffff` | `cover_play` | Triangle arrow |

`theme_render_cover()` in `src/theme.c` does a sequential string replace of the original hex
values. **Constraint:** `cover_screen` must not equal `#ffffff` (the original play color),
or the screen color will be overwritten when the play replacement runs. Use `#fafafa` or any
off-by-one value for a near-white screen. All other cover colors are unconstrained.

### Per-theme cover color mapping

| Theme | Body | Tab | Screen | Play |
|-------|------|-----|--------|------|
| SPRUCE | `text` (light sage) | `highlight_bg` (dark green) | `secondary` (mid green) | `highlight_text` (white) |
| night_contrast | `secondary` (grey) | `highlight_bg` (dark orange) | `text` (off-white) | `secondary` (grey) |
| light_contrast | `highlight_bg` (violet) | `secondary` (grey) | `highlight_text` (near-white) | `highlight_bg` (violet) |
| light_sepia | `highlight_bg` (sage) | `secondary` (grey) | `background` (cream) | `highlight_bg` (sage) |
| vampire | `secondary` (dark scarlet) | `highlight_bg` (red) | `background` (black) | `highlight_bg` (red) |
| coffee_dark | `text` (cream) | `highlight_bg` (coffee) | `secondary` (tan) | `highlight_text` (white) |
| cream_latte | `highlight_bg` (tan) | `secondary` (med brown) | `text` (dark brown) | `highlight_bg` (tan) |
| nautical | `secondary` (blue-grey) | `highlight_bg` (med blue) | `highlight_text` (yellow) | `highlight_bg` (med blue) |
| nordic_frost | `text` (dark slate) | `highlight_bg` (teal) | `secondary` (blue-grey) | `highlight_text` (dark slate) |
| night | `secondary` (grey) | `highlight_bg` (blue) | `highlight_bg` (blue) | `highlight_text` (black) |

All shadow colors use the original SVG dark (`#17292d`) except light_contrast, light_sepia,
vampire, and night which use their `text`/`background` black.

---

## Help Overlay Performance (A30)

The controls reference overlay (`help_draw`) was calling `TTF_RenderUTF8_Blended` +
`SDL_CreateTextureFromSurface` for 16 hint items every frame — far too slow on ARMv7,
causing the main loop to stall and produce audio blips.

Fix (in `main.c`): when the overlay is first opened, render it once into a
`SDL_TEXTUREACCESS_TARGET` texture via `SDL_SetRenderTarget`. Each subsequent frame just
blits the cached texture with `SDL_RenderCopy`. The cache is destroyed on dismiss or theme
change, forcing a fresh render next open.

This pattern should be applied to any other static full-screen overlay that gets too slow
(tutorial slides are a candidate if they show the same symptoms).

---

## Status Bar

A persistent status bar sits at the top of the screen showing system state at a glance.

**Visibility:**
- Always shown in all browser views (Folder Grid, Season List, File List, History)
- Shown during playback only when the OSD is visible

**Contents (left → right):**

| Region | Content |
|--------|---------|
| Left | Clock (`HH:MM`, 24-hour, local time) |
| Center | "GVU" title |
| Right | WiFi signal bars · Battery percentage · Battery icon |

**Sizing:**
- Height: `sc(40, win_w)` — 40px at 640px wide, scales with resolution
- Text (clock, "GVU", battery %) uses the main `font` (18px base, scaled)
- WiFi bars and battery icon drawn at 80% of bar height (`bar_h * 4 / 5`)

**System data sources (read on A30; gracefully absent on desktop):**

| Element | Source | Update interval |
|---------|--------|----------------|
| Battery % | `/sys/class/power_supply/battery/capacity` | 30s |
| Charging state | `/sys/class/power_supply/battery/status` | 30s |
| WiFi signal | `/proc/net/wireless` (link quality 0–70) | 5s |

All reads are cached; the files are only opened at the interval above to avoid hammering
the kernel on every frame. Missing files (desktop build) leave the indicators blank without
error.

**Browser layout:**
`main.c` uses `SDL_RenderSetViewport` to constrain browser content to `y = sbar_h` and
below, so browser tiles never overlap the bar. `win_h - sbar_h` is passed to `browser_draw`
so layout metrics treat the content area as the full available height.

**Playback — volume / brightness indicators:**
`player.c` includes `statusbar.h` and calls `statusbar_height(win_w)` to offset the
top-anchored volume and brightness indicator popups below the bar. This keeps them visible
even when the status bar is showing during OSD.

**Implementation:** `src/statusbar.c` / `src/statusbar.h`. Public API:

```c
int  statusbar_height(int win_w);   /* bar height in px at this window width */
void statusbar_draw(SDL_Renderer *renderer, TTF_Font *font,
                    const Theme *theme, int win_w, int win_h);
```

---

## Dynamic UI Scaling (Multi-Resolution Support)

GVU targets four SpruceOS screen sizes: 640×480, 750×560, 1024×768, and 1280×720. All layout
constants scale proportionally at runtime; no rebuild is needed per device.

### Resolution at runtime

`win_w` / `win_h` are runtime variables in `main.c` (not compile-time macros). On A30 they
are hard-coded to 640×480. On non-A30 (desktop test) builds they can be overridden:

```
./gvu 1280 720
./gvu 1024 768
```

### Scaling helpers

Each source file that does layout defines a local helper (or uses the one in `browser.c`):

```c
static inline int sc(int base, int w) { return (int)(base * w / 640.0f + 0.5f); }
```

`overlay.c` also has `sc_h(base, h)` for panel heights (base = 480):

```c
static inline int sc_h(int base, int h) { return (int)(base * h / 480.0f + 0.5f); }
```

### Font sizes

Set in `main.c` before window creation:

```c
int font_sz  = (int)(18.0f * win_w / 640.0f + 0.5f);  /* main font */
int font_ssz = (int)(14.0f * win_w / 640.0f + 0.5f);  /* small font */
```

Base sizes bumped from the original 16/13 to 18/14 for readability at 640×480.

### Hintbar spacing

`src/hintbar.c` derives all spacing from `TTF_FontHeight(font_small)` rather than fixed
pixel values. Since the font size scales with `win_w`, glyph padding, H-pad, label gap,
and item gap all scale automatically without needing `win_w` passed through the call chain:

```c
static inline int glyph_pad(TTF_Font *f)  { return h * 2 / 7; }   /* ≈ 4px at 14px font */
static inline int h_pad(TTF_Font *f)      { return h * 3 / 7; }   /* ≈ 6px */
static inline int glabel_gap(TTF_Font *f) { return h * 5 / 14; }  /* ≈ 5px */
static inline int item_gap(TTF_Font *f)   { return h * 9 / 7; }   /* ≈ 18px */
```

### Panel heights (overlay.c)

Dialog/overlay panels scale with `win_h` to maintain the same proportional footprint:

| Panel | Base height (480) |
|-------|------------------|
| Controls reference | 300 px |
| Tutorial | 250 px |
| Resume prompt | 160 px |
| Up-next / Error | 150 / 160 px |

### Tuning

All base values were chosen to look correct at 640×480. If a specific resolution looks
wrong, adjust the base value in the relevant `sc()` call — the scaling to other resolutions
follows automatically. Values that may need tuning based on tester feedback are OSD height
(`sc(80, win_w)` in `player.c`) and the browser hint bar height (`sc(24, win_w)`).

---

## Media Scan Paths

```
/mnt/SDCARD/Media/
/mnt/SDCARD/Roms/MEDIA/
```

(Configurable via `GVU_TEST_ROOTS` compile flag for local development:
`/tmp/gvu_test/Media/` and `/tmp/gvu_test/Roms/MEDIA/`)

Supported extensions: `.mp4`, `.mkv`, `.avi`, `.mov`, `.webm`

### 3-Level Library Detection

`scan_dir()` in `filebrowser.c` classifies each directory with a two-pass scan:

| Case | Condition | Result |
|------|-----------|--------|
| Flat folder | Directory has ≥1 direct video file | `MediaFolder` with `is_show=0`, files list |
| Show container | No direct videos, but ≥1 immediate child dir that has direct videos | `MediaFolder` with `is_show=1`, `seasons[]` array |
| Skip / recurse | Neither of the above | Recurse into sub-dirs, no entry added |

**Single-season promotion:** A show with exactly 1 season has its files promoted directly into the `MediaFolder` (`is_show=0`). The season level is skipped entirely in the UI — identical to a flat folder.

**Cover art lookup:** `find_cover()` checks for `cover.jpg` then `cover.png` in the directory. For shows, the show-level cover is used in the folder grid; per-season covers appear as thumbnails in the season list.

### Example directory layouts

```
# Flat folder (unchanged from original)
Media/
  My Movie (2020)/
    my_movie.mkv

# Single-season show → promoted to flat (no season screen shown)
Media/
  Firefly/
    Season 1/
      s01e01.mkv
      s01e02.mkv

# Multi-season show → full 3-level navigation
Media/
  Breaking Bad/
    Season 1/
      s01e01.mkv
    Season 2/
      s02e01.mkv
```

---

## Cover Art Scraping

Pressing **Y** in the Folder Grid triggers on-device cover art fetching for the selected folder.

### Flow

1. A confirmation overlay appears ("Fetch Cover Art?" + folder name + "Sources: TMDB, TVMaze").
   A = confirm, B = cancel.
2. On confirmation, `main.c` launches `resources/scrape_covers.sh <folder_path> [tmdb_key]`
   via `posix_spawn` (stdout+stderr → `/tmp/gvu_scrape.log`).
3. A progress overlay ("Fetching cover art...") is shown while the script runs. B cancels via
   `kill(scrape_pid, SIGTERM)`.
4. The script writes `ok` or `error` to `/tmp/gvu_scrape_done` on completion (sentinel file).
   `main.c` polls `access(SCRAPE_DONE_FILE, F_OK)` every frame — no `waitpid()` needed.
   (`SIGCHLD=SIG_IGN` auto-reaps children, making `waitpid()` return `ECHILD`.)
5. On `ok`, `main.c` updates `lib.folders[idx].cover`, invalidates the cached folder texture
   and backdrop, and **invalidates the season texture cache** so it reloads on the next draw.
6. **Live season cover refresh**: The script writes `ok` immediately after saving the show
   cover, then continues downloading season covers in the background. `main.c` watches the
   scraped folder for up to 60 seconds, checking once per second. When a new `cover.jpg`
   appears in a season subdirectory, it updates `Season.cover` and invalidates the season
   texture cache — season covers appear live without restarting the app.

### `scrape_covers.sh`

- **BusyBox wget only — no `--timeout`, no HTTPS.** GVU spawns scripts via `posix_spawn`
  which resolves to BusyBox wget v1.27.2 in its PATH. This version only supports
  `-c -q -O -Y -P -S -U` flags — `--timeout` causes "unrecognized option" and an immediate
  non-zero exit. GNU Wget 1.20.3 (which does support `--timeout`) is present elsewhere on the
  system but NOT in GVU's PATH. All `--timeout` flags have been removed from the script.
  The SpruceOS wget is also
  without TLS support; `https://` URLs fail immediately with "Scheme missing". All API and
  image URLs in the script use `http://`. Do not change them back to `https://`.
- **Season detection**: if the folder basename matches `Season N` / `S01` / etc., the parent
  directory name is used as the search term (so scraping from inside a season folder still
  finds the show cover and fetches season-specific art).
- **Bulk season art**: when called on a show folder (not a season subfolder), the script
  writes `ok` after saving the show cover, then fetches `api.tvmaze.com/shows/{id}/seasons`
  once and iterates over season subdirectories (matching `Season N` / `S01` patterns),
  downloading each season's artwork to `{season_dir}/cover.jpg`. Seasons that already have
  a cover are skipped.
- **Source 1 — TMDB** (`api.themoviedb.org/3/search/multi`): requires `tmdb_key` in `gvu.conf`.
  Parses `poster_path` from the JSON response using only BusyBox utilities (`tr`, `grep`, `sed`).
- **Source 2 — TVMaze** (`api.tvmaze.com/search/shows`): no key needed, TV shows only.
  **Always queried**, even when TMDB provided the show cover — `show_id` from TVMaze is
  required for season art bulk scraping. TVMaze image is only used as `cover_url` when TMDB
  found nothing. Uses the `/search/shows` (array) endpoint rather than `/singlesearch/shows`
  — the latter returns HTTP 404 on no match, causing wget to exit non-zero; the search
  endpoint always returns HTTP 200.
  Season-specific image extraction: uses `grep -A 20 '"number":N$'` (end-of-line anchor, not
  `[^0-9]`) — after `tr ',' '\n'` each field ends at EOL with no trailing character, so
  `[^0-9]` never matches. Season `"original"` is ~13 comma-splits after `"number"`, so `-A 20`
  is required (earlier `-A 5` was silently finding nothing).
- Downloads the image to a temp path (`/tmp/gvu_cover_$$.jpg`), then `mv`s it to
  `<folder>/cover.jpg` atomically — a cancelled scrape never leaves a partial file on disk.
- A catch-all `trap ... EXIT` always writes to the sentinel file, even on unexpected exits
  caused by `set -e`, so the GVU progress overlay is never stuck.

### TMDB API key

#### How it's distributed

The key is **never stored in the git repo**. It lives in `.tmdb_key` in the repo root
(listed in `.gitignore`). `package_gvu_a30.sh` reads it at package time and writes it into
`gvu.conf` inside the release zip. Users who clone and build from source get TVMaze-only
scraping by default; users who download a release zip get TMDB included automatically.

#### Add or update the key

```bash
# On the build machine — put your TMDB v3 API key in .tmdb_key:
echo "your_32char_key_here" > ~/Projects/GVU/gvu/.tmdb_key

# Rebuild the package — the new key is picked up automatically:
bash cross-compile/miyoo-a30/package_gvu_a30.sh
```

To update the key on a device that's already running GVU, edit `gvu.conf` directly:

```bash
ssh spruce@192.168.1.62
# then on device:
vi /mnt/SDCARD/App/GVU/gvu.conf
# add or change: tmdb_key = your_new_key_here
```

Or deploy `gvu.conf` from the newly built package over SSH.

#### Remove the key

To build a TVMaze-only package (no TMDB):

```bash
rm ~/Projects/GVU/gvu/.tmdb_key
bash cross-compile/miyoo-a30/package_gvu_a30.sh
# Output: "TMDB key: not found (.tmdb_key missing) — TVMaze-only package"
```

To remove the key from a running device:

```bash
ssh spruce@192.168.1.62 'sed -i "/tmdb_key/d" /mnt/SDCARD/App/GVU/gvu.conf'
```

#### Where it's used in code

`config_load("gvu.conf")` in `main.c` reads the key into `g_tmdb_key` (static in
`src/theme.c`). `config_tmdb_key()` returns it. In `main.c`, the scrape launch block
passes it as a second argument to `scrape_covers.sh`:

```c
const char *key = config_tmdb_key();
argv_buf[2] = (key && key[0]) ? key : NULL;
```

In `scrape_covers.sh`, `$2` is the key — if empty/absent the TMDB block is skipped
and only TVMaze runs.

#### Getting a TMDB API key

1. Create a free account at [themoviedb.org](https://www.themoviedb.org)
2. Settings → API → Request an API key → Developer
3. Application URL: `https://github.com/amruthwo/GVU`
4. Copy the **API Key (v3 auth)** — a 32-character hex string

### Layout preferences

The folder grid layout and season list layout are stored independently in `gvu.conf`:

```
layout = 0        # 0=LARGE (2-col grid), 1=SMALL (4-col grid), 2=LIST
season_layout = 2 # independent value for the season view
```

Saved immediately when SELECT is pressed to cycle either layout. Restored on next launch
before the first frame is drawn. Managed via `config_get/set_layout()` and
`config_get/set_season_layout()` in `src/theme.c`; `BrowserState` holds `layout` and
`season_layout` fields applied from config after `browser_init()`.

### Attribution

Both TMDB and TVMaze require attribution in UIs that use their data. The confirmation overlay
displays "Sources: TMDB, TVMaze" before the user initiates a fetch.

### Clearing cover art (`clear_covers.sh`)

`resources/clear_covers.sh` is an on-device utility script that removes all `cover.jpg` and
`cover.png` files from GVU's media roots. Useful for resetting cover art before a fresh scrape.

**Media roots it clears:**
- `/mnt/SDCARD/Roms/MEDIA/` — fully cleared
- `/mnt/SDCARD/Media/` — cleared, **except** `/mnt/SDCARD/Media/Music/` and everything inside it

The Music folder exclusion exists because another app uses `/mnt/SDCARD/Media/` for music and
manages its own cover art there.

**Running it:**
- From **DinguxCommander**: navigate to `/mnt/SDCARD/App/GVU/resources/`, highlight
  `clear_covers.sh`, and use the execute option (usually Y or the context menu)
- From **SSH**: `sh /mnt/SDCARD/App/GVU/resources/clear_covers.sh`

The script prints each removed file and a final count to stdout. It is included in the release
zip and deployed by `make miyoo-a30-deploy` alongside `scrape_covers.sh`. It is not called by
GVU itself — it's a manual maintenance tool only.

**Implementation notes (BusyBox gotchas fixed):**
- Uses `find ... > /tmp/file` + `while IFS= read -r f` instead of `for f in $(find ...)`.
  The `$(find ...)` approach word-splits on whitespace, breaking any path that contains
  spaces — e.g. `Season 1`, `Rick and Morty`. Season covers and any show with spaces in
  its name would be silently skipped.
- The skip check (`Media/Music`) is gated with `[ -n "$skip" ]`. Without the guard, when
  `skip=""` the `case` pattern `"$skip"/*` expands to `/*`, which matches every absolute
  path — causing all files to be skipped and nothing deleted.
- `find` uses `\( -name X -o -name Y \)` grouping for correct BusyBox precedence.

---

## Known Issues / TODO

### Screen tearing — FIXED ✓
Previously `a30_flip()` wrote directly into the displayed fb page, causing the display
controller to read partially-written frames. Fixed with double-buffering: write the rotation
into the hidden page, then `FBIOPAN_DISPLAY` to atomically switch the display controller to
that page, then swap front/back roles for the next frame. Same pattern as the PixelReader A30
port — confirmed safe on the sunxi overlay layer.

`FBIOPAN_DISPLAY` is safe. `FBIOPUT_VSCREENINFO` is not — the latter resets the display mode
and breaks the overlay until reboot. Never call it.

### Volume keys — FIXED ✓
KEY_VOLUMEUP (115) and KEY_VOLUMEDOWN (114) appear on `/dev/input/event3` alongside all other
buttons. They were simply absent from the evdev keymap. Added mappings:

```
KEY_VOLUMEUP   → SDLK_EQUALS  → player_volume_up()   (+10%)
KEY_VOLUMEDOWN → SDLK_MINUS   → player_volume_dn()   (-10%)
```

SpruceOS may also process the volume key events to adjust ALSA system volume. Both work
independently — no conflict.

### Sleep/wake audio recovery — IN PROGRESS ⚠

**What currently works (deployed as of 2026-03-20):**
- FBIOPAN_DISPLAY blocking: fixed — permanently disabled after first wake, replaced with
  back→front memcpy. See `a30_screen_wake()` in `src/a30_screen.c`.
- Wake detection: working — SDL_GetTicks frame-gap > 2000ms while playing.
- DAC volume restore: working — `amixer sset 'digital volume' 63` via `posix_spawn`.
- Cascade guard: in place — `wake_prev_frame = SDL_GetTicks()` set AFTER `audio_wake()`
  returns, so the time spent inside audio_wake doesn't look like another sleep gap.

**What doesn't work yet:** Audio is silent after wake. The ALSA PCM device enters a
continuous underrun storm after wake and doesn't recover without being closed and reopened.

---

#### Root cause analysis

After A30 sleep/wake, two things happen simultaneously:

**1. ALSA PCM XRUN storm.** The ALSA PCM driver enters XRUN state. SDL's audio thread calls
`snd_pcm_writei` → gets `-EPIPE` → calls `snd_pcm_recover()` → retries — repeatedly,
at 30–87× per second. This produces no audible output and consumes significant CPU on ARMv7.

**2. DAC volume reset to 0.** The sunxi hardware resets `digital volume` to 0 on every wake.
Even if ALSA recovers, the DAC is silent until `amixer sset 'digital volume' 63` is called.

**3. FBIOPAN_DISPLAY blocks for 100–700+ seconds.** The sunxi framebuffer driver's
`FBIOPAN_DISPLAY` ioctl waits for VBL (vsync). After wake the display controller can take
hundreds of seconds before vsync is available again. **This is now fixed** — `a30_screen_wake()`
permanently switches to the memcpy path after first wake. The flag is a permanent bool (not
a frame counter), so calling `a30_screen_wake()` multiple times is safe.

---

#### Approaches tried (in order)

**Rev 1 — ring clear + clock adjust, no SDL close/reopen**
- `audio_wake()`: lock ring mutex, subtract `ring.filled/bps` from `a->clock`, clear ring
  pointers, signal `not_full`.
- Result: ~5 second audio-ahead desync. Root cause: the audio **decode thread** was blocked
  in `ring_write()` (ring was full). When the ring is cleared, `ring_write` unblocks
  immediately and its next call sets `a->clock = decoded_pts`. `decoded_pts` is 2–3 seconds
  ahead of the playing position (ring was full = 2.97s of buffered audio). The decode thread
  overwrites our clock correction within microseconds.
- Lesson: cannot correct the clock from the main thread while the decode thread owns it.

**Rev 2 — SDL_CloseAudioDevice + reopen, no ring clear**
- `audio_wake()`: `SDL_CloseAudioDevice`, `SDL_OpenAudioDevice`, `SDL_PauseAudioDevice(0)`,
  `posix_spawn amixer`.
- Result: cascade of audio_wake calls, eventually watchdog kill (exit 137). Root cause:
  `SDL_CloseAudioDevice` joins SDL's audio thread. On A30, after wake, that thread is stuck
  in `snd_pcm_recover()` and the join blocked **2+ seconds**. The cascade guard
  (`wake_prev_frame = SDL_GetTicks()`) had not yet been added at this point, so the 2s block
  appeared as a new sleep gap → second `audio_wake()` fired → cascade.

  **Important: Rev 2 was never tested WITH the cascade guard in place.** The cascade guard
  (`wake_prev_frame = SDL_GetTicks()` AFTER `audio_wake` returns) was added later. Rev 2 +
  cascade guard is the **most promising untested approach** — see next steps below.

**Rev 3 — no SDL close/reopen, only amixer (current deployed state)**
- `audio_wake()`: just `posix_spawn amixer`. No device close/reopen, no ring clear.
- Result: no audio after wake. ALSA underrun storm produces 87+ rapid snd_pcm_recover calls
  in ~2.6s, draining the ring and consuming enough CPU to slow the main loop. The main loop
  eventually sees another >2000ms gap → `audio_wake` fires again → cascade. Eventually
  watchdog kills (exit 137) OR the device becomes unresponsive for 40–54 seconds.
- Lesson: without closing and reopening the SDL audio device, the ALSA PCM does not recover
  from XRUN on the A30 sunxi driver.

**Rev 4 — player_seek_to to flush ring via sentinel (tried, reverted)**
- `audio_wake()`: same as Rev 3. In `main.c` wake block: also call
  `player_seek_to(&player, audio_get_clock(&player.audio))`.
- Intent: flush sentinel reaches audio decode thread → decode thread clears ring → callback
  writes silence → XRUN storm stops.
- Result: **much worse** — 10–54 second freezes. Root cause: `player_seek_to()` calls
  `demux_request_seek()` which calls `av_seek_frame()` internally (via demux thread). On a
  long video at position ~860s on SD card, `av_seek_frame` takes 10+ seconds. This blocked
  the main loop. Additionally, the flush sentinel cannot reach the audio decode thread while
  the thread is blocked in `ring_write()` (ring full, callback draining it slowly).
- Lesson: do not call `player_seek_to` from the wake path.

---

#### What the log shows (Rev 4, from /tmp/gvu.log)

```
audio_wake: gap 3707ms — sleep/wake        # first wake (real sleep), ring=FULL (2.972s)
[x79] ALSA underrun occurred
audio_wake: gap 10582ms — sleep/wake       # av_seek_frame blocking the main loop
audio_wake: (clock=914.610 ring_delay=0.348s)
audio_wake: gap 5142ms — sleep/wake        # clock going backwards (seek to earlier pos)
audio_wake: (clock=913.756 ring_delay=2.972s)
audio_wake: gap 48854ms — sleep/wake       # 48 second freeze
audio_wake: (clock=916.892 ring_delay=2.972s)
...
```

Note: `ring_delay=2.972s` (= AUDIO_RING_SIZE full) at most wake detections confirms the ring
is never cleared — the sentinel approach failed.

---

#### Next step to try: Rev 2 + cascade guard

The cascade guard (`wake_prev_frame = SDL_GetTicks()` after `audio_wake` returns) was added
after Rev 2 was abandoned. Rev 2 WITH cascade guard has never been tested and is the most
likely fix:

**In `src/audio.c`, `audio_wake()`:**
```c
void audio_wake(AudioCtx *a) {
    if (!a->dev) return;
    double bps = AUDIO_OUT_RATE * AUDIO_OUT_CHANNELS * 2.0;
    fprintf(stderr, "audio_wake: (clock=%.3f ring_delay=%.3fs)\n",
            a->clock, a->ring.filled / bps);

    /* Clear the ring first — unblocks the decode thread from ring_write,
       and means the callback has nothing to drain while we're in CloseAudioDevice. */
    SDL_LockMutex(a->ring.mutex);
    double ring_delay = a->ring.filled / bps;
    a->ring.filled = a->ring.read_pos = a->ring.write_pos = 0;
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);
    /* Adjust clock to compensate for the ring that was cleared */
    SDL_LockMutex(a->clock_mutex);
    a->clock -= ring_delay;
    if (a->clock < 0.0) a->clock = 0.0;
    SDL_UnlockMutex(a->clock_mutex);

    /* Close and reopen the SDL audio device to reset the ALSA PCM state.
       SDL_CloseAudioDevice joins SDL's audio thread, which may take 2+ seconds
       while snd_pcm_recover runs — this is acceptable because the cascade guard
       in main.c sets wake_prev_frame = SDL_GetTicks() AFTER this returns,
       so the 2s block does not trigger a second audio_wake. */
    SDL_CloseAudioDevice(a->dev);
    a->dev = 0;

    SDL_AudioSpec want = {
        .freq = AUDIO_OUT_RATE, .format = AUDIO_SDL_FORMAT,
        .channels = AUDIO_OUT_CHANNELS, .samples = AUDIO_SDL_SAMPLES,
        .callback = audio_callback, .userdata = a,
    };
    SDL_AudioSpec got;
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (a->dev) SDL_PauseAudioDevice(a->dev, 0);

#ifdef GVU_A30
    /* Restore DAC volume — reset to 0 by hardware on every wake */
    static char *child_argv[] = {
        "sh", "-c", "amixer sset 'digital volume' 63 >/dev/null 2>&1", NULL
    };
    static char *child_env[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", NULL
    };
    pid_t pid;
    posix_spawn(&pid, "/bin/sh", NULL, NULL, child_argv, child_env);
#endif
}
```

**In `src/main.c`, wake detection block — no changes needed beyond what's already there:**
```c
audio_wake(&player.audio);        /* may block ~2s inside SDL_CloseAudioDevice */
a30_screen_wake();
if (player.state == PLAYER_PAUSED)
    SDL_PauseAudioDevice(player.audio.dev, 1);
player_show_osd(&player);
wake_prev_frame = SDL_GetTicks(); /* cascade guard: reset AFTER audio_wake returns */
```

**Clock concern with Rev 2 + ring clear:**
The decode thread will fill the ring again during the ~2s wait inside `SDL_CloseAudioDevice`.
When the new device starts playing, `a->clock` will have been advanced by the decode thread
to a new decoded-ahead position. `audio_get_clock()` = `a->clock - ring_delay - sdl_delay`
should still return approximately the right position since ring_delay will reflect the newly
buffered data. Some brief A/V desync (~0.5s) is expected but should self-correct as video
sync catches up.

**If Rev 2 + cascade guard still cascades (unlikely but possible):**
The cascade guard could fail if the ALSA thread takes more than the `wake_prev_frame` timer
allows. Add a rate-limit: after `audio_wake`, don't allow another wake for 10 seconds:
```c
static Uint32 last_wake_ms = 0;
if (/* gap detected */ && SDL_GetTicks() - last_wake_ms > 10000) {
    last_wake_ms = SDL_GetTicks();
    audio_wake(&player.audio);
    ...
    wake_prev_frame = SDL_GetTicks();
}
```

---

#### Other pitfalls (learned during development)

- `fork()` on a 350MB process takes ~3s on ARMv7 — looks like a sleep/wake gap and triggers
  a second `audio_wake`. Use `posix_spawn` only.
- `KEY_POWER` on `/dev/input/event0` is buffered while sleeping and delivered on wake —
  intercepting it as a quit signal causes immediate exit on wake. Don't use it.
- Stray cross-compiled libs in `libs32/` break shell invocation via `system()` because
  `LD_LIBRARY_PATH` points there. Use `posix_spawn` with a clean `child_env[]` array.
- `a30_screen_wake()` must be a permanent bool flag (not a frame counter). If it resets a
  counter each time it's called, multiple calls during a cascade reset the counter and extend
  the blocking period. The current implementation is correct (bool set once, never cleared).
- After `audio_wake` reopens the device, re-apply the player's pause state:
  `if (player.state == PLAYER_PAUSED) SDL_PauseAudioDevice(player.audio.dev, 1)`

### Video frame memory leak — FIXED ✓

`video_decode_thread` in `src/video.c` was using `av_image_alloc()` to create the converted
YUV420P frame, then calling `fq_push()` which internally calls `av_frame_move_ref()`. After
the move, `cvt->data[0]` is NULL (zeroed by move_ref), so the subsequent `av_freep(&cvt->data[0])`
was a no-op. When `av_frame_free()` later freed the queued frame, it checked `buf[0]` (which
`av_image_alloc` never sets — it's not ref-counted), found it NULL, and silently skipped freeing
the pixel data. Every decoded frame leaked ~3MB at 1080p — ~90MB/s — causing OOM kill
(lowmemorykiller, exit 137) after ~60 seconds of playback.

**Fix:** replaced `av_image_alloc` + `av_freep` with `av_frame_get_buffer`, which allocates
proper ref-counted buffers. `av_frame_free` then correctly decrements the refcount and frees
the data. Memory is now stable at ~27MB RSS with no growth during playback.

### audio_stop deadlock — FIXED ✓

`audio_stop()` in `src/audio.c` set `a->abort = 1` and signaled `ring.not_full`, then called
`SDL_WaitThread()` to join the audio decode thread. But if the thread was blocked in
`packet_queue_get()` waiting for packets (rather than in `ring_write()`), the ring signal
never reached it. The thread hung forever; `player_close()` (called on R2/L2 file skip and
at EOS auto-advance) deadlocked indefinitely.

The demux thread is still running at this point (`demux_stop` is called after `audio_stop`
in `player_close`), so the audio packet queue may be empty with no packets arriving.

**Fix:** `audio_stop` now also calls `packet_queue_abort(a->pkt_queue)` before the join,
which sets `q->abort = 1` and signals `not_empty`, waking the thread regardless of which
blocking call it's in.

### avformat_find_stream_info stalling on SD card — FIXED ✓

`player_open()` calls `decoder_probe()` and `demux_open()` in sequence, both of which call
`avformat_find_stream_info()` on the main thread with default FFmpeg probe limits (5MB /
5 seconds of content). On the A30's slow SD card, each call could take 10–15 seconds. With
two calls per file open, R2/L2 file skip froze the main loop for 20–30 seconds.

**Fix:** `fmt->probesize = 1024 * 1024` (1MB) and `fmt->max_analyze_duration = 500000` (0.5s)
set on the format context before each `avformat_find_stream_info` call, in both `decoder_probe`
and `demux_open`. For MP4/MKV files the codec info is always in the container header, so these
limits are never actually reached — the call completes in milliseconds.

### False-positive sleep/wake detection on file skip — FIXED ✓

The sleep/wake gap detector in `main.c` checks `frame_start - wake_prev_frame > 2000ms` while
`mode == MODE_PLAYBACK && player.state != PLAYER_STOPPED`. The issue: `player_close()` and
`do_play()` run synchronously on the main loop iteration that handles R2/L2. The main loop is
blocked for the duration (previously 20–30s, now much shorter with the probesize fix and
deadlock fix). The loop never sees `PLAYER_STOPPED` because it's stuck — it jumps directly
from PLAYING to PLAYING on the next iteration. `wake_prev_frame` retains its pre-skip value,
so the gap detector sees a multi-second gap and misidentifies it as hardware sleep/wake,
incorrectly calling `audio_wake()`.

**Fix:** `wake_prev_frame = 0` is set immediately before `player_close()` in the R2 and L2
handlers. When `wake_prev_frame == 0`, the gap detector condition `wake_prev_frame > 0` is
false and `audio_wake` is suppressed for that transition.

### Video decoder thread count — FIXED ✓ (A30)

FFmpeg defaults to spawning one decode thread per CPU core. On the A30's quad-core Cortex-A53,
this means 4 threads for video decode alone, which causes severe thermal throttling under
sustained load. `FF_THREAD_FRAME` mode also keeps extra reference frame copies per thread,
significantly increasing memory pressure.

**Fix (`#ifdef GVU_A30`):** `v->codec_ctx->thread_count = 1` before `avcodec_open2()` in
`src/video.c`. Single-threaded decode on A53 at ~1.6GHz handles 720p H.264 comfortably; 1080p
is borderline but usable. This also reduces FFmpeg's internal DPB memory footprint.

### Buffer sizes reduced (A30) ✓

As part of the memory pressure investigation:
- `VIDEO_FRAME_QUEUE_SIZE`: 8 → 4 (saves ~12MB at 1080p of decoded YUV frame buffers)
- `PKT_QUEUE_MAX`: 256 → 64 (reduces compressed packet backlog; video packets can be large)

These are defined in `src/video.h` and `src/decoder.h` respectively.

### D-pad hold-to-scroll — NEW ✓

`gpio-keys-polled` never generates EV_REP events. Software key-repeat implemented in
`a30_poll_events()` (`src/a30_screen.c`): static state tracks the held key, fires a synthetic
`SDL_KEYDOWN` with `repeat=1` after a 300ms initial delay, then every 80ms. Only UP/DOWN/LEFT/RIGHT
repeat. `browser_handle_event()` filters out `repeat=1` events for all non-navigation keys
so action buttons (A, B, R1, etc.) cannot accidentally fire while scrolling.

Tuning constants: `KEY_REPEAT_DELAY_MS 300`, `KEY_REPEAT_PERIOD_MS 80` in `src/a30_screen.c`.

### Subtitles
Not yet implemented. `.srt` files alongside videos are ignored. START button is currently
unbound during playback (no subtitle toggle). Post-MVP.

### Cover art / icon — FIXED ✓
`icon.png` is generated at startup from `resources/default_cover.svg` recolored in the active
theme, and regenerated after every theme cycle (R1 in the browser). `theme_save_icon()` in
`src/theme.c` handles both operations. The file is written to the app directory (`icon.png`
beside the binary), which is where `config.json` points PyUI to look for it.

This is A30-only (`#ifdef GVU_A30` guard in `main.c`). On the desktop test build the call is
skipped since there is no SpruceOS launcher to update.

### History / resume state path
Currently stored as flat files in the app directory. Planned location:
`/mnt/SDCARD/Saves/CurrentProfile/states/GVU/gvu.state` — not yet moved.

### Non-A30 devices
64-bit build and multi-device `launch.sh`/`config.json` not yet done. The codebase is
structured to support it (`platform.c`, `#ifdef GVU_A30` guards throughout) but the 64-bit
cross-compile toolchain and packaging have not been set up.

---

## GitHub Repository

The canonical source is at **https://github.com/amruthwo/GVU** (SSH: `git@github.com:amruthwo/GVU.git`).
The default branch is `master`.

### First-time clone

```bash
git clone git@github.com:amruthwo/GVU.git ~/Projects/GVU/gvu
cd ~/Projects/GVU/gvu
```

After cloning, restore the TMDB key (not committed):

```bash
echo "your_32char_tmdb_key" > .tmdb_key
```

### Pushing a change

The normal workflow — edit, build/test, commit, push:

```bash
# 1. Build and deploy to device for testing
docker run --rm -v /home/amruthwo/Projects/GVU/gvu:/gvu:z gvu-a30 \
    bash -c "make -C /gvu miyoo-a30-build && cp /gvu/gvu32 /gvu/build/gvu32"
docker run --rm -v /home/amruthwo/Projects/GVU/gvu:/gvu:z gvu-a30 \
    python3 /gvu/cross-compile/miyoo-a30/patch_verneed.py /gvu/build/gvu32
scp build/gvu32 spruce@192.168.1.62:/mnt/SDCARD/App/GVU/

# 2. Commit (from repo root ~/Projects/GVU/gvu)
git add src/whatever.c src/whatever.h
git commit -m "Short description of change"

# 3. Push
git push origin master
```

### Docker note

The `make miyoo-a30-docker` Makefile target rebuilds the Docker image from scratch and
calls `build_inside_docker.sh` in one step, but the script currently fails when run
through `sh <relative-path>` inside the container (SELinux / podman volume mount quirk).
Until that's fixed, use the two-command form above: `make -C /gvu miyoo-a30-build` then
`cp /gvu/gvu32 /gvu/build/gvu32`. Both run inside the same `gvu-a30` image.

If the Docker image needs to be rebuilt (e.g. Dockerfile.gvu changed or the image was
deleted), use:

```bash
make miyoo-a30-docker   # rebuilds image only; ignores the broken build_inside_docker step
# Then cross-compile manually as above
```

### What NOT to commit

- `.tmdb_key` — gitignored, must never be committed (contains the API key)
- `gvu32`, `build/` — build artifacts, also gitignored
- `gvu.conf` at the repo root — this is a local-only dev config; the real device config
  is generated by `package_gvu_a30.sh` and written into the release zip

### Branching / tagging

Currently everything is on `master` with no feature branches. When preparing a release:

```bash
git tag v1.0
git push origin v1.0
bash cross-compile/miyoo-a30/package_gvu_a30.sh 1.0
# Upload GVU-1.0.zip to GitHub Releases
```

---

## BusyBox on SpruceOS — Limitations and Workarounds

Scripts that run **on-device** (`scrape_covers.sh` and anything launched via `posix_spawn`)
execute under SpruceOS's BusyBox environment. This is meaningfully different from a desktop
Linux shell. Every workaround below was discovered the hard way.

### wget (BusyBox v1.27.2)

The `wget` in GVU's PATH is **BusyBox wget v1.27.2**, not GNU Wget.

| Feature | BusyBox v1.27.2 | GNU Wget 1.20.3 |
|---------|----------------|-----------------|
| `--timeout=N` | **Not supported** — exits non-zero immediately | Supported |
| HTTPS / TLS | **Not supported** — "Scheme missing" | Supported |
| `-q` (quiet) | Supported | Supported |
| `-O <file>` | Supported | Supported |
| `--spider` | Not supported | Supported |

**Key rules:**
- **Never use `--timeout`** — BusyBox wget treats it as an unrecognized option and exits 1,
  failing `set -e`. Remove all `--timeout` flags; the script relies on SpruceOS's watchdog
  to kill the process if it hangs completely.
- **Use `http://` not `https://`** — BusyBox wget has no TLS. All TMDB and TVMaze API URLs
  use `http://`. Do not change them back to `https://` — they will fail silently on-device
  even though they work fine from an SSH session (SSH uses GNU Wget which IS in a different
  PATH and does support TLS).
- GNU Wget 1.20.3 is present at `/usr/bin/wget` on the A30 but its PATH is not the same
  as GVU's `posix_spawn` environment. Always test `wget` behavior via `posix_spawn` output
  (check `/tmp/gvu_scrape.log`), not from an SSH shell.

### Shell (`sh` = BusyBox ash)

All on-device scripts use `/bin/sh` which is BusyBox ash — not bash. Do not use:
- `bash`-specific syntax (arrays, `[[ ]]`, `$''`, `<<<`, process substitution `<(...)`)
- `local` outside functions (works in ash, but be cautious)
- `echo -e` — use `printf` instead for escape sequences

`set -e` works correctly in ash and is used in `scrape_covers.sh`.

### grep

BusyBox grep supports:
- `-E` (extended regex), `-i` (case-insensitive), `-A N` (after context), `-q` (quiet)
- **Does NOT support** `\d`, `\w`, etc. — use `[0-9]`, `[a-zA-Z]` instead

**End-of-line anchor issue with `tr ',' '\n'` pipelines:**
When JSON is flattened with `tr ',' '\n'`, each field ends at EOL with no trailing
character. The pattern `'"number":5[^0-9]'` looks for a non-digit after `5` — but at
end-of-line there is no character, so `[^0-9]` never matches. Use a bare end-of-line
anchor: `'"number":5$'` (the `$` matches the end of line). This caused silently empty
output for season-number matching until the bug was found by testing locally.

**`-A N` must be large enough:**
TVMaze season JSON has `"number"` and `"original"` separated by ~13 comma-splits. With
`tr ',' '\n'` flattening, `-A 5` (5 lines after the match) was too small to reach the
`"original"` field. `-A 20` is required.

### Word-splitting on `find` output

**Never use `for f in $(find ...)` on-device.** The shell word-splits the output on
whitespace, so any path containing spaces — `Season 1`, `Rick and Morty`, etc. — is broken
into multiple tokens and silently fails. The correct pattern for BusyBox ash:

```sh
find "$dir" \( -name "cover.jpg" -o -name "cover.png" \) > /tmp/list_$$.txt
while IFS= read -r f; do
    # handle $f safely — spaces preserved
done < /tmp/list_$$.txt
rm -f /tmp/list_$$.txt
```

`while IFS= read -r` reads one full line per iteration regardless of spaces. This is
standard POSIX sh and works correctly in BusyBox ash.

### sed / tr / awk

BusyBox sed and tr are standard and work as expected for simple substitutions. awk is
present (`awk`, BusyBox awk). Multi-expression sed works:

```sh
sed 's/ /%20/g; s/!/%21/g; ...'   # OK in BusyBox sed
```

### posix_spawn from C and PATH

`posix_spawn` inherits the parent process's environment **only if you pass `envp = NULL`**.
GVU always passes an explicit `child_env[]` array with a clean `PATH` to avoid `LD_LIBRARY_PATH`
contamination (the app's `LD_LIBRARY_PATH` points to `libs32/`, which contains only `libz.so.1`
but still affects child process linking). The clean PATH used:

```c
"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
```

This PATH resolves to BusyBox `wget`, BusyBox `sh`, etc. — not GNU tools.

### SSH vs on-device behavior

Commands that succeed in an SSH session can fail when run by GVU via `posix_spawn`:

| Issue | SSH | posix_spawn |
|-------|-----|-------------|
| `wget https://...` | Works (GNU wget, has TLS) | Fails (BusyBox wget, no TLS) |
| `wget --timeout=10` | Works | Fails with exit 1 |
| Inline `sh -c 'cmd1 && cmd2'` | Often works | Can fail if SpruceOS SSH wrapper mangles quotes |
| Multi-step shell commands | Easy to test | Must write to `/tmp/script.sh`, scp, then run |

**Workaround for SSH testing of on-device shell logic:**
Write the script to `/tmp/test.sh` on the device, then run it with
`ssh spruce@192.168.1.62 "sh /tmp/test.sh"` rather than trying to inline complex shell
commands. Inline commands with `&&`, `||`, or quotes frequently get mangled by the SpruceOS
SSH wrapper.

### `system()` vs `posix_spawn`

Never use `system()` from GVU. `system()` internally calls `fork()`, which on a 350MB
process takes ~3 seconds on ARMv7 — long enough to trigger the sleep/wake gap detector
and fire a spurious `audio_wake()`. Always use `posix_spawn`.

### Sentinel file pattern

Scripts signal completion back to GVU via a sentinel file (`/tmp/gvu_scrape_done`):
`echo "ok" > /tmp/gvu_scrape_done` on success, `echo "error" > ...` on failure.
`main.c` polls `access(SCRAPE_DONE_FILE, F_OK)` every frame — no `waitpid()` needed
(`SIGCHLD=SIG_IGN` auto-reaps children). Always use a `trap ... EXIT` to write the
sentinel even on unexpected `set -e` exits so GVU's progress overlay is never stuck:

```sh
trap 'rm -f "$tmpfile"; [ -f /tmp/gvu_scrape_done ] || echo "error" > /tmp/gvu_scrape_done' EXIT
```

---

## FFmpeg Codecs (A30 static build)

The Docker image builds FFmpeg 5.1.6 with `--disable-everything` and selectively enables:

**Decoders:** h264, hevc, vp8, vp9, av1, mpeg4, mpeg2video, mjpeg, aac, mp3, mp3float,
vorbis, flac, opus, ac3, eac3, dca, truehd, pcm_s16le, pcm_s24le, pcm_f32le, pcm_s16be,
subrip, ass, ssa, webvtt

**Demuxers:** mov, matroska, mp4, avi, flv, ogg, mp3, wav, flac, aac, mpegts, mpegvideo,
srt, webvtt, hls, pcm_s16le

**Parsers:** h264, hevc, vp8, vp9, av1, mpeg4video, aac, mpegaudio, vorbis, flac, opus, ac3

**Protocols:** file

**Enabled:** swscale, swresample, avformat, avcodec, avutil

---

## Reference Projects

- `~/Projects/PixelReader-port/pixel-reader/` — A30 fb0 bypass, glibc_compat.c, patch_verneed.py;
  all three were reused directly in GVU
- `~/Projects/GVU/gvu/` — this project
- SpruceOS source: https://github.com/spruceUI/spruceOS — `helperFunctions.sh`, platform detection
- FFmpeg's `ffplay.c` — canonical reference for demux/decode/sync architecture
- nanosvg — single-header SVG parser/rasterizer: https://github.com/memononen/nanosvg
