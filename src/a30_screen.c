#ifdef GVU_A30
/* -------------------------------------------------------------------------
 * a30_screen.c — Miyoo A30 framebuffer + evdev input
 *
 * Display topology
 * ----------------
 *   Physical panel  : 480 × 640, landscape-mounted (wired as 480 wide × 640 tall)
 *   GVU canvas      : 640 × 480 (landscape game-like layout)
 *   Required rotation: 90° CCW to map GVU's (x,y) → panel's (col, row)
 *
 *     GVU pixel (x, y)  →  panel pixel (y, W-1-x)
 *     where W = GVU canvas width = 640
 *
 *   fb0 is the overlay layer; PyUI leaves yoffset = 640 (page 1 shown).
 *   We write to page 1 (offset = fb_yoffset * fb_stride_px).
 *   Every pixel must have alpha = 0xFF (overlay transparency).
 *
 * Button / key mapping (A30 SpruceOS)
 * ------------------------------------
 *   A       KEY_SPACE      (57)  → SDLK_SPACE
 *   B       KEY_LEFTCTRL   (29)  → SDLK_LCTRL
 *   X       KEY_LEFTSHIFT  (42)  → SDLK_LSHIFT
 *   Y       KEY_LEFTALT    (56)  → SDLK_LALT
 *   L1      KEY_TAB        (15)  → SDLK_PAGEUP
 *   R1      KEY_BACKSPACE  (14)  → SDLK_PAGEDOWN
 *   L2      KEY_E          (18)  → SDLK_COMMA
 *   R2      KEY_T          (20)  → SDLK_PERIOD
 *   SELECT  KEY_RIGHTCTRL  (97)  → SDLK_RCTRL
 *   START   KEY_ENTER      (28)  → SDLK_RETURN
 *   MENU    KEY_ESC         (1)  → SDLK_ESCAPE
 *   UP      KEY_UP         (103) → SDLK_UP
 *   DOWN    KEY_DOWN       (108) → SDLK_DOWN
 *   LEFT    KEY_LEFT       (105) → SDLK_LEFT
 *   RIGHT   KEY_RIGHT      (106) → SDLK_RIGHT
 * ---------------------------------------------------------------------- */

#include "a30_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <errno.h>
#include <SDL2/SDL.h>
#ifdef __ARM_NEON__
#include <arm_neon.h>
#endif

/* -------------------------------------------------------------------------
 * Framebuffer state
 * ---------------------------------------------------------------------- */

#define FB_DEVICE   "/dev/fb0"
#define INPUT_DEV   "/dev/input/event3"

/* GVU canvas dimensions (SDL surface we receive) */
#define GVU_W  640
#define GVU_H  480

/* Physical panel dimensions after rotation */
#define PANEL_W  480
#define PANEL_H  640

static int      s_fb_fd          = -1;
static Uint32  *s_fb_mem         = NULL;   /* mmap base */
static size_t   s_fb_size        = 0;
static int      s_fb_stride      = PANEL_W; /* pixels per row */
static int      s_fb_yoffset     = 0;       /* currently displayed page (rows) */
static int      s_fb_back_yoff   = 0;       /* back buffer page we write to (rows) */
/* FBIOPAN_DISPLAY blocks for a full display-scan period (~28ms) when called
 * shortly after vsync, because the fast rotation finishes near the TOP of the
 * scan rather than the bottom.  Disabling pan (always writing to the
 * displayed page) eliminates this wait entirely: flip drops from ~28ms to
 * ~3ms, the main loop runs at 60fps, and the pipeline reaches ~87% speed on
 * 720p 60fps content.  Tearing is the trade-off; for video it is subtle. */
static int      s_fb_pan_disabled = 1;      /* 1 = direct write, no FBIOPAN_DISPLAY */

static int      s_input_fd    = -1;

/* -------------------------------------------------------------------------
 * a30_screen_init
 * ---------------------------------------------------------------------- */

int a30_screen_init(void) {
    /* --- Open framebuffer --- */
    s_fb_fd = open(FB_DEVICE, O_RDWR);
    if (s_fb_fd < 0) {
        perror("a30_screen_init: open " FB_DEVICE);
        return -1;
    }

    /* Query variable screen info for yoffset + actual stride */
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("a30_screen_init: ioctl FBIOGET_*SCREENINFO");
        close(s_fb_fd); s_fb_fd = -1;
        return -1;
    }

    /* stride in pixels; finfo.line_length is bytes per line */
    s_fb_stride = (int)(finfo.line_length / (vinfo.bits_per_pixel / 8));

    s_fb_size = (size_t)finfo.smem_len;
    s_fb_mem  = (Uint32 *)mmap(NULL, s_fb_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, s_fb_fd, 0);
    if (s_fb_mem == MAP_FAILED) {
        perror("a30_screen_init: mmap fb0");
        close(s_fb_fd); s_fb_fd = -1; s_fb_mem = NULL;
        return -1;
    }

    /* PyUI leaves yoffset = 640 (page 1 displayed).
       We write to the hidden page and use FBIOPAN_DISPLAY to flip —
       this eliminates tearing without touching FBIOPUT_VSCREENINFO. */
    s_fb_yoffset   = (int)vinfo.yoffset;
    s_fb_back_yoff = (s_fb_yoffset == 0) ? PANEL_H : 0;

    fprintf(stderr, "a30_screen: fb0 %dx%d bpp=%d stride=%d yoff=%d back_yoff=%d\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
            s_fb_stride, s_fb_yoffset, s_fb_back_yoff);

    /* --- Open evdev input --- */
    s_input_fd = open(INPUT_DEV, O_RDONLY | O_NONBLOCK);
    if (s_input_fd < 0) {
        /* Non-fatal: fall back to no hardware input */
        perror("a30_screen_init: open " INPUT_DEV " (non-fatal)");
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * a30_flip  — rotate 90° CCW into the back buffer, then FBIOPAN_DISPLAY
 *
 * Double-buffering: we always write to the hidden page and pan the display
 * controller to it only after the write is complete. This eliminates tearing
 * without touching FBIOPUT_VSCREENINFO (which would break the overlay).
 *
 * GVU pixel (x, y)  →  panel pixel at row=(GVU_W-1-x), col=y
 *   panel index = (GVU_W-1-x) * s_fb_stride + y
 * ---------------------------------------------------------------------- */

void a30_flip(SDL_Surface *surface) {
    if (!s_fb_mem || !surface) return;

    SDL_LockSurface(surface);

    const Uint32 *src   = (const Uint32 *)surface->pixels;
    const int     pitch = surface->pitch / 4;   /* pixels per row */

    /* Post-wake: write directly to the displayed (front) page — no back
     * buffer, no memcpy, no FBIOPAN_DISPLAY.  Tearing is acceptable.
     * Normal: write to the hidden back buffer page, then pan. */
    int dst_yoff = s_fb_pan_disabled ? s_fb_yoffset : s_fb_back_yoff;
    Uint32 *dst  = s_fb_mem + (size_t)dst_yoff * (size_t)s_fb_stride;

    /* 90° CCW rotation: src GVU_W×GVU_H (landscape) → dst PANEL_W×PANEL_H
     *
     * dst pixel (r, c)  =  src pixel (c, GVU_W-1-r)
     *
     * Loop order: outer = dst rows (r), inner = dst cols (c).
     * This writes SEQUENTIALLY to each dst row, which is critical for
     * non-cached framebuffer memory: sequential writes fill the CPU write
     * buffer and flush in bursts instead of per-pixel bus transactions. */
    for (int r = 0; r < PANEL_H; r++) {
        const int src_x = GVU_W - 1 - r;   /* src column for this dst row */
        Uint32 *out = dst + (size_t)r * (size_t)s_fb_stride;
#ifdef __ARM_NEON__
        /* Process 8 pixels at a time: 8 scalar loads + 2 NEON 16-byte stores.
         * Two back-to-back vst1q_u32 fill one 32-byte AXI burst — matching the
         * bus width of the Cortex-A7 memory interconnect for maximum throughput
         * to the uncached framebuffer.
         * Prefetch the next 8 source rows ahead to hide strided-load latency.
         * PANEL_W=480 is divisible by 8 so no tail loop is needed. */
        const uint32x4_t alpha_v = vdupq_n_u32(0xFF000000u);
        for (unsigned int c = 0; c < (unsigned)PANEL_W; c += 8) {
            __builtin_prefetch(&src[(c+8) * pitch + src_x], 0, 0);
            uint32_t tmp[8];
            tmp[0] = src[(c+0) * pitch + src_x];
            tmp[1] = src[(c+1) * pitch + src_x];
            tmp[2] = src[(c+2) * pitch + src_x];
            tmp[3] = src[(c+3) * pitch + src_x];
            tmp[4] = src[(c+4) * pitch + src_x];
            tmp[5] = src[(c+5) * pitch + src_x];
            tmp[6] = src[(c+6) * pitch + src_x];
            tmp[7] = src[(c+7) * pitch + src_x];
            vst1q_u32(out + c,     vorrq_u32(vld1q_u32(tmp),   alpha_v));
            vst1q_u32(out + c + 4, vorrq_u32(vld1q_u32(tmp+4), alpha_v));
        }
#else
        for (int c = 0; c < PANEL_W; c++) {
            out[c] = src[c * pitch + src_x] | 0xFF000000u;
        }
#endif
    }

    SDL_UnlockSurface(surface);

    if (!s_fb_pan_disabled) {
        /* Normal: pan display controller to the back buffer page. */
        struct fb_var_screeninfo vinfo;
        if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
            vinfo.yoffset = (uint32_t)s_fb_back_yoff;
            if (ioctl(s_fb_fd, FBIOPAN_DISPLAY, &vinfo) == 0) {
                s_fb_yoffset   = s_fb_back_yoff;
                s_fb_back_yoff = (s_fb_yoffset == 0) ? PANEL_H : 0;
            }
        }
    }
    /* Post-wake: already wrote directly to front page; nothing more to do. */
}

/* -------------------------------------------------------------------------
 * a30_surface_to_portrait — pre-rotate SDL landscape surface to portrait BGRA
 *
 * Rotates the 640×480 SDL surface 90°CCW into a 480×640 portrait heap buffer
 * using the same NEON transform as a30_flip().  The resulting buffer can be
 * passed to a30_flip_video() so that composite reads are sequential instead of
 * strided — eliminating the L1 cache thrash from reading a column out of the
 * SDL surface row by row.
 *
 * Typical cost: ~3ms.  Call once per OSD-dirty frame, then reuse for N frames.
 * ---------------------------------------------------------------------- */

void a30_surface_to_portrait(SDL_Surface *surf, Uint32 *out) {
    if (!surf || !out) return;
    SDL_LockSurface(surf);
    const Uint32 *src   = (const Uint32 *)surf->pixels;
    const int     pitch = surf->pitch / 4;
#ifdef __ARM_NEON__
    const uint32x4_t alpha_v = vdupq_n_u32(0xFF000000u);
    for (int r = 0; r < PANEL_H; r++) {
        const int src_x    = GVU_W - 1 - r;
        Uint32   *row_out  = out + (size_t)r * (size_t)PANEL_W;
        /* PANEL_W=480 is divisible by 8 — no tail loop needed. */
        for (unsigned int c = 0; c < (unsigned)PANEL_W; c += 8) {
            __builtin_prefetch(&src[(c+8) * pitch + src_x], 0, 0);
            uint32_t tmp[8];
            tmp[0] = src[(c+0) * pitch + src_x];
            tmp[1] = src[(c+1) * pitch + src_x];
            tmp[2] = src[(c+2) * pitch + src_x];
            tmp[3] = src[(c+3) * pitch + src_x];
            tmp[4] = src[(c+4) * pitch + src_x];
            tmp[5] = src[(c+5) * pitch + src_x];
            tmp[6] = src[(c+6) * pitch + src_x];
            tmp[7] = src[(c+7) * pitch + src_x];
            vst1q_u32(row_out + c,     vorrq_u32(vld1q_u32(tmp),   alpha_v));
            vst1q_u32(row_out + c + 4, vorrq_u32(vld1q_u32(tmp+4), alpha_v));
        }
    }
#else
    for (int r = 0; r < PANEL_H; r++) {
        const int src_x   = GVU_W - 1 - r;
        Uint32   *row_out = out + (size_t)r * (size_t)PANEL_W;
        for (int c = 0; c < PANEL_W; c++)
            row_out[c] = src[c * pitch + src_x] | 0xFF000000u;
    }
#endif
    SDL_UnlockSurface(surf);
}

/* -------------------------------------------------------------------------
 * a30_flip_video — portrait-direct blit for video frames
 *
 * The sws thread's yuv420p_to_portrait_bgra_2x() kernel produces a
 * portrait-format BGRA buffer (width=port_w, height=PANEL_H).  Each buffer
 * row corresponds directly to one panel row, so writing it to the fb is a
 * simple sequential blit with no rotation.
 *
 * osd_portrait=NULL: pure sequential portrait blit + black letterboxes (~3ms).
 * osd_portrait!=NULL: single-pass composite — each fb row written once in order,
 *   reading video from portrait_bgra and OSD from osd_portrait sequentially.
 *   Both source buffers are portrait-format so all reads are sequential,
 *   eliminating the strided-column L1 cache misses of the old SDL path (~2ms).
 * ---------------------------------------------------------------------- */

void a30_flip_video(const Uint32 *osd_portrait,
                    const Uint32 *portrait_bgra,
                    int port_w, float zoom_t) {
    if (!s_fb_mem || !portrait_bgra) return;

    /* Zoom geometry (uniform scale, aspect preserved):
     *   display_w  — panel columns used by video (port_w at FIT, PANEL_W at FILL)
     *   col_off    — black columns on each side
     *   src_rows   — portrait buffer rows used (crops top/bottom symmetrically)
     *   row_off    — first source row (centres the vertical crop)
     * At zoom_t=0 all values reduce to the 1:1 FIT case. */
    int display_w = port_w + (int)((PANEL_W - port_w) * zoom_t + 0.5f);
    if (display_w > PANEL_W) display_w = PANEL_W;
    int col_off  = (PANEL_W - display_w) / 2;
    int src_rows = PANEL_H * port_w / display_w;
    if (src_rows > PANEL_H) src_rows = PANEL_H;
    int row_off  = (PANEL_H - src_rows) / 2;

    int dst_yoff = s_fb_yoffset;
    Uint32 *fb   = s_fb_mem + (size_t)dst_yoff * (size_t)s_fb_stride;

    /* FIT fast path (zoom_t≈0): memcpy each row, no scale arithmetic. */
    if (zoom_t < 0.001f) {
        if (!osd_portrait) {
            /* No OSD: pure sequential blit with black letterboxes (~3ms). */
            for (int r = 0; r < PANEL_H; r++) {
                Uint32 *out = fb + (size_t)r * (size_t)s_fb_stride;
                for (int c = 0; c < col_off; c++)
                    out[c] = 0xFF000000u;
                memcpy(out + col_off,
                       portrait_bgra + (size_t)r * (size_t)port_w,
                       (size_t)port_w * sizeof(Uint32));
                for (int c = col_off + port_w; c < PANEL_W; c++)
                    out[c] = 0xFF000000u;
            }
            return;
        }
        /* FIT + OSD composite: sequential reads from pre-rotated portrait OSD.
         * All reads from osd_portrait and portrait_bgra are sequential —
         * no strided column access, no L1 cache thrash (~2ms). */
#ifdef __ARM_NEON__
        const uint32x4_t rgb_mask = vdupq_n_u32(0x00FFFFFFu);
#endif
        for (int r = 0; r < PANEL_H; r++) {
            Uint32       *out      = fb + (size_t)r * (size_t)s_fb_stride;
            const Uint32 *osd_row  = osd_portrait  + (size_t)r * (size_t)PANEL_W;
            const Uint32 *port_row = portrait_bgra  + (size_t)r * (size_t)port_w;
            /* Left letterbox */
            for (int c = 0; c < col_off; c++) {
                Uint32 ui = osd_row[c];
                out[c] = (ui & 0x00FFFFFFu) ? ui : 0xFF000000u;
            }
            /* Video region: NEON 4-wide blend where possible */
#ifdef __ARM_NEON__
            int neon_end = col_off + (port_w & ~3);
            for (int c = col_off; c < neon_end; c += 4) {
                uint32x4_t osd = vld1q_u32(osd_row  + c);
                uint32x4_t vid = vld1q_u32(port_row + c - col_off);
                /* sel: all-ones lanes where OSD pixel is non-black */
                uint32x4_t sel = vtstq_u32(osd, rgb_mask);
                vst1q_u32(out + c, vbslq_u32(sel, osd, vid));
            }
            /* Scalar tail for port_w not divisible by 4 */
            for (int c = neon_end; c < col_off + port_w; c++) {
                Uint32 ui = osd_row[c];
                out[c] = (ui & 0x00FFFFFFu) ? ui : port_row[c - col_off];
            }
#else
            for (int c = col_off; c < col_off + port_w; c++) {
                Uint32 ui = osd_row[c];
                out[c] = (ui & 0x00FFFFFFu) ? ui : port_row[c - col_off];
            }
#endif
            /* Right letterbox */
            for (int c = col_off + port_w; c < PANEL_W; c++) {
                Uint32 ui = osd_row[c];
                out[c] = (ui & 0x00FFFFFFu) ? ui : 0xFF000000u;
            }
        }
        return;
    }

    /* Zoom path (WIDE/FILL): nearest-neighbour scale from portrait buffer.
     * OSD pixels from pre-rotated portrait buffer are independent of zoom —
     * the OSD is a full-panel overlay composited over whatever video pixel
     * is underneath. */
    for (int r = 0; r < PANEL_H; r++) {
        int src_row = row_off + r * src_rows / PANEL_H;
        const Uint32 *port_row = portrait_bgra  + (size_t)src_row * (size_t)port_w;
        const Uint32 *osd_row  = osd_portrait ? osd_portrait + (size_t)r * (size_t)PANEL_W : NULL;
        Uint32       *out      = fb + (size_t)r * (size_t)s_fb_stride;

        for (int c = 0; c < PANEL_W; c++) {
            Uint32 ui = osd_row ? osd_row[c] : 0u;
            if (osd_row && (ui & 0x00FFFFFFu)) {
                out[c] = ui;
            } else if (c < col_off || c >= col_off + display_w) {
                out[c] = 0xFF000000u;
            } else {
                out[c] = port_row[(c - col_off) * port_w / display_w];
            }
        }
    }
}

void a30_screen_wake(void) {
    if (!s_fb_pan_disabled)
        fprintf(stderr, "a30_screen_wake: FBIOPAN_DISPLAY disabled for rest of session\n");
    s_fb_pan_disabled = 1;
}

/* -------------------------------------------------------------------------
 * Key mapping table
 * ---------------------------------------------------------------------- */

typedef struct {
    int       linux_code;
    SDL_Keycode sdl_sym;
} KeyMap;

static const KeyMap s_keymap[] = {
    { KEY_SPACE,     SDLK_SPACE    },   /* A        */
    { KEY_LEFTCTRL,  SDLK_LCTRL   },   /* B        */
    { KEY_LEFTSHIFT, SDLK_LSHIFT  },   /* X        */
    { KEY_LEFTALT,   SDLK_LALT    },   /* Y        */
    { KEY_TAB,       SDLK_PAGEUP  },   /* L1       */
    { KEY_BACKSPACE, SDLK_PAGEDOWN},   /* R1       */
    { KEY_E,         SDLK_COMMA   },   /* L2       */
    { KEY_T,         SDLK_PERIOD  },   /* R2       */
    { KEY_RIGHTCTRL, SDLK_RCTRL   },   /* SELECT   */
    { KEY_ENTER,     SDLK_RETURN  },   /* START    */
    { KEY_ESC,       SDLK_ESCAPE  },   /* MENU     */
    { KEY_UP,        SDLK_UP      },   /* D-pad UP */
    { KEY_DOWN,      SDLK_DOWN    },   /* D-pad DN */
    { KEY_LEFT,      SDLK_LEFT    },   /* D-pad LT */
    { KEY_RIGHT,      SDLK_RIGHT   },   /* D-pad RT */
    { KEY_VOLUMEUP,   SDLK_EQUALS },   /* Vol+     */
    { KEY_VOLUMEDOWN, SDLK_MINUS  },   /* Vol-     */
    { 0,              0           },   /* sentinel */
};

static SDL_Keycode lookup_keycode(int linux_code) {
    for (int i = 0; s_keymap[i].linux_code; i++) {
        if (s_keymap[i].linux_code == linux_code)
            return s_keymap[i].sdl_sym;
    }
    return SDLK_UNKNOWN;
}

/* -------------------------------------------------------------------------
 * a30_poll_events — drain evdev, inject SDL2 keyboard events
 *
 * gpio-keys-polled never generates EV_REP (value=2), so we implement
 * key-repeat ourselves: 300ms initial delay, then one repeat every 80ms.
 * Only navigation keys repeat; action buttons fire once per physical press.
 * ---------------------------------------------------------------------- */

#define KEY_REPEAT_DELAY_MS  300
#define KEY_REPEAT_PERIOD_MS  80

static SDL_Keycode s_held_key    = SDLK_UNKNOWN;
static Uint32      s_held_since  = 0;
static Uint32      s_last_repeat = 0;

static int is_repeatable(SDL_Keycode sym) {
    return sym == SDLK_UP || sym == SDLK_DOWN ||
           sym == SDLK_LEFT || sym == SDLK_RIGHT;
}

static void push_key(SDL_Keycode sym, int repeat) {
    SDL_Event sdl_ev;
    memset(&sdl_ev, 0, sizeof(sdl_ev));
    sdl_ev.type            = SDL_KEYDOWN;
    sdl_ev.key.type        = SDL_KEYDOWN;
    sdl_ev.key.state       = SDL_PRESSED;
    sdl_ev.key.repeat      = repeat;
    sdl_ev.key.keysym.sym      = sym;
    sdl_ev.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
    sdl_ev.key.keysym.mod      = KMOD_NONE;
    SDL_PushEvent(&sdl_ev);
}

void a30_poll_events(void) {
    if (s_input_fd < 0) return;

    struct input_event ev;
    while (read(s_input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type != EV_KEY) continue;
        if (ev.value != 0 && ev.value != 1) continue;  /* ignore OS repeats */

        SDL_Keycode sym = lookup_keycode(ev.code);
        if (sym == SDLK_UNKNOWN) continue;

        if (ev.value == 1) {  /* key down */
            push_key(sym, 0);
            if (is_repeatable(sym)) {
                s_held_key    = sym;
                s_held_since  = SDL_GetTicks();
                s_last_repeat = s_held_since;
            }
        } else {              /* key up */
            SDL_Event sdl_ev;
            memset(&sdl_ev, 0, sizeof(sdl_ev));
            sdl_ev.type            = SDL_KEYUP;
            sdl_ev.key.type        = SDL_KEYUP;
            sdl_ev.key.state       = SDL_RELEASED;
            sdl_ev.key.keysym.sym      = sym;
            sdl_ev.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
            sdl_ev.key.keysym.mod      = KMOD_NONE;
            SDL_PushEvent(&sdl_ev);
            if (s_held_key == sym)
                s_held_key = SDLK_UNKNOWN;
        }
    }

    /* Inject software repeats for held navigation keys */
    if (s_held_key != SDLK_UNKNOWN) {
        Uint32 now = SDL_GetTicks();
        if (now - s_held_since >= KEY_REPEAT_DELAY_MS &&
            now - s_last_repeat >= KEY_REPEAT_PERIOD_MS) {
            push_key(s_held_key, 1);
            s_last_repeat = now;
        }
    }
}

/* -------------------------------------------------------------------------
 * a30_screen_close
 * ---------------------------------------------------------------------- */

void a30_screen_close(void) {
    if (s_input_fd >= 0) { close(s_input_fd); s_input_fd = -1; }
    if (s_fb_mem && s_fb_mem != MAP_FAILED) {
        munmap(s_fb_mem, s_fb_size);
        s_fb_mem = NULL;
    }
    if (s_fb_fd >= 0) { close(s_fb_fd); s_fb_fd = -1; }
}

#endif /* GVU_A30 */
