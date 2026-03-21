#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "decoder.h"
#include "audio.h"
#include "video.h"
#include "theme.h"

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} PlayerState;

typedef enum {
    ZOOM_FIT = 0,   /* letterbox / pillarbox, aspect preserved (t=0.0) */
    ZOOM_WIDE,      /* reduced bars, slight edge crop, aspect preserved (t=0.5) */
    ZOOM_FILL,      /* crop to fill window entirely, aspect preserved   (t=1.0) */
    ZOOM_COUNT
} ZoomMode;

typedef struct {
    DemuxCtx    demux;
    AudioCtx    audio;
    VideoCtx    video;
    ProbeInfo   probe;
    PlayerState state;
    char        path[1024];

    /* SDL texture for the current video frame (YUV420P / IYUV).
       Owned by the player; recreated when video dimensions change. */
    SDL_Texture *video_tex;

    /* Set to 1 when EOS sentinel received from the frame queue */
    int          eos;

    /* OSD visibility */
    int          osd_visible;
    Uint32       osd_hide_at;  /* SDL_GetTicks() value when OSD should hide */

    /* Volume (mirrors audio.volume; kept here for OSD drawing) */
    float        volume;           /* 0.0–1.0 */
    int          muted;            /* 1 = muted; audio.volume forced to 0 */
    int          vol_osd_visible;
    Uint32       vol_osd_hide_at;

    /* Brightness — 1.0 = full, 0.0 = black (SDL overlay dimming) */
    float        brightness;
    int          bri_osd_visible;
    Uint32       bri_osd_hide_at;

    /* Zoom mode */
    ZoomMode     zoom_mode;
    int          zoom_osd_visible;
    Uint32       zoom_osd_hide_at;

    /* Audio track OSD */
    int          audio_osd_visible;
    Uint32       audio_osd_hide_at;
    char         audio_osd_label[64];
} Player;

/* Open file, initialise demux + audio + video. Does not start playback. */
int  player_open  (Player *p, const char *path, SDL_Renderer *renderer,
                   char *errbuf, int errbuf_sz);

void player_play  (Player *p);
void player_pause (Player *p);
void player_resume(Player *p);

/* Call once per frame in the main loop.
   Checks for new video frames, updates video_tex if one is due (A/V sync).
   Returns 1 if the frame was updated. */
int  player_update(Player *p);

/* Stop and release everything. Safe to call multiple times. */
/* Seek by delta_sec relative to current position (negative = rewind).
   Shows OSD automatically. */
void player_seek   (Player *p, double delta_sec);

/* Seek to an absolute position in seconds. Does NOT show the OSD. */
void player_seek_to(Player *p, double pos_sec);

/* Show the OSD for 3 seconds. */
void player_show_osd(Player *p);

/* Volume control — step is 0.1; clamps to [0.0, 1.0]. */
void player_set_volume  (Player *p, float vol);
void player_volume_up   (Player *p);
void player_volume_dn   (Player *p);
void player_toggle_mute (Player *p);

/* Brightness control — step is 0.1; clamps to [0.1, 1.0]. */
void player_brightness_up(Player *p);
void player_brightness_dn(Player *p);

/* Cycle zoom mode: FIT → WIDE → FILL → FIT */
void player_zoom_cycle(Player *p);

/* Cycle to the next audio track (no-op if only one track). */
void player_cycle_audio(Player *p);

void player_close (Player *p);

/* Render the current frame (or audio-only screen) to the window. */
void player_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                 const Player *p, const Theme *theme, int win_w, int win_h);
