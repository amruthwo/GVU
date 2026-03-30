#ifndef BRICK_SCREEN_H
#define BRICK_SCREEN_H
#ifdef GVU_TRIMUI_BRICK
/* -------------------------------------------------------------------------
 * brick_screen.h — Trimui Brick framebuffer + evdev input
 * ---------------------------------------------------------------------- */

#include <SDL2/SDL.h>
#include "platform.h"

/* Panel dimensions — resolved at runtime from g_display_w/h */
#define BRICK_W  g_display_w
#define BRICK_H  g_display_h

/* Pre-allocated OSD buffer: one ARGB8888 pixel per panel pixel */
#define BRICK_PORTRAIT_BUF_PIXELS  ((size_t)g_display_w * (size_t)g_display_h)

int  brick_screen_init(void);
void brick_screen_close(void);

/* UI flip: blit 1024×768 SDL surface directly to fb0 */
void brick_flip(SDL_Surface *surface);

/* Copy SDL surface pixels into a flat BGRA buffer (no rotation — display is landscape).
 * out must be BRICK_PORTRAIT_BUF_PIXELS Uint32s. */
void brick_surface_to_bgra(SDL_Surface *surf, Uint32 *out);

/* Video flip: blit landscape BGRA buffer (BRICK_W × land_h) to fb0.
 * osd_bgra=NULL → pure video blit.
 * osd_bgra!=NULL → composite OSD over video. */
void brick_flip_video(const Uint32 *osd_bgra,
                      const Uint32 *landscape_bgra,
                      int land_h, float zoom_t, float brightness);

/* Wake from sleep — disables double buffering for rest of session */
void brick_screen_wake(void);

/* Drain evdev events and inject SDL2 keyboard events */
void brick_poll_events(void);

#endif /* GVU_TRIMUI_BRICK */
#endif /* BRICK_SCREEN_H */
