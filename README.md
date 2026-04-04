<div align="center">
  <img src="images/GVU_Icon.png" width="120" alt="GVU icon" />
  <h1>GVU</h1>
  <p>Video player for SpruceOS handheld devices</p>
</div>

---

![GVU screenshot](images/gvu_screenshot.png)

GVU is a native video player for SpruceOS devices. It has a three-level media browser (shows → seasons → episodes), automatic cover art fetching, watch history with resume, .srt subtitle support with on-device subtitle download, and a clean fullscreen playback UI with OSD. It's written in C around FFmpeg and SDL2.

---

## Supported Devices

| Device | Notes |
|---|---|
| Miyoo A30 | 640×480, ARMv7. Works great. |
| TrimUI Brick / Hammer | 1024×768, AArch64. Works great. |
| Miyoo Flip V1/V2 | 640×480, AArch64. Works great. |
| Miyoo Mini Flip (V4) | 752×560, ARMv7. Audio via SigmaStar bridge. |
| Miyoo Mini V2/V3 | 640×480, ARMv7. Video works; audio broken on some firmware. |

---

## Installation

1. Download the latest release zip from the [Releases](../../releases) page.
2. Extract the zip to your SD card — it creates `/mnt/SDCARD/App/GVU/`.
3. Launch from the SpruceOS app menu.

On first launch, GVU scans your media folders and builds its library. Make sure your video files are in `/mnt/SDCARD/Roms/MEDIA/` or a subfolder organized by show name.

---

## Features

- **File browser** — three-level hierarchy (shows → seasons → files), folder grid with cover art
- **Cover art** — automatic fetch from TMDB and TVMaze (press Y on any show)
- **Playback** — fullscreen, hardware-scaled, software-decoded H.264/H.265/VP9/MP4/MKV/AVI
- **Seek** — frame-accurate, ±10s / ±60s by default
- **Watch history** — remembers where you left off across all shows
- **Subtitles** — load local .srt files or search and download from SubDL / Podnapisi
- **Themes** — ten color themes, cycle with the hint bar
- **OSD** — progress bar, current time, title, volume
- **Volume sync** — reads and mirrors the device hardware volume at startup
- **Status bar** — clock, title, WiFi signal, battery level

---

## Basic Usage

### Navigation

- **D-pad** — navigate the file browser
- **A** — open folder / play file
- **B** — back
- **Hold D-pad up/down** — fast scroll through long lists

### Playback controls

| Button | Action |
|---|---|
| D-pad left/right | Seek ±10 seconds |
| D-pad up/down | Seek ±60 seconds |
| A | Play / Pause |
| B | Back to browser |
| L1 / R1 | Previous / next file |
| L2 / R2 | Zoom cycle |
| X | Toggle subtitles |
| Y | Subtitle sync adjust |
| SELECT | Toggle OSD |
| START | Help overlay |
| Volume up/down | Adjust volume |

### Cover art

Press **Y** on any show in the browser to scrape cover art from TMDB and TVMaze. A TMDB API key (free) improves results — enter it in Settings. TVMaze requires no key.

### Subtitles

Local .srt files are loaded automatically if they share a filename with the video. To download subtitles, open the OSD (SELECT), go to the subtitle menu, search by title, pick a result from the list.

---

## Configuration

Settings are stored in `gvu.conf` in the app folder. Most options are accessible from the in-app settings menu. Common things you might want to set:

```
tmdb_key = your_tmdb_api_key_here
subdl_key = your_subdl_api_key_here
theme = spruce
```

TMDB keys are free at [themoviedb.org/settings/api](https://www.themoviedb.org/settings/api).
SubDL keys are free at [subdl.com](https://subdl.com).

---

## Supported Formats

Video: H.264, H.265/HEVC, MPEG-4, VP8, VP9
Audio: AAC, MP3, AC3, Opus, Vorbis, FLAC
Containers: MP4, MKV, AVI, WebM, MOV

All decoding is software. H.264 at 480p runs smoothly on all devices. H.265 works on faster hardware (Brick, Flip); A30 handles it at 480p.

---

## Developer Notes

See [gvu-handoff.md](gvu-handoff.md) for full build instructions, device-specific quirks, architecture notes, and SpruceOS integration details.
