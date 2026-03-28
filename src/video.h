#pragma once
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
#include "decoder.h"

/* -------------------------------------------------------------------------
 * Raw frame queue — YUV frames between H.264 decode thread and sws thread
 * ---------------------------------------------------------------------- */

#define VIDEO_RAW_QUEUE_SIZE 2

typedef struct {
    AVFrame *frame;   /* NULL = EOS/flush sentinel */
    double   pts;
} RawVideoFrame;

typedef struct {
    RawVideoFrame frames[VIDEO_RAW_QUEUE_SIZE];
    int           head, tail, count;
    SDL_mutex    *mutex;
    SDL_cond     *not_empty;
    SDL_cond     *not_full;
} RawQueue;

/* -------------------------------------------------------------------------
 * Frame queue — BGRA frames waiting to be rendered
 * ---------------------------------------------------------------------- */

#define VIDEO_FRAME_QUEUE_SIZE 4

typedef struct {
    AVFrame *frame;
    double   pts;   /* presentation timestamp in seconds */
} VideoFrame;

typedef struct {
    VideoFrame  frames[VIDEO_FRAME_QUEUE_SIZE];
    int         head, tail, count;
    SDL_mutex  *mutex;
    SDL_cond   *not_empty;
    SDL_cond   *not_full;
} FrameQueue;

/* -------------------------------------------------------------------------
 * Video context
 * ---------------------------------------------------------------------- */

typedef struct {
    AVCodecContext    *codec_ctx;
    struct SwsContext *sws;
    int                sws_src_w, sws_src_h;
    int                sws_src_fmt;   /* cached enum AVPixelFormat */
    int                native_w, native_h;   /* video stream dimensions */
    int                tex_w, tex_h;         /* pre-scaled texture dimensions */
    RawQueue           raw_queue;     /* H.264→sws pipeline buffer */
    FrameQueue         frame_queue;
    SDL_Thread        *thread;       /* H.264 decode thread */
    SDL_Thread        *sws_thread;   /* sws conversion thread */
    PacketQueue       *pkt_queue;     /* borrowed from DemuxCtx */
    AVRational         time_base;
    int                abort;
} VideoCtx;

int  video_open (VideoCtx *v, AVCodecParameters *cp, AVRational time_base,
                 PacketQueue *pkt_queue, char *errbuf, int errbuf_sz);
void video_start(VideoCtx *v);
void video_stop (VideoCtx *v);
void video_close(VideoCtx *v);

/* Peek at the next decoded frame without removing it from the queue.
   Returns 1 if a frame is available, 0 if the queue is empty. */
int  video_peek_frame  (VideoCtx *v, VideoFrame *out);
void video_pop_frame   (VideoCtx *v);  /* remove the front frame */
/* Discard all frames currently in the queue.  Call from the main thread
   before a seek to prevent "too early" frames from blocking the decode
   thread in fq_push and stalling sentinel delivery. */
void video_flush_frames(VideoCtx *v);

/* FIT  — letterbox/pillarbox dst rect; pass srcrect=NULL to SDL_RenderCopy. */
SDL_Rect video_fit_rect(int src_w, int src_h, int dst_w, int dst_h);

/* General zoom interpolation between FIT (t=0.0) and FILL (t=1.0).
   t=0.5 gives "wide" — reduced bars, slight edge crop, aspect preserved.
   Fills *srcrect and *dstrect for use with SDL_RenderCopy. */
void video_zoom_rects(int src_w, int src_h, int dst_w, int dst_h, float t,
                      SDL_Rect *srcrect, SDL_Rect *dstrect);
