#ifdef GVU_TRIMUI_BRICK
/* -------------------------------------------------------------------------
 * brick_screen.c — Trimui Brick framebuffer + evdev input
 *
 * Display topology
 * ----------------
 *   Physical panel  : 1024 × 768, landscape
 *   GVU canvas      : 1024 × 768 (matches panel — no rotation required)
 *   Pixel format    : ARGB8888 (alpha at [31:24], R [23:16], G [15:8], B [7:0])
 *                     In memory (LE): B G R A  →  ImageMagick "BGRA"
 *
 *   fb0 is the primary display layer.
 *   PyUI leaves yoffset = 768 (page 1 shown).
 *   We write directly to the displayed page (no FBIOPAN_DISPLAY).
 *   Every pixel must have alpha = 0xFF.
 *
 * Button / key mapping (Trimui Brick — Nintendo-style labels, Xbox evdev codes)
 * ---------------------------------------------------------------------------
 *   The physical buttons show Nintendo labels (A=right, B=bottom, X=top, Y=left)
 *   but the evdev driver reports Xbox button codes.  Map accordingly:
 *   D-pad  ABS_HAT0X / ABS_HAT0Y  →  SDLK_LEFT/RIGHT / SDLK_UP/DOWN
 *   A      BTN_EAST   (0x131)      →  SDLK_SPACE   (confirm — right button)
 *   B      BTN_SOUTH  (0x130)      →  SDLK_LCTRL   (back — bottom button)
 *   X      BTN_NORTH  (0x133)      →  SDLK_LALT    (top button — Xbox "Y" code, Nintendo "X" label)
 *   Y      BTN_WEST   (0x134)      →  SDLK_LSHIFT  (left button — Xbox "X" code, Nintendo "Y" label)
 *   L1     BTN_TL     (0x136)      →  SDLK_PAGEUP
 *   R1     BTN_TR     (0x137)      →  SDLK_PAGEDOWN
 *   L2     ABS_Z analog (>127)     →  SDLK_COMMA   (threshold)
 *   R2     ABS_RZ analog (>127)    →  SDLK_PERIOD  (threshold)
 *   SELECT BTN_SELECT (0x13A)      →  SDLK_RCTRL
 *   START  BTN_START  (0x13B)      →  SDLK_RETURN
 *   MENU   BTN_MODE   (0x13C)      →  SDLK_ESCAPE
 *   Vol+   KEY_VOLUMEUP   (0x73)   →  SDLK_EQUALS
 *   Vol-   KEY_VOLUMEDOWN (0x72)   →  SDLK_MINUS
 * ---------------------------------------------------------------------- */

#include "brick_screen.h"

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
#include <poll.h>
#include <SDL2/SDL.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

/* -------------------------------------------------------------------------
 * Framebuffer state
 * ---------------------------------------------------------------------- */

#define FB_DEVICE   "/dev/fb0"
/* Input device path: resolved at runtime from g_input_dev (set by platform_init_from_env) */

static int      s_fb_fd         = -1;
static Uint32  *s_fb_mem        = NULL;
static size_t   s_fb_size       = 0;
static int      s_fb_stride     = 0;         /* pixels per row; set by brick_screen_init */
static int      s_fb_yoffset    = 0;        /* currently displayed page (rows) */
static int      s_fb_back_yoff  = 0;        /* back buffer page we write to (rows) */
/* FBIOPAN_DISPLAY blocks ~16ms at 60Hz (one scan period) on this driver,
 * which is acceptable for ≤30fps content.  At 60fps it caps output to ~30fps.
 * Enable by default for tear-free output; disable at runtime if frame drops
 * are detected on high-fps content. */
static int      s_fb_pan_disabled = 0;
/* Wake grace period: if pan is re-enabled, skip FBIOPAN_DISPLAY briefly. */
static int      s_fb_wake_frames = 0;
#define WAKE_GRACE_FRAMES 30   /* ~0.5s at 60fps */

static int      s_use_drm     = 0;  /* 1 when DRM/KMS path is active */

static int      s_input_fd    = -1;

static void brick_pageflip(void);   /* forward declaration */

/* =========================================================================
 * DRM/KMS page-flip path — for single-page fb0 devices (e.g. Miyoo Flip)
 *
 * Uses raw kernel DRM UAPI ioctls — no libdrm link dependency.
 * Struct layouts match the stable kernel UAPI (unchanged since Linux 3.x).
 * Activated by flip_drm_init() in brick_screen_init(); falls back to fb0.
 * ========================================================================= */

/* --- Minimal kernel DRM UAPI struct definitions (stable ABI) --- */
#define GVU_DRM_DISPLAY_MODE_LEN   32
#define GVU_DRM_MODE_CONNECTED      1
#define GVU_DRM_MODE_PAGE_FLIP_EVENT 0x01u
#define GVU_DRM_EVENT_FLIP_COMPLETE  0x02u

struct gvu_drm_mode_modeinfo {   /* sizeof = 68 */
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char     name[GVU_DRM_DISPLAY_MODE_LEN];
};
struct gvu_drm_mode_card_res {   /* sizeof = 64 */
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct gvu_drm_mode_get_connector { /* sizeof = 80 */
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct gvu_drm_mode_get_encoder { /* sizeof = 20 */
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};
struct gvu_drm_mode_crtc {        /* sizeof = 104 */
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct gvu_drm_mode_modeinfo mode;
};
struct gvu_drm_mode_fb_cmd {      /* sizeof = 28 */
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct gvu_drm_mode_create_dumb { /* sizeof = 32 */
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};
struct gvu_drm_mode_map_dumb {    /* sizeof = 16 */
    uint32_t handle, pad;
    uint64_t offset;
};
struct gvu_drm_mode_destroy_dumb { /* sizeof = 4 */
    uint32_t handle;
};
struct gvu_drm_mode_page_flip {   /* sizeof = 24 */
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
};
struct gvu_drm_event {            /* sizeof = 8 */
    uint32_t type, length;
};

/* Ioctl numbers — _IOWR('d', cmd, struct) embeds sizeof at compile time */
#define GVU_DRM_IOCTL_MODE_GETRESOURCES \
    _IOWR('d', 0xA0, struct gvu_drm_mode_card_res)
#define GVU_DRM_IOCTL_MODE_GETCRTC \
    _IOWR('d', 0xA1, struct gvu_drm_mode_crtc)
#define GVU_DRM_IOCTL_MODE_SETCRTC \
    _IOWR('d', 0xA2, struct gvu_drm_mode_crtc)
#define GVU_DRM_IOCTL_MODE_GETENCODER \
    _IOWR('d', 0xA6, struct gvu_drm_mode_get_encoder)
#define GVU_DRM_IOCTL_MODE_GETCONNECTOR \
    _IOWR('d', 0xA7, struct gvu_drm_mode_get_connector)
#define GVU_DRM_IOCTL_MODE_ADDFB \
    _IOWR('d', 0xAE, struct gvu_drm_mode_fb_cmd)
#define GVU_DRM_IOCTL_MODE_RMFB \
    _IOWR('d', 0xAF, unsigned int)
#define GVU_DRM_IOCTL_MODE_PAGE_FLIP \
    _IOWR('d', 0xB0, struct gvu_drm_mode_page_flip)
#define GVU_DRM_IOCTL_MODE_CREATE_DUMB \
    _IOWR('d', 0xB2, struct gvu_drm_mode_create_dumb)
#define GVU_DRM_IOCTL_MODE_MAP_DUMB \
    _IOWR('d', 0xB3, struct gvu_drm_mode_map_dumb)
#define GVU_DRM_IOCTL_MODE_DESTROY_DUMB \
    _IOWR('d', 0xB4, struct gvu_drm_mode_destroy_dumb)

/* --- DRM/KMS state --- */
static int      s_drm_fd         = -1;
static uint32_t s_drm_crtc_id    = 0;
static uint32_t s_drm_conn_id    = 0;
static struct gvu_drm_mode_modeinfo s_drm_mode;
static uint32_t s_drm_fb[2]      = {0, 0};
static uint32_t s_drm_handle[2]  = {0, 0};
static Uint32  *s_drm_buf[2]     = {NULL, NULL};
static uint32_t s_drm_pitch      = 0;   /* bytes per row */
static int      s_drm_back       = 1;   /* index of back buffer (front=0 shown first) */
static size_t   s_drm_buf_size   = 0;

/* --- DRM helpers --- */

static void flip_drm_close(void) {
    for (int b = 0; b < 2; b++) {
        if (s_drm_buf[b] && s_drm_buf[b] != MAP_FAILED) {
            munmap(s_drm_buf[b], s_drm_buf_size);
            s_drm_buf[b] = NULL;
        }
        if (s_drm_fb[b]) {
            unsigned int fbid = s_drm_fb[b];
            ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_RMFB, &fbid);
            s_drm_fb[b] = 0;
        }
        if (s_drm_handle[b]) {
            struct gvu_drm_mode_destroy_dumb dd = { s_drm_handle[b] };
            ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            s_drm_handle[b] = 0;
        }
    }
    if (s_drm_fd >= 0) { close(s_drm_fd); s_drm_fd = -1; }
}

/* Queue a DRM page flip to the back buffer and wait for vsync completion. */
static void flip_drm_pageflip(void) {
    struct gvu_drm_mode_page_flip pf = {
        .crtc_id  = s_drm_crtc_id,
        .fb_id    = s_drm_fb[s_drm_back],
        .flags    = GVU_DRM_MODE_PAGE_FLIP_EVENT,
    };
    if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_PAGE_FLIP, &pf) < 0) return;

    /* Wait up to 50ms for the flip-complete event (vsync) */
    struct pollfd pfd = { .fd = s_drm_fd, .events = POLLIN };
    if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
        /* Drain the event — we only care that it arrived */
        char evbuf[64];
        (void)read(s_drm_fd, evbuf, sizeof(evbuf));
    }
    s_drm_back ^= 1;
}

/* Initialise DRM/KMS: find connector+CRTC, allocate dumb buffers, set mode.
 * Returns 0 on success, -1 on any failure (caller falls back to fb0). */
static int flip_drm_init(void) {
    s_drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (s_drm_fd < 0) {
        fprintf(stderr, "flip_drm_init: open /dev/dri/card0 failed: %s\n", strerror(errno));
        return -1;
    }

    /* Get display resources */
    struct gvu_drm_mode_card_res res = {0};
    if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "flip_drm_init: GETRESOURCES failed: %s\n", strerror(errno));
        goto fail;
    }

    fprintf(stderr, "flip_drm_init: res: conns=%u crtcs=%u\n",
            res.count_connectors, res.count_crtcs);
    uint32_t nc = res.count_connectors ? res.count_connectors : 1;
    uint32_t nt = res.count_crtcs      ? res.count_crtcs      : 1;
    uint32_t *conn_ids = (uint32_t *)calloc(nc, sizeof(uint32_t));
    uint32_t *crtc_ids = (uint32_t *)calloc(nt, sizeof(uint32_t));
    if (!conn_ids || !crtc_ids) {
        fprintf(stderr, "flip_drm_init: calloc failed\n");
        free(conn_ids); free(crtc_ids); goto fail;
    }
    /* Zero the struct before second call — kernel rejects non-NULL unused ptr fields */
    {
        uint32_t nc2 = res.count_connectors, nt2 = res.count_crtcs;
        memset(&res, 0, sizeof(res));
        res.count_connectors   = nc2;
        res.count_crtcs        = nt2;
        res.connector_id_ptr   = (uint64_t)(uintptr_t)conn_ids;
        res.crtc_id_ptr        = (uint64_t)(uintptr_t)crtc_ids;
    }
    if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "flip_drm_init: GETRESOURCES(2) failed: %s\n", strerror(errno));
        free(conn_ids); free(crtc_ids); goto fail;
    }

    /* Find the first connected connector */
    for (uint32_t i = 0; i < res.count_connectors && !s_drm_conn_id; i++) {
        struct gvu_drm_mode_get_connector c = { .connector_id = conn_ids[i] };
        if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) continue;
        if (c.connection != GVU_DRM_MODE_CONNECTED) continue;

        s_drm_conn_id = conn_ids[i];

        /* Fetch mode list */
        uint32_t nm = c.count_modes;
        struct gvu_drm_mode_modeinfo *modes =
            (struct gvu_drm_mode_modeinfo *)calloc(nm, sizeof(*modes));
        if (modes) {
            struct gvu_drm_mode_get_connector c2 = {
                .connector_id = s_drm_conn_id,
                .modes_ptr    = (uint64_t)(uintptr_t)modes,
                .count_modes  = nm,
            };
            ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_GETCONNECTOR, &c2);
        }

        /* Get current CRTC via encoder */
        if (c.encoder_id) {
            struct gvu_drm_mode_get_encoder enc = { .encoder_id = c.encoder_id };
            if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_GETENCODER, &enc) == 0
                    && enc.crtc_id) {
                s_drm_crtc_id = enc.crtc_id;
                struct gvu_drm_mode_crtc crtc = { .crtc_id = s_drm_crtc_id };
                if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_GETCRTC, &crtc) == 0
                        && crtc.mode_valid)
                    s_drm_mode = crtc.mode;
            }
        }
        /* Fallback: use first mode from connector list */
        if (!s_drm_mode.hdisplay && modes && nm > 0)
            s_drm_mode = modes[0];
        /* Fallback: use first CRTC */
        if (!s_drm_crtc_id && res.count_crtcs > 0)
            s_drm_crtc_id = crtc_ids[0];

        free(modes);
    }
    free(conn_ids);
    free(crtc_ids);

    if (!s_drm_conn_id || !s_drm_crtc_id || !s_drm_mode.hdisplay) {
        fprintf(stderr, "flip_drm_init: no usable connector/CRTC/mode\n");
        goto fail;
    }
    fprintf(stderr, "flip_drm_init: conn=%u crtc=%u mode=%ux%u@%u\n",
            s_drm_conn_id, s_drm_crtc_id,
            s_drm_mode.hdisplay, s_drm_mode.vdisplay, s_drm_mode.vrefresh);

    /* Allocate two dumb buffers */
    for (int b = 0; b < 2; b++) {
        struct gvu_drm_mode_create_dumb cd = {
            .width  = s_drm_mode.hdisplay,
            .height = s_drm_mode.vdisplay,
            .bpp    = 32,
        };
        if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
            fprintf(stderr, "flip_drm_init: CREATE_DUMB[%d] failed: %s\n",
                    b, strerror(errno));
            goto fail;
        }
        s_drm_handle[b] = cd.handle;
        s_drm_pitch     = cd.pitch;
        s_drm_buf_size  = (size_t)cd.height * (size_t)cd.pitch;

        struct gvu_drm_mode_fb_cmd fb = {
            .width  = cd.width,
            .height = cd.height,
            .pitch  = cd.pitch,
            .bpp    = 32,
            .depth  = 24,
            .handle = cd.handle,
        };
        if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
            fprintf(stderr, "flip_drm_init: ADDFB[%d] failed: %s\n",
                    b, strerror(errno));
            goto fail;
        }
        s_drm_fb[b] = fb.fb_id;

        struct gvu_drm_mode_map_dumb md = { .handle = cd.handle };
        if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
            fprintf(stderr, "flip_drm_init: MAP_DUMB[%d] failed: %s\n", b, strerror(errno));
            goto fail;
        }
        s_drm_buf[b] = (Uint32 *)mmap(NULL, s_drm_buf_size,
                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                       s_drm_fd, (off_t)md.offset);
        if (s_drm_buf[b] == MAP_FAILED) {
            fprintf(stderr, "flip_drm_init: mmap[%d] failed: %s\n", b, strerror(errno));
            s_drm_buf[b] = NULL; goto fail;
        }
        memset(s_drm_buf[b], 0, s_drm_buf_size);
    }

    /* Set mode — display buffer 0 initially */
    struct gvu_drm_mode_crtc set = {
        .crtc_id              = s_drm_crtc_id,
        .fb_id                = s_drm_fb[0],
        .set_connectors_ptr   = (uint64_t)(uintptr_t)&s_drm_conn_id,
        .count_connectors     = 1,
        .mode                 = s_drm_mode,
        .mode_valid           = 1,
    };
    if (ioctl(s_drm_fd, GVU_DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        fprintf(stderr, "flip_drm_init: SETCRTC failed: %s\n", strerror(errno));
        goto fail;
    }

    s_drm_back = 1;   /* buffer 0 is front, 1 is back */
    fprintf(stderr, "flip_drm_init: DRM/KMS ready, pitch=%u\n", s_drm_pitch);
    return 0;

fail:
    flip_drm_close();
    return -1;
}

/* -------------------------------------------------------------------------
 * end DRM/KMS section
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * brick_screen_init
 * ---------------------------------------------------------------------- */

int brick_screen_init(void) {
    /* Try DRM/KMS first.  On devices with single-page fb0 (e.g. Miyoo Flip)
     * DRM gives tear-free vsync-locked output.  Falls back to fb0 if unavailable. */
    if (flip_drm_init() == 0) {
        s_use_drm = 1;
        /* DRM stride (pixels/row) must match BRICK_W for the write loops */
        s_fb_stride = (int)(s_drm_pitch / 4);
        s_input_fd  = open(g_input_dev, O_RDONLY | O_NONBLOCK);
        if (s_input_fd < 0)
            fprintf(stderr, "brick_screen_init: open %s: %s (non-fatal)\n",
                    g_input_dev, strerror(errno));
        return 0;
    }

    /* --- fb0 fallback --- */
    s_fb_fd = open(FB_DEVICE, O_RDWR);
    if (s_fb_fd < 0) {
        perror("brick_screen_init: open " FB_DEVICE);
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("brick_screen_init: ioctl FBIOGET_*SCREENINFO");
        close(s_fb_fd); s_fb_fd = -1;
        return -1;
    }

    s_fb_stride = (int)(finfo.line_length / (vinfo.bits_per_pixel / 8));

    /* If fb0 only has one virtual page, try expanding to two via FBIOPUT_VSCREENINFO. */
    {
        size_t page_bytes = (size_t)vinfo.yres * (size_t)finfo.line_length;
        if ((size_t)finfo.smem_len < 2 * page_bytes) {
            vinfo.yres_virtual = vinfo.yres * 2;
            if (ioctl(s_fb_fd, FBIOPUT_VSCREENINFO, &vinfo) == 0) {
                ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &finfo);
                fprintf(stderr, "brick_screen: expanded virtual_yres to %u, smem now %u\n",
                        vinfo.yres_virtual, finfo.smem_len);
            } else {
                vinfo.yres_virtual = vinfo.yres;
                fprintf(stderr, "brick_screen: FBIOPUT_VSCREENINFO failed (%s)\n",
                        strerror(errno));
            }
        }
    }

    s_fb_size   = (size_t)finfo.smem_len;
    s_fb_mem    = (Uint32 *)mmap(NULL, s_fb_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, s_fb_fd, 0);
    if (s_fb_mem == MAP_FAILED) {
        perror("brick_screen_init: mmap fb0");
        close(s_fb_fd); s_fb_fd = -1; s_fb_mem = NULL;
        return -1;
    }

    s_fb_yoffset   = (int)vinfo.yoffset;
    s_fb_back_yoff = (s_fb_yoffset == 0) ? BRICK_H : 0;

    /* If fb0 still doesn't have room for two pages, disable double-buffering. */
    {
        size_t page_bytes = (size_t)vinfo.yres * (size_t)finfo.line_length;
        if (s_fb_size < 2 * page_bytes) {
            s_fb_pan_disabled = 1;
            s_fb_back_yoff    = s_fb_yoffset;
            fprintf(stderr, "brick_screen: single-page fb0 (smem=%zu < %zu), pan disabled\n",
                    s_fb_size, 2 * page_bytes);
        }
    }

    fprintf(stderr, "brick_screen: fb0 %dx%d bpp=%d stride=%d yoff=%d back_yoff=%d\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
            s_fb_stride, s_fb_yoffset, s_fb_back_yoff);

    memset(s_fb_mem, 0, s_fb_size);

    s_input_fd = open(g_input_dev, O_RDONLY | O_NONBLOCK);
    if (s_input_fd < 0)
        fprintf(stderr, "brick_screen_init: open %s: %s (non-fatal)\n",
                g_input_dev, strerror(errno));

    return 0;
}

/* -------------------------------------------------------------------------
 * brick_flip — direct blit SDL surface to fb0 (no rotation)
 *
 * The display is already landscape-oriented and matches the GVU canvas,
 * so this is a straightforward row-by-row copy with NEON alpha OR.
 * ---------------------------------------------------------------------- */

void brick_flip(SDL_Surface *surface) {
    if (!surface || (!s_fb_mem && !s_use_drm)) return;

    SDL_LockSurface(surface);

    const Uint32 *src   = (const Uint32 *)surface->pixels;
    const int     pitch = surface->pitch / 4;
    /* Write to DRM back buffer, fb0 back page, or fb0 direct (single-page) */
    Uint32 *dst;
    if (s_use_drm) {
        dst = s_drm_buf[s_drm_back];
    } else {
        int dst_yoff = s_fb_pan_disabled ? s_fb_yoffset : s_fb_back_yoff;
        dst = s_fb_mem + (size_t)dst_yoff * (size_t)s_fb_stride;
    }

#ifdef __aarch64__
    const uint32x4_t alpha_v = vdupq_n_u32(0xFF000000u);
    for (int r = 0; r < BRICK_H; r++) {
        const Uint32 *in  = src + (size_t)r * (size_t)pitch;
        Uint32       *out = dst + (size_t)r * (size_t)s_fb_stride;
        /* BRICK_W=1024 is divisible by 8 — no tail loop needed. */
        for (int c = 0; c < BRICK_W; c += 8) {
            vst1q_u32(out + c,     vorrq_u32(vld1q_u32(in + c),     alpha_v));
            vst1q_u32(out + c + 4, vorrq_u32(vld1q_u32(in + c + 4), alpha_v));
        }
    }
#else
    for (int r = 0; r < BRICK_H; r++) {
        const Uint32 *in  = src + (size_t)r * (size_t)pitch;
        Uint32       *out = dst + (size_t)r * (size_t)s_fb_stride;
        for (int c = 0; c < BRICK_W; c++)
            out[c] = in[c] | 0xFF000000u;
    }
#endif

    SDL_UnlockSurface(surface);
    brick_pageflip();
}

/* -------------------------------------------------------------------------
 * brick_surface_to_bgra — copy SDL surface into a flat landscape BGRA buffer
 *
 * Pixels are copied as-is (alpha preserved).  The caller must clear the SDL
 * surface with alpha=0 before rendering OSD so that empty pixels are
 * transparent (alpha=0) and OSD pixels are opaque (alpha>0).  brick_flip_video
 * uses the alpha channel to composite OSD over video.
 * out must be BRICK_PORTRAIT_BUF_PIXELS Uint32s (= BRICK_W × BRICK_H).
 * ---------------------------------------------------------------------- */

void brick_surface_to_bgra(SDL_Surface *surf, Uint32 *out) {
    if (!surf || !out) return;
    SDL_LockSurface(surf);
    const Uint32 *src   = (const Uint32 *)surf->pixels;
    const int     pitch = surf->pitch / 4;
#ifdef __aarch64__
    for (int r = 0; r < BRICK_H; r++) {
        const Uint32 *in      = src + (size_t)r * (size_t)pitch;
        Uint32       *row_out = out + (size_t)r * (size_t)BRICK_W;
        /* 8-wide copy, no alpha modification — preserve original alpha */
        for (int c = 0; c < BRICK_W; c += 8) {
            vst1q_u32(row_out + c,     vld1q_u32(in + c));
            vst1q_u32(row_out + c + 4, vld1q_u32(in + c + 4));
        }
    }
#else
    for (int r = 0; r < BRICK_H; r++) {
        const Uint32 *in      = src + (size_t)r * (size_t)pitch;
        Uint32       *row_out = out + (size_t)r * (size_t)BRICK_W;
        for (int c = 0; c < BRICK_W; c++)
            row_out[c] = in[c];
    }
#endif
    SDL_UnlockSurface(surf);
}

/* -------------------------------------------------------------------------
 * brick_flip_video — landscape BGRA blit for video frames
 *
 * landscape_bgra: BRICK_W × land_h buffer (land_h ≤ BRICK_H).
 * Letterboxed top/bottom with black rows.  osd_bgra overlays the video.
 *
 * For 16:9 video at FIT: land_h = BRICK_W * 9/16 = 576, row_off = 96.
 * For 4:3  video at FIT: land_h = BRICK_H = 768, fills panel exactly.
 * ---------------------------------------------------------------------- */

static void brick_pageflip(void) {
    if (s_use_drm) { flip_drm_pageflip(); return; }
    if (s_fb_pan_disabled) return;  /* direct write — no flip needed */
    if (s_fb_wake_frames > 0) { s_fb_wake_frames--; return; }
    struct fb_var_screeninfo vinfo;
    if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        vinfo.yoffset  = (uint32_t)s_fb_back_yoff;
        vinfo.activate = 0;  /* FB_ACTIVATE_NOW */
        if (ioctl(s_fb_fd, FBIOPAN_DISPLAY, &vinfo) == 0) {
            s_fb_yoffset   = s_fb_back_yoff;
            s_fb_back_yoff = (s_fb_yoffset == 0) ? BRICK_H : 0;
        }
    }
}

/* -------------------------------------------------------------------------
 * Pixel helpers: bilinear interpolation and brightness scaling
 * ---------------------------------------------------------------------- */

/* Linearly interpolate R,G,B between two pixels.  t=0 → a, t=255 → b.
 * Alpha is taken from pixel a. */
static inline Uint32 lerp_pixel(Uint32 a, Uint32 b, int t) {
    if (t <= 0)   return a;
    if (t >= 255) return b;
    int t1 = 256 - t;
    Uint32 R = (((a >> 16) & 0xFFu) * (unsigned)t1 + ((b >> 16) & 0xFFu) * (unsigned)t) >> 8;
    Uint32 G = (((a >>  8) & 0xFFu) * (unsigned)t1 + ((b >>  8) & 0xFFu) * (unsigned)t) >> 8;
    Uint32 B = (( a        & 0xFFu) * (unsigned)t1 + ( b        & 0xFFu) * (unsigned)t) >> 8;
    return (a & 0xFF000000u) | (R << 16) | (G << 8) | B;
}

/* Scale R,G,B channels by bri256/256 (fixed-point brightness).
 * bri256=256 → full brightness (no-op), bri256=0 → black. Alpha preserved. */
static inline Uint32 apply_brightness(Uint32 px, int bri256) {
    if (bri256 >= 256) return px;
    Uint32 R = (((px >> 16) & 0xFFu) * (unsigned)bri256) >> 8;
    Uint32 G = (((px >>  8) & 0xFFu) * (unsigned)bri256) >> 8;
    Uint32 B = (( px        & 0xFFu) * (unsigned)bri256) >> 8;
    return (px & 0xFF000000u) | (R << 16) | (G << 8) | B;
}

/* Alpha-blend an OSD pixel over a video pixel; returns opaque result. */
static inline Uint32 alpha_blend(Uint32 osd, Uint8 osd_a, Uint32 vid)
{
    unsigned a  = osd_a;
    unsigned a0 = 255u - a;
    Uint32 R = ((((osd >> 16) & 0xFFu) * a + ((vid >> 16) & 0xFFu) * a0) >> 8);
    Uint32 G = ((((osd >>  8) & 0xFFu) * a + ((vid >>  8) & 0xFFu) * a0) >> 8);
    Uint32 B = ((( osd        & 0xFFu) * a + ( vid        & 0xFFu) * a0) >> 8);
    return 0xFF000000u | (R << 16) | (G << 8) | B;
}

/* Returns 1 if the first row of the OSD buffer has any partial-alpha pixel. */
static int osd_has_partial_alpha(const Uint32 *osd_bgra)
{
    for (int c = 0; c < BRICK_W; c++) {
        Uint8 a = (Uint8)(osd_bgra[c] >> 24);
        if (a != 0 && a != 0xFF) return 1;
    }
    return 0;
}

/* Composite OSD over video with correct partial-alpha blending + brightness. */
static inline Uint32 composite_pixel(Uint32 osd, Uint32 vid, int bri256)
{
    Uint8 a = (Uint8)(osd >> 24);
    Uint32 px;
    if (a == 0xFF)      px = osd;
    else if (a == 0)    px = vid | 0xFF000000u;
    else                px = alpha_blend(osd, a, vid);
    return apply_brightness(px, bri256);
}

void brick_flip_video(const Uint32 *osd_bgra,
                      const Uint32 *landscape_bgra,
                      int land_h, float zoom_t, float brightness) {
    if (!landscape_bgra || (!s_fb_mem && !s_use_drm)) return;

    int bri256 = (int)(brightness * 256.0f);
    if (bri256 > 256) bri256 = 256;
    if (bri256 < 0)   bri256 = 0;

    /* Zoom geometry (uniform scale, aspect preserved):
     *   display_h — panel rows used by video
     *   row_off   — black rows above/below
     *   src_cols  — source columns used (crops left/right when zoom > 0) */
    int display_h = land_h + (int)((BRICK_H - land_h) * zoom_t + 0.5f);
    if (display_h > BRICK_H) display_h = BRICK_H;
    int row_off  = (BRICK_H - display_h) / 2;
    int src_cols = BRICK_W * land_h / display_h;
    if (src_cols > BRICK_W) src_cols = BRICK_W;
    int col_off  = (BRICK_W - src_cols) / 2;

    Uint32 *fb;
    if (s_use_drm) {
        fb = s_drm_buf[s_drm_back];
    } else {
        int fv_dst_yoff = s_fb_pan_disabled ? s_fb_yoffset : s_fb_back_yoff;
        fb = s_fb_mem + (size_t)fv_dst_yoff * (size_t)s_fb_stride;
    }

    /* ------------------------------------------------------------------ */
    /* FIT fast path (zoom_t ≈ 0)                                          */
    /* ------------------------------------------------------------------ */
    if (zoom_t < 0.001f) {
        if (!osd_bgra) {
            /* Pure video blit with black letterboxes */
            for (int r = 0; r < BRICK_H; r++) {
                Uint32 *out = fb + (size_t)r * (size_t)s_fb_stride;
                if (r < row_off || r >= row_off + land_h) {
#ifdef __aarch64__
                    const uint32x4_t alpha_v = vdupq_n_u32(0xFF000000u);
                    for (int c = 0; c < BRICK_W; c += 4)
                        vst1q_u32(out + c, alpha_v);
#else
                    for (int c = 0; c < BRICK_W; c++)
                        out[c] = 0xFF000000u;
#endif
                } else if (bri256 >= 256) {
                    /* Full brightness — fast memcpy */
                    memcpy(out,
                           landscape_bgra + (size_t)(r - row_off) * (size_t)BRICK_W,
                           BRICK_W * sizeof(Uint32));
                } else {
                    /* Dimmed — per-pixel brightness scale */
                    const Uint32 *in = landscape_bgra + (size_t)(r - row_off) * (size_t)BRICK_W;
                    for (int c = 0; c < BRICK_W; c++)
                        out[c] = apply_brightness(in[c] | 0xFF000000u, bri256);
                }
            }
            brick_pageflip(); return;
        }

        /* FIT + OSD composite */
        int partial = osd_has_partial_alpha(osd_bgra);
#ifdef __aarch64__
        if (bri256 >= 256 && !partial) {
            /* Full brightness, no partial-alpha OSD — NEON binary composite */
            const uint32x4_t alpha_mask = vdupq_n_u32(0xFF000000u);
            const uint32x4_t black_v    = vdupq_n_u32(0xFF000000u);
            for (int r = 0; r < BRICK_H; r++) {
                Uint32       *out     = fb + (size_t)r * (size_t)s_fb_stride;
                const Uint32 *osd_row = osd_bgra + (size_t)r * (size_t)BRICK_W;
                int in_video = (r >= row_off && r < row_off + land_h);
                const Uint32 *vid_row = in_video
                    ? landscape_bgra + (size_t)(r - row_off) * (size_t)BRICK_W : NULL;
                int neon_end = BRICK_W & ~3;
                for (int c = 0; c < neon_end; c += 4) {
                    uint32x4_t osd = vld1q_u32(osd_row + c);
                    uint32x4_t vid = vid_row ? vld1q_u32(vid_row + c) : black_v;
                    uint32x4_t sel = vtstq_u32(osd, alpha_mask);
                    vst1q_u32(out + c, vbslq_u32(sel, vorrq_u32(osd, alpha_mask), vid));
                }
                for (int c = neon_end; c < BRICK_W; c++) {
                    Uint32 ui = osd_row[c];
                    out[c] = (ui >> 24) ? (ui | 0xFF000000u)
                           : (vid_row ? vid_row[c] : 0xFF000000u);
                }
            }
            brick_pageflip(); return;
        }
#endif
        /* FIT + OSD + brightness — scalar with correct partial-alpha blending */
        for (int r = 0; r < BRICK_H; r++) {
            Uint32       *out     = fb + (size_t)r * (size_t)s_fb_stride;
            const Uint32 *osd_row = osd_bgra + (size_t)r * (size_t)BRICK_W;
            int in_video = (r >= row_off && r < row_off + land_h);
            const Uint32 *vid_row = in_video
                ? landscape_bgra + (size_t)(r - row_off) * (size_t)BRICK_W : NULL;
            for (int c = 0; c < BRICK_W; c++) {
                Uint32 vid = vid_row ? (vid_row[c] | 0xFF000000u) : 0xFF000000u;
                out[c] = composite_pixel(osd_row[c], vid, bri256);
            }
        }
        brick_pageflip(); return;
    }

    /* ------------------------------------------------------------------ */
    /* Zoom path: bilinear scale + brightness                               */
    /* Column position in 8.8 fixed-point — accumulate to avoid per-pixel  */
    /* float multiply in the inner loop.                                    */
    /* ------------------------------------------------------------------ */
    int col_step_fp  = (src_cols << 8) / BRICK_W;   /* src pixels per output pixel * 256 */
    int col_start_fp = col_off << 8;

    for (int r = 0; r < BRICK_H; r++) {
        Uint32       *out     = fb + (size_t)r * (size_t)s_fb_stride;
        const Uint32 *osd_row = osd_bgra ? osd_bgra + (size_t)r * (size_t)BRICK_W : NULL;

        /* Row bilinear: fractional source row in 8.8 fixed-point */
        float src_row_f = (r - row_off) * (float)land_h / display_h;
        int src_r0  = (int)src_row_f;
        int vf      = (int)((src_row_f - (float)src_r0) * 256.0f);
        int in_video = (src_r0 >= 0 && src_r0 < land_h);
        int src_r1  = src_r0 + 1;
        if (src_r1 >= land_h) src_r1 = land_h - 1;

        const Uint32 *vid_r0 = in_video
            ? landscape_bgra + (size_t)src_r0 * (size_t)BRICK_W : NULL;
        const Uint32 *vid_r1 = in_video
            ? landscape_bgra + (size_t)src_r1 * (size_t)BRICK_W : NULL;

        int src_col_fp = col_start_fp;
        for (int c = 0; c < BRICK_W; c++, src_col_fp += col_step_fp) {
            Uint32 vid_px;
            if (vid_r0) {
                int sc0 = src_col_fp >> 8;
                int uf  = src_col_fp & 0xFF;
                int sc1 = sc0 + 1;
                if (sc1 >= BRICK_W) sc1 = BRICK_W - 1;
                Uint32 top = lerp_pixel(vid_r0[sc0], vid_r0[sc1], uf);
                Uint32 bot = lerp_pixel(vid_r1[sc0], vid_r1[sc1], uf);
                vid_px = lerp_pixel(top, bot, vf) | 0xFF000000u;
            } else {
                vid_px = 0xFF000000u;
            }
            if (osd_row)
                out[c] = composite_pixel(osd_row[c], vid_px, bri256);
            else
                out[c] = apply_brightness(vid_px, bri256);
        }
    }

    brick_pageflip();
}

void brick_screen_wake(void) {
    if (s_use_drm) return;  /* DRM page-flip is always vsync-locked; no grace needed */
    s_fb_wake_frames = WAKE_GRACE_FRAMES;
}

/* -------------------------------------------------------------------------
 * Input handling — Xbox gamepad (EV_KEY + EV_ABS)
 * ---------------------------------------------------------------------- */

/* Button codes — defined in linux/input-event-codes.h; add only if missing */
#ifndef BTN_SOUTH
#define BTN_SOUTH   0x130
#endif
#ifndef BTN_EAST
#define BTN_EAST    0x131
#endif
#ifndef BTN_NORTH
#define BTN_NORTH   0x133
#endif
#ifndef BTN_WEST
#define BTN_WEST    0x134
#endif
#ifndef BTN_TL
#define BTN_TL      0x136
#endif
#ifndef BTN_TR
#define BTN_TR      0x137
#endif

#define TRIGGER_THRESHOLD 64

typedef struct { int linux_code; SDL_Keycode sdl_sym; } KeyMap;

static const KeyMap s_keymap[] = {
    { BTN_EAST,         SDLK_SPACE    },  /* A (right) */
    { BTN_SOUTH,        SDLK_LCTRL   },  /* B (bottom) */
    { BTN_NORTH,        SDLK_LALT    },  /* X (top) — in evdev this is Xbox Y, but device labels it X */
    { BTN_WEST,         SDLK_LSHIFT  },  /* Y (left) — in evdev this is Xbox X, but device labels it Y */
    { BTN_TL,           SDLK_PAGEUP  },  /* L1       */
    { BTN_TR,           SDLK_PAGEDOWN},  /* R1       */
    { BTN_SELECT,       SDLK_RCTRL   },  /* SELECT   */
    { BTN_START,        SDLK_RETURN  },  /* START    */
    { BTN_MODE,         SDLK_ESCAPE  },  /* MENU     */
    { KEY_VOLUMEUP,     SDLK_EQUALS  },  /* Vol+     */
    { KEY_VOLUMEDOWN,   SDLK_MINUS   },  /* Vol-     */
    { 0, 0 },
};

static SDL_Keycode lookup_keycode(int linux_code) {
    for (int i = 0; s_keymap[i].linux_code; i++)
        if (s_keymap[i].linux_code == linux_code)
            return s_keymap[i].sdl_sym;
    return SDLK_UNKNOWN;
}

#define KEY_REPEAT_DELAY_MS  300
#define KEY_REPEAT_PERIOD_MS  80

static SDL_Keycode s_held_key    = SDLK_UNKNOWN;
static Uint32      s_held_since  = 0;
static Uint32      s_last_repeat = 0;

/* Analog trigger state (L2/R2) — track above/below threshold */
static int s_l2_pressed = 0;
static int s_r2_pressed = 0;

static int is_repeatable(SDL_Keycode sym) {
    return sym == SDLK_UP || sym == SDLK_DOWN ||
           sym == SDLK_LEFT || sym == SDLK_RIGHT;
}

static void push_key(SDL_Keycode sym, int down, int repeat) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type             = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.type         = ev.type;
    ev.key.state        = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.repeat       = repeat;
    ev.key.keysym.sym      = sym;
    ev.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
    ev.key.keysym.mod      = KMOD_NONE;
    SDL_PushEvent(&ev);
}

static void handle_hat(int axis, int value) {
    /* HAT axis: -1 = up/left, 0 = center, +1 = down/right */
    SDL_Keycode neg = (axis == ABS_HAT0X) ? SDLK_LEFT  : SDLK_UP;
    SDL_Keycode pos = (axis == ABS_HAT0X) ? SDLK_RIGHT : SDLK_DOWN;

    if (value < 0) {
        push_key(neg, 1, 0);
        if (is_repeatable(neg)) {
            s_held_key    = neg;
            s_held_since  = SDL_GetTicks();
            s_last_repeat = s_held_since;
        }
    } else if (value > 0) {
        push_key(pos, 1, 0);
        if (is_repeatable(pos)) {
            s_held_key    = pos;
            s_held_since  = SDL_GetTicks();
            s_last_repeat = s_held_since;
        }
    } else {
        /* Center — release whichever direction was held */
        if (s_held_key == neg || s_held_key == pos) {
            push_key(s_held_key, 0, 0);
            s_held_key = SDLK_UNKNOWN;
        }
    }
}

static void handle_trigger(int axis, int value) {
    int *pressed = (axis == ABS_Z) ? &s_l2_pressed : &s_r2_pressed;
    SDL_Keycode sym = (axis == ABS_Z) ? SDLK_COMMA : SDLK_PERIOD;
    int now_pressed = (value > TRIGGER_THRESHOLD);
    if (now_pressed && !*pressed) {
        push_key(sym, 1, 0);
        *pressed = 1;
    } else if (!now_pressed && *pressed) {
        push_key(sym, 0, 0);
        *pressed = 0;
    }
}

void brick_poll_events(void) {
    if (s_input_fd < 0) return;

    struct input_event ev;
    while (read(s_input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_KEY) {
            if (ev.value != 0 && ev.value != 1) continue;
            SDL_Keycode sym = lookup_keycode(ev.code);
            if (sym == SDLK_UNKNOWN) continue;

            push_key(sym, ev.value, 0);
            if (ev.value == 1 && is_repeatable(sym)) {
                s_held_key    = sym;
                s_held_since  = SDL_GetTicks();
                s_last_repeat = s_held_since;
            } else if (ev.value == 0 && s_held_key == sym) {
                s_held_key = SDLK_UNKNOWN;
            }

        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)
                handle_hat(ev.code, ev.value);
            else if (ev.code == ABS_Z || ev.code == ABS_RZ)
                handle_trigger(ev.code, ev.value);
        }
    }

    /* Software key-repeat for held navigation */
    if (s_held_key != SDLK_UNKNOWN) {
        Uint32 now = SDL_GetTicks();
        if (now - s_held_since >= KEY_REPEAT_DELAY_MS &&
            now - s_last_repeat >= KEY_REPEAT_PERIOD_MS) {
            push_key(s_held_key, 1, 1);
            s_last_repeat = now;
        }
    }
}

/* -------------------------------------------------------------------------
 * brick_screen_close
 * ---------------------------------------------------------------------- */

void brick_screen_close(void) {
    if (s_input_fd >= 0) { close(s_input_fd); s_input_fd = -1; }
    if (s_use_drm) {
        /* Clear both DRM buffers to black before teardown */
        for (int b = 0; b < 2; b++)
            if (s_drm_buf[b]) memset(s_drm_buf[b], 0, s_drm_buf_size);
        flip_drm_close();
        s_use_drm = 0;
    } else {
        if (s_fb_mem && s_fb_mem != MAP_FAILED) {
            memset(s_fb_mem, 0, s_fb_size);
            munmap(s_fb_mem, s_fb_size);
            s_fb_mem = NULL;
        }
        if (s_fb_fd >= 0) { close(s_fb_fd); s_fb_fd = -1; }
    }
}

#endif /* GVU_TRIMUI_BRICK */
