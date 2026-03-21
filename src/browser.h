#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "filebrowser.h"
#include "theme.h"

/* -------------------------------------------------------------------------
 * Layout & view modes
 * ---------------------------------------------------------------------- */

typedef enum {
    LAYOUT_LARGE = 0,   /* 2-column grid, big tiles  */
    LAYOUT_SMALL,       /* 4-column grid, small tiles */
    LAYOUT_LIST,        /* single-column list         */
    LAYOUT_COUNT
} BrowserLayout;

typedef enum {
    VIEW_FOLDERS = 0,   /* top-level: grid of media folders */
    VIEW_FILES,         /* inside a folder: list of video files */
} BrowserView;

typedef enum {
    BROWSER_ACTION_NONE = 0,
    BROWSER_ACTION_PLAY,         /* user selected a file — path in action_path */
    BROWSER_ACTION_QUIT,
    BROWSER_ACTION_THEME_CYCLE,  /* R1 pressed — caller should call theme_cycle()
                                    then re-render the default cover texture */
    BROWSER_ACTION_SCRAPE_COVERS, /* Y pressed — caller shows confirm + runs scrape */
} BrowserAction;

/* -------------------------------------------------------------------------
 * Browser state
 * ---------------------------------------------------------------------- */

typedef struct {
    BrowserView   view;
    BrowserLayout layout;
    int           selected;     /* highlighted index in current view         */
    int           scroll_row;   /* first visible row (grid) or item (list)   */
    int           folder_idx;   /* which folder is open (VIEW_FILES)         */

    /* set when action == BROWSER_ACTION_PLAY */
    char          action_path[1024];
    BrowserAction action;

    /* Per-file progress cache — rebuilt when folder_idx changes.
       Values: -1.0 = no data, 0.0–1.0 = progress ratio (>=0.95 = completed). */
    float        *file_progress;
    int           prog_folder_idx;  /* folder_idx the cache was built for, -1 = invalid */

    /* B-to-exit confirmation toast */
    int    exit_confirm;      /* 1 = awaiting second B press */
    Uint32 exit_confirm_at;   /* SDL_GetTicks() when confirm was triggered */
} BrowserState;

/* -------------------------------------------------------------------------
 * Cover texture cache — one SDL_Texture* per MediaFolder
 * ---------------------------------------------------------------------- */

typedef struct {
    SDL_Texture **textures;   /* parallel to lib->folders */
    int           count;

    /* Blurred backdrop for VIEW_FILES — rebuilt on folder change */
    SDL_Texture  *backdrop;
    int           backdrop_idx;  /* folder_idx it was built for; -1 = none */
} CoverCache;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/* Call after library_scan(). Allocates the cover texture cache slots. */
void browser_init(BrowserState *state, CoverCache *cache, const MediaLibrary *lib);

/* Draw current view. Call every frame. */
void browser_draw(SDL_Renderer *renderer, TTF_Font *font, TTF_Font *font_small,
                  BrowserState *state, CoverCache *cache, MediaLibrary *lib,
                  SDL_Texture *default_cover, const Theme *theme,
                  int win_w, int win_h);

/* Handle one SDL event. Returns true if event was consumed. */
int browser_handle_event(BrowserState *state, const MediaLibrary *lib,
                         const SDL_Event *ev);

/* Free cached cover textures (not the MediaLibrary itself). */
void cover_cache_free(CoverCache *cache);

/* Free heap allocations inside BrowserState (call before exit). */
void browser_state_free(BrowserState *state);
