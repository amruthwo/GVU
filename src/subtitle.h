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
} SubCtx;

/* Load .srt file corresponding to video_path (replaces extension with .srt).
   Initialises ctx. Returns number of entries loaded (0 if no .srt found). */
int  sub_load  (SubCtx *ctx, const char *video_path);
void sub_free  (SubCtx *ctx);

/* Toggle subtitle visibility. No-op if no subtitles were loaded. */
void sub_toggle(SubCtx *ctx);

/* Return the active entry for pos_sec, or NULL if none / disabled. */
const SubEntry *sub_get(const SubCtx *ctx, double pos_sec);
