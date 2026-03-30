#include "video.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* -------------------------------------------------------------------------
 * Raw queue (YUV frames: H.264 decode thread → sws thread)
 * ---------------------------------------------------------------------- */

static void rq_init(RawQueue *q) {
    memset(q, 0, sizeof(*q));
    q->mutex     = SDL_CreateMutex();
    q->not_empty = SDL_CreateCond();
    q->not_full  = SDL_CreateCond();
}

static void rq_destroy(RawQueue *q) {
    SDL_LockMutex(q->mutex);
    for (int i = 0; i < q->count; i++) {
        RawVideoFrame *f = &q->frames[(q->head + i) % VIDEO_RAW_QUEUE_SIZE];
        av_frame_free(&f->frame);
    }
    SDL_UnlockMutex(q->mutex);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->not_empty);
    SDL_DestroyCond(q->not_full);
    memset(q, 0, sizeof(*q));
}

/* Push a raw YUV frame (or NULL sentinel for EOS/flush).
   Blocks if full. Returns -1 on abort. */
static int rq_push(RawQueue *q, AVFrame *frame, double pts, int *abort) {
    SDL_LockMutex(q->mutex);
    while (q->count == VIDEO_RAW_QUEUE_SIZE && !*abort)
        SDL_CondWait(q->not_full, q->mutex);
    if (*abort) { SDL_UnlockMutex(q->mutex); return -1; }

    RawVideoFrame *rf = &q->frames[q->tail];
    rf->frame = frame;   /* takes ownership; NULL = sentinel */
    rf->pts   = pts;
    q->tail   = (q->tail + 1) % VIDEO_RAW_QUEUE_SIZE;
    q->count++;
    SDL_CondSignal(q->not_empty);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

/* Pop a raw frame (blocks if empty). Returns -1 on abort. */
static int rq_pop(RawQueue *q, RawVideoFrame *out, int *abort) {
    SDL_LockMutex(q->mutex);
    while (q->count == 0 && !*abort)
        SDL_CondWait(q->not_empty, q->mutex);
    if (*abort && q->count == 0) { SDL_UnlockMutex(q->mutex); return -1; }

    *out    = q->frames[q->head];
    q->head = (q->head + 1) % VIDEO_RAW_QUEUE_SIZE;
    q->count--;
    SDL_CondSignal(q->not_full);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

/* Drain and free all entries (called on seek). */
static void rq_flush(RawQueue *q) {
    SDL_LockMutex(q->mutex);
    for (int i = 0; i < q->count; i++) {
        RawVideoFrame *f = &q->frames[(q->head + i) % VIDEO_RAW_QUEUE_SIZE];
        av_frame_free(&f->frame);
    }
    q->head = q->tail = q->count = 0;
    SDL_CondBroadcast(q->not_full);
    SDL_UnlockMutex(q->mutex);
}

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
    rq_flush(&v->raw_queue);
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

    /* Output BGRA (= SDL_PIXELFORMAT_ARGB8888 on little-endian ARM) so
       SDL_RenderCopy does a plain blit instead of YUV→RGB in software. */
    v->sws = sws_getContext(frame->width, frame->height,
                            (enum AVPixelFormat)frame->format,
                            v->tex_w, v->tex_h,
                            AV_PIX_FMT_BGRA,
                            SWS_FAST_BILINEAR, NULL, NULL, NULL);
    v->sws_src_w   = frame->width;
    v->sws_src_h   = frame->height;
    v->sws_src_fmt = frame->format;
    return v->sws;
}

/* -------------------------------------------------------------------------
 * Decode thread — H.264 decode only; pushes raw YUV frames to raw_queue
 * ---------------------------------------------------------------------- */

static int video_decode_thread(void *userdata) {
    VideoCtx *v   = (VideoCtx *)userdata;
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *raw = av_frame_alloc();

    while (!v->abort) {
        int ret = packet_queue_get(v->pkt_queue, pkt);
        if (ret < 0) break;  /* abort */
        if (ret == 0) {      /* seek-flush: reset codec + drain both queues */
            avcodec_flush_buffers(v->codec_ctx);
            rq_flush(&v->raw_queue);
            SDL_LockMutex(v->frame_queue.mutex);
            for (int i = 0; i < v->frame_queue.count; i++)
                av_frame_free(&v->frame_queue.frames[
                    (v->frame_queue.head + i) % VIDEO_FRAME_QUEUE_SIZE].frame);
            v->frame_queue.head = v->frame_queue.tail = v->frame_queue.count = 0;
            SDL_CondBroadcast(v->frame_queue.not_full);
            SDL_UnlockMutex(v->frame_queue.mutex);
            continue;
        }
        if (!pkt->data) {    /* EOS: send NULL sentinel through pipeline */
            avcodec_flush_buffers(v->codec_ctx);
            av_packet_unref(pkt);
            rq_push(&v->raw_queue, NULL, -1.0, &v->abort);
            break;
        }

        avcodec_send_packet(v->codec_ctx, pkt);
        av_packet_unref(pkt);

        while (!v->abort &&
               avcodec_receive_frame(v->codec_ctx, raw) == 0) {
            double pts = (raw->pts != AV_NOPTS_VALUE)
                         ? raw->pts * av_q2d(v->time_base)
                         : 0.0;
            /* Move raw frame into a heap-allocated wrapper and push to
               raw_queue for the sws thread to consume. */
            AVFrame *copy = av_frame_alloc();
            av_frame_move_ref(copy, raw);
            if (rq_push(&v->raw_queue, copy, pts, &v->abort) < 0)
                av_frame_free(&copy);
            av_frame_unref(raw);
        }
    }

    av_frame_free(&raw);
    av_packet_free(&pkt);
    return 0;
}

/* -------------------------------------------------------------------------
 * Portrait kernel (A30 only) — YUV420P → BGRA + 90°CCW + 2× downscale
 * ---------------------------------------------------------------------- */

#ifdef GVU_A30
/* BT.601 limited-range YUV→BGRA.
 * Returns little-endian uint32 with B=byte0, G=byte1, R=byte2, A=0xFF=byte3.
 * That is BGRA memory layout = SDL ARGB8888 pixel value. */
static inline uint32_t yuv601_to_bgra(int y, int u, int v) {
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c           + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d           + 128) >> 8;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Combine YUV420P→BGRA, 90°CCW rotation, and 2× downscale in one pass.
 *
 * src: YUV420P frame (e.g. 1280×720)
 * dst: BGRA frame with width = src->height/2 and height = src->width/2
 *      (portrait layout: dst width = number of panel columns covered by video,
 *       dst height = number of panel rows)
 *
 * Portrait mapping (matches a30_flip rotation):
 *   portrait row pr = (src->width/2 - 1) - (sx/2)
 *   portrait col pc = sy/2
 *
 * Iterates source-row-major (outer sy, inner sx) so Y and UV reads are
 * nearly sequential.  Portrait writes are strided (stride = dst->width pixels)
 * but go to heap-cached memory, far cheaper than uncached framebuffer writes.
 */
static void yuv420p_to_portrait_bgra_2x(const AVFrame *src, AVFrame *dst) {
    const int sw          = src->width;
    const int sh          = src->height;
    uint32_t *portrait    = (uint32_t *)dst->data[0];
    const int port_stride = dst->linesize[0] / 4;   /* pixels per portrait row */

    const uint8_t *Y_base = src->data[0];
    const uint8_t *U_base = src->data[1];
    const uint8_t *V_base = src->data[2];
    const int Y_stride    = src->linesize[0];
    const int U_stride    = src->linesize[1];
    const int V_stride    = src->linesize[2];
    const int pr_max      = (sw >> 1) - 1;

    for (int sy = 0; sy < sh; sy += 2) {
        const int pc         = sy >> 1;
        const uint8_t *Y0    = Y_base + sy * Y_stride;
        const uint8_t *Y1    = Y_base + (sy + 1) * Y_stride;
        const uint8_t *U_row = U_base + (sy >> 1) * U_stride;
        const uint8_t *V_row = V_base + (sy >> 1) * V_stride;

        for (int sx = 0; sx < sw; sx += 2) {
            const int pr  = pr_max - (sx >> 1);
            const int y_v = ((int)Y0[sx] + Y0[sx+1] + Y1[sx] + Y1[sx+1] + 2) >> 2;
            portrait[(size_t)pr * port_stride + pc] =
                yuv601_to_bgra(y_v, U_row[sx >> 1], V_row[sx >> 1]);
        }
    }
}
#endif /* GVU_A30 */

/* -------------------------------------------------------------------------
 * sws thread — pops raw YUV frames, converts to BGRA, pushes to frame_queue
 *
 * Runs in parallel with the decode thread: while the decode thread decodes
 * frame N+1, the sws thread converts frame N.  Pipeline throughput is
 * max(decode_time, sws_time) instead of their sum.
 * ---------------------------------------------------------------------- */

static int video_sws_thread(void *userdata) {
    VideoCtx *v   = (VideoCtx *)userdata;
    AVFrame  *cvt = av_frame_alloc();

    while (!v->abort) {
        RawVideoFrame rf;
        if (rq_pop(&v->raw_queue, &rf, &v->abort) < 0) break;

        if (!rf.frame) {  /* EOS sentinel — forward to frame_queue */
            fq_push(&v->frame_queue, cvt, -1.0, &v->abort);
            break;
        }

#ifdef GVU_A30
        if (v->portrait_direct) {
            /* Custom kernel: combine YUV→BGRA + rotate + 2× downscale in one
             * pass instead of separate sws_scale + a30_flip() rotation.
             * Portrait frame: width = src_h/2, height = src_w/2 */
            cvt->width  = rf.frame->height / 2;
            cvt->height = rf.frame->width  / 2;
            cvt->format = AV_PIX_FMT_BGRA;
            av_frame_get_buffer(cvt, 32);

            yuv420p_to_portrait_bgra_2x(rf.frame, cvt);
            fq_push(&v->frame_queue, cvt, rf.pts, &v->abort);
            av_frame_unref(cvt);
            av_frame_free(&rf.frame);
            continue;
        }
#endif /* GVU_A30 */

        struct SwsContext *sws = get_sws(v, rf.frame);
        if (sws) {
            cvt->width  = v->tex_w;
            cvt->height = v->tex_h;
            cvt->format = AV_PIX_FMT_BGRA;
            av_frame_get_buffer(cvt, 32);

            sws_scale(sws,
                      (const uint8_t * const *)rf.frame->data, rf.frame->linesize,
                      0, rf.frame->height,
                      cvt->data, cvt->linesize);
            fq_push(&v->frame_queue, cvt, rf.pts, &v->abort);
            av_frame_unref(cvt);
        } else {
            /* No sws context — push raw frame directly */
            fq_push(&v->frame_queue, rf.frame, rf.pts, &v->abort);
            rf.frame = NULL;
        }
        av_frame_free(&rf.frame);
    }

    av_frame_free(&cvt);
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
#ifdef GVU_A30
    /* Pre-scale to the display's fit rect so SDL does a 1:1 blit instead of
       a software bilinear scale on the full native resolution.  libswscale is
       NEON-optimised; SDL's software renderer is not.  For 1280x720 this
       reduces the SDL YUV->RGB + scale work from ~921k to ~230k pixels. */
    {
        SDL_Rect fit = video_fit_rect(cp->width, cp->height, 640, 480);
        v->tex_w = fit.w;
        v->tex_h = fit.h;
        /* Portrait-direct: if source is exactly 2× the fit-rect dimensions,
           the custom kernel can replace libswscale entirely. The output frame
           is portrait-oriented (width=tex_h, height=tex_w) and bypasses the
           SDL texture upload + a30_flip() rotation. */
        if (cp->width == 2 * fit.w && cp->height == 2 * fit.h)
            v->portrait_direct = 1;
    }
#elif defined(GVU_TRIMUI_BRICK)
    /* Pre-scale to fit 1024×768 so SDL blits at 1:1 (no software bilinear).
       When tex_w == BRICK_W the decoded BGRA frame is full panel-width, so we
       can blit it directly to fb0 via brick_flip_video() without going through
       the SDL texture upload + software render path (landscape_direct). */
    {
        SDL_Rect fit = video_fit_rect(cp->width, cp->height, 1024, 768);
        v->tex_w = fit.w;
        v->tex_h = fit.h;
        if (fit.w == 1024)
            v->landscape_direct = 1;
    }
#else
    v->tex_w = v->native_w;
    v->tex_h = v->native_h;
#endif

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
       for the OS / audio / demux. */
    v->codec_ctx->thread_count = 1;

    /* Skip the H.264 deblocking (loop) filter on all frames.  The filter
       consumes ~30% of decode time on Cortex-A7; skipping it is a common
       embedded-device trade-off.  Slight edge softness, no other artifacts. */
    v->codec_ctx->skip_loop_filter = AVDISCARD_ALL;

    /* Allow the decoder to use intra-refresh, approximate IDCT, and other
       fast shortcuts that trade a little accuracy for speed. */
    v->codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
#endif

    int ret = avcodec_open2(v->codec_ctx, codec, NULL);
    if (ret < 0) {
        if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_sz);
        avcodec_free_context(&v->codec_ctx);
        return -1;
    }

    rq_init(&v->raw_queue);
    fq_init(&v->frame_queue);
    return 0;
}

void video_start(VideoCtx *v) {
    v->thread     = SDL_CreateThread(video_decode_thread, "video_dec", v);
    v->sws_thread = SDL_CreateThread(video_sws_thread,    "video_sws", v);
}

void video_stop(VideoCtx *v) {
    v->abort = 1;
    /* Wake all blocked threads */
    SDL_LockMutex(v->raw_queue.mutex);
    SDL_CondBroadcast(v->raw_queue.not_empty);
    SDL_CondBroadcast(v->raw_queue.not_full);
    SDL_UnlockMutex(v->raw_queue.mutex);
    SDL_LockMutex(v->frame_queue.mutex);
    SDL_CondBroadcast(v->frame_queue.not_full);
    SDL_UnlockMutex(v->frame_queue.mutex);
    if (v->thread)     { SDL_WaitThread(v->thread,     NULL); v->thread     = NULL; }
    if (v->sws_thread) { SDL_WaitThread(v->sws_thread, NULL); v->sws_thread = NULL; }
}

void video_close(VideoCtx *v) {
    if (v->sws)       sws_freeContext(v->sws);
    if (v->codec_ctx) avcodec_free_context(&v->codec_ctx);
    rq_destroy(&v->raw_queue);
    fq_destroy(&v->frame_queue);
    memset(v, 0, sizeof(*v));
}
