#include "audio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef GVU_A30
#include <unistd.h>
#include <spawn.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

/* -------------------------------------------------------------------------
 * Ring buffer
 * ---------------------------------------------------------------------- */

static int ring_init(AudioRingBuf *r) {
    r->buf      = malloc(AUDIO_RING_SIZE);
    if (!r->buf) return -1;
    r->capacity = AUDIO_RING_SIZE;
    r->filled   = 0;
    r->read_pos = r->write_pos = 0;
    r->mutex    = SDL_CreateMutex();
    r->not_full = SDL_CreateCond();
    return 0;
}

static void ring_destroy(AudioRingBuf *r) {
    free(r->buf);
    SDL_DestroyMutex(r->mutex);
    SDL_DestroyCond(r->not_full);
    memset(r, 0, sizeof(*r));
}

/* Write exactly `len` bytes from `src` to the ring buffer.
   Blocks if the ring is full. Returns -1 if aborted. */
static int ring_write(AudioRingBuf *r, const uint8_t *src, int len,
                      int *abort_flag) {
    int written = 0;
    while (written < len) {
        SDL_LockMutex(r->mutex);
        while (r->filled == r->capacity && !*abort_flag)
            SDL_CondWait(r->not_full, r->mutex);
        if (*abort_flag) { SDL_UnlockMutex(r->mutex); return -1; }

        int space = r->capacity - r->filled;
        int chunk = len - written;
        if (chunk > space) chunk = space;

        /* Handle wrap-around */
        int tail_space = r->capacity - r->write_pos;
        if (chunk > tail_space) {
            memcpy(r->buf + r->write_pos, src + written, (size_t)tail_space);
            memcpy(r->buf, src + written + tail_space, (size_t)(chunk - tail_space));
        } else {
            memcpy(r->buf + r->write_pos, src + written, (size_t)chunk);
        }
        r->write_pos = (r->write_pos + chunk) % r->capacity;
        r->filled   += chunk;
        written     += chunk;
        SDL_UnlockMutex(r->mutex);
    }
    return 0;
}

/* SDL audio callback — called from SDL's audio thread.
   Must never block. Reads up to `len` bytes; pads with silence if underrun. */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    AudioCtx    *a = (AudioCtx *)userdata;

    /* After sleep/wake the A30 ALSA driver fires a storm of underruns while
       it reinitialises.  Each underrun causes the callback to consume ring
       data that ALSA never actually plays, producing drift and pops.
       Serve silence (without touching the ring) until the grace window expires
       so the storm exhausts itself against nothing. */
    Uint32 silence_until = atomic_load_explicit(&a->wake_silence_until,
                                                memory_order_relaxed);
    if (silence_until && SDL_GetTicks() < silence_until) {
        memset(stream, 0, (size_t)len);
        return;
    }

    AudioRingBuf *r = &a->ring;
    int read = 0;

    SDL_LockMutex(r->mutex);
    int avail = r->filled < len ? r->filled : len;
    if (avail > 0) {
        int tail = r->capacity - r->read_pos;
        if (avail > tail) {
            memcpy(stream,        r->buf + r->read_pos, (size_t)tail);
            memcpy(stream + tail, r->buf,               (size_t)(avail - tail));
        } else {
            memcpy(stream, r->buf + r->read_pos, (size_t)avail);
        }
        r->read_pos = (r->read_pos + avail) % r->capacity;
        r->filled  -= avail;
        read        = avail;
        SDL_CondSignal(r->not_full);
    }
    SDL_UnlockMutex(r->mutex);

    /* Silence for any underrun */
    if (read < len)
        memset(stream + read, 0, (size_t)(len - read));

    /* Apply app-level volume by scaling S16 samples in-place */
    float vol = atomic_load_explicit(&a->volume, memory_order_relaxed);
    if (vol < 0.999f) {
        int16_t *samples = (int16_t *)stream;
        int n = len / 2;
        for (int i = 0; i < n; i++)
            samples[i] = (int16_t)(samples[i] * vol);
    }
}

/* -------------------------------------------------------------------------
 * Audio decode thread
 * ---------------------------------------------------------------------- */

static int audio_decode_thread(void *userdata) {
    AudioCtx *a   = (AudioCtx *)userdata;
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();

    /* Output buffer: enough for one frame resampled */
    int out_buf_size = 192000; /* 1s at 48kHz stereo S16 — more than enough */
    uint8_t *out_buf = malloc((size_t)out_buf_size);

    while (!a->abort) {
        int ret = packet_queue_get(a->pkt_queue, pkt);
        if (ret < 0) break;  /* abort */
        if (ret == 0) {      /* seek-flush sentinel: reset codec + ring */
            avcodec_flush_buffers(a->codec_ctx);
            /* Clear buffered audio so old samples don't play after seek */
            SDL_LockMutex(a->ring.mutex);
            a->ring.filled = a->ring.read_pos = a->ring.write_pos = 0;
            SDL_CondSignal(a->ring.not_full);
            SDL_UnlockMutex(a->ring.mutex);
            /* Release the seek gate set by audio_switch_stream: now that the
               demux has seeked and flushed, decoded frames from this point
               forward are at the right position and should be played. */
            atomic_store(&a->clock_seek_active, 0);
            /* Do NOT reset a->clock here — player_seek / player_seek_to already
               set it optimistically to the seek target.  Resetting to 0 would
               undo that and cause a momentary A/V sync jump. */
            continue;
        }
        if (!pkt->data) {    /* EOS */
            avcodec_flush_buffers(a->codec_ctx);
            av_packet_unref(pkt);
            a->eos = 1;
            break;
        }

        avcodec_send_packet(a->codec_ctx, pkt);
        av_packet_unref(pkt);

        while (!a->abort &&
               avcodec_receive_frame(a->codec_ctx, frm) == 0) {

            int out_samples = swr_get_out_samples(a->swr, frm->nb_samples);
            int out_bytes   = out_samples * AUDIO_OUT_CHANNELS * 2; /* S16 */
            if (out_bytes > out_buf_size) {
                out_buf_size = out_bytes;
                out_buf = realloc(out_buf, (size_t)out_buf_size);
            }

            uint8_t *out_ptr = out_buf;
            int converted = swr_convert(a->swr,
                                        &out_ptr, out_samples,
                                        (const uint8_t **)frm->data,
                                        frm->nb_samples);
            if (converted > 0 &&
                !atomic_load_explicit(&a->clock_seek_active,
                                      memory_order_relaxed)) {
                /* Seek gate is clear: write audio and update clock.
                   While clock_seek_active is 1 (set by audio_switch_stream
                   and cleared by the seek-flush sentinel above) we silently
                   discard decoded frames so the ring stays empty, the SDL
                   callback plays silence, and the clock remains pinned at the
                   seek target — preventing video sync from chasing a clock
                   that hasn't settled yet. */
                int bytes = converted * AUDIO_OUT_CHANNELS * 2;
                ring_write(&a->ring, out_buf, bytes, &a->abort);

                /* Update clock: PTS of end of this frame */
                if (frm->pts != AV_NOPTS_VALUE) {
                    double pts = (double)frm->pts *
                                 av_q2d(a->codec_ctx->pkt_timebase) +
                                 (double)converted / a->out_rate;
                    SDL_LockMutex(a->clock_mutex);
                    a->clock = pts;
                    SDL_UnlockMutex(a->clock_mutex);
                }
            }
            av_frame_unref(frm);
        }
    }

    free(out_buf);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int audio_open(AudioCtx *a, AVCodecParameters *codec_params,
               PacketQueue *pkt_queue, char *errbuf, int errbuf_sz) {
    memset(a, 0, sizeof(*a));
    a->pkt_queue  = pkt_queue;
    a->clock_mutex = SDL_CreateMutex();
    atomic_store(&a->volume, 1.0f);
    atomic_store(&a->clock_seek_active, 0);

    /* Find and open codec */
    const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz,
                             "No decoder for codec id %d", codec_params->codec_id);
        return -1;
    }
    a->codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(a->codec_ctx, codec_params);

    int ret = avcodec_open2(a->codec_ctx, codec, NULL);
    if (ret < 0) {
        if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_sz);
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }

    /* Ring buffer */
    if (ring_init(&a->ring) < 0) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz, "ring buffer alloc failed");
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }

    /* SDL audio device — open first to discover the actual negotiated rate.
     * Allow frequency change so devices that only support 44100Hz (e.g.
     * Allwinner V3s on Miyoo Mini/Flip) don't fail with "Couldn't set
     * audio frequency". */
    SDL_AudioSpec want = {
        .freq     = AUDIO_OUT_RATE,
        .format   = AUDIO_SDL_FORMAT,
        .channels = AUDIO_OUT_CHANNELS,
        .samples  = AUDIO_SDL_SAMPLES,
        .callback = audio_callback,
        .userdata = a,
    };
    SDL_AudioSpec got;
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got,
                                 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (a->dev == 0) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz,
                             "SDL_OpenAudioDevice: %s", SDL_GetError());
        ring_destroy(&a->ring);
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }
    a->out_rate = got.freq;
    fprintf(stderr, "audio_open: want=%dHz got=%dHz fmt=%d ch=%d\n",
            AUDIO_OUT_RATE, got.freq, got.format, got.channels);

    /* Set up swresample: input = file's format, output = S16 stereo at got.freq */
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, AUDIO_OUT_CHANNELS);

    ret = swr_alloc_set_opts2(&a->swr,
                              &out_layout,           AV_SAMPLE_FMT_S16, a->out_rate,
                              &codec_params->ch_layout,
                              (enum AVSampleFormat)codec_params->format,
                              codec_params->sample_rate,
                              0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || swr_init(a->swr) < 0) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz, "swr_init failed");
        SDL_CloseAudioDevice(a->dev); a->dev = 0;
        ring_destroy(&a->ring);
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }
    return 0;
}

void audio_start(AudioCtx *a) {
    SDL_PauseAudioDevice(a->dev, 0); /* start playback */
    a->thread = SDL_CreateThread(audio_decode_thread, "audio", a);
}

void audio_pause(AudioCtx *a, int pause) {
    SDL_PauseAudioDevice(a->dev, pause);
}

void audio_stop(AudioCtx *a) {
    a->abort = 1;
    /* Wake the decode thread if it's blocked on ring_write */
    SDL_LockMutex(a->ring.mutex);
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);
    /* Wake the decode thread if it's blocked in packet_queue_get waiting for
       packets — ring signal alone won't reach it in that case, causing
       SDL_WaitThread to hang until demux_stop aborts the queue (too late). */
    if (a->pkt_queue) packet_queue_abort(a->pkt_queue);
    if (a->thread) { SDL_WaitThread(a->thread, NULL); a->thread = NULL; }
    SDL_PauseAudioDevice(a->dev, 1);
}

void audio_close(AudioCtx *a) {
    if (a->dev)      SDL_CloseAudioDevice(a->dev);
    if (a->swr)      swr_free(&a->swr);
    if (a->codec_ctx) avcodec_free_context(&a->codec_ctx);
    if (a->clock_mutex) SDL_DestroyMutex(a->clock_mutex);
    ring_destroy(&a->ring);
    memset(a, 0, sizeof(*a));
}

int audio_switch_stream(AudioCtx *a, PacketQueue *pkt_queue,
                        AVCodecParameters *cp, AVRational time_base,
                        char *errbuf, int errbuf_sz) {
    /* Stop decode thread.
       Signal the ring (wakes thread if blocked in ring_write) and abort the
       packet queue (wakes thread if blocked in packet_queue_get, and avoids
       the deadlock where packet_queue_put_flush blocks on a full queue while
       the thread is exiting without consuming another packet). */
    a->abort = 1;
    SDL_LockMutex(a->ring.mutex);
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);
    packet_queue_abort(pkt_queue);
    if (a->thread) { SDL_WaitThread(a->thread, NULL); a->thread = NULL; }

    /* Flush queue and reset its abort flag so it can be reused */
    packet_queue_flush(pkt_queue);
    SDL_LockMutex(pkt_queue->mutex);
    pkt_queue->abort = 0;
    SDL_UnlockMutex(pkt_queue->mutex);

    /* Reinit codec */
    avcodec_free_context(&a->codec_ctx);
    const AVCodec *codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz,
                             "No decoder for codec id %d", cp->codec_id);
        return -1;
    }
    a->codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(a->codec_ctx, cp);
    a->codec_ctx->pkt_timebase = time_base;
    int ret = avcodec_open2(a->codec_ctx, codec, NULL);
    if (ret < 0) {
        if (errbuf) av_strerror(ret, errbuf, (size_t)errbuf_sz);
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }

    /* Reinit swresample for new channel layout / format / rate */
    swr_free(&a->swr);
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, AUDIO_OUT_CHANNELS);
    ret = swr_alloc_set_opts2(&a->swr,
                              &out_layout,           AV_SAMPLE_FMT_S16, a->out_rate,
                              &cp->ch_layout,
                              (enum AVSampleFormat)cp->format,
                              cp->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || swr_init(a->swr) < 0) {
        if (errbuf) snprintf(errbuf, (size_t)errbuf_sz, "swr_init failed");
        avcodec_free_context(&a->codec_ctx);
        return -1;
    }

    /* Clear ring buffer, but first subtract the ring's buffered delay from
       the clock so it reflects the actual playing position rather than the
       decoded-but-not-yet-played position.  Without this, audio_get_clock()
       jumps forward by ~ring_delay seconds on the next call (ring suddenly
       empty) and video sync scrambles to catch up for several seconds. */
    SDL_LockMutex(a->ring.mutex);
    double bytes_per_sec = a->out_rate * AUDIO_OUT_CHANNELS * 2.0;
    double ring_delay    = a->ring.filled / bytes_per_sec;
    a->ring.filled = a->ring.read_pos = a->ring.write_pos = 0;
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);

    SDL_LockMutex(a->clock_mutex);
    a->clock -= ring_delay;
    if (a->clock < 0.0) a->clock = 0.0;
    SDL_UnlockMutex(a->clock_mutex);

    /* Restart decode thread (SDL device stays open — no audio gap).
       Raise the seek gate before the thread starts so it discards any frames
       decoded from the demux's ahead position; the gate drops automatically
       when the seek-flush sentinel (sent by demux_request_seek in
       player_cycle_audio) reaches the decode thread. */
    a->abort = 0;
    a->eos   = 0;
    atomic_store(&a->clock_seek_active, 1);
    audio_start(a);  /* SDL_PauseAudioDevice(dev, 0) is a no-op if already running */
    return 0;
}

/* -------------------------------------------------------------------------
 * audio_wake — reinit SDL audio device + restore DAC after A30 sleep/wake
 * ---------------------------------------------------------------------- */

void audio_wake(AudioCtx *a) {
    if (!a->dev) return;

    double bps = a->out_rate * AUDIO_OUT_CHANNELS * 2.0;

    SDL_LockMutex(a->ring.mutex);
    fprintf(stderr, "audio_wake: (clock=%.3f ring_delay=%.3fs)\n",
            a->clock, a->ring.filled / bps);
    SDL_UnlockMutex(a->ring.mutex);

    SDL_CloseAudioDevice(a->dev);
    a->dev = 0;

    SDL_LockMutex(a->ring.mutex);
    a->ring.filled = a->ring.read_pos = a->ring.write_pos = 0;
    SDL_CondSignal(a->ring.not_full);
    SDL_UnlockMutex(a->ring.mutex);

    packet_queue_flush(a->pkt_queue);
    fprintf(stderr, "audio_wake: ring + queue cleared\n");

    SDL_AudioSpec want = {
        .freq     = a->out_rate,
        .format   = AUDIO_SDL_FORMAT,
        .channels = AUDIO_OUT_CHANNELS,
        .samples  = AUDIO_SDL_SAMPLES,
        .callback = audio_callback,
        .userdata = a,
    };
    SDL_AudioSpec got;
    /* Don't allow frequency change — a different rate causes pitch shift.
       On A30 the OSS device may not be immediately ready after wake; retry
       a few times with brief delays before giving up. */
    int attempts = 5;
    while (attempts-- > 0) {
        a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (a->dev) break;
        fprintf(stderr, "audio_wake: SDL_OpenAudioDevice failed (%d left): %s\n",
                attempts, SDL_GetError());
        SDL_Delay(200);
    }
    if (!a->dev)
        fprintf(stderr, "audio_wake: giving up — audio will be silent\n");

}

double audio_get_clock(const AudioCtx *a) {
    SDL_LockMutex(a->clock_mutex);
    double c = a->clock;
    SDL_UnlockMutex(a->clock_mutex);

    /* `clock` is the PTS of the end of the last *decoded* chunk.
       Subtract the audio that has been decoded but not yet played:
         - bytes sitting in the ring buffer
         - samples in SDL's internal device buffer                    */
    SDL_LockMutex(a->ring.mutex);
    int buffered_bytes = a->ring.filled;
    SDL_UnlockMutex(a->ring.mutex);

    double bytes_per_sec = a->out_rate * AUDIO_OUT_CHANNELS * 2.0; /* S16 */
    double ring_delay    = buffered_bytes / bytes_per_sec;
    double sdl_delay     = (double)AUDIO_SDL_SAMPLES / a->out_rate;

    double actual = c - ring_delay - sdl_delay;
    return actual > 0.0 ? actual : 0.0;
}
