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
#include "platform.h"

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

/* Size of the pre-rotated portrait OSD buffer in pixels.
   For rotation!=0 devices (A30): g_panel_w × g_panel_h = 480 × 640.
   For rotation==0 devices (Miyoo Mini): g_panel_w × g_panel_h = 640 × 480.
   Allocate A30_PORTRAIT_BUF_PIXELS * sizeof(Uint32) bytes for this buffer. */
#define A30_PORTRAIT_BUF_PIXELS  ((size_t)g_panel_w * (size_t)g_panel_h)

/* Pre-rotate an SDL landscape surface (640×480, ARGB8888) into a portrait
   BGRA buffer (480×640) using the same 90°CCW transform as a30_flip().
   out_portrait must point to at least A30_PORTRAIT_BUF_PIXELS * sizeof(Uint32)
   bytes.  Call this once per OSD-dirty frame, then pass the buffer to
   a30_flip_video() so that composite reads are sequential (not strided). */
void a30_surface_to_portrait(SDL_Surface *surf, Uint32 *out_portrait);

/* Portrait-direct blit: write a portrait-format BGRA buffer (produced by the
   sws thread's yuv420p_to_portrait_bgra_2x kernel) directly to fb0.
   Eliminates the strided 90°CCW rotation of a30_flip() for the video pixels.

   osd_portrait:  pre-rotated portrait OSD buffer (from a30_surface_to_portrait),
                  or NULL for the fast path (pure sequential blit, no OSD).
   portrait_bgra: BGRA buffer, port_w × PANEL_H pixels (row-major, row=panel row)
   port_w:        portrait buffer width in pixels (e.g. 360 for 720p FIT)
   zoom_t:        zoom interpolation: 0.0=FIT (black bars), 0.5=WIDE, 1.0=FILL (no bars)

   When osd_portrait=NULL and zoom_t=0 the SDL surface is not accessed (~3ms).
   When osd_portrait!=NULL OSD pixels are composited using sequential reads (~2ms). */
void a30_flip_video(const Uint32 *osd_portrait,
                    const Uint32 *portrait_bgra,
                    int port_w, float zoom_t);

/* Call immediately after sleep/wake is detected.
   Permanently disables FBIOPAN_DISPLAY for the rest of the session —
   the sunxi driver can block for 100+ seconds after wake waiting for vsync.
   After this call a30_flip() writes directly to the displayed page. */
void a30_screen_wake(void);

#endif /* GVU_A30 */
