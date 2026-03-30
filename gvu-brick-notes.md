# GVU Trimui Brick Port Notes

## Device Specs
- **CPU**: Rockchip RK3566 (quad Cortex-A55 @ 1.8GHz)
- **Display**: 1024×768 landscape, fb0, ARGB8888
- **OS**: SpruceOS (Brick/Flip variant), glibc 2.33
- **SSH**: `spruce@192.168.1.45`
- **App path**: `/mnt/SDCARD/App/GVU/`
- **Binary**: `gvu64` (aarch64)
- **Input**: `/dev/input/event3` — Xbox-style gamepad (EV_KEY + EV_ABS)

---

## Build Environment

### Docker image
```
cross-compile/trimui-brick/Dockerfile.gvu
```
- `FROM debian:bullseye` — **must be bullseye, not bookworm**. Bookworm's aarch64 sysroot pulls in GLIBC_2.34/2.35 symbols; device has glibc 2.33 → runtime crash with "GLIBC_2.35 not found". Bullseye sysroot max is 2.31.
- Cross-compiler: `gcc-aarch64-linux-gnu` / `g++-aarch64-linux-gnu`
- `CFLAGS_ARCH="-march=armv8-a -O2"`
- Libraries built from source (static): SDL2 2.26.5, libjpeg-turbo 2.1.5.1, libpng 1.6.39, FreeType 2.13.0, SDL2_image 2.6.3, SDL2_ttf 2.20.2, FFmpeg 5.1.6
- `zlib1g-dev` required in apt-get for libpng configure

### Build commands
```sh
cd ~/Projects/GVU/gvu

# Build Docker image (first time only)
env -u USER podman build -t gvu-brick \
  -f cross-compile/trimui-brick/Dockerfile.gvu \
  cross-compile/trimui-brick/

# Build binary
env -u USER podman run --rm -v "$(pwd):/gvu:z" localhost/gvu-brick:latest \
  sh /gvu/cross-compile/trimui-brick/build_inside_docker.sh

# Deploy (sftp bypasses dropbear wrapper's shell mangling)
sftp spruce@192.168.1.45 <<'EOF'
put build/gvu64 /mnt/SDCARD/App/GVU/gvu64
EOF
```

### Makefile targets
```
make trimui-brick-build     # build inside Docker
make trimui-brick-docker    # build Docker image
make trimui-brick-package VERSION=x.y.z   # build + zip
make trimui-brick-deploy    # sftp deploy
```

---

## Framebuffer

| Property | Value |
|---|---|
| Device | `/dev/fb0` |
| Resolution | 1024×768 |
| Format | ARGB8888 |
| Stride | 1024 px (4096 bytes/row) |
| Virtual size | 1024×16384 (21+ pages) |
| yoffset at launch | 768 (PyUI leaves page 1 displayed) |

**FBIOPAN_DISPLAY disabled** (`s_fb_pan_disabled = 1`): like the A30, this ioctl blocks for a full scan period (~16ms at 60Hz), causing frame drops on 720p60. GVU writes directly to the displayed page (yoffset=768) and accepts tearing. In practice, tearing is nearly invisible on landscape video.

**Wake grace period**: `brick_screen_wake()` sets `s_fb_wake_frames = 30` — if FBIOPAN_DISPLAY is ever re-enabled, skips it for 30 frames after wake in case the display controller is reinitializing.

**Alpha**: every pixel must have alpha=0xFF (bit 31 set) or it will be transparent. `brick_flip()` ORs `0xFF000000u` on all pixels. `brick_flip_video()` forces alpha on output pixels explicitly.

---

## Input (event3)

The device has **Nintendo-style physical button labels** (A=right, B=bottom, X=top, Y=left) but the evdev driver reports **Xbox-style button codes**. Mapping must account for this discrepancy:

| Physical label | Position | evdev code | SDL key |
|---|---|---|---|
| A | right | BTN_EAST (0x131) | SDLK_SPACE (confirm) |
| B | bottom | BTN_SOUTH (0x130) | SDLK_LCTRL (back) |
| X | top | BTN_NORTH (0x133) | SDLK_LALT (zoom/Y-action) |
| Y | left | BTN_WEST (0x134) | SDLK_LSHIFT (audio/X-action) |
| L1 | — | BTN_TL (0x136) | SDLK_PAGEUP |
| R1 | — | BTN_TR (0x137) | SDLK_PAGEDOWN |
| L2 | — | ABS_Z > 127 | SDLK_COMMA |
| R2 | — | ABS_RZ > 127 | SDLK_PERIOD |
| SELECT | — | BTN_SELECT (0x13A) | SDLK_RCTRL |
| START | — | BTN_START (0x13B) | SDLK_RETURN |
| MENU | — | BTN_MODE (0x13C) | SDLK_ESCAPE |
| Vol+ | — | KEY_VOLUMEUP (0x73) | SDLK_EQUALS |
| Vol- | — | KEY_VOLUMEDOWN (0x72) | SDLK_MINUS |

D-pad: `ABS_HAT0X` (±1) → LEFT/RIGHT, `ABS_HAT0Y` (±1) → UP/DOWN.

**Note**: X maps to SDLK_LALT and Y maps to SDLK_LSHIFT (seemingly reversed) because in main.c the LSHIFT key triggers "X" actions (cycle audio, force subtitle re-download) and LALT triggers "Y" actions (zoom cycle). This matches the physical button positions: top button (labeled X) drives LALT, left button (labeled Y) drives LSHIFT.

---

## System Paths (SpruceOS Brick/Flip)

| Resource | Path |
|---|---|
| Python 3.10 binary | `/mnt/SDCARD/spruce/flip/bin/python3` |
| PYTHONHOME | `/mnt/SDCARD/spruce/flip` |
| Battery capacity | `/sys/class/power_supply/axp2202-battery/capacity` |
| Battery status | `/sys/class/power_supply/axp2202-battery/status` |
| WiFi carrier | `/sys/class/net/wlan0/carrier` (1=connected) |

These differ from A30 paths. GVU uses `get_platform() == PLATFORM_BRICK` to select the correct paths at runtime.

---

## Video Pipeline

### landscape_direct (primary path for landscape content)
When `tex_w == 1024` (i.e., the decoded fit rect is full panel-width):
1. sws thread decodes YUV → BGRA at `tex_w × tex_h` directly
2. `player_update()` steals the AVFrame (no SDL_UpdateTexture)
3. `brick_flip_video(osd_ptr, frame->data[0], frame->height, zoom_t, brightness)` composites and blits to fb0
4. OSD re-rendered at most once per 4 frames (when visible)

This bypasses SDL's software texture upload and renderer entirely for the video path.

**Coverage**: 16:9 (1024×576), 4:3 (1024×768), 2.35:1 (1024×435), most landscape content.
**Fallback**: Portrait-source videos (tex_w < 1024) use the SDL renderer path.

### OSD Compositing (CRITICAL — alpha-based)

The SDL surface used for OSD capture is cleared with **alpha=0** (transparent) before rendering. `brick_surface_to_bgra()` copies pixels as-is without modifying alpha.

`brick_flip_video()` composites using the **alpha channel** as the selector:
- `alpha == 0` → use video pixel (empty OSD area)
- `alpha > 0` → use OSD pixel (text, backgrounds, bars)

**Do NOT use RGB test** (`ui & 0x00FFFFFFu`): OSD backgrounds are pure-black (RGB=0,0,0) and would be invisible if tested by RGB.

The SDL brightness dimming overlay (`SDL_RenderFillRect` with `BLENDMODE_BLEND`) is **skipped** for landscape_direct — applying it over alpha=0 transparent pixels makes them opaque, which `brick_flip_video` then treats as OSD content, showing black over video. Brightness is applied directly in `brick_flip_video` instead.

### Zoom (bilinear)

The zoom path in `brick_flip_video` uses **bilinear interpolation** for smooth scaling:
- Vertical: fractional source row computed as float, interpolated between `src_row0` and `src_row1`
- Horizontal: fixed-point column stepping (`col_start_fp + c * col_step_fp`, 8.8 format) — fast integer accumulate, no per-pixel float multiply
- Both dimensions lerped with `lerp_pixel()` (per-channel 8-bit fixed-point multiply)

FIT mode (zoom_t=0) uses `memcpy` at full brightness and a per-pixel brightness loop when dimmed.

### Brightness

Passed as `float brightness` to `brick_flip_video`. Applied via `apply_brightness(px, bri256)` which scales R, G, B channels by `bri256/256` (fixed-point), preserving alpha. Applied to both video pixels and OSD pixels.

### Volume OSD

The Brick has its own system-level volume OSD. GVU's volume overlay is suppressed (`#ifdef GVU_TRIMUI_BRICK` — only show when `p->muted`). The mute indicator is preserved.

### SDL renderer path (fallback)
- sws → SDL_UpdateTexture → SDL_RenderCopy → brick_flip(hw_surf)
- Used for portrait-source video and all non-playback UI
- SDL brightness overlay works correctly on this path (surface cleared with alpha=0xFF)

---

## Platform Detection
`platform.c` detects `PLATFORM_BRICK` via "TG3040" in `/proc/cpuinfo`.
`platform_name()` returns `"TrimUI Brick"`.

---

## Deployment Notes
- Dropbear SSH wrapper on the Brick mangles shell special characters (`;`, `&&`, `|`, `>`). Always use `sftp` for file transfer, not `scp` with shell commands.
- The binary is ~37MB (fully static except libz). Transfer takes ~60-90s over WiFi.
- No VERNEED patching needed (device glibc 2.33 is new enough; bullseye cross-compiler sysroot max is 2.31).

---

## Known Issues / Future Work
- Portrait-source video falls back to SDL renderer path (no landscape_direct). Rare in practice.
- Subtitle search requires SpruceOS Python — not portable to MinUI/NextUI. Long-term: replace `fetch_subtitles.py` with a compiled C binary using libcurl.
