#include "player.h"
#include "hintbar.h"
#include "statusbar.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include "subtitle.h"

#define AV_SYNC_THRESHOLD_SEC  0.040   /* 40ms — within one frame at 25fps */
#define AV_NOSYNC_THRESHOLD_SEC 10.0   /* drop frames further than 10s behind */

/* -------------------------------------------------------------------------
 * Open
 * ---------------------------------------------------------------------- */

int player_open(Player *p, const char *path, SDL_Renderer *renderer,
                char *errbuf, int errbuf_sz) {
    memset(p, 0, sizeof(*p));
    snprintf(p->path, sizeof(p->path), "%s", path);

    if (decoder_probe(path, &p->probe, errbuf, errbuf_sz) < 0)
        return -1;

    if (demux_open(&p->demux, path, errbuf, errbuf_sz) < 0)
        return -1;

    /* Audio */
    if (p->demux.audio_stream_idx >= 0) {
        AVStream *as = p->demux.fmt_ctx->streams[p->demux.audio_stream_idx];
        if (audio_open(&p->audio, as->codecpar,
                       &p->demux.audio_queue, errbuf, errbuf_sz) < 0) {
            demux_close(&p->demux);
            return -1;
        }
        p->audio.codec_ctx->pkt_timebase = as->time_base;
    }

    /* Video */
    if (p->demux.video_stream_idx >= 0) {
        AVStream *vs = p->demux.fmt_ctx->streams[p->demux.video_stream_idx];
        if (video_open(&p->video, vs->codecpar, vs->time_base,
                       &p->demux.video_queue, errbuf, errbuf_sz) < 0) {
            audio_close(&p->audio);
            demux_close(&p->demux);
            return -1;
        }

        /* Create the YUV streaming texture at the video's native resolution */
        p->video_tex = SDL_CreateTexture(renderer,
                                         SDL_PIXELFORMAT_IYUV,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         p->video.native_w,
                                         p->video.native_h);
        if (!p->video_tex) {
            fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
            video_close(&p->video);
            audio_close(&p->audio);
            demux_close(&p->demux);
            return -1;
        }
    }

    p->state      = PLAYER_STOPPED;
    p->volume     = 1.0f;
    p->brightness = 1.0f;

    sub_load(&p->subtitle, path);
    return 0;
}

/* -------------------------------------------------------------------------
 * Playback control
 * ---------------------------------------------------------------------- */

void player_play(Player *p) {
    if (p->state != PLAYER_STOPPED) return;
    if (p->demux.audio_stream_idx >= 0) audio_start(&p->audio);
    if (p->demux.video_stream_idx >= 0) video_start(&p->video);
    demux_start(&p->demux);
    p->state = PLAYER_PLAYING;
}

void player_pause(Player *p) {
    if (p->state != PLAYER_PLAYING) return;
    audio_pause(&p->audio, 1);
    p->state = PLAYER_PAUSED;
}

void player_resume(Player *p) {
    if (p->state != PLAYER_PAUSED) return;
    audio_pause(&p->audio, 0);
    p->state = PLAYER_PLAYING;
}

void player_show_osd(Player *p) {
    p->osd_visible = 1;
    p->osd_hide_at = SDL_GetTicks() + 3000;
}

void player_set_volume(Player *p, float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    p->volume = vol;
    atomic_store(&p->audio.volume, vol);
    p->vol_osd_visible = 1;
    p->vol_osd_hide_at = SDL_GetTicks() + 1500;
}

void player_volume_up(Player *p) { player_set_volume(p, p->volume + 0.1f); }
void player_volume_dn(Player *p) { player_set_volume(p, p->volume - 0.1f); }

void player_toggle_mute(Player *p) {
    p->muted = !p->muted;
    atomic_store(&p->audio.volume, p->muted ? 0.0f : p->volume);
    p->vol_osd_visible = 1;
    p->vol_osd_hide_at = SDL_GetTicks() + 1500;
}

static void set_brightness(Player *p, float b) {
    if (b < 0.1f) b = 0.1f;
    if (b > 1.0f) b = 1.0f;
    p->brightness = b;
    p->bri_osd_visible = 1;
    p->bri_osd_hide_at = SDL_GetTicks() + 1500;
}
void player_brightness_up(Player *p) { set_brightness(p, p->brightness + 0.1f); }
void player_brightness_dn(Player *p) { set_brightness(p, p->brightness - 0.1f); }

void player_cycle_audio(Player *p) {
    if (p->demux.audio_stream_count <= 1) return;

    double current_pos = audio_get_clock(&p->audio);
    int new_idx = demux_cycle_audio(&p->demux);
    AVStream *as = p->demux.fmt_ctx->streams[new_idx];

    char errbuf[256] = {0};
    if (audio_switch_stream(&p->audio, &p->demux.audio_queue,
                            as->codecpar, as->time_base,
                            errbuf, sizeof(errbuf)) < 0) {
        fprintf(stderr, "audio_switch_stream: %s\n", errbuf);
        return;
    }

    if (p->demux.video_stream_idx >= 0) {
        /* Step 1: flush the frame queue directly.  The video decode thread
           may have pre-decoded several seconds of "future" frames (pts >
           current_pos) and be blocked in fq_push waiting for the main thread
           to pop them.  The main thread never pops them because their diff is
           > AV_SYNC_THRESHOLD, so the thread can never reach the flush
           sentinel — causing the 3-4s freeze.  Clearing here unblocks it. */
        video_flush_frames(&p->video);
        /* Step 2: flush the video packet queue and inject a sentinel so the
           decode thread also flushes its codec state.  The demux_request_seek
           below will send a second sentinel after repositioning the file. */
        packet_queue_flush(&p->demux.video_queue);
        packet_queue_put_flush(&p->demux.video_queue);
    }

    /* Seek both streams back to where we were.  The demux was several seconds
       ahead; this repositions it to current_pos and sends fresh flush
       sentinels to both decode threads so they restart cleanly from a
       keyframe — eliminating the forward jump and H.264 reference errors. */
    player_seek_to(p, current_pos);

    /* Build OSD label — use language tag if available */
    int pos = 0;
    for (int i = 0; i < p->demux.audio_stream_count; i++) {
        if (p->demux.audio_stream_indices[i] == new_idx) { pos = i; break; }
    }
    AVDictionaryEntry *lang = av_dict_get(as->metadata, "language", NULL, 0);
    if (lang)
        snprintf(p->audio_osd_label, sizeof(p->audio_osd_label),
                 "Audio %d/%d: %s", pos + 1, p->demux.audio_stream_count, lang->value);
    else
        snprintf(p->audio_osd_label, sizeof(p->audio_osd_label),
                 "Audio %d/%d", pos + 1, p->demux.audio_stream_count);

    p->audio_osd_visible = 1;
    p->audio_osd_hide_at = SDL_GetTicks() + 2000;
}

void player_zoom_cycle(Player *p) {
    p->zoom_mode = (ZoomMode)((p->zoom_mode + 1) % ZOOM_COUNT);
    p->zoom_osd_visible = 1;
    p->zoom_osd_hide_at = SDL_GetTicks() + 1500;
}

void player_toggle_subs(Player *p) {
    if (p->subtitle.count == 0) {
        snprintf(p->sub_osd_label, sizeof(p->sub_osd_label), "No subtitles");
    } else {
        sub_toggle(&p->subtitle);
        snprintf(p->sub_osd_label, sizeof(p->sub_osd_label),
                 "Subtitles %s", p->subtitle.enabled ? "ON" : "OFF");
    }
    p->sub_osd_visible = 1;
    p->sub_osd_hide_at = SDL_GetTicks() + 1500;
}

void player_seek(Player *p, double delta_sec) {
    if (p->state == PLAYER_STOPPED) return;
    double current = audio_get_clock(&p->audio);
    double target  = current + delta_sec;
    if (target < 0) target = 0;
    if (target > p->probe.duration_sec) target = p->probe.duration_sec;
    /* Flush the decoded frame queue so the decode thread isn't blocked in
       fq_push waiting for the main thread to consume stale future frames.
       Without this, backward seeks leave high-PTS frames in the queue that
       never pass the A/V sync check, permanently deadlocking the video path. */
    if (p->demux.video_stream_idx >= 0)
        video_flush_frames(&p->video);
    demux_request_seek(&p->demux, target);
    /* Optimistically update clock so OSD shows the right position immediately */
    SDL_LockMutex(p->audio.clock_mutex);
    p->audio.clock = target;
    SDL_UnlockMutex(p->audio.clock_mutex);
    player_show_osd(p);
}

void player_seek_to(Player *p, double pos_sec) {
    if (p->state == PLAYER_STOPPED) return;
    double target = pos_sec;
    if (target < 0) target = 0;
    if (target > p->probe.duration_sec) target = p->probe.duration_sec;
    if (p->demux.video_stream_idx >= 0)
        video_flush_frames(&p->video);
    demux_request_seek(&p->demux, target);
    SDL_LockMutex(p->audio.clock_mutex);
    p->audio.clock = target;
    SDL_UnlockMutex(p->audio.clock_mutex);
}

void player_close(Player *p) {
    /* Stop threads first */
    audio_stop(&p->audio);
    video_stop(&p->video);
    demux_stop(&p->demux);

    /* Free resources */
    audio_close(&p->audio);
    video_close(&p->video);
    demux_close(&p->demux);

    if (p->video_tex) { SDL_DestroyTexture(p->video_tex); p->video_tex = NULL; }
    sub_free(&p->subtitle);
    p->state = PLAYER_STOPPED;
}

/* -------------------------------------------------------------------------
 * A/V sync + texture update — call once per render loop iteration
 * ---------------------------------------------------------------------- */

int player_update(Player *p) {
    /* Auto-hide OSD after 3s during playback */
    if (p->osd_visible && p->state == PLAYER_PLAYING &&
        SDL_GetTicks() >= p->osd_hide_at)
        p->osd_visible = 0;

    /* Auto-hide volume / brightness / zoom OSD */
    if (p->vol_osd_visible  && SDL_GetTicks() >= p->vol_osd_hide_at)
        p->vol_osd_visible  = 0;
    if (p->bri_osd_visible  && SDL_GetTicks() >= p->bri_osd_hide_at)
        p->bri_osd_visible  = 0;
    if (p->zoom_osd_visible  && SDL_GetTicks() >= p->zoom_osd_hide_at)
        p->zoom_osd_visible  = 0;
    if (p->audio_osd_visible && SDL_GetTicks() >= p->audio_osd_hide_at)
        p->audio_osd_visible = 0;
    if (p->sub_osd_visible   && SDL_GetTicks() >= p->sub_osd_hide_at)
        p->sub_osd_visible   = 0;

    /* Audio-only EOS: no video queue, so check audio thread directly */
    if (!p->eos && p->demux.video_stream_idx < 0 && p->audio.eos)
        p->eos = 1;

    if (p->state == PLAYER_STOPPED || p->state == PLAYER_PAUSED || !p->video_tex) return 0;

    /* Use audio clock if audio is present, otherwise wall clock */
    double master_clock = (p->demux.audio_stream_idx >= 0)
                          ? audio_get_clock(&p->audio)
                          : 0.0;

    /* When audio EOS is reached and the ring is fully drained, the audio
       clock freezes.  Any video frames with PTS beyond that frozen value
       would be permanently "too early" and the EOS sentinel would never
       be reached.  Detect this case so we can force the remaining frames
       through below. */
    int audio_done = 0;
    if (p->demux.audio_stream_idx >= 0) {
        SDL_LockMutex(p->audio.ring.mutex);
        audio_done = p->audio.eos && (p->audio.ring.filled == 0);
        SDL_UnlockMutex(p->audio.ring.mutex);
    }

    int updated = 0;

    /* Drain video frames that are due or overdue */
    while (1) {
        VideoFrame vf;
        if (!video_peek_frame(&p->video, &vf)) break;

        if (vf.pts < 0) {
            /* EOS sentinel */
            video_pop_frame(&p->video);
            p->eos = 1;
            break;
        }

        double diff = vf.pts - master_clock;

        if (diff > AV_SYNC_THRESHOLD_SEC && !audio_done) {
            /* Frame is too early — display it later.
               If audio is done and the clock is frozen, skip this check
               so the remaining video frames drain and EOS is detected. */
            break;
        }

        if (diff < -AV_NOSYNC_THRESHOLD_SEC) {
            /* Way behind — drop without displaying */
            video_pop_frame(&p->video);
            continue;
        }

        /* Frame is due (or slightly late) — upload to texture */
        AVFrame *frame = vf.frame;
        SDL_UpdateYUVTexture(p->video_tex, NULL,
                             frame->data[0], frame->linesize[0],
                             frame->data[1], frame->linesize[1],
                             frame->data[2], frame->linesize[2]);
        video_pop_frame(&p->video);
        updated = 1;

        /* Only show one frame per render loop iteration */
        break;
    }

    return updated;
}

/* -------------------------------------------------------------------------
 * Draw helpers
 * ---------------------------------------------------------------------- */

static inline int sc(int base, int w) { return (int)(base * w / 640.0f + 0.5f); }

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawColor(r, R, G, B, 0xff);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

static void fill_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h,
                               int rad, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    if (rad <= 0 || rad * 2 >= w || rad * 2 >= h) {
        if (rad * 2 >= h) rad = h / 2;
        if (rad <= 0) {
            SDL_Rect rect = {x, y, w, h};
            SDL_RenderFillRect(r, &rect);
            if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
            return;
        }
    }
    SDL_Rect c = {x + rad, y, w - 2 * rad, h};
    SDL_RenderFillRect(r, &c);
    SDL_Rect left  = {x,         y + rad, rad, h - 2 * rad};
    SDL_Rect right = {x + w - rad, y + rad, rad, h - 2 * rad};
    SDL_RenderFillRect(r, &left);
    SDL_RenderFillRect(r, &right);
    for (int dy = 0; dy < rad; dy++) {
        int dist = rad - dy;
        int span = (int)sqrtf((float)(rad * rad - dist * dist));
        SDL_RenderDrawLine(r, x + rad - span, y + dy,         x + rad - 1,         y + dy);
        SDL_RenderDrawLine(r, x + w - rad,    y + dy,         x + w - rad + span - 1, y + dy);
        SDL_RenderDrawLine(r, x + rad - span, y + h - 1 - dy, x + rad - 1,         y + h - 1 - dy);
        SDL_RenderDrawLine(r, x + w - rad,    y + h - 1 - dy, x + w - rad + span - 1, y + h - 1 - dy);
    }
    if (A != 0xff) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *text,
                      int x, int y, int max_w, Uint8 R, Uint8 G, Uint8 B) {
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

/* -------------------------------------------------------------------------
 * Draw
 * ---------------------------------------------------------------------- */

/* Draw the OSD overlay (progress bar, timestamp, filename, hint bar).
   Called when osd_visible is true, or always in audio-only mode. */
static void draw_osd(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                     const Player *p, const Theme *t, int win_w, int win_h) {
    double clock = audio_get_clock(&p->audio);
    char pos[32], dur[32];
    format_duration(clock,                pos, sizeof(pos));
    format_duration(p->probe.duration_sec, dur, sizeof(dur));

    int osd_h    = sc(80, win_w);
    int osd_y    = win_h - osd_h;
    int bar_h    = sc(6, win_w);
    int bar_y    = osd_y + sc(12, win_w);
    int bar_x    = sc(16, win_w);
    int bar_w    = win_w - sc(32, win_w);
    int text_y   = bar_y + bar_h + sc(8, win_w);

    /* Semi-transparent background */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
    SDL_Rect bg = { 0, osd_y, win_w, osd_h };
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    /* Progress bar track */
    fill_rect(r, bar_x, bar_y, bar_w, bar_h,
              t->secondary.r, t->secondary.g, t->secondary.b);
    /* Progress bar fill */
    if (p->probe.duration_sec > 0) {
        float frac = (float)(clock / p->probe.duration_sec);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        fill_rect(r, bar_x, bar_y, (int)(bar_w * frac), bar_h,
                  t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b);
        /* Scrubber dot */
        int dot_x = bar_x + (int)(bar_w * frac) - 4;
        fill_rect(r, dot_x, bar_y - 3, 8, bar_h + 6,
                  t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
    }

    /* Timestamp left, total right */
    draw_text(r, font, pos, bar_x, text_y, 120, 0xff, 0xff, 0xff);
    char dur_label[40];
    snprintf(dur_label, sizeof(dur_label), "%s", dur);
    int dur_w = 0, dur_dummy = 0;
    TTF_SizeUTF8(font, dur_label, &dur_w, &dur_dummy);
    draw_text(r, font, dur_label, win_w - sc(16, win_w) - dur_w, text_y,
              dur_w + 4, 0xff, 0xff, 0xff);

    /* Filename centred */
    const char *slash = strrchr(p->path, '/');
    const char *name  = slash ? slash + 1 : p->path;
    int name_w = 0;
    TTF_SizeUTF8(font, name, &name_w, &dur_dummy);
    int name_x = (win_w - name_w) / 2;
    if (name_x < sc(8, win_w)) name_x = sc(8, win_w);
    draw_text(r, font, name, name_x, text_y, win_w - sc(16, win_w),
              t->text.r, t->text.g, t->text.b);

    /* Pause icon — centred in the area above the OSD bar */
    if (p->state == PLAYER_PAUSED) {
        int glyph_h  = sc(40, win_w);
        int bar2_w   = sc(10, win_w);
        int bar2_gap = sc(4,  win_w);
        int glyph_y  = osd_y / 2 - glyph_h / 2;
        fill_rect(r, win_w / 2 - bar2_gap - bar2_w, glyph_y, bar2_w, glyph_h,
                  t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
        fill_rect(r, win_w / 2 + bar2_gap, glyph_y, bar2_w, glyph_h,
                  t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
    }

    /* Hint bar — drawn at the very bottom of the OSD area */
    static const HintItem play_hints[] = {
        { "A",   "Pause" }, { "\xe2\x86\x90\xe2\x86\x92", "\xc2\xb1" "10s" },
        { "L1",  "–60s" }, { "R1",  "+60s" },
        { "START", "Subs" }, { "B", "Stop" },
    };
    static const HintItem pause_hints[] = {
        { "A",   "Resume" }, { "\xe2\x86\x90\xe2\x86\x92", "\xc2\xb1" "10s" },
        { "L1",  "–60s"  }, { "R1",  "+60s" },
        { "START", "Subs" }, { "B", "Stop" },
    };
    const HintItem *hints = (p->state == PLAYER_PAUSED) ? pause_hints : play_hints;
    int hint_count = 6;
    int hint_bar_h = sc(24, win_w);
    int hint_y     = win_h - hint_bar_h;
    /* glyph_h: font height + ~28% padding (matches hintbar.c's glyph_pad) */
    int gh = TTF_FontHeight(font_small) + TTF_FontHeight(font_small) * 2 / 7;
    /* item_gap: ~129% of font height (matches hintbar.c's item_gap) */
    int ig = TTF_FontHeight(font_small) * 9 / 7;
    int total_w = 0;
    for (int i = 0; i < hint_count; i++)
        total_w += hintbar_item_width(font_small, &hints[i], gh)
                   + (i < hint_count - 1 ? ig : 0);
    int hint_x = (win_w - total_w) / 2;
    if (hint_x < sc(8, win_w)) hint_x = sc(8, win_w);
    hintbar_draw_items(r, font_small, hints, hint_count, t, hint_x, hint_y, hint_bar_h);
}

/* Generic segmented bar used for both volume and brightness OSD */
static void draw_indicator_bar(SDL_Renderer *r, TTF_Font *font,
                               const Theme *t, const char *label,
                               float value, int box_x, int box_y, int win_w) {
    int steps  = 10;
    int filled = (int)(value * steps + 0.5f);
    int seg_w  = sc(14, win_w);
    int seg_h  = sc(10, win_w);
    int gap    = sc(3,  win_w);
    int pad    = sc(8,  win_w);
    int bar_w  = steps * (seg_w + gap) - gap;
    int bar_h  = seg_h + pad * 2 + TTF_FontHeight(font) + sc(4, win_w);
    int label_slot = sc(46, win_w);
    int box_w  = bar_w + pad * 4 + label_slot;

    fill_rounded_rect(r, box_x, box_y, box_w, bar_h, sc(8, win_w),
                      0, 0, 0, 180);

    /* The box is always a dark semi-transparent black, so use light fixed
       colors for text and unfilled segments to guarantee readability across
       all themes.  Filled segments use highlight_bg which is always
       theme-appropriate and visible against the dark background. */
    draw_text(r, font, label, box_x + pad, box_y + pad,
              label_slot, 0xc0, 0xc0, 0xc0);

    int sx = box_x + pad + label_slot;
    int sy = box_y + pad;
    for (int i = 0; i < steps; i++) {
        int x = sx + i * (seg_w + gap);
        if (i < filled)
            fill_rect(r, x, sy, seg_w, seg_h,
                      t->highlight_bg.r, t->highlight_bg.g, t->highlight_bg.b);
        else
            fill_rect(r, x, sy, seg_w, seg_h, 0x50, 0x50, 0x50);
    }

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(value * 100.0f + 0.5f));
    draw_text(r, font, pct, sx, sy + seg_h + sc(4, win_w), sc(60, win_w),
              0xff, 0xff, 0xff);
}

/* Draw subtitle text centred near the bottom.
   bottom_y is the y coordinate of the bottom boundary (OSD top, or win_h). */
static void draw_subtitle_text(SDL_Renderer *r, TTF_Font *font,
                               const char *text, int win_w, int bottom_y) {
    int pad = sc(8, win_w);
    int lh  = TTF_FontHeight(font);

    /* Count lines and find max width */
    char buf[SUB_TEXT_MAX];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int line_count = 1;
    for (char *p = buf; *p; p++)
        if (*p == '\n') line_count++;

    int max_w = 0;
    char tmp[SUB_TEXT_MAX];
    strncpy(tmp, buf, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *scan = tmp;
    while (scan) {
        char *nl = strchr(scan, '\n');
        if (nl) *nl = '\0';
        int lw = 0, dummy = 0;
        TTF_SizeUTF8(font, scan, &lw, &dummy);
        if (lw > max_w) max_w = lw;
        scan = nl ? nl + 1 : NULL;
    }

    if (max_w <= 0) return;
    if (max_w > win_w - pad * 4) max_w = win_w - pad * 4;

    int total_h = line_count * lh + (line_count - 1) * sc(2, win_w);
    int box_w   = max_w + pad * 2;
    int box_h   = total_h + pad * 2;
    int box_x   = (win_w - box_w) / 2;
    int box_y   = bottom_y - box_h - pad;

    fill_rounded_rect(r, box_x, box_y, box_w, box_h, sc(8, win_w),
                      0, 0, 0, 180);

    /* Draw each line */
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = buf;
    int row = 0;
    while (tok) {
        char *nl = strchr(tok, '\n');
        if (nl) *nl = '\0';
        int lw = 0, dummy = 0;
        TTF_SizeUTF8(font, tok, &lw, &dummy);
        int tx = box_x + (box_w - lw) / 2;
        int ty = box_y + pad + row * (lh + sc(2, win_w));
        draw_text(r, font, tok, tx, ty, lw + 2, 0xff, 0xff, 0xff);
        tok = nl ? nl + 1 : NULL;
        row++;
    }
}

static void draw_volume_bar(SDL_Renderer *r, TTF_Font *font,
                            const Player *p, const Theme *t, int win_w) {
    /* Anchored to top-right: pre-compute box_w to position it */
    int pad   = sc(8, win_w);
    int seg_w = sc(14, win_w);
    int gap   = sc(3,  win_w);
    int box_w = 10 * (seg_w + gap) - gap + pad * 4 + sc(46, win_w);
    int box_x = win_w - box_w - pad * 2;
    draw_indicator_bar(r, font, t,
                       p->muted ? "MUTE" : "VOL",
                       p->muted ? 0.0f : p->volume,
                       box_x, statusbar_height(win_w) + pad, win_w);
}

static void draw_brightness_bar(SDL_Renderer *r, TTF_Font *font,
                                const Player *p, const Theme *t, int win_w) {
    /* Anchored to top-left */
    int pad = sc(8, win_w);
    draw_indicator_bar(r, font, t, "BRI", p->brightness, pad * 2, statusbar_height(win_w) + pad, win_w);
}

void player_draw(SDL_Renderer *r, TTF_Font *font, TTF_Font *font_small,
                 const Player *p, const Theme *t, int win_w, int win_h) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0xff);
    SDL_RenderClear(r);

    /* Auto-hide OSD check is done in player_update(); we just read the flag */
    int show_osd = p->osd_visible || (p->state == PLAYER_PAUSED);

    if (p->video_tex && p->probe.video_stream_idx >= 0) {
        /* Video — render according to zoom mode */
        {
            static const float zoom_t[] = { 0.0f, 0.5f, 1.0f }; /* FIT, WIDE, FILL */
            SDL_Rect src, dst;
            video_zoom_rects(p->video.native_w, p->video.native_h,
                             win_w, win_h, zoom_t[p->zoom_mode], &src, &dst);
            SDL_RenderCopy(r, p->video_tex, &src, &dst);
        }

        /* Subtitles — drawn above video, below OSD */
        {
            double pos = audio_get_clock(&p->audio);
            const SubEntry *sub = sub_get(&p->subtitle, pos);
            if (sub) {
                int osd_h  = show_osd ? sc(80, win_w) : 0;
                int bottom = win_h - osd_h - sc(4, win_w);
                draw_subtitle_text(r, font, sub->text, win_w, bottom);
            }
        }

        if (show_osd)
            draw_osd(r, font, font_small, p, t, win_w, win_h);
    } else {

        /* Audio-only: always show OSD */
        int lh = TTF_FontHeight(font);
        int y  = win_h / 4;
        const char *slash = strrchr(p->path, '/');
        int pad = sc(8, win_w);
        draw_text(r, font, slash ? slash + 1 : p->path,
                  pad, y, win_w - pad * 2,
                  t->text.r, t->text.g, t->text.b);
        y += lh + pad;
        char info[128];
        snprintf(info, sizeof(info), "%s  %d Hz  %d ch",
                 p->probe.audio_codec, p->probe.sample_rate, p->probe.channels);
        draw_text(r, font, info, pad, y, win_w - pad * 2,
                  t->secondary.r, t->secondary.g, t->secondary.b);
        draw_osd(r, font, font_small, p, t, win_w, win_h);
    }

    /* Brightness dimming overlay — drawn over everything except the HUD bars */
    if (p->brightness < 0.999f) {
        Uint8 alpha = (Uint8)((1.0f - p->brightness) * 220.0f);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, alpha);
        SDL_Rect full = { 0, 0, win_w, win_h };
        SDL_RenderFillRect(r, &full);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    }

    if (p->vol_osd_visible || p->muted)
        draw_volume_bar(r, font, p, t, win_w);
    if (p->bri_osd_visible)
        draw_brightness_bar(r, font, p, t, win_w);

    /* Reusable centred label helper for zoom + audio + sub OSD */
    int osd_row = 0; /* increments so stacked labels don't overlap */
    if (p->zoom_osd_visible || p->audio_osd_visible || p->sub_osd_visible) {
        /* Measure the taller of the two so row height is consistent */
        int lh = TTF_FontHeight(font);
        int pad   = sc(8, win_w);
        int row_h = lh + pad * 2 + sc(4, win_w);
        int sbar_h = statusbar_height(win_w);

        int toast_rad = sc(12, win_w);
        if (p->zoom_osd_visible) {
            static const char *zoom_names[] = { "Zoom: Fit", "Zoom: Wide", "Zoom: Fill" };
            const char *label = zoom_names[p->zoom_mode];
            int lw = 0, dummy = 0;
            TTF_SizeUTF8(font, label, &lw, &dummy);
            int bx = (win_w - lw) / 2 - pad;
            int by = sbar_h + pad * 2 + osd_row * row_h;
            fill_rounded_rect(r, bx, by, lw + pad * 2, lh + pad * 2, toast_rad,
                              0, 0, 0, 180);
            draw_text(r, font, label, bx + pad, by + pad, lw,
                      t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
            osd_row++;
        }

        if (p->audio_osd_visible) {
            int lw = 0, dummy = 0;
            TTF_SizeUTF8(font, p->audio_osd_label, &lw, &dummy);
            int bx = (win_w - lw) / 2 - pad;
            int by = sbar_h + pad * 2 + osd_row * row_h;
            fill_rounded_rect(r, bx, by, lw + pad * 2, lh + pad * 2, toast_rad,
                              0, 0, 0, 180);
            draw_text(r, font, p->audio_osd_label, bx + pad, by + pad, lw,
                      t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
            osd_row++;
        }

        if (p->sub_osd_visible) {
            int lw = 0, dummy = 0;
            TTF_SizeUTF8(font, p->sub_osd_label, &lw, &dummy);
            int bx = (win_w - lw) / 2 - pad;
            int by = sbar_h + pad * 2 + osd_row * row_h;
            fill_rounded_rect(r, bx, by, lw + pad * 2, lh + pad * 2, toast_rad,
                              0, 0, 0, 180);
            draw_text(r, font, p->sub_osd_label, bx + pad, by + pad, lw,
                      t->highlight_text.r, t->highlight_text.g, t->highlight_text.b);
        }
    }
}
