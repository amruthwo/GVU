#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL2/SDL_image.h>

/* nanosvg — single-header, compiled exactly once here */
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../thirdparty/nanosvg.h"
#include "../thirdparty/nanosvgrast.h"

/* -------------------------------------------------------------------------
 * Theme table
 * ---------------------------------------------------------------------- */

/*
 * Cover icon color layout:  body / tab / shadow / screen / play
 *
 * These reuse the theme's existing colors, just remapped to the SVG elements.
 * Shadow opacity is 6% (set in SVG) — only the tint color is changed here.
 */
const Theme THEMES[] = {
    /*            bg              text            secondary       hl_bg           hl_text */
    { "SPRUCE",
      {0x14,0x23,0x1e}, {0xd2,0xe1,0xd2}, {0x64,0x82,0x6e}, {0x32,0x64,0x46}, {0xff,0xff,0xff},
      /* cover: body=text, tab=hl_bg, shadow=dark, screen=secondary, play=hl_text */
      {0xd2,0xe1,0xd2}, {0x32,0x64,0x46}, {0x17,0x29,0x2d}, {0x64,0x82,0x6e}, {0xff,0xff,0xff} },

    { "night_contrast",
      {0x00,0x00,0x00}, {0xf0,0xf0,0xf0}, {0x60,0x60,0x60}, {0x96,0x26,0x00}, {0xf0,0xf0,0xf0},
      /* cover: body=secondary, screen=text (swapped), play=secondary */
      {0x60,0x60,0x60}, {0x96,0x26,0x00}, {0x17,0x29,0x2d}, {0xf0,0xf0,0xf0}, {0x60,0x60,0x60} },

    { "light_contrast",
      {0xff,0xff,0xff}, {0x00,0x00,0x00}, {0xa0,0xa0,0xa0}, {0xa3,0x51,0xc8}, {0xfa,0xfa,0xfa},
      /* cover: body=hl_bg(lavender), tab=secondary(grey), shadow=text(black),
                screen=hl_text(near-white), play=hl_bg(lavender) */
      {0xa3,0x51,0xc8}, {0xa0,0xa0,0xa0}, {0x00,0x00,0x00}, {0xfa,0xfa,0xfa}, {0xa3,0x51,0xc8} },

    { "light_sepia",
      {0xfa,0xf0,0xdc}, {0x00,0x00,0x00}, {0xa0,0xa0,0xa0}, {0x78,0x9c,0x70}, {0xfa,0xf0,0xdc},
      /* cover: body=hl_bg(sage), tab=secondary(grey), shadow=text(black),
                screen=background(cream), play=hl_bg(sage) */
      {0x78,0x9c,0x70}, {0xa0,0xa0,0xa0}, {0x00,0x00,0x00}, {0xfa,0xf0,0xdc}, {0x78,0x9c,0x70} },

    { "vampire",
      {0x00,0x00,0x00}, {0xc0,0x00,0x00}, {0x60,0x00,0x00}, {0xc0,0x00,0x00}, {0x00,0x00,0x00},
      /* cover: body=secondary(scarlet), tab=hl_bg(red), shadow=background(black),
                screen=background(black), play=hl_bg(red) */
      {0x60,0x00,0x00}, {0xc0,0x00,0x00}, {0x00,0x00,0x00}, {0x00,0x00,0x00}, {0xc0,0x00,0x00} },

    { "coffee_dark",
      {0x2b,0x1f,0x16}, {0xf5,0xe6,0xd3}, {0xa0,0x8c,0x78}, {0x6f,0x4e,0x37}, {0xff,0xff,0xff},
      /* cover: body=text, tab=hl_bg, shadow=dark, screen=secondary, play=hl_text */
      {0xf5,0xe6,0xd3}, {0x6f,0x4e,0x37}, {0x17,0x29,0x2d}, {0xa0,0x8c,0x78}, {0xff,0xff,0xff} },

    { "cream_latte",
      {0xf5,0xe6,0xd3}, {0x2b,0x1f,0x16}, {0x78,0x64,0x50}, {0xd2,0xb4,0x8c}, {0x2b,0x1f,0x16},
      /* cover: body=hl_bg(tan), tab=secondary(light brown), shadow=text(dark brown),
                screen=text(dark brown), play=hl_bg(tan) */
      {0xd2,0xb4,0x8c}, {0x78,0x64,0x50}, {0x2b,0x1f,0x16}, {0x2b,0x1f,0x16}, {0xd2,0xb4,0x8c} },

    { "nautical",
      {0x0f,0x19,0x2d}, {0xd4,0xaf,0x37}, {0x78,0x8c,0xb4}, {0x38,0x58,0x9a}, {0xff,0xdc,0x64},
      /* cover: body=secondary(lighter blue), tab=hl_bg(medium blue),
                screen=hl_text(yellow), play=hl_bg(medium blue) */
      {0x78,0x8c,0xb4}, {0x38,0x58,0x9a}, {0x17,0x29,0x2d}, {0xff,0xdc,0x64}, {0x38,0x58,0x9a} },

    { "nordic_frost",
      {0xec,0xef,0xf4}, {0x2e,0x34,0x40}, {0x81,0xa1,0xc1}, {0x88,0xc0,0xd0}, {0x2e,0x34,0x40},
      /* cover: body=text, tab=hl_bg, shadow=dark, screen=secondary, play=hl_text */
      {0x2e,0x34,0x40}, {0x88,0xc0,0xd0}, {0x17,0x29,0x2d}, {0x81,0xa1,0xc1}, {0x2e,0x34,0x40} },

    { "night",
      {0x0d,0x0d,0x10}, {0xf2,0xf2,0xf2}, {0x52,0x52,0x5e}, {0x7a,0xb2,0xde}, {0x0d,0x0d,0x10},
      /* cover: body=secondary(grey), tab=hl_bg(blue), shadow=background(black),
                screen=hl_bg(blue), play=hl_text(black) */
      {0x52,0x52,0x5e}, {0x7a,0xb2,0xde}, {0x0d,0x0d,0x10}, {0x7a,0xb2,0xde}, {0x0d,0x0d,0x10} },
};
const int THEME_COUNT = (int)(sizeof(THEMES) / sizeof(THEMES[0]));

/* -------------------------------------------------------------------------
 * Active theme
 * ---------------------------------------------------------------------- */

static int  g_theme_idx     = 0;  /* default: SPRUCE */
static int  g_firstrun_done = 0;
static char g_tmdb_key[128] = "";
static int  g_layout        = 0;  /* 0=LARGE, 1=SMALL, 2=LIST */
static int  g_season_layout = 0;

const Theme *theme_get(void) {
    return &THEMES[g_theme_idx];
}

void theme_cycle(void) {
    g_theme_idx = (g_theme_idx + 1) % THEME_COUNT;
}

int config_firstrun_done(void) { return g_firstrun_done; }
void config_set_firstrun_done(void) { g_firstrun_done = 1; }

const char *config_tmdb_key(void) { return g_tmdb_key; }
void config_set_tmdb_key(const char *key) {
    strncpy(g_tmdb_key, key ? key : "", sizeof(g_tmdb_key) - 1);
    g_tmdb_key[sizeof(g_tmdb_key) - 1] = '\0';
}

int  config_get_layout(void)        { return g_layout; }
void config_set_layout(int l)       { g_layout = l; }
int  config_get_season_layout(void) { return g_season_layout; }
void config_set_season_layout(int l){ g_season_layout = l; }

void config_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "theme = %s\n", THEMES[g_theme_idx].name);
    fprintf(f, "firstrun_done = %d\n", g_firstrun_done);
    if (g_tmdb_key[0])
        fprintf(f, "tmdb_key = %s\n", g_tmdb_key);
    fprintf(f, "layout = %d\n", g_layout);
    fprintf(f, "season_layout = %d\n", g_season_layout);
    fclose(f);
}

int theme_set(const char *name) {
    for (int i = 0; i < THEME_COUNT; i++) {
        if (strcasecmp(THEMES[i].name, name) == 0) {
            g_theme_idx = i;
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * gvu.conf  —  key=value, # comments, whitespace trimmed
 * ---------------------------------------------------------------------- */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

void config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == '\0') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcasecmp(key, "theme") == 0) {
            if (!theme_set(val))
                fprintf(stderr, "gvu.conf: unknown theme '%s'\n", val);
        } else if (strcasecmp(key, "firstrun_done") == 0) {
            g_firstrun_done = (atoi(val) != 0);
        } else if (strcasecmp(key, "tmdb_key") == 0) {
            strncpy(g_tmdb_key, val, sizeof(g_tmdb_key) - 1);
            g_tmdb_key[sizeof(g_tmdb_key) - 1] = '\0';
        } else if (strcasecmp(key, "layout") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 2) g_layout = v;
        } else if (strcasecmp(key, "season_layout") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 2) g_season_layout = v;
        }
    }
    fclose(f);
}

/* -------------------------------------------------------------------------
 * SVG recoloring
 *
 * The default_cover.svg style block maps classes to colors:
 *   cls-1  fill  →  theme.text          (page body)
 *   cls-2  fill  →  theme.highlight_bg  (folded corner)
 *   cls-5  fill  →  theme.secondary     (video rectangle)
 *   cls-6  fill  →  theme.highlight_text (play triangle)
 *
 * cls-3 (shadow opacity) and cls-4 (shadow fill) are left unchanged.
 *
 * Strategy: keep the original SVG text in memory; on each rasterize call,
 * copy it, then replace the known original hex values with theme hex values
 * inside the <style> block only.
 * ---------------------------------------------------------------------- */

/* Replace all occurrences of `old` with `new_s` in `buf` (in place).
   `buf` must have at least `buf_size` bytes allocated. */
static void str_replace_inplace(char *buf, size_t buf_size,
                                 const char *old, const char *new_s) {
    size_t old_len = strlen(old);
    size_t new_len = strlen(new_s);
    char *pos = buf;

    while ((pos = strstr(pos, old)) != NULL) {
        if (new_len != old_len) {
            size_t tail = strlen(pos + old_len) + 1;
            size_t used = (size_t)(pos - buf) + new_len + tail;
            if (used > buf_size) break; /* safety: don't overflow */
            memmove(pos + new_len, pos + old_len, tail);
        }
        memcpy(pos, new_s, new_len);
        pos += new_len;
    }
}

static void rgb_to_hex(const RGB *c, char out[8]) {
    snprintf(out, 8, "#%02x%02x%02x", c->r, c->g, c->b);
}

/* Original hardcoded fill colors in default_cover.svg, in replacement order.
   Replacements run sequentially, so each new color must not equal any
   *later* original value to avoid inadvertent double-substitution. */
static const char *SVG_ORIG_BODY   = "#dde5e8";
static const char *SVG_ORIG_TAB    = "#71c6c4";
static const char *SVG_ORIG_SHADOW = "#17292d";
static const char *SVG_ORIG_SCREEN = "#afc3c9";
static const char *SVG_ORIG_PLAY   = "#ffffff";

static char *svg_recolor(const char *svg_orig, const Theme *theme) {
    size_t len = strlen(svg_orig);
    size_t buf_size = len + 256;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;
    memcpy(buf, svg_orig, len + 1);

    char hex[8];

    rgb_to_hex(&theme->cover_body,   hex); str_replace_inplace(buf, buf_size, SVG_ORIG_BODY,   hex);
    rgb_to_hex(&theme->cover_tab,    hex); str_replace_inplace(buf, buf_size, SVG_ORIG_TAB,    hex);
    rgb_to_hex(&theme->cover_shadow, hex); str_replace_inplace(buf, buf_size, SVG_ORIG_SHADOW, hex);
    rgb_to_hex(&theme->cover_screen, hex); str_replace_inplace(buf, buf_size, SVG_ORIG_SCREEN, hex);
    rgb_to_hex(&theme->cover_play,   hex); str_replace_inplace(buf, buf_size, SVG_ORIG_PLAY,   hex);

    return buf;
}

/* -------------------------------------------------------------------------
 * File helper
 * ---------------------------------------------------------------------- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* -------------------------------------------------------------------------
 * Rasterize SVG → pixels (shared helper)
 * ---------------------------------------------------------------------- */

/* Rasterize the current theme's cover SVG to a freshly malloc'd RGBA pixel
   buffer (COVER_SIZE × COVER_SIZE × 4 bytes). Caller must free(). */
static unsigned char *svg_rasterize(const char *svg_path) {
    char *svg_orig = read_file(svg_path);
    if (!svg_orig) {
        fprintf(stderr, "theme: could not read %s\n", svg_path);
        return NULL;
    }

    char *svg_colored = svg_recolor(svg_orig, theme_get());
    free(svg_orig);
    if (!svg_colored) return NULL;

    NSVGimage *img = nsvgParse(svg_colored, "px", 96.0f);
    free(svg_colored);
    if (!img) {
        fprintf(stderr, "theme: nsvgParse failed\n");
        return NULL;
    }

    float scale = (float)COVER_SIZE / img->width;
    unsigned char *pixels = malloc((size_t)(COVER_SIZE * COVER_SIZE * 4));
    if (!pixels) { nsvgDelete(img); return NULL; }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { free(pixels); nsvgDelete(img); return NULL; }

    nsvgRasterize(rast, img, 0, 0, scale, pixels, COVER_SIZE, COVER_SIZE, COVER_SIZE * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    return pixels;
}

/* -------------------------------------------------------------------------
 * Rasterize SVG → SDL_Texture
 * ---------------------------------------------------------------------- */

SDL_Texture *theme_render_cover(SDL_Renderer *renderer, const char *svg_path) {
    unsigned char *pixels = svg_rasterize(svg_path);
    if (!pixels) return NULL;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, COVER_SIZE, COVER_SIZE, 32, COVER_SIZE * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        free(pixels);
        fprintf(stderr, "theme: SDL_CreateRGBSurfaceWithFormatFrom: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    free(pixels);
    return tex;
}

int theme_save_icon(const char *svg_path, const char *png_path) {
    unsigned char *pixels = svg_rasterize(svg_path);
    if (!pixels) return -1;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, COVER_SIZE, COVER_SIZE, 32, COVER_SIZE * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) { free(pixels); return -1; }

    int ret = IMG_SavePNG(surf, png_path);
    SDL_FreeSurface(surf);
    free(pixels);
    if (ret != 0)
        fprintf(stderr, "theme_save_icon: %s\n", IMG_GetError());
    return ret == 0 ? 0 : -1;
}
