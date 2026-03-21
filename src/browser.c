#include "browser.h"
#include "hintbar.h"
#include "resume.h"
#include <SDL2/SDL_image.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Layout geometry
 * ---------------------------------------------------------------------- */

static inline int sc(int base, int w) { return (int)(base * w / 640.0f + 0.5f); }

typedef struct {
    int cols;
    int tile_w, tile_h;
    int img_h;
    int name_h;
    int content_h;
    int rows_visible;
    int hint_bar_h;
    int padding;
} LayoutMetrics;

static LayoutMetrics compute_metrics(BrowserLayout layout, int win_w, int win_h) {
    LayoutMetrics m;
    m.hint_bar_h = sc(24, win_w);
    m.padding    = sc(8,  win_w);
    m.content_h  = win_h - m.hint_bar_h;

    switch (layout) {
        case LAYOUT_LARGE:
            m.cols   = 2;
            m.tile_w = (win_w - m.padding * (m.cols + 1)) / m.cols;
            m.name_h = sc(36, win_w);
            m.tile_h = (m.content_h - m.padding * 3) / 2;
            m.img_h  = m.tile_h - m.name_h - m.padding;
            break;
        case LAYOUT_SMALL:
            m.cols   = 4;
            m.tile_w = (win_w - m.padding * (m.cols + 1)) / m.cols;
            m.name_h = sc(28, win_w);
            m.tile_h = (m.content_h - m.padding * 4) / 3;
            m.img_h  = m.tile_h - m.name_h - m.padding;
            break;
        default: /* LAYOUT_LIST */
            m.cols        = 1;
            m.tile_w      = win_w - m.padding * 2;
            m.tile_h      = sc(52, win_w);
            m.img_h       = m.tile_h - m.padding * 2;
            m.name_h      = m.tile_h;
            break;
    }
    m.rows_visible = m.content_h / (m.tile_h + m.padding);
    return m;
}

/* -------------------------------------------------------------------------
 * Drawing primitives
 * ---------------------------------------------------------------------- */

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *text,
                      int x, int y, int max_w,
                      Uint8 R, Uint8 G, Uint8 B) {
    SDL_Color col = { R, G, B, 255 };
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst = { x, y, surf->w < max_w ? surf->w : max_w, surf->h };
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

/* Slightly darken an RGB value for alternating row tint */
static Uint8 dim(Uint8 v, int amt) {
    return (v > amt) ? (Uint8)(v - amt) : 0;
}

/* Semi-transparent pill background — used to ensure text contrast over backdrops. */
static void fill_pill_bg(SDL_Renderer *r, int x, int y, int w, int h,
                         Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    int rad = h / 2;
    /* Centre rect */
    if (w > 2 * rad) {
        SDL_Rect rect = { x + rad, y, w - 2 * rad, h };
        SDL_RenderFillRect(r, &rect);
    }
    /* Left and right semicircle caps */
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = (int)sqrtf((float)(rad * rad - dy * dy));
        SDL_RenderDrawLine(r, x + rad - dx, y + rad + dy,
                              x + rad,      y + rad + dy);
        SDL_RenderDrawLine(r, x + w - rad,      y + rad + dy,
                              x + w - rad + dx, y + rad + dy);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

/* -------------------------------------------------------------------------
 * Cover texture loading (lazy, cached)
 * ---------------------------------------------------------------------- */

static SDL_Texture *load_cover(SDL_Renderer *renderer, const char *path) {
    SDL_Surface *surf = IMG_Load(path);
    if (!surf) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
}

static SDL_Texture *get_cover(SDL_Renderer *renderer, CoverCache *cache,
                               const MediaLibrary *lib, int idx,
                               SDL_Texture *default_cover) {
    if (cache->textures[idx]) return cache->textures[idx];
    const MediaFolder *f = &lib->folders[idx];
    if (f->cover) {
        SDL_Texture *t = load_cover(renderer, f->cover);
        if (t) { cache->textures[idx] = t; return t; }
    }
    return default_cover;
}

/* -------------------------------------------------------------------------
 * Backdrop: blurred cover image for the file-list view
 * ---------------------------------------------------------------------- */

/* Separable box-blur, horizontal pass.  src and dst are flat w×h arrays
   (stride = w).  Uses clamped-edge extension. */
static void blur_h(const Uint32 *src, Uint32 *dst, int w, int h, int r) {
    int div = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        const Uint32 *row = src + y * w;
        int sr = 0, sg = 0, sb = 0;
        /* Initialise window [-r..r] for x=0 */
        for (int k = -r; k <= r; k++) {
            int xi = k < 0 ? 0 : (k >= w ? w - 1 : k);
            Uint32 c = row[xi];
            sr += (c >> 16) & 0xff;
            sg += (c >>  8) & 0xff;
            sb +=  c        & 0xff;
        }
        for (int x = 0; x < w; x++) {
            dst[y * w + x] = 0xFF000000u
                           | ((Uint32)(sr / div) << 16)
                           | ((Uint32)(sg / div) <<  8)
                           |  (Uint32)(sb / div);
            int rem = x - r;     if (rem < 0)  rem = 0;
            int add = x + r + 1; if (add >= w) add = w - 1;
            Uint32 cr = row[rem], ca = row[add];
            sr += ((ca >> 16) & 0xff) - ((cr >> 16) & 0xff);
            sg += ((ca >>  8) & 0xff) - ((cr >>  8) & 0xff);
            sb += ( ca        & 0xff) - ( cr        & 0xff);
        }
    }
}

/* Separable box-blur, vertical pass. */
static void blur_v(const Uint32 *src, Uint32 *dst, int w, int h, int r) {
    int div = 2 * r + 1;
    for (int x = 0; x < w; x++) {
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            int yi = k < 0 ? 0 : (k >= h ? h - 1 : k);
            Uint32 c = src[yi * w + x];
            sr += (c >> 16) & 0xff;
            sg += (c >>  8) & 0xff;
            sb +=  c        & 0xff;
        }
        for (int y = 0; y < h; y++) {
            dst[y * w + x] = 0xFF000000u
                           | ((Uint32)(sr / div) << 16)
                           | ((Uint32)(sg / div) <<  8)
                           |  (Uint32)(sb / div);
            int rem = y - r;     if (rem < 0)  rem = 0;
            int add = y + r + 1; if (add >= h) add = h - 1;
            Uint32 cr = src[rem * w + x], ca = src[add * w + x];
            sr += ((ca >> 16) & 0xff) - ((cr >> 16) & 0xff);
            sg += ((ca >>  8) & 0xff) - ((cr >>  8) & 0xff);
            sb += ( ca        & 0xff) - ( cr        & 0xff);
        }
    }
}

/* 3 passes of separable box blur ≈ Gaussian blur.  Operates in-place on a
   flat w×h ARGB8888 pixel array.  Allocates two scratch buffers then frees. */
static void gaussian_blur(Uint32 *pixels, int w, int h, int r) {
    size_t n = (size_t)w * h;
    Uint32 *A = malloc(n * sizeof(Uint32));
    Uint32 *B = malloc(n * sizeof(Uint32));
    if (!A || !B) { free(A); free(B); return; }
    memcpy(A, pixels, n * sizeof(Uint32));
    for (int p = 0; p < 3; p++) {
        blur_h(A, B, w, h, r);
        blur_v(B, A, w, h, r);
    }
    memcpy(pixels, A, n * sizeof(Uint32));
    free(A); free(B);
}

/* Build a blurred backdrop texture: cover-fill scale → center-crop → blur. */
static SDL_Texture *build_backdrop(SDL_Renderer *renderer,
                                   const char *cover_path,
                                   int win_w, int win_h) {
    SDL_Surface *orig = IMG_Load(cover_path);
    if (!orig) return NULL;

    SDL_Surface *src = SDL_ConvertSurfaceFormat(orig, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(orig);
    if (!src) return NULL;

    /* Scale so image fills win_w × win_h (cover fill, not letterbox) */
    float sx = (float)win_w / src->w;
    float sy = (float)win_h / src->h;
    float s  = sx > sy ? sx : sy;
    int   sw = (int)(src->w * s + 0.5f);
    int   sh = (int)(src->h * s + 0.5f);

    SDL_Surface *scaled = SDL_CreateRGBSurfaceWithFormat(0, sw, sh, 32,
                                                          SDL_PIXELFORMAT_ARGB8888);
    if (!scaled) { SDL_FreeSurface(src); return NULL; }
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_BlitScaled(src, NULL, scaled, NULL);
    SDL_FreeSurface(src);

    /* Center-crop to exactly win_w × win_h */
    SDL_Surface *bg = SDL_CreateRGBSurfaceWithFormat(0, win_w, win_h, 32,
                                                      SDL_PIXELFORMAT_ARGB8888);
    if (!bg) { SDL_FreeSurface(scaled); return NULL; }
    SDL_Rect crop = { (sw - win_w) / 2, (sh - win_h) / 2, win_w, win_h };
    SDL_SetSurfaceBlendMode(scaled, SDL_BLENDMODE_NONE);
    SDL_BlitSurface(scaled, &crop, bg, NULL);
    SDL_FreeSurface(scaled);

    /* Copy pixels to a flat array (handles SDL surface pitch padding) */
    int     n      = win_w * win_h;
    Uint32 *pixels = malloc((size_t)n * sizeof(Uint32));
    if (pixels) {
        SDL_LockSurface(bg);
        for (int y = 0; y < win_h; y++)
            memcpy(pixels + y * win_w,
                   (Uint8 *)bg->pixels + y * bg->pitch,
                   (size_t)win_w * sizeof(Uint32));
        SDL_UnlockSurface(bg);

        gaussian_blur(pixels, win_w, win_h, 10);

        SDL_LockSurface(bg);
        for (int y = 0; y < win_h; y++)
            memcpy((Uint8 *)bg->pixels + y * bg->pitch,
                   pixels + y * win_w,
                   (size_t)win_w * sizeof(Uint32));
        SDL_UnlockSurface(bg);
        free(pixels);
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, bg);
    SDL_FreeSurface(bg);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    return tex;
}

/* Ensure the cached backdrop matches the current folder; rebuild if not. */
static void ensure_backdrop(SDL_Renderer *renderer, CoverCache *cache,
                             const MediaLibrary *lib, int folder_idx,
                             int win_w, int win_h) {
    if (cache->backdrop_idx == folder_idx) return;
    if (cache->backdrop) { SDL_DestroyTexture(cache->backdrop); cache->backdrop = NULL; }
    cache->backdrop_idx = folder_idx;
    const char *cover = lib->folders[folder_idx].cover;
    if (cover)
        cache->backdrop = build_backdrop(renderer, cover, win_w, win_h);
}

/* -------------------------------------------------------------------------
 * Scroll clamping
 * ---------------------------------------------------------------------- */

static void clamp_scroll(BrowserState *state, int item_count,
                         const LayoutMetrics *m) {
    if (item_count == 0) { state->scroll_row = 0; return; }
    int sel_row = state->selected / m->cols;
    if (sel_row < state->scroll_row)
        state->scroll_row = sel_row;
    if (sel_row >= state->scroll_row + m->rows_visible)
        state->scroll_row = sel_row - m->rows_visible + 1;
    int max_row = (item_count - 1) / m->cols - m->rows_visible + 1;
    if (max_row < 0) max_row = 0;
    if (state->scroll_row > max_row) state->scroll_row = max_row;
    if (state->scroll_row < 0)       state->scroll_row = 0;
}

/* -------------------------------------------------------------------------
 * Folder grid
 * ---------------------------------------------------------------------- */

static void draw_folder_grid(SDL_Renderer *renderer, TTF_Font *font,
                              BrowserState *state, CoverCache *cache,
                              const MediaLibrary *lib, SDL_Texture *default_cover,
                              const Theme *t, int win_w, int win_h) {
    LayoutMetrics m = compute_metrics(state->layout, win_w, win_h);
    clamp_scroll(state, lib->folder_count, &m);

    int first = state->scroll_row * m.cols;
    int last  = first + m.rows_visible * m.cols;
    if (last > lib->folder_count) last = lib->folder_count;

    for (int i = first; i < last; i++) {
        int row = (i / m.cols) - state->scroll_row;
        int col = i % m.cols;
        int x   = m.padding + col * (m.tile_w + m.padding);
        int y   = m.padding + row * (m.tile_h + m.padding);
        int sel = (i == state->selected);

        if (state->layout == LAYOUT_LIST) {
            /* Alternating row tint */
            if (sel) {
                fill_rect(renderer, x, y, m.tile_w, m.tile_h,
                          t->highlight_bg.r, t->highlight_bg.g,
                          t->highlight_bg.b, 0xff);
            } else if (i % 2 == 0) {
                fill_rect(renderer, x, y, m.tile_w, m.tile_h,
                          dim(t->background.r, 8),
                          dim(t->background.g, 8),
                          dim(t->background.b, 8), 0xff);
            }

            /* Thumbnail — fit within thumb box, preserve aspect ratio */
            int thumb_w = (int)(m.img_h * 4.0f / 3.0f);
            SDL_Texture *cover = get_cover(renderer, cache, lib, i, default_cover);
            if (cover) {
                int tw, th;
                SDL_QueryTexture(cover, NULL, NULL, &tw, &th);
                float scale = (tw > 0 && th > 0)
                    ? ((float)thumb_w / tw < (float)m.img_h / th
                       ? (float)thumb_w / tw : (float)m.img_h / th)
                    : 1.0f;
                int dw = (int)(tw * scale);
                int dh = (int)(th * scale);
                SDL_Rect dst = { x + m.padding + (thumb_w - dw) / 2,
                                 y + m.padding + (m.img_h  - dh) / 2, dw, dh };
                SDL_RenderCopy(renderer, cover, NULL, &dst);
            }

            /* Name */
            RGB tc = sel ? t->highlight_text : t->text;
            draw_text(renderer, font, lib->folders[i].name,
                      x + m.padding + thumb_w + m.padding,
                      y + (m.tile_h - TTF_FontHeight(font)) / 2,
                      m.tile_w - thumb_w - m.padding * 3,
                      tc.r, tc.g, tc.b);

        } else {
            /* Grid tile — soft semi-transparent highlight border */
            if (sel) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                fill_rect(renderer, x - 2, y - 2, m.tile_w + 4, m.tile_h + 4,
                          t->highlight_bg.r, t->highlight_bg.g,
                          t->highlight_bg.b, 170);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            }

            /* Cover image — fit within tile, preserve aspect ratio */
            SDL_Texture *cover = get_cover(renderer, cache, lib, i, default_cover);
            if (cover) {
                int tw, th;
                SDL_QueryTexture(cover, NULL, NULL, &tw, &th);
                /* Scale to fit within tile_w × img_h, centred */
                float scale = (tw > 0 && th > 0)
                    ? ((float)m.tile_w / tw < (float)m.img_h / th
                       ? (float)m.tile_w / tw : (float)m.img_h / th)
                    : 1.0f;
                int dw = (int)(tw * scale);
                int dh = (int)(th * scale);
                SDL_Rect dst = { x + (m.tile_w - dw) / 2,
                                 y + (m.img_h  - dh) / 2, dw, dh };
                SDL_RenderCopy(renderer, cover, NULL, &dst);
            } else {
                fill_rect(renderer, x, y, m.tile_w, m.img_h,
                          t->secondary.r, t->secondary.g, t->secondary.b, 0xff);
            }

            /* Name strip */
            RGB strip = sel ? t->highlight_bg : (RGB){ dim(t->background.r, 10),
                                                        dim(t->background.g, 10),
                                                        dim(t->background.b, 10) };
            fill_rect(renderer, x, y + m.img_h, m.tile_w, m.name_h,
                      strip.r, strip.g, strip.b, 0xff);

            RGB tc = sel ? t->highlight_text : t->text;
            {
                int name_w = 0, dummy = 0;
                TTF_SizeUTF8(font, lib->folders[i].name, &name_w, &dummy);
                int name_x = x + (m.tile_w - name_w) / 2;
                if (name_x < x + 2) name_x = x + 2; /* clamp if wider than tile */
                draw_text(renderer, font, lib->folders[i].name,
                          name_x, y + m.img_h + (m.name_h - TTF_FontHeight(font)) / 2,
                          m.tile_w - 4, tc.r, tc.g, tc.b);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * File list
 * ---------------------------------------------------------------------- */

static void draw_file_list(SDL_Renderer *renderer, TTF_Font *font,
                           BrowserState *state, CoverCache *cache,
                           const MediaLibrary *lib,
                           const Theme *t, int win_w, int win_h) {
    const MediaFolder *folder = &lib->folders[state->folder_idx];
    int has_backdrop = (cache->backdrop != NULL);
    int hint_bar_h = sc(24, win_w);
    int padding    = sc(8,  win_w);
    int row_h      = sc(52, win_w);

    /* Rebuild progress cache when the folder changes */
    if (state->prog_folder_idx != state->folder_idx) {
        free(state->file_progress);
        state->prog_folder_idx = state->folder_idx;
        state->file_progress   = NULL;
        if (folder->file_count > 0) {
            state->file_progress = malloc((size_t)folder->file_count * sizeof(float));
            if (state->file_progress) {
                for (int i = 0; i < folder->file_count; i++)
                    state->file_progress[i] = -1.0f;
                ResumeEntry *entries = NULL;
                int ec = resume_load_all(&entries);
                for (int ei = 0; ei < ec; ei++) {
                    if (entries[ei].duration < 5.0) continue;
                    for (int fi = 0; fi < folder->file_count; fi++) {
                        if (strcmp(entries[ei].path, folder->files[fi].path) == 0) {
                            state->file_progress[fi] =
                                (float)(entries[ei].position / entries[ei].duration);
                            break;
                        }
                    }
                }
                free(entries);
            }
        }
    }
    int content_h = win_h - hint_bar_h - row_h; /* reserve bottom row for folder name */
    int visible   = content_h / row_h;

    /* Draw blurred backdrop + dim overlay */
    if (has_backdrop) {
        SDL_RenderCopy(renderer, cache->backdrop, NULL, NULL);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        fill_rect(renderer, 0, 0, win_w, win_h, 0, 0, 0, 120);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    /* Clamp scroll */
    if (state->selected < state->scroll_row)
        state->scroll_row = state->selected;
    if (state->selected >= state->scroll_row + visible)
        state->scroll_row = state->selected - visible + 1;
    if (state->scroll_row < 0) state->scroll_row = 0;

    for (int i = state->scroll_row; i < state->scroll_row + visible; i++) {
        if (i >= folder->file_count) break;
        int row = i - state->scroll_row;
        int y   = row * row_h;
        int sel = (i == state->selected);

        if (has_backdrop) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            if (sel) {
                fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                          t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 200);
            } else if (row % 2 == 0) {
                fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                          0, 0, 0, 40);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        } else {
            if (sel) {
                fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                          t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 0xff);
            } else if (row % 2 == 0) {
                fill_rect(renderer, padding, y, win_w - padding * 2, row_h,
                          dim(t->background.r, 8),
                          dim(t->background.g, 8),
                          dim(t->background.b, 8), 0xff);
            }
        }

        /* Pill background behind filename when a backdrop is active */
        if (has_backdrop && !sel) {
            int fh = TTF_FontHeight(font);
            int tw = 0, dummy = 0;
            TTF_SizeUTF8(font, folder->files[i].name, &tw, &dummy);
            int pill_h = fh + 10;
            int pill_pad = sc(8, win_w);
            int pill_x = padding * 2 - pill_pad;
            int pill_w = tw + pill_pad * 2;
            int max_pill_w = win_w - padding * 2 - pill_pad;
            if (pill_w > max_pill_w) pill_w = max_pill_w;
            int pill_y = y + (row_h - pill_h) / 2;
            fill_pill_bg(renderer, pill_x, pill_y, pill_w, pill_h,
                         t->background.r, t->background.g, t->background.b, 170);
        }

        RGB tc = sel ? t->highlight_text : t->text;
        draw_text(renderer, font, folder->files[i].name,
                  padding * 2,
                  y + (row_h - TTF_FontHeight(font)) / 2,
                  win_w - padding * 4,
                  tc.r, tc.g, tc.b);

        /* Progress bar — thin strip at the bottom of the row */
        if (state->file_progress && state->file_progress[i] >= 0.0f) {
            float prog = state->file_progress[i];
            int bar_y  = y + row_h - 3;
            int full_w = win_w - padding * 2;
            if (prog >= 0.95f) {
                /* Completed — faint full-width bar in secondary color */
                fill_rect(renderer, padding, bar_y, full_w, 3,
                          t->secondary.r, t->secondary.g, t->secondary.b, 120);
            } else if (prog > 0.0f) {
                /* In-progress — filled portion in highlight_bg */
                int fill_w = (int)(full_w * prog);
                fill_rect(renderer, padding, bar_y, fill_w, 3,
                          t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b, 200);
            }
        }
    }

    /* Folder name header at bottom */
    int header_y = win_h - hint_bar_h - row_h;
    if (has_backdrop) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        fill_rect(renderer, 0, header_y, win_w, row_h, 0, 0, 0, 170);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    } else {
        fill_rect(renderer, 0, header_y, win_w, row_h,
                  dim(t->background.r, 15),
                  dim(t->background.g, 15),
                  dim(t->background.b, 15), 0xff);
    }
    draw_text(renderer, font, folder->name,
              padding, header_y + (row_h - TTF_FontHeight(font)) / 2,
              win_w - padding * 2,
              t->secondary.r, t->secondary.g, t->secondary.b);
}

/* -------------------------------------------------------------------------
 * Hint bar
 * ---------------------------------------------------------------------- */

static void draw_hint_bar(SDL_Renderer *renderer, TTF_Font *font,
                          TTF_Font *font_small, const BrowserState *state,
                          const Theme *t, int win_w, int win_h) {
    static const HintItem folder_hints[] = {
        { "A",   "Open"    },
        { "B",   "Exit"    },
        { "SEL", "Layout"  },
        { "X",   "History" },
        { "R1",  "Theme"   },
    };
    static const HintItem file_hints[] = {
        { "A",    "Play"    },
        { "B",    "Back"    },
        { "L2",   "Prev"    },
        { "R2",   "Next"    },
        { "X",    "History" },
        { "MENU", "Exit"    },
    };
    const HintItem *items = (state->view == VIEW_FOLDERS) ? folder_hints : file_hints;
    int item_count = (state->view == VIEW_FOLDERS) ? 5 : 6;
    hintbar_draw_row(renderer, font, font_small, items, item_count, t, win_w, win_h);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void browser_init(BrowserState *state, CoverCache *cache,
                  const MediaLibrary *lib) {
    memset(state, 0, sizeof(*state));
    state->view   = VIEW_FOLDERS;
    state->layout = LAYOUT_LARGE;

    cache->count        = lib->folder_count;
    cache->textures     = calloc((size_t)(lib->folder_count ? lib->folder_count : 1),
                                 sizeof(SDL_Texture *));
    cache->backdrop     = NULL;
    cache->backdrop_idx = -1;

    state->file_progress    = NULL;
    state->prog_folder_idx  = -1;
}

void browser_draw(SDL_Renderer *renderer, TTF_Font *font, TTF_Font *font_small,
                  BrowserState *state, CoverCache *cache, MediaLibrary *lib,
                  SDL_Texture *default_cover, const Theme *theme,
                  int win_w, int win_h) {
    (void)font_small;

    SDL_SetRenderDrawColor(renderer,
        theme->background.r, theme->background.g, theme->background.b, 0xff);
    SDL_RenderClear(renderer);

    if (state->view == VIEW_FOLDERS) {
        if (lib->folder_count == 0) {
            int padding = sc(8, win_w);
            draw_text(renderer, font, "No media found.",
                      padding, padding, win_w - padding * 2,
                      theme->secondary.r, theme->secondary.g, theme->secondary.b);
        } else {
            draw_folder_grid(renderer, font, state, cache, lib,
                             default_cover, theme, win_w, win_h);
        }
    } else {
        ensure_backdrop(renderer, cache, lib, state->folder_idx, win_w, win_h);
        draw_file_list(renderer, font, state, cache, lib, theme, win_w, win_h);
    }

    /* Exit-confirm toast — auto-clears after 3 s */
    if (state->exit_confirm) {
        if (SDL_GetTicks() - state->exit_confirm_at > 3000) {
            state->exit_confirm = 0;
        } else {
            static const HintItem toast_hint = { "B", "again to exit" };
            int hint_h  = sc(24, win_w);
            int toast_h = sc(30, win_w);
            int toast_w = win_w / 2;
            int toast_x = (win_w - toast_w) / 2;
            int toast_y = win_h - hint_h - sc(6, win_w) - toast_h;

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 210);
            SDL_Rect bg = { toast_x, toast_y, toast_w, toast_h };
            SDL_RenderFillRect(renderer, &bg);
            /* Highlight border */
            SDL_SetRenderDrawColor(renderer,
                                   theme->highlight_bg.r,
                                   theme->highlight_bg.g,
                                   theme->highlight_bg.b, 180);
            SDL_RenderDrawRect(renderer, &bg);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

            int glyph_h = TTF_FontHeight(font_small)
                          + TTF_FontHeight(font_small) * 2 / 7;
            int iw = hintbar_item_width(font_small, &toast_hint, glyph_h);
            int ix = toast_x + (toast_w - iw) / 2;
            hintbar_draw_items(renderer, font_small, &toast_hint, 1,
                               theme, ix, toast_y, toast_h);
        }
    }

    draw_hint_bar(renderer, font, font_small, state, theme, win_w, win_h);
}

int browser_handle_event(BrowserState *state, const MediaLibrary *lib,
                         const SDL_Event *ev) {
    if (ev->type != SDL_KEYDOWN) return 0;

    SDL_Keycode key = ev->key.keysym.sym;

    /* Allow held-key repeat only for navigation — not for action buttons */
    if (ev->key.repeat &&
        key != SDLK_UP && key != SDLK_DOWN &&
        key != SDLK_LEFT && key != SDLK_RIGHT)
        return 0;

    if (state->view == VIEW_FOLDERS) {
        int count = lib->folder_count;
        LayoutMetrics m = compute_metrics(state->layout, 640, 480);

        /* Any navigation key cancels a pending exit-confirm toast */
        if (state->exit_confirm
                && key != SDLK_LCTRL && key != SDLK_BACKSPACE) {
            state->exit_confirm = 0;
        }

        /* B button — two-press exit with toast confirmation */
        if (key == SDLK_LCTRL || key == SDLK_BACKSPACE) {
            if (state->exit_confirm) {
                state->action = BROWSER_ACTION_QUIT;
            } else {
                state->exit_confirm    = 1;
                state->exit_confirm_at = SDL_GetTicks();
            }
            return 1;
        }

        if (key == SDLK_UP)    { state->selected -= m.cols; if (state->selected < 0) state->selected = 0; return 1; }
        if (key == SDLK_DOWN)  { state->selected += m.cols; if (state->selected >= count) state->selected = count - 1; return 1; }
        if (key == SDLK_LEFT)  { if (state->selected > 0)         state->selected--; return 1; }
        if (key == SDLK_RIGHT) { if (state->selected < count - 1) state->selected++; return 1; }

        if (key == SDLK_RETURN || key == SDLK_SPACE) {
            if (count > 0) {
                state->folder_idx = state->selected;
                state->selected   = 0;
                state->scroll_row = 0;
                state->view       = VIEW_FILES;
            }
            return 1;
        }
        if (key == SDLK_RCTRL || key == SDLK_TAB) {
            state->layout = (state->layout + 1) % LAYOUT_COUNT;
            return 1;
        }
        if (key == SDLK_PAGEDOWN) {
            state->action = BROWSER_ACTION_THEME_CYCLE;
            return 1;
        }

    } else {
        const MediaFolder *folder = &lib->folders[state->folder_idx];
        int count = folder->file_count;

        if (key == SDLK_UP)   { if (state->selected > 0)          state->selected--; return 1; }
        if (key == SDLK_DOWN) { if (state->selected < count - 1)  state->selected++; return 1; }

        if (key == SDLK_RETURN || key == SDLK_SPACE) {
            if (count > 0) {
                snprintf(state->action_path, sizeof(state->action_path),
                         "%s", folder->files[state->selected].path);
                state->action = BROWSER_ACTION_PLAY;
            }
            return 1;
        }
        if (key == SDLK_LCTRL || key == SDLK_BACKSPACE) {
            state->selected   = state->folder_idx;
            state->scroll_row = state->folder_idx /
                                compute_metrics(state->layout, 640, 480).cols;
            state->view = VIEW_FOLDERS;
            return 1;
        }
        if (key == SDLK_RCTRL || key == SDLK_TAB) {
            state->layout = (state->layout + 1) % LAYOUT_COUNT;
            return 1;
        }
        if (key == SDLK_PAGEDOWN) {
            state->action = BROWSER_ACTION_THEME_CYCLE;
            return 1;
        }
        /* L2 / R2 — jump to previous / next folder without leaving VIEW_FILES */
        if (key == SDLK_COMMA && state->folder_idx > 0) {
            state->folder_idx--;
            state->selected   = 0;
            state->scroll_row = 0;
            return 1;
        }
        if (key == SDLK_PERIOD && state->folder_idx < lib->folder_count - 1) {
            state->folder_idx++;
            state->selected   = 0;
            state->scroll_row = 0;
            return 1;
        }
    }

    return 0;
}

void browser_state_free(BrowserState *state) {
    free(state->file_progress);
    state->file_progress   = NULL;
    state->prog_folder_idx = -1;
}

void cover_cache_free(CoverCache *cache) {
    for (int i = 0; i < cache->count; i++) {
        if (cache->textures[i]) SDL_DestroyTexture(cache->textures[i]);
    }
    free(cache->textures);
    if (cache->backdrop) SDL_DestroyTexture(cache->backdrop);
    memset(cache, 0, sizeof(*cache));
}
