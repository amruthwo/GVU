#include "video.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* -------------------------------------------------------------------------
 * Frame queue
 * ---------------------------------------------------------------------- */

static void fq_init(FrameQueue *q) {
    memset(q, 0, sizeof(*q));
    q->mutex     = SDL_CreateMutex();
    q->not_empty = SDL_CreateCond();
    q->not_full  = SDL_CreateCond();
}

static void fq_destroy(FrameQueue *q) {
    SDL_LockMutex(q->mutex);
    for (int i = 0; i < q->count; i++) {
        VideoFrame *vf = &q->frames[(q->head + i) % VIDEO_FRAME_QUEUE_SIZE];
        av_frame_free(&vf->frame);
    }
    SDL_UnlockMutex(q->mutex);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->not_empty);
    SDL_DestroyCond(q->not_full);
    memset(q, 0, sizeof(*q));
}

/* Push a frame into the queue (blocks if full). Returns -1 on abort. */
static int fq_push(FrameQueue *q, AVFrame *frame, double pts, int *abort) {
    SDL_LockMutex(q->mutex);
    while (q->count == VIDEO_FRAME_QUEUE_SIZE && !*abort)
        SDL_CondWait(q->not_full, q->mutex);
    if (*abort) { SDL_UnlockMutex(q->mutex); return -1; }

    VideoFrame *vf = &q->frames[q->tail];
    vf->frame = av_frame_alloc();
    av_frame_move_ref(vf->frame, frame);
    vf->pts   = pts;
    q->tail   = (q->tail + 1) % VIDEO_FRAME_QUEUE_SIZE;
    q->count++;
    SDL_CondSignal(q->not_empty);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

int video_peek_frame(VideoCtx *v, VideoFrame *out) {
    FrameQueue *q = &v->frame_queue;
    SDL_LockMutex(q->mutex);
    int avail = q->count > 0;
    if (avail) *out = q->frames[q->head];
    SDL_UnlockMutex(q->mutex);
    return avail;
}

void video_flush_frames(VideoCtx *v) {
    FrameQueue *q = &v->frame_queue;
    SDL_LockMutex(q->mutex);
    for (int i = 0; i < q->count; i++)
        av_frame_free(&q->frames[(q->head + i) % VIDEO_FRAME_QUEUE_SIZE].frame);
    q->head = q->tail = q->count = 0;
    SDL_CondBroadcast(q->not_full);
    SDL_UnlockMutex(q->mutex);
}

void video_pop_frame(VideoCtx *v) {
    FrameQueue *q = &v->frame_queue;
    SDL_LockMutex(q->mutex);
    if (q->count > 0) {
        av_frame_free(&q->frames[q->head].frame);
        q->head = (q->head + 1) % VIDEO_FRAME_QUEUE_SIZE;
        q->count--;
        SDL_CondSignal(q->not_full);
    }
    SDL_UnlockMutex(q->mutex);
}

/* -------------------------------------------------------------------------
 * Aspect ratio helper
 * ---------------------------------------------------------------------- */

SDL_Rect video_fit_rect(int src_w, int src_h, int dst_w, int dst_h) {
    float sa = (float)src_w / (float)src_h;
    float da = (float)dst_w / (float)dst_h;
    int w, h;
    if (sa > da) { w = dst_w; h = (int)(dst_w / sa); }
    else         { h = dst_h; w = (int)(dst_h * sa); }
    return (SDL_Rect){ (dst_w - w) / 2, (dst_h - h) / 2, w, h };
}

void video_zoom_rects(int src_w, int src_h, int dst_w, int dst_h, float t,
                      SDL_Rect *srcrect, SDL_Rect *dstrect) {
    float fit_scale  = (float)dst_w/src_w < (float)dst_h/src_h
                       ? (float)dst_w/src_w : (float)dst_h/src_h;
    float fill_scale = (float)dst_w/src_w > (float)dst_h/src_h
                       ? (float)dst_w/src_w : (float)dst_h/src_h;
    float scale = fit_scale + (fill_scale - fit_scale) * t;

    /* How much of the window we actually fill at this scale */
    int out_w = (int)(src_w * scale);
    int out_h = (int)(src_h * scale);
    int crop_w = out_w < dst_w ? out_w : dst_w;
    int crop_h = out_h < dst_h ? out_h : dst_h;

    /* Source region (centred crop in texture coords) */
    float sw = crop_w / scale;
    float sh = crop_h / scale;
    *srcrect = (SDL_Rect){ (int)((src_w - sw) / 2), (int)((src_h - sh) / 2),
                           (int)sw, (int)sh };

    /* Dest region (centred in window) */
    *dstrect = (SDL_Rect){ (dst_w - crop_w) / 2, (dst_h - crop_h) / 2,
                           crop_w, crop_h };
}

/* -------------------------------------------------------------------------
 * swscale — lazy init / reinit when source format changes
 * ---------------------------------------------------------------------- */

static struct SwsContext *get_sws(VideoCtx *v, AVFrame *frame) {
    if (v->sws &&
        v->sws_src_w   == frame->width &&
        v->sws_src_h   == frame->height &&
        v->sws_src_fmt == frame->format)
        return v->sws;

    if (v->sws) { sws_freeContext(v->sws); v->sws = NULL; }

    v->sws = sws_getContext(frame->width, frame->height,
                            (enum AVPixelFormat)frame->format,
                            frame->width, frame->height,
                            AV_PIX_FMT_YUV420P,
                            SWS_BILINEAR, NULL, NULL, NULL);
    v->sws_src_w   = frame->width;
    v->sws_src_h   = frame->height;
    v->sws_src_fmt = frame->format;
    return v->sws;
}

/* -------------------------------------------------------------------------
 * Decode thread
 * ---------------------------------------------------------------------- */

static int video_decode_thread(void *userdata) {
    VideoCtx *v   = (VideoCtx *)userdata;
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *raw = av_frame_alloc();
    AVFrame  *cvt = av_frame_alloc(); /* YUV420P converted frame */

    while (!v->abort) {
        int ret = packet_queue_get(v->pkt_queue, pkt);
        if (ret < 0) break;  /* abort */
        if (ret == 0) {      /* seek-flush: reset codec + discard buffered frames */
            avcodec_flush_buffers(v->codec_ctx);
            SDL_LockMutex(v->frame_queue.mutex);
            for (int i = 0; i < v->frame_queue.count; i++)
                av_frame_free(&v->frame_queue.frames[
                    (v->frame_queue.head + i) % VIDEO_FRAME_QUEUE_SIZE].frame);
            v->frame_queue.head = v->frame_queue.tail = v->frame_queue.count = 0;
            SDL_CondBroadcast(v->frame_queue.not_full);
            SDL_UnlockMutex(v->frame_queue.mutex);
            continue;
        }
        if (!pkt->data) {    /* EOS */
            avcodec_flush_buffers(v->codec_ctx);
            av_packet_unref(pkt);
            fq_push(&v->frame_queue, raw, -1.0, &v->abort);
            break;
        }

        avcodec_send_packet(v->codec_ctx, pkt);
        av_packet_unref(pkt);

        while (!v->abort &&
               avcodec_receive_frame(v->codec_ctx, raw) == 0) {

            /* PTS in seconds */
            double pts = (raw->pts != AV_NOPTS_VALUE)
                         ? raw->pts * av_q2d(v->time_base)
                         : 0.0;

            /* Convert to YUV420P if needed.
               Use av_frame_get_buffer (ref-counted) not av_image_alloc (raw
               pointer) so that av_frame_free in video_pop_frame actually
               releases the buffer — av_image_alloc sets data[] but not buf[],
               so av_frame_free silently skips the free, leaking every frame. */
            struct SwsContext *sws = get_sws(v, raw);
            if (sws) {
                cvt->width  = raw->width;
                cvt->height = raw->height;
                cvt->format = AV_PIX_FMT_YUV420P;
                av_frame_get_buffer(cvt, 32);
                sws_scale(sws,
                          (const uint8_t * const *)raw->data, raw->linesize,
                          0, raw->height,
                          cvt->data, cvt->linesize);
                fq_push(&v->frame_queue, cvt, pts, &v->abort);
                av_frame_unref(cvt);
            } else {
                fq_push(&v->frame_queue, raw, pts, &v->abort);
            }
            av_frame_unref(raw);
        }
    }

    av_frame_free(&cvt);
    av_frame_free(&raw);
    av_packet_free(&pkt);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int video_open(VideoCtx *v, AVCodecParameters *cp, AVRational time_base,
               PacketQueue *pkt_queue, char *errbuf, int errbuf_sz) {
    memset(v, 0, sizeof(*v));
    v->pkt_queue  = pkt_queue;
    v->time_base  = time_base;
    v->native_w   = cp->width;
    v->native_h   = cp->height;

    const AVCodec *codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz,
                             "No decoder for codec id %d", cp->codec_id);
        return -1;
    }
    v->codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(v->codec_ctx, cp);
    v->codec_ctx->pkt_timebase = time_base;
#ifdef GVU_A30
    /* Single decode thread — default (all cores) exhausts memory and causes
       thermal throttling on the A30.  Single-threaded decode avoids FFmpeg's
       internal per-thread reference frame DPB copies and frees the other cores
       for the OS / audio / demux.  The A53 at ~1.6 GHz handles 720p H.264
       single-threaded comfortably; 1080p is borderline but usable. */
    v->codec_ctx->thread_count = 1;
#endif

    int ret = avcodec_open2(v->codec_ctx, codec, NULL);
    if (ret < 0) {
        if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_sz);
        avcodec_free_context(&v->codec_ctx);
        return -1;
    }

    fq_init(&v->frame_queue);
    return 0;
}

void video_start(VideoCtx *v) {
    v->thread = SDL_CreateThread(video_decode_thread, "video", v);
}

void video_stop(VideoCtx *v) {
    v->abort = 1;
    /* Wake the decode thread if it's blocked on fq_push */
    SDL_LockMutex(v->frame_queue.mutex);
    SDL_CondBroadcast(v->frame_queue.not_full);
    SDL_UnlockMutex(v->frame_queue.mutex);
    if (v->thread) { SDL_WaitThread(v->thread, NULL); v->thread = NULL; }
}

void video_close(VideoCtx *v) {
    if (v->sws)      sws_freeContext(v->sws);
    if (v->codec_ctx) avcodec_free_context(&v->codec_ctx);
    fq_destroy(&v->frame_queue);
    memset(v, 0, sizeof(*v));
}
