# GVU — Developer Handoff

> Universal branch. Covers A30, Brick, Flip, Mini Flip V4, and Mini V2/V3.
> Last updated: 2026-04-04.

---

## Table of Contents

- [What Is This](#what-is-this)
- [Repository Layout](#repository-layout)
- [Getting Started on a New Machine](#getting-started-on-a-new-machine)
- [Build System](#build-system)
- [Source Layout](#source-layout)
- [Architecture](#architecture)
- [Device Notes](#device-notes)
- [Button Mappings](#button-mappings)
- [Color Themes](#color-themes)
- [Cover Art Scraping](#cover-art-scraping)
- [Subtitle Download](#subtitle-download)
- [API Keys](#api-keys)
- [On-Device Shell Gotchas](#on-device-shell-gotchas)
- [FFmpeg Codec Selection](#ffmpeg-codec-selection)
- [SpruceOS Handoff Notes](#sprucos-handoff-notes)

---

## What Is This

GVU is a standalone video player for SpruceOS devices. It has a file browser, cover art, fullscreen playback with OSD, seek, A/V sync, subtitles (local .srt + download via SubDL/Podnapisi), watch history, resume, and a themed UI. It's written in C around FFmpeg + SDL2.

The reason it exists: no open-source handheld player had A/V sync that held up on these devices (they're single-core ARM chips and the timing model matters a lot). GMU's audio-only architecture wasn't adaptable. Starting from scratch was cleaner.

The `universal` branch is the main development branch. It builds two binaries — `gvu32` (ARMv7 for A30) and `gvu64` (AArch64 for everything else) — packaged into a single zip.

---

## Repository Layout

```
gvu/
├── src/                          main C sources
├── resources/                    fonts, SVGs, shell scripts, CA bundle
│   ├── fonts/DejaVuSans.ttf
│   ├── default_cover.svg/png
│   ├── scrape_covers.sh          on-device cover art fetch (TMDB + TVMaze)
│   ├── clear_covers.sh           removes all cached covers from media roots
│   ├── fetch_subs.c              (compiled into fetch_subs32/64, not a script)
│   ├── cacert.pem                Mozilla CA bundle for TLS (fetch_subs)
│   └── api/                      placeholder text files (actual keys gitignored)
├── cross-compile/
│   ├── miyoo-a30/
│   │   ├── Dockerfile.gvu        builds SDL2/FFmpeg for ARMv7 static link
│   │   ├── build_inside_docker.sh
│   │   ├── patch_verneed.py      patches GLIBC_2.28/2.29 → GLIBC_2.4
│   └── universal/
│       ├── package_gvu_universal.sh
│   │   └── gvu_base/             launch.sh, config.json for SpruceOS
│   ├── trimui-brick/
│       ├── Dockerfile.gvu        builds SDL2/FFmpeg for AArch64 static link
│       ├── build_inside_docker.sh
│       └── gvu_base/
├── images/                       README assets (icon + screenshot)
├── Makefile
└── gvu-handoff.md                this file
```

**Never commit:** `gvu.conf`, `gvu.conf.bak`, `*.zip`, `gvu32`, `gvu64`, `build/`, `cross-compile/deps/`, `.tmdb_key`, `.subdl_key`, `resources/api/SubDL_API.txt`, `resources/api/TMDB_API.txt`.

---

## Getting Started on a New Machine

### Prerequisites

- Docker or Podman (Podman recommended)
- SSH access to a test device (optional but useful)
- git, make, python3 (for patch_verneed.py)

> **Podman note:** If your shell has `USER` set to something unexpected (e.g., an SSH session), prefix every `podman run` with `env -u USER`. This prevents lchown EINVAL errors inside the container.

### Clone and build

```sh
git clone git@github.com:<org>/gvu.git
cd gvu
git checkout universal

# Build the A30 Docker image (first time only, ~10 minutes)
podman build -t gvu-a30-builder -f cross-compile/miyoo-a30/Dockerfile.gvu cross-compile/miyoo-a30/

# Build the Brick/Flip Docker image (first time only)
podman build -t gvu-brick -f cross-compile/trimui-brick/Dockerfile.gvu cross-compile/trimui-brick/

# Build both binaries
env -u USER podman run --rm -v "$(pwd):/gvu:z" gvu-a30-builder  sh /gvu/cross-compile/miyoo-a30/build_inside_docker.sh
env -u USER podman run --rm -v "$(pwd):/gvu:z" gvu-brick         sh /gvu/cross-compile/trimui-brick/build_inside_docker.sh

# Package (creates GVU_v<VERSION>.zip)
sh cross-compile/universal/package_gvu_universal.sh 0.2.1
```

Outputs: `build/gvu32`, `build/gvu64`, `build/fetch_subs32`, `build/fetch_subs64`, and a zip in the repo root. The package script copies these into the `bin32/`/`bin64/` layout expected by `launch.sh`.

### Deploy to device

For a quick binary-only update (skip packaging):

```sh
# A30
scp build/gvu32 spruce@<ip>:/mnt/SDCARD/App/GVU/bin32/gvu

# Brick / Flip (sftp required on some firmware versions)
sftp spruce@<ip>
put build/gvu64 /mnt/SDCARD/App/GVU/bin64/gvu
```

### What not to commit

API key files, `gvu.conf`, build outputs, and zip archives are all gitignored. See `.gitignore` for the full list. The `resources/api/` directory has placeholder `.txt` files committed — those are stubs only. Actual keys go in `gvu.conf` (added via the settings UI on-device, or written manually).

---

## Build System

### Docker containers

Two separate containers, one per architecture:

| Container | Image tag | Target | Output |
|---|---|---|---|
| `cross-compile/miyoo-a30/Dockerfile.gvu` | `gvu-a30-builder` | ARMv7 (arm-linux-gnueabihf) | `gvu32`, `fetch_subs32` |
| `cross-compile/trimui-brick/Dockerfile.gvu` | `gvu-brick` | AArch64 | `gvu64`, `fetch_subs64` |

Each container builds SDL2, FFmpeg, OpenSSL 1.1.1w, and libcurl from source with static linking, so the binary has no shared library dependencies beyond glibc.

### VERNEED patching (A30 only)

The A30 runs glibc 2.23. Some glibc symbols used by FFmpeg or SDL2 are tagged `GLIBC_2.28` or `GLIBC_2.29` in the ELF VERNEED table, which causes an immediate linker failure at launch on the device. `patch_verneed.py` rewrites those version tags to `GLIBC_2.4` (the minimum glibc version that has those symbols). This is run automatically in `build_inside_docker.sh` after the link step.

```sh
python3 cross-compile/miyoo-a30/patch_verneed.py build/gvu32
```

### Makefile targets

```sh
make miyoo-a30-build          # ARMv7 build (run inside Docker)
make trimui-brick-build       # AArch64 build (run inside Docker)
make clean
```

---

## Source Layout

```
src/
├── main.c            event loop, mode dispatch, all platform guards
├── platform.c/.h     /proc/cpuinfo detection → PLATFORM enum + g_display_w/h
├── decoder.c/.h      FFmpeg demux thread, packet queues, seek
├── video.c/.h        video decode thread, frame queue, swscale, SDL texture upload
├── audio.c/.h        audio decode thread, resample, SDL audio callback
├── player.c/.h       open/play/pause/seek/zoom/volume/subtitle toggle
├── browser.c/.h      folder grid, season list, file list, cover cache, layout prefs
├── filebrowser.c/.h  3-level media library scan (shows → seasons → files)
├── history.c/.h      watch history (in-progress + completed files)
├── hintbar.c/.h      button hint bar: glyphs, pills, rounded buttons
├── overlay.c/.h      help overlay, resume prompt, up-next card, error screen
├── theme.c/.h        color themes, SVG recolor, nanosvg rasterizer
├── resume.c/.h       resume position save/load, clear-all
├── statusbar.c/.h    clock (left), title (center), WiFi + battery (right)
├── a30_screen.c/.h   A30: fb0 mmap, 90° CCW rotation, evdev input injection
└── glibc_compat.c    fcntl64@GLIBC_2.28 shim for glibc 2.23
```

`main.c` is the hub. Everything else is called from there. Platform-specific code is gated with `#ifdef GVU_A30`, `#ifdef GVU_TRIMUI_BRICK`, etc.

---

## Architecture

### Threading model

Four threads: main, demux, video decode, audio decode.

```
[Main Thread]
  SDL event loop → key dispatch → player/browser commands
  player_update() → A/V sync, SDL texture upload, blit
  a30_flip() [A30 only] → rotate + write to fb0

[Demux Thread]
  av_read_frame() → video packet queue + audio packet queue

[Video Decode Thread]
  avcodec_send_packet() → avcodec_receive_frame() → swscale → frame queue

[Audio Decode Thread]
  avcodec_send_packet() → avcodec_receive_frame() → resample → ring buffer

[SDL Audio Callback]
  drains ring buffer → writes to SDL audio stream
```

One rule worth knowing: always flush the codec before seeking. The video and audio threads have a flush packet in the queue protocol. If you skip the flush, you get ghost frames from the previous position on every seek.

### A/V sync

The audio clock drives sync. The video thread drops or holds frames to match it. Two thresholds control behavior (`player.c`): `AV_SYNC_THRESHOLD_SEC = 0.040` (40ms) — if video is ahead of audio by more than this, hold the frame. `AV_NOSYNC_THRESHOLD_SEC = 0.15` (150ms) — if video is more than 150ms behind audio, drop the frame without displaying it to catch up.

On slow devices (A30, Mini Flip) the video decode thread can't always keep up with a 24fps H.264 stream. The frame queue has a max depth (4 frames); if it's full, the decode thread waits. The main thread's blit path checks the audio clock before deciding which frame to show.

### Display pipelines

**A30:** `SDL_VIDEODRIVER=dummy` keeps SDL out of fb0. `a30_flip()` takes the 640×480 SDL surface, rotates it 90° CCW into a second framebuffer page, then calls `FBIOPAN_DISPLAY` to flip to that page. This gives tear-free double-buffering. Alpha must be forced to `0xFF` on every pixel — fb0 is an overlay layer and transparent pixels show the PyUI layer underneath.

**Brick/Flip:** Standard SDL2 with hardware-accelerated renderer. Tear-free via DRM vsync on Flip (fb0 is single-page on Flip, so `FBIOPAN_DISPLAY` would crash — SDL2's DRM backend handles it instead).

**Mini Flip V4:** Same as Brick/Flip path. `launch.sh` detects V4 by checking fb0 for the `752x560p` mode string and overrides `GVU_DISPLAY_W=752 GVU_DISPLAY_H=560 GVU_DISPLAY_ROTATION=180`, which platform.c picks up via env vars at startup.

**Mini V2/V3:** SDL2 640×480. Audio is silent. Root cause: the mmiyoo audio backend only exists in SpruceOS's custom SDL2 build, which doesn't support `SDL_VIDEODRIVER=dummy`. Since GVU needs dummy video to use its own fb0 path, it can't use that SDL2 — so audio is set to `SDL_AUDIODRIVER=dummy` and runs silent.

---

## Device Notes

### Miyoo A30

- **Binary:** `gvu32` (ARMv7 hard-float)
- **Display:** fb0 is 480×640 portrait, ARGB8888. App renders 640×480; `a30_flip()` rotates 90° CCW into fb0.
- **SDL:** `SDL_VIDEODRIVER=dummy` is mandatory. SDL must never touch fb0 directly.
- **Audio:** OSS/DSP (`SDL_AUDIODRIVER=dsp`). Hardware volume sync at startup via `amixer`.
- **glibc:** 2.23. VERNEED patching required. `glibc_compat.c` provides the `fcntl64` shim.
- **Sleep/wake:** After wake, `FBIOPAN_DISPLAY` can block for 100+ seconds waiting for vsync from the display controller. Fix is in `a30_screen.c` — see the `s_fb_wake_frames` guard.
- **App dir on device:** `/mnt/SDCARD/App/GVU/`

### TrimUI Brick (and Hammer)

- **Binary:** `gvu64` (AArch64)
- **Display:** 1024×768 SDL2, hardware renderer
- **Audio:** SDL audio via OSS (`SDL_AUDIODRIVER=dsp`). **Do not let SDL2 try ALSA on the Brick** — it causes a kernel D-state lockup that requires a hard reboot. This is set in `launch.sh`.
- **App dir on device:** `/mnt/SDCARD/App/GVU/`

### Miyoo Flip (V1/V2)

- **Binary:** `gvu64`
- **Display:** 640×480. Single-page fb0 — `FBIOPAN_DISPLAY` will crash. SDL2's DRM/KMS backend provides vsync instead.
- **Audio:** ALSA, `plughw:0,0`. `launch.sh` writes a `.asoundrc` to `$HOME` before launch. Without it, ALSA fails with "device busy" because PyUI holds hw:0,0 exclusively.
- **App dir on device:** `/mnt/SDCARD/App/GVU/`

### Miyoo Mini Flip (V4)

- **Binary:** `gvu32` (ARMv7)
- **Display:** 752×560, 180° rotation. `launch.sh` overrides display dimensions and rotation via `GVU_DISPLAY_W/H/ROTATION` env vars, which `platform.c` reads at startup.
- **Audio:** SigmaStar MI_AO hardware. SpruceOS ships `libpadsp.so` which bridges `/dev/dsp` to the MI_AO layer. `launch.sh` sets `SDL_AUDIODRIVER=dsp` and `LD_PRELOAD=/customer/lib/libpadsp.so`.

### Miyoo Mini V2/V3

- **Binary:** `gvu32`
- **Display:** 640×480 SDL2
- **Audio:** Silent. The mmiyoo audio backend only exists in SpruceOS's custom SDL2 build, which doesn't support `SDL_VIDEODRIVER=dummy`. Since GVU requires dummy video to drive its own fb0 path, these are incompatible. Audio runs as `SDL_AUDIODRIVER=dummy`.

---

## Button Mappings

### Keysym reference

The physical buttons map to SDL keysyms as follows (A30 and Mini/Flip share the same logical mapping — the evdev codes differ but the keysym output is identical):

| Physical button | SDL keysym |
|---|---|
| A | SDLK_SPACE |
| B | SDLK_LCTRL |
| X | SDLK_LSHIFT |
| Y | SDLK_LALT |
| L1 | SDLK_PAGEUP |
| R1 | SDLK_PAGEDOWN |
| L2 | SDLK_COMMA |
| R2 | SDLK_PERIOD |
| SELECT | SDLK_RCTRL |
| START | SDLK_RETURN |
| MENU | SDLK_ESCAPE |
| Vol+ | SDLK_EQUALS |
| Vol- | SDLK_MINUS |

Note: on A30 the L1/L2/R1/R2 evdev codes differ from Mini/Flip — this is handled transparently in `a30_screen.c` via separate keymaps. The keysym output is the same on all devices.

### Playback

| Button | Action |
|---|---|
| A | Play / pause |
| B | Back to browser (saves resume position) |
| D-pad left/right | Seek ±10s |
| D-pad up/down | Brightness ± |
| L1 | Seek -60s |
| R1 | Seek +60s |
| L2 | Previous file in folder/season |
| R2 | Next file in folder/season |
| X | Cycle audio track |
| Y | Zoom cycle |
| SELECT | Toggle mute |
| START | Toggle subtitle on/off (or open downloader if no subtitle loaded) |
| START + D-pad left | Subtitle sync -0.5s |
| START + D-pad right | Subtitle sync +0.5s |
| START + D-pad up | Cycle subtitle speed |
| START + D-pad down | Reset subtitle sync and speed |
| START + X | Force re-download subtitles |
| Vol+ / Vol- | Hardware volume |

### File browser (folder grid and season list)

| Button | Action |
|---|---|
| D-pad | Navigate (hold for fast scroll) |
| A | Open folder / enter season |
| B | Back (press twice at top level to exit) |
| X | Open watch history |
| Y | Scrape cover art for selected show (folder grid only) |
| SELECT | Cycle view layout |
| R1 | Cycle color theme |
| L2/R2 | Previous/next season (in file list view) |

### File list (inside a folder/season)

| Button | Action |
|---|---|
| D-pad up/down | Navigate |
| A | Play file |
| B | Back to season/folder |
| SELECT | Cycle view layout |
| R1 | Cycle color theme |
| L2 | Previous season or previous folder |
| R2 | Next season or next folder |
| START | Open subtitle downloader for selected file |

### History page

| Button | Action |
|---|---|
| D-pad up/down | Navigate |
| A or START | Play selected entry |
| B or X | Back to browser |
| SELECT | Clear all history |

---

## Color Themes

Ten built-in themes. Press R1 anywhere in the browser to cycle through them.

`monochrome` was previously called `night_contrast`. If you see that name in old config files or notes, it's the same theme.

### Color field reference

Each theme defines 18 color fields across four groups:

**UI colors** — used throughout the interface:

| Field | Used for |
|---|---|
| `background` | Main background fill |
| `text` | Primary text and icons |
| `secondary` | Dimmed text, inactive elements |
| `highlight_bg` | Selected item background, accent fills |
| `highlight_text` | Text on top of a highlight_bg fill |
| `statusbar_fg` | Status bar text and icons (clock, battery, WiFi) |

**Cover art** (`default_cover.svg` recolored per theme):

| Field | SVG element |
|---|---|
| `cover_body` | Cassette body / main shape |
| `cover_tab` | Label tab accent |
| `cover_shadow` | Drop shadow / dark inset |
| `cover_screen` | Screen / window area |
| `cover_play` | Play triangle |

**App icon** (`app_icon.svg` recolored per theme — saved as `icon.png` for the SpruceOS launcher):

| Field | SVG element |
|---|---|
| `icon_ring` | Outer ring outline |
| `icon_circle` | Center circle fill |
| `icon_outer` | Left and right arrow elements |
| `icon_center` | Center arrow |

**Folder cover** (`default_folder.svg` recolored per theme — shown for folders that have no scraped cover):

| Field | SVG element |
|---|---|
| `folder_tab` | Small tab / accent element |
| `folder_screen` | Light panel area |
| `folder_body` | Main folder body |

---

### SPRUCE (default)

| Field | Hex |
|---|---|
| background | `#14231e` |
| text | `#d2e1d2` |
| secondary | `#64826e` |
| highlight_bg | `#326446` |
| highlight_text | `#ffffff` |
| statusbar_fg | `#d2e1d2` |
| cover_body | `#d2e1d2` |
| cover_tab | `#326446` |
| cover_shadow | `#17292d` |
| cover_screen | `#64826e` |
| cover_play | `#ffffff` |
| icon_ring | `#326446` |
| icon_circle | `#d2e1d2` |
| icon_outer | `#64826e` |
| icon_center | `#326446` |
| folder_tab | `#326446` |
| folder_screen | `#d2e1d2` |
| folder_body | `#64826e` |

### monochrome

| Field | Hex |
|---|---|
| background | `#000000` |
| text | `#f0f0f0` |
| secondary | `#808080` |
| highlight_bg | `#f0f0f0` |
| highlight_text | `#000000` |
| statusbar_fg | `#f0f0f0` |
| cover_body | `#808080` |
| cover_tab | `#f0f0f0` |
| cover_shadow | `#17292d` |
| cover_screen | `#f0f0f0` |
| cover_play | `#808080` |
| icon_ring | `#808080` |
| icon_circle | `#000000` |
| icon_outer | `#808080` |
| icon_center | `#f0f0f0` |
| folder_tab | `#404040` |
| folder_screen | `#f0f0f0` |
| folder_body | `#808080` |

### light_contrast

| Field | Hex |
|---|---|
| background | `#ffffff` |
| text | `#000000` |
| secondary | `#a0a0a0` |
| highlight_bg | `#a351c8` |
| highlight_text | `#fafafa` |
| statusbar_fg | `#a351c8` |
| cover_body | `#a351c8` |
| cover_tab | `#a0a0a0` |
| cover_shadow | `#000000` |
| cover_screen | `#fafafa` |
| cover_play | `#a351c8` |
| icon_ring | `#a351c8` |
| icon_circle | `#fafafa` |
| icon_outer | `#a0a0a0` |
| icon_center | `#a351c8` |
| folder_tab | `#000000` |
| folder_screen | `#a0a0a0` |
| folder_body | `#a351c8` |

### light_sepia

| Field | Hex |
|---|---|
| background | `#faf0dc` |
| text | `#000000` |
| secondary | `#a0a0a0` |
| highlight_bg | `#789c70` |
| highlight_text | `#faf0dc` |
| statusbar_fg | `#789c70` |
| cover_body | `#789c70` |
| cover_tab | `#a0a0a0` |
| cover_shadow | `#000000` |
| cover_screen | `#faf0dc` |
| cover_play | `#789c70` |
| icon_ring | `#faf0dc` |
| icon_circle | `#789c70` |
| icon_outer | `#a0a0a0` |
| icon_center | `#faf0dc` |
| folder_tab | `#a0a0a0` |
| folder_screen | `#e8d5b0` |
| folder_body | `#789c70` |

### vampire

| Field | Hex |
|---|---|
| background | `#000000` |
| text | `#c00000` |
| secondary | `#c04040` |
| highlight_bg | `#c00000` |
| highlight_text | `#000000` |
| statusbar_fg | `#c00000` |
| cover_body | `#600000` |
| cover_tab | `#c00000` |
| cover_shadow | `#000000` |
| cover_screen | `#000000` |
| cover_play | `#c00000` |
| icon_ring | `#600000` |
| icon_circle | `#000000` |
| icon_outer | `#600000` |
| icon_center | `#c00000` |
| folder_tab | `#222222` |
| folder_screen | `#c00000` |
| folder_body | `#600000` |

### coffee_dark

| Field | Hex |
|---|---|
| background | `#2b1f16` |
| text | `#f5e6d3` |
| secondary | `#a08c78` |
| highlight_bg | `#6f4e37` |
| highlight_text | `#ffffff` |
| statusbar_fg | `#d2b48c` |
| cover_body | `#f5e6d3` |
| cover_tab | `#6f4e37` |
| cover_shadow | `#17292d` |
| cover_screen | `#a08c78` |
| cover_play | `#ffffff` |
| icon_ring | `#6f4e37` |
| icon_circle | `#6f4e37` |
| icon_outer | `#f5e6d3` |
| icon_center | `#a08c78` |
| folder_tab | `#6f4e37` |
| folder_screen | `#f5e6d3` |
| folder_body | `#a08c78` |

### cream_latte

| Field | Hex |
|---|---|
| background | `#f5e6d3` |
| text | `#2b1f16` |
| secondary | `#786450` |
| highlight_bg | `#d2b48c` |
| highlight_text | `#2b1f16` |
| statusbar_fg | `#6f4e37` |
| cover_body | `#d2b48c` |
| cover_tab | `#786450` |
| cover_shadow | `#2b1f16` |
| cover_screen | `#2b1f16` |
| cover_play | `#d2b48c` |
| icon_ring | `#786450` |
| icon_circle | `#d2b48c` |
| icon_outer | `#2b1f16` |
| icon_center | `#786450` |
| folder_tab | `#2b1f16` |
| folder_screen | `#786450` |
| folder_body | `#d2b48c` |

### nautical

| Field | Hex |
|---|---|
| background | `#0f192d` |
| text | `#d4af37` |
| secondary | `#788cb4` |
| highlight_bg | `#38589a` |
| highlight_text | `#ffdc64` |
| statusbar_fg | `#d4af37` |
| cover_body | `#788cb4` |
| cover_tab | `#38589a` |
| cover_shadow | `#17292d` |
| cover_screen | `#ffdc64` |
| cover_play | `#38589a` |
| icon_ring | `#ffdc64` |
| icon_circle | `#38589a` |
| icon_outer | `#788cb4` |
| icon_center | `#ffdc64` |
| folder_tab | `#38589a` |
| folder_screen | `#ffdc64` |
| folder_body | `#788cb4` |

### nordic_frost

| Field | Hex |
|---|---|
| background | `#eceff4` |
| text | `#2e3440` |
| secondary | `#81a1c1` |
| highlight_bg | `#88c0d0` |
| highlight_text | `#2e3440` |
| statusbar_fg | `#88c0d0` |
| cover_body | `#2e3440` |
| cover_tab | `#88c0d0` |
| cover_shadow | `#17292d` |
| cover_screen | `#81a1c1` |
| cover_play | `#2e3440` |
| icon_ring | `#2e3440` |
| icon_circle | `#88c0d0` |
| icon_outer | `#81a1c1` |
| icon_center | `#17292d` |
| folder_tab | `#81a1c1` |
| folder_screen | `#88c0d0` |
| folder_body | `#2e3440` |

### night

| Field | Hex |
|---|---|
| background | `#0d0d10` |
| text | `#f2f2f2` |
| secondary | `#8888a0` |
| highlight_bg | `#7ab2de` |
| highlight_text | `#0d0d10` |
| statusbar_fg | `#7ab2de` |
| cover_body | `#52525e` |
| cover_tab | `#7ab2de` |
| cover_shadow | `#0d0d10` |
| cover_screen | `#7ab2de` |
| cover_play | `#0d0d10` |
| icon_ring | `#7ab2de` |
| icon_circle | `#0d0d10` |
| icon_outer | `#52525e` |
| icon_center | `#7ab2de` |
| folder_tab | `#52525e` |
| folder_screen | `#7ab2de` |
| folder_body | `#8888a0` |

### Rounded corner values

Rounded rectangles are used in the OSD and toast notifications (`player.c`). Radii are computed via the `sc(base, win_w)` helper which scales linearly from a 640px base:

| Element | Base radius | At 640px | At 1024px |
|---|---|---|---|
| OSD bar, volume bar | `sc(8, w)` | 8px | 13px |
| Subtitle/status toasts | `sc(8, w)` | 8px | 13px |
| OSD background box | `sc(16, w)` | 16px | 26px |

The browser (folder grid, season list, file list) uses standard `SDL_RenderFillRect` — no rounded corners.

### Folder icon and help overlay caching

The folder icon is generated from `default_cover.svg` with the current theme's accent color applied at startup. The help overlay slides are also rasterized once and cached — if you change the theme, the cache is invalidated and redrawn.

---

## Cover Art Scraping

Cover art is fetched by `resources/scrape_covers.sh`, which is spawned as a subprocess by `main.c` when the user presses Y on a show in the browser.

The script uses `wget` (GNU wget via the inherited PATH). It queries TMDB first (poster + season images), falls back to TVMaze if TMDB has no results or no API key. Images are saved as `cover.jpg` next to the media files.

**Sentinel file protocol:** `main.c` drops a `.scrape_pending` file before spawning the subprocess, removes it when the subprocess exits, and polls for `cover.jpg` to update the UI without blocking the main loop.

**Important:** The subprocess is spawned with `posix_spawn` passing `environ` explicitly. Earlier versions passed `NULL` which produces an empty environment on glibc 2.23 — PATH is unset, `wget` can't be found, every request silently fails. Always pass `environ`:

```c
extern char **environ;
// ...
posix_spawn(&scrape_pid, "/bin/sh", &fa, NULL, argv_buf, environ);
```

TMDB API key is read from `gvu.conf` (`tmdb_key = ...`). If the key is absent, TMDB is skipped and TVMaze is used alone. TVMaze is free and requires no key.

---

## Subtitle Download

`fetch_subs` is a compiled C binary (not a shell script). It links statically against libcurl + OpenSSL 1.1.1w and handles the SubDL and Podnapisi APIs, JSON parsing, and ZIP extraction entirely in C. This replaced the earlier Python-based approach because SpruceOS devices don't have a reliable Python environment.

Flow:
1. User initiates subtitle search from the OSD menu.
2. `main.c` spawns `fetch_subs32` or `fetch_subs64` (from `$APP_DIR/bin32` or `bin64`) with the filename and language code as arguments.
3. `fetch_subs` queries SubDL first, Podnapisi as fallback, returns a JSON results list.
4. User picks a result; `main.c` spawns `fetch_subs` again with the download URL.
5. `fetch_subs` downloads and extracts the ZIP, returns the `.srt` path.
6. Player loads the `.srt` file immediately.

TLS certificates: `resources/cacert.pem` (Mozilla CA bundle, ~220KB) is shipped in the zip and loaded by `fetch_subs`. Path passed via `GVU_CACERT_PATH` env var (set in `launch.sh`). Falls back to system CAs if the env var is not set.

---

## API Keys

Two keys are used: TMDB (for cover art) and SubDL (for subtitle search).

Both keys are **gitignored**. They are never committed to the repo. The `resources/api/` directory has stub `.txt` files committed as placeholders only.

On-device, keys are stored in `gvu.conf`:

```
tmdb_key = your_key_here
subdl_key = your_key_here
```

The settings UI on the device lets the user enter keys without editing the file manually. Keys persist across app updates as long as `gvu.conf` is not deleted.

For development, you can write `gvu.conf` manually in the app directory before first launch.

---

## On-Device Shell Gotchas

These have all caused debugging headaches at some point.

**`wget` vs `curl`:** SpruceOS ships a `curl` binary in `spruce/bin/` but it's the wrong architecture on some devices (ARM64 binary on an ARMv7 device). It will silently fail or segfault. Always use `wget --no-check-certificate -T 10 -q` for HTTP requests in shell scripts.

**`posix_spawn` and the environment:** `posix_spawn` with `envp=NULL` creates a completely empty environment on glibc 2.23. PATH, LD_LIBRARY_PATH, HOME — all unset. Any subprocess that calls `wget`, `sh`, or any external tool will fail with "not found". Always pass `environ` explicitly. This has caught us twice (subtitles and cover scraping).

**SSH command quoting:** SpruceOS's SSH server mangles shell metacharacters passed inline. Commands with `&&`, `||`, pipes, or redirections passed directly on the SSH command line don't work as expected. The safest approach: write a script to `/tmp/test.sh` and run `/bin/sh /tmp/test.sh`.

**Shell is `/bin/sh` (busybox ash):** No bash, no arrays, no `[[`. Don't use bashisms in shell scripts that run on-device. `scrape_covers.sh` and `clear_covers.sh` are intentionally POSIX sh.

**`grep` is busybox grep:** No `-P` (Perl regex), limited `-E` support. Use basic regexes only.

**`system()` is forbidden in the main thread:** Never call `system()` from the SDL event loop — it blocks until the child exits. All subprocess spawning goes through `posix_spawn`. See `sub_spawn()` and `scrape_spawn()` in `main.c`.

**Sentinel pattern:** The app can't wait on a subprocess without blocking. The pattern used throughout: drop a sentinel file before spawn, poll for its removal (or for an output file appearing) from the main loop at ~1Hz.

---

## FFmpeg Codec Selection

FFmpeg is built with a limited codec set to keep binary size reasonable:

- Video: H.264, H.265/HEVC, MPEG-4, VP8, VP9 (software decode only)
- Audio: AAC, MP3, AC3, Opus, Vorbis, FLAC, PCM variants
- Containers: MP4, MKV, AVI, WebM, MOV

Hardware decode is not enabled. The A30's VPU driver is not exposed in a way that integrates cleanly with FFmpeg's hwaccel API on SpruceOS. Software H.264 at 480p/720p runs fine on the Brick and Flip; the A30 handles 480p H.264 at 24fps without dropping frames.

H.265 on A30 is usable for 480p content but can drop frames at 30fps. Not recommended for high-bitrate sources on A30.

---

## SpruceOS Handoff Notes

These notes are for the SpruceOS team if they want to take over development or integrate GVU more deeply into the OS.

### Current state

GVU is working on A30, Brick, and Flip. Mini Flip V4 is working (audio via libpadsp). Mini V2/V3 audio is broken on some firmware — likely an ALSA config difference, not yet diagnosed.

The app ships as a zip that extracts to `/mnt/SDCARD/App/GVU/`. It launches via SpruceOS's standard `config.json` + `launch.sh` mechanism. No OS changes are required to install it.

### If you want to move it to Emu/MEDIA/

SpruceOS's `Emu/MEDIA/` folder is the standard location for video players that should appear in the media browser. Moving GVU there would mean it appears as the default video handler rather than a standalone app.

What changes:
- `launch.sh` would need to accept a file path as `$1` (SpruceOS passes the selected file to the emulator binary)
- The file browser startup mode would be skipped if `$1` is set — launch directly into playback
- `config.json` would move to `Emu/MEDIA/config.json`
- The SpruceOS media browser handles the file picker; GVU's own browser would only be used as a fallback

What stays the same: the binaries, resources, API keys, subtitle workflow — none of that changes.

### Build system notes

The Docker images are self-contained. Anyone with Podman or Docker can build from scratch without any pre-installed cross-compilers. The images take 10–15 minutes to build on first run; subsequent builds are fast (the heavy dependencies are cached in layers).

The VERNEED patch (`patch_verneed.py`) is the one non-obvious step. It's run automatically in `build_inside_docker.sh`. If you're seeing `GLIBC_2.28 not found` errors on A30, the patch didn't run or the binary was replaced after patching.

### What's not done yet

- Mini V2/V3 audio: silent by design — the mmiyoo audio backend requires SpruceOS's SDL2 which doesn't support `SDL_VIDEODRIVER=dummy`. Fixing this would require building a custom SDL2 with both mmiyoo audio and dummy video support, or finding another audio path on those devices
- Multiple audio track support: works for 2 tracks but crashes on some files with 3+ tracks — needs a bounds check in `player.c`
- Hardware decode: would help A30 significantly for H.264 720p content if the VPU can be accessed
- Gamepad rumble on Brick/Flip: SDL2 supports it, would be a nice touch for seek feedback
- Network playback: not started

### Contact / history

Development log in git history. The `A30` branch on GitHub is a clean record of the A30-only v0.1.1/v0.1.2 development history. The `universal` branch is the current main branch with all devices.
