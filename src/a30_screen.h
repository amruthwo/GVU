#pragma once
/* -------------------------------------------------------------------------
 * a30_screen.h — Miyoo A30 framebuffer render path + evdev input
 *
 * The A30 runs SpruceOS with SDL_VIDEODRIVER=dummy, so SDL2 renders into a
 * CPU-side SDL_Surface.  a30_flip() writes that surface to /dev/fb0 with a
 * 90° CCW rotation (the panel is mounted in landscape, but we run 640×480
 * portrait-style; the hardware compositor expects a 480×640 portrait write).
 *
 * a30_poll_events() reads /dev/input/event3 (O_NONBLOCK) and injects
 * SDL2 keyboard events so the rest of GVU needs zero changes.
 * ---------------------------------------------------------------------- */

#ifdef GVU_A30

#include <SDL2/SDL.h>

/* Must be called once after SDL_Init.
   Opens /dev/fb0, mmaps the framebuffer, caches yoffset + stride.
   Returns 0 on success, -1 on error (writes message to stderr). */
int  a30_screen_init(void);

/* Blit SDL_Surface to the visible fb0 page with 90° CCW rotation.
   surface must be WIN_W × WIN_H (640×480) ARGB8888.
   Call once per frame, after SDL_RenderPresent. */
void a30_flip(SDL_Surface *surface);

/* Non-blocking: drain /dev/input/event3, inject SDL2 key events.
   Call at the top of each frame's SDL_PollEvent loop. */
void a30_poll_events(void);

/* Release resources (munmap, close fds).  Call on shutdown. */
void a30_screen_close(void);

/* Portrait-direct blit: write a portrait-format BGRA buffer (produced by the
   sws thread's yuv420p_to_portrait_bgra_2x kernel) directly to fb0.
   Eliminates the strided 90°CCW rotation of a30_flip() for the video pixels.

   portrait_bgra: BGRA buffer, port_w × PANEL_H pixels (row-major, row=panel row)
   port_w:        portrait buffer width in pixels (e.g. 360 for 720p FIT)
   zoom_t:        zoom interpolation: 0.0=FIT (black bars), 0.5=WIDE, 1.0=FILL (no bars)
   has_ui:        0 = fast path (pure sequential blit + black letterboxes);
                  1 = composite path (overlay non-black SDL surface pixels for OSD)

   When has_ui=0 and zoom_t=0 the SDL surface is not accessed at all (~3ms).
   When has_ui=1 the SDL surface is composited on top. */
void a30_flip_video(SDL_Surface *ui_surf,
                    const Uint32 *portrait_bgra,
                    int port_w, float zoom_t, int has_ui);

/* Call immediately after sleep/wake is detected.
   Permanently disables FBIOPAN_DISPLAY for the rest of the session —
   the sunxi driver can block for 100+ seconds after wake waiting for vsync.
   After this call a30_flip() writes directly to the displayed page. */
void a30_screen_wake(void);

#endif /* GVU_A30 */
