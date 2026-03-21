#pragma once
#include <SDL2/SDL.h>

/* -------------------------------------------------------------------------
 * Color + theme definitions
 * ---------------------------------------------------------------------- */

typedef struct {
    Uint8 r, g, b;
} RGB;

typedef struct {
    const char *name;
    RGB background;
    RGB text;
    RGB secondary;
    RGB highlight_bg;
    RGB highlight_text;
    /* Cover icon colors (SVG recoloring, independent of UI colors) */
    RGB cover_body;
    RGB cover_tab;
    RGB cover_shadow;
    RGB cover_screen;
    RGB cover_play;
} Theme;

/* All built-in themes (NULL-terminated) */
extern const Theme THEMES[];
extern const int   THEME_COUNT;

/* -------------------------------------------------------------------------
 * Active theme
 * ---------------------------------------------------------------------- */

/* Returns a pointer into THEMES[]. Always valid (defaults to SPRUCE). */
const Theme *theme_get(void);

/* Set by name — returns 0 if not found (theme unchanged). */
int theme_set(const char *name);

/* Advance to the next theme, wrapping around. */
void theme_cycle(void);

/* Write the current theme name (and firstrun_done flag) to the config file. */
void config_save(const char *path);

/* First-run tutorial tracking. */
int  config_firstrun_done(void);
void config_set_firstrun_done(void);

/* -------------------------------------------------------------------------
 * gvu.conf
 * ---------------------------------------------------------------------- */

/* Load theme (and future config keys) from a conf file.
   Call once at startup; silently ignores missing file. */
void config_load(const char *path);

/* -------------------------------------------------------------------------
 * Default cover art — SVG recolored + rasterized per theme
 * ---------------------------------------------------------------------- */

/* Rasterize default_cover.svg with the current theme applied.
   Returns a new SDL_Texture (caller must SDL_DestroyTexture when done),
   or NULL on error. Size is always COVER_SIZE × COVER_SIZE. */
#define COVER_SIZE 256

SDL_Texture *theme_render_cover(SDL_Renderer *renderer, const char *svg_path);

/* Rasterize the current theme's cover SVG and save it as a PNG file.
   Used to keep the SpruceOS launcher icon (icon.png) in sync with the
   active theme. Call at startup and after every theme_cycle().
   Returns 0 on success, -1 on error. */
int theme_save_icon(const char *svg_path, const char *png_path);
