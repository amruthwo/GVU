/*
 * render_overlay_test.c — render tutorial and help overlays at a given
 * resolution, save screenshots as PNG, then quit.
 *
 * Build:
 *   gcc -o /tmp/render_overlay_test \
 *       tools/render_overlay_test.c src/overlay.c src/hintbar.c src/theme.c \
 *       -Isrc $(sdl2-config --cflags) -lSDL2_ttf $(sdl2-config --libs) \
 *       -DGVU_TRIMUI_BRICK -lm -lpng
 *
 * Usage:
 *   /tmp/render_overlay_test 1280 720    # TSP
 *   /tmp/render_overlay_test 640 480     # reference / Brick-class
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "overlay.h"
#include "theme.h"
#include "hintbar.h"

/* Stub config functions overlay.c / hintbar.c may reference via theme.h */

static void save_bmp(SDL_Renderer *r, const char *path, int w, int h)
{
    /* ReadPixels must happen BEFORE RenderPresent — it reads the current
       (unswapped) backbuffer; after Present the backbuffer content is undefined
       on double-buffered OpenGL renderers. */
    SDL_Surface *surf = SDL_CreateRGBSurface(0, w, h, 32,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
    SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    SDL_RenderPresent(r);   /* show on screen after saving */
    printf("Saved %s\n", path);
}

int main(int argc, char **argv)
{
    int win_w = 1280, win_h = 720;
    if (argc >= 3) { win_w = atoi(argv[1]); win_h = atoi(argv[2]); }

    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();

    SDL_Window *win = SDL_CreateWindow("Overlay Test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, SDL_WINDOW_SHOWN);
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    /* Font sizing — same logic as main.c (with the ui_scale fix) */
    float ui_scale = win_w / 640.0f;
    { float hs = win_h / 480.0f; if (hs < ui_scale) ui_scale = hs; }
    int font_sz  = (int)(18.0f * ui_scale + 0.5f);
    int font_ssz = (int)(14.0f * ui_scale + 0.5f);

    printf("win=%dx%d  ui_scale=%.3f  font=%d  font_small=%d\n",
           win_w, win_h, ui_scale, font_sz, font_ssz);

    TTF_Font *font       = TTF_OpenFont("resources/fonts/DejaVuSans.ttf", font_sz);
    TTF_Font *font_small = TTF_OpenFont("resources/fonts/DejaVuSans.ttf", font_ssz);
    if (!font || !font_small) {
        fprintf(stderr, "Font load failed: %s\n", TTF_GetError());
        return 1;
    }

    const Theme *theme = &THEMES[0];   /* SPRUCE */

    /* ---- Tutorial slides ---- */
    TutorialState tut = { .active = 1, .slide = 0 };
    for (int s = 0; s < 6; s++) {
        tut.slide = s;
        SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
        SDL_RenderClear(r);
        tutorial_draw(r, font, font_small, &tut, theme, win_w, win_h);
        char path[128];
        snprintf(path, sizeof(path), "/tmp/tut_slide%d_%dx%d.bmp", s, win_w, win_h);
        save_bmp(r, path, win_w, win_h);
    }

    /* ---- Help / controls reference ---- */
    SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
    SDL_RenderClear(r);
    help_draw(r, font, font_small, theme, win_w, win_h);
    char help_path[128];
    snprintf(help_path, sizeof(help_path), "/tmp/help_%dx%d.bmp", win_w, win_h);
    save_bmp(r, help_path, win_w, win_h);

    TTF_CloseFont(font);
    TTF_CloseFont(font_small);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
