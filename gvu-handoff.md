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
│   │   ├── package_gvu_universal.sh
│   │   └── gvu_base/             launch.sh, config.json for SpruceOS
│   └── trimui-brick/
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
sh cross-compile/miyoo-a30/package_gvu_universal.sh 0.2.1
```

Outputs: `build/gvu32`, `build/gvu64`, `build/bin32/fetch_subs32`, `build/bin64/fetch_subs64`, and a zip in the repo root.

### Deploy to device

```sh
# A30 (192.168.1.62 by default on SpruceOS)
scp build/gvu32 spruce@<ip>:/mnt/SDCARD/App/GVU/gvu32

# Brick / Flip (sftp required on some firmware versions)
sftp spruce@<ip>
put build/gvu64 /mnt/SDCARD/App/GVU/gvu64
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

Each container builds SDL2, FFmpeg, mbedTLS, and libcurl from source with static linking, so the binary has no shared library dependencies beyond glibc.

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

The audio clock drives sync. The video thread drops or holds frames to match it. There's a configurable catchup threshold (default 250ms) — if video falls more than 250ms behind audio, it skips frames to catch up. If video is ahead of audio, it holds the current frame.

On slow devices (A30, Mini Flip) the video decode thread can't always keep up with a 24fps H.264 stream. The frame queue has a max depth (4 frames); if it's full, the decode thread waits. The main thread's blit path checks the audio clock before deciding which frame to show.

### Display pipelines

**A30:** `SDL_VIDEODRIVER=dummy` keeps SDL out of fb0. `a30_flip()` takes the 640×480 SDL surface, rotates it 90° CCW into a second framebuffer page, then calls `FBIOPAN_DISPLAY` to flip to that page. This gives tear-free double-buffering. Alpha must be forced to `0xFF` on every pixel — fb0 is an overlay layer and transparent pixels show the PyUI layer underneath.

**Brick/Flip:** Standard SDL2 with hardware-accelerated renderer. Tear-free via DRM vsync on Flip (fb0 is single-page on Flip, so `FBIOPAN_DISPLAY` would crash — SDL2's DRM backend handles it instead).

**Mini Flip V4:** Same as Brick/Flip path but the display is 752×560 and rotated 180°. SDL2 handles it via `SDL_HINT_ORIENTATIONS`.

**Mini V2/V3:** SDL2 640×480. Audio is silent on some firmware versions — known issue, not yet resolved.

---

## Device Notes

### Miyoo A30

- **Binary:** `gvu32` (ARMv7 hard-float)
- **Display:** fb0 is 480×640 portrait, ARGB8888. App renders 640×480; `a30_flip()` rotates 90° CCW into fb0.
- **SDL:** `SDL_VIDEODRIVER=dummy` is mandatory. SDL must never touch fb0 directly.
- **Audio:** ALSA. Hardware volume sync at startup via `amixer`. SpruceOS sets the ALSA mixer on launch, so GVU reads the current level and mirrors it.
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
- **Audio:** ALSA, `plughw:0,0`. A `.asoundrc` is written to the home dir at launch if not present. Without it, ALSA fails with "device busy" on Flip.
- **App dir on device:** `/mnt/SDCARD/App/GVU/`

### Miyoo Mini Flip (V4)

- **Binary:** `gvu32` (ARMv7)
- **Display:** 752×560. SDL2. 180° rotation applied.
- **Audio:** SigmaStar MI_AO hardware. SpruceOS ships `libpadsp.so` which bridges `/dev/dsp` to the MI_AO layer. `SDL_AUDIODRIVER=dsp` + LD_PRELOAD of libpadsp makes it work.
- **Volume:** Device volume buttons work via ALSA mixer events. GVU reads and mirrors hardware volume at startup.

### Miyoo Mini V2/V3

- **Binary:** `gvu32`
- **Display:** 640×480 SDL2
- **Audio:** Silent on some firmware versions. Root cause not yet identified.

---

## Button Mappings

### Playback

| Button | Action |
|---|---|
| D-pad left/right | Seek ±10s |
| D-pad up/down | Seek ±60s |
| L1/R1 | Previous/next file in folder |
| A | Play/pause |
| B | Exit to browser |
| X | Toggle subtitles |
| Y | Subtitle sync adjust |
| SELECT | Toggle OSD |
| START | Toggle help overlay |
| Volume up/down | Hardware volume (synced to device mixer) |
| L2/R2 | Zoom cycle |

### File browser

| Button | Action |
|---|---|
| D-pad up/down | Navigate (hold to scroll fast) |
| D-pad left | Back / up a level |
| A | Open folder or play file |
| B | Back |
| Y | Scrape cover art for selected show |
| SELECT | Clear watch history |
| START | Help overlay |
| L1/R1 | Switch browser view layout |

### History page

| Button | Action |
|---|---|
| D-pad up/down | Navigate |
| A | Resume playback |
| B | Back |
| X | Remove entry |
| SELECT | Clear all history |

---

## Color Themes

Ten built-in themes. Cycle with L1/R1 on the theme picker (accessible from the settings menu).

| Theme name | Character |
|---|---|
| `spruce` | Dark green + cream, default |
| `monochrome` | High-contrast black and white |
| `slate` | Cool grey-blue |
| `amber` | Warm amber on dark |
| `crimson` | Deep red accent |
| `ocean` | Teal/blue on dark |
| `dusk` | Purple-grey |
| `sakura` | Pink-rose on light |
| `forest` | Muted green |
| `papyrus` | Warm light/sepia |

`monochrome` was previously called `night_contrast`. If you see that name in old config files or notes, it's the same theme.

### Rounded corner values

The UI uses rounded rectangles throughout. Default radii (in pixels, at base 640×480):

| Element | Radius |
|---|---|
| Card tiles (browser grid) | 10 |
| Season / file list rows | 8 |
| OSD bar | 12 |
| Help overlay panels | 14 |
| Hint bar pills | 8 |
| Status bar | 0 (flat) |

These scale proportionally with `g_display_w/g_display_h` on larger screens (Brick, Smart Pro).

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

`fetch_subs` is a compiled C binary (not a shell script). It links statically against libcurl + mbedTLS and handles the SubDL and Podnapisi APIs, JSON parsing, and ZIP extraction entirely in C. This replaced the earlier Python-based approach because SpruceOS devices don't have a reliable Python environment.

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

- Video: H.264 (libx264 decode), H.265/HEVC, MPEG-4, VP8, VP9 (software decode only)
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

- Mini V2/V3 audio: needs diagnosis on-device with `aplay` to find the correct ALSA device string
- Multiple audio track support: works for 2 tracks but crashes on some files with 3+ tracks — needs a bounds check in `player.c`
- Hardware decode: would help A30 significantly for H.264 720p content if the VPU can be accessed
- Gamepad rumble on Brick/Flip: SDL2 supports it, would be a nice touch for seek feedback
- Network playback: not started

### Contact / history

Development log in git history. The `A30` branch on GitHub is a clean record of the A30-only v0.1.1/v0.1.2 development history. The `universal` branch is the current main branch with all devices.
