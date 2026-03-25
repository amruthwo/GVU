#pragma once

/* Maximum text length per subtitle entry (bytes, including null terminator).
   Multi-line entries store '\n' between lines. */
#define SUB_TEXT_MAX 512

typedef struct {
    double start_sec;
    double end_sec;
    char   text[SUB_TEXT_MAX];
} SubEntry;

typedef struct {
    SubEntry *entries;
    int       count;
    int       enabled;      /* 1 = subtitles on, 0 = off */
    double    delay_sec;    /* offset added to pos_sec before lookup; + = later, - = earlier */
    float     speed;        /* multiplier applied to pos_sec before lookup (default 1.0).
                               Use to correct PAL/NTSC frame-rate drift:
                               1.043 (=25/23.976) when sub made for 23.976 fps, video is 25 fps;
                               0.959 (=23.976/25) for the reverse. */
} SubCtx;

/* Load .srt file corresponding to video_path (replaces extension with .srt).
   Initialises ctx. Returns number of entries loaded (0 if no .srt found). */
int  sub_load  (SubCtx *ctx, const char *video_path);
void sub_free  (SubCtx *ctx);

/* Toggle subtitle visibility. No-op if no subtitles were loaded. */
void sub_toggle(SubCtx *ctx);

/* Return the active entry for pos_sec, or NULL if none / disabled. */
const SubEntry *sub_get(const SubCtx *ctx, double pos_sec);
