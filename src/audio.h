#pragma once
#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <libavcodec/avcodec.h>
#include "decoder.h"

/* -------------------------------------------------------------------------
 * Output format (fixed — swresample converts to this)
 * ---------------------------------------------------------------------- */

#define AUDIO_OUT_RATE     44100
#define AUDIO_OUT_CHANNELS 2
#define AUDIO_SDL_FORMAT   AUDIO_S16SYS  /* signed 16-bit, native endian */
#define AUDIO_SDL_SAMPLES  4096          /* callback buffer size in frames */

/* -------------------------------------------------------------------------
 * Ring buffer — lock-free reads from SDL callback, locked writes from decoder
 * ---------------------------------------------------------------------- */

#define AUDIO_RING_SIZE (1024 * 512)  /* 512 KB ≈ 3s at 44.1kHz stereo S16 */

typedef struct {
    uint8_t   *buf;
    int        capacity;
    int        filled;      /* bytes ready to read             */
    int        read_pos;
    int        write_pos;
    SDL_mutex *mutex;
    SDL_cond  *not_full;    /* signaled when space freed       */
} AudioRingBuf;

/* -------------------------------------------------------------------------
 * Audio context
 * ---------------------------------------------------------------------- */

typedef struct {
    AVCodecContext    *codec_ctx;
    struct SwrContext *swr;
    AudioRingBuf       ring;
    SDL_AudioDeviceID  dev;
    SDL_Thread        *thread;
    PacketQueue       *pkt_queue;   /* borrowed pointer — not owned */
    int                abort;
    int                eos;         /* set to 1 when decode thread reaches EOS */
    _Atomic float      volume;      /* 0.0 = mute, 1.0 = full; written from main thread,
                                       read from SDL audio callback — atomic for safety */
    _Atomic int        clock_seek_active; /* 1 while a seek is in flight after an audio track
                                             switch: decode thread discards frames (no ring
                                             writes, no clock updates) until the flush sentinel
                                             arrives, keeping the clock pinned at the seek
                                             target so video sync is never disturbed */
    _Atomic Uint32     wake_silence_until; /* SDL_GetTicks() deadline: callback outputs silence
                                              without consuming the ring until this expires, so
                                              the ALSA post-wake underrun storm drains against
                                              empty output rather than real audio data */
    double             clock;       /* PTS of last decoded sample (seconds) */
    SDL_mutex         *clock_mutex;
} AudioCtx;

/* Open audio codec and SDL audio device.
   codec_params: from the audio stream's codecpar.
   pkt_queue: the demux context's audio_queue (borrowed). */
int  audio_open (AudioCtx *a, AVCodecParameters *codec_params,
                 PacketQueue *pkt_queue, char *errbuf, int errbuf_sz);
void audio_start(AudioCtx *a);
void audio_pause(AudioCtx *a, int pause); /* 1=pause, 0=resume */
void audio_stop (AudioCtx *a);            /* signals abort, joins thread */
void audio_close(AudioCtx *a);            /* frees all resources */

double audio_get_clock(const AudioCtx *a);

/* Reinit SDL audio device after A30 sleep/wake.  Call from main thread only.
   Clears the ring buffer, reopens the device, and restores the DAC volume. */
void audio_wake(AudioCtx *a);

/* Switch to a new audio stream mid-playback.
   Stops the decode thread, reinitialises codec+swr, restarts.
   The SDL device stays open to avoid a gap.
   pkt_queue is the demux audio queue — used to wake the decode thread. */
int audio_switch_stream(AudioCtx *a, PacketQueue *pkt_queue,
                        AVCodecParameters *cp, AVRational time_base,
                        char *errbuf, int errbuf_sz);
