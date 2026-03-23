#include "subtitle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Parse SRT timecode "HH:MM:SS,mmm" → seconds. Returns -1 on failure. */
static double parse_tc(const char *s) {
    int h, m, sec, ms;
    if (sscanf(s, "%d:%d:%d,%d", &h, &m, &sec, &ms) == 4)
        return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    return -1.0;
}

/* Build the .srt path for video_path into buf[buf_sz].
   Replaces everything after the last '.' with ".srt".
   If no extension found, appends ".srt". */
static int make_srt_path(const char *video_path, char *buf, int buf_sz) {
    const char *slash = strrchr(video_path, '/');
    const char *dot   = strrchr(video_path, '.');
    /* dot must be after the last slash (i.e. in the filename, not a directory) */
    if (dot && (!slash || dot > slash)) {
        int base = (int)(dot - video_path);
        if (base + 5 > buf_sz) return 0;
        memcpy(buf, video_path, base);
        memcpy(buf + base, ".srt", 5);
    } else {
        int base = (int)strlen(video_path);
        if (base + 5 > buf_sz) return 0;
        memcpy(buf, video_path, base);
        memcpy(buf + base, ".srt", 5);
    }
    return 1;
}

/* Strip inline HTML-style tags (<i>, <b>, <font ...>, etc.) and SSA/ASS
   override blocks ({\an8}, {\i1}, etc.) from src into dst[dst_sz].
   Also strips leading/trailing whitespace per line. */
static void strip_markup(const char *src, char *dst, int dst_sz) {
    int o = 0;
    int in_tag = 0;
    int in_ssa = 0;
    for (const char *p = src; *p && o < dst_sz - 1; p++) {
        if (in_ssa) {
            if (*p == '}') in_ssa = 0;
        } else if (in_tag) {
            if (*p == '>') in_tag = 0;
        } else if (*p == '<') {
            in_tag = 1;
        } else if (*p == '{') {
            in_ssa = 1;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int sub_load(SubCtx *ctx, const char *video_path) {
    memset(ctx, 0, sizeof(*ctx));

    char srt_path[1024];
    if (!make_srt_path(video_path, srt_path, sizeof(srt_path))) return 0;

    FILE *f = fopen(srt_path, "r");
    if (!f) return 0;

    int capacity = 64;
    ctx->entries = malloc(capacity * sizeof(SubEntry));
    if (!ctx->entries) { fclose(f); return 0; }

    char line[1024];
    int  state = 0;   /* 0=seek_index  1=seek_timecode  2=collect_text */
    SubEntry *cur = NULL;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing CR/LF */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
            line[--len] = '\0';

        if (state == 0) {
            /* Expect a sequence number — skip it */
            if (len > 0) state = 1;

        } else if (state == 1) {
            /* Timecode line: "HH:MM:SS,mmm --> HH:MM:SS,mmm" */
            char *arrow = strstr(line, " --> ");
            if (arrow) {
                double start = parse_tc(line);
                double end   = parse_tc(arrow + 5);
                if (start >= 0.0 && end >= 0.0) {
                    if (ctx->count >= capacity) {
                        capacity *= 2;
                        SubEntry *tmp = realloc(ctx->entries,
                                                capacity * sizeof(SubEntry));
                        if (!tmp) break;
                        ctx->entries = tmp;
                    }
                    cur = &ctx->entries[ctx->count++];
                    cur->start_sec = start;
                    cur->end_sec   = end;
                    cur->text[0]   = '\0';
                    state = 2;
                }
            }

        } else { /* state == 2: collecting text lines */
            if (len == 0) {
                state = 0;
                cur   = NULL;
            } else if (cur) {
                /* Append (with newline separator for multi-line entries) */
                int tlen = (int)strlen(cur->text);
                if (tlen > 0 && tlen < SUB_TEXT_MAX - 1)
                    cur->text[tlen++] = '\n';
                char clean[SUB_TEXT_MAX];
                strip_markup(line, clean, sizeof(clean));
                int avail = SUB_TEXT_MAX - tlen - 1;
                if (avail > 0)
                    strncat(cur->text + tlen, clean,
                            (size_t)avail);
            }
        }
    }
    fclose(f);

    if (ctx->count == 0) {
        free(ctx->entries);
        ctx->entries = NULL;
        return 0;
    }

    ctx->enabled = 1;
    fprintf(stderr, "subtitle: loaded %d entries from %s\n",
            ctx->count, srt_path);
    return ctx->count;
}

void sub_free(SubCtx *ctx) {
    free(ctx->entries);
    memset(ctx, 0, sizeof(*ctx));
}

void sub_toggle(SubCtx *ctx) {
    if (ctx->count > 0)
        ctx->enabled = !ctx->enabled;
}

const SubEntry *sub_get(const SubCtx *ctx, double pos_sec) {
    if (!ctx->enabled || !ctx->entries || ctx->count == 0) return NULL;
    /* Apply delay: subtract so a positive delay shifts entries to appear later
       relative to the video clock. */
    double adj = pos_sec - ctx->delay_sec;
    /* Linear scan — typical SRT has < 1000 entries; negligible on ARMv7 */
    for (int i = 0; i < ctx->count; i++) {
        if (adj >= ctx->entries[i].start_sec &&
            adj <  ctx->entries[i].end_sec)
            return &ctx->entries[i];
    }
    return NULL;
}
