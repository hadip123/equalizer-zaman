#include "audio.h"
#include <pulse/pulseaudio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/*  Ring buffer                                                        */
/* ------------------------------------------------------------------ */
#define RING_SIZE (1024 * 8)
static float  ring[RING_SIZE];
static int    ring_wp = 0;
static int    ring_rp = 0;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;

static void ring_write(const float *data, int frames) {
    pthread_mutex_lock(&ring_lock);
    for (int i = 0; i < frames; i++) {
        ring[ring_wp] = data[i];
        ring_wp = (ring_wp + 1) % RING_SIZE;
        if (ring_wp == ring_rp)
            ring_rp = (ring_rp + 1) % RING_SIZE; /* overwrite oldest */
    }
    pthread_mutex_unlock(&ring_lock);
}

static int ring_read(float *buf, int count) {
    pthread_mutex_lock(&ring_lock);
    int avail = (ring_wp - ring_rp + RING_SIZE) % RING_SIZE;
    if (avail < count) {
        pthread_mutex_unlock(&ring_lock);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        buf[i] = ring[ring_rp];
        ring_rp = (ring_rp + 1) % RING_SIZE;
    }
    pthread_mutex_unlock(&ring_lock);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  FFT                                                                */
/* ------------------------------------------------------------------ */
#define FFT_N 1024

static float fft_win[FFT_N];
static int   fft_initialised = 0;

static void fft_init(void) {
    for (int i = 0; i < FFT_N; i++)
        fft_win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_N - 1)));
    fft_initialised = 1;
}

static void fft_compute(float *real, float *imag, int n) {
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (i < j) {
            float t = real[i]; real[i] = real[j]; real[j] = t;
            t = imag[i]; imag[i] = imag[j]; imag[j] = t;
        }
        int m = n >> 1;
        while (m && j >= m) { j -= m; m >>= 1; }
        j += m;
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float w_real = cosf(ang);
        float w_imag = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cur_real = 1.0f, cur_imag = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float t_real = cur_real * real[i + k + len/2] - cur_imag * imag[i + k + len/2];
                float t_imag = cur_real * imag[i + k + len/2] + cur_imag * real[i + k + len/2];
                real[i + k + len/2] = real[i + k] - t_real;
                imag[i + k + len/2] = imag[i + k] - t_imag;
                real[i + k] += t_real;
                imag[i + k] += t_imag;
                float n_real = cur_real * w_real - cur_imag * w_imag;
                float n_imag = cur_real * w_imag + cur_imag * w_real;
                cur_real = n_real;
                cur_imag = n_imag;
            }
        }
    }
}

static void compute_magnitudes(const float *samples, float *mags, int num_mags) {
    float real[FFT_N], imag[FFT_N];
    for (int i = 0; i < FFT_N; i++) {
        real[i] = samples[i] * fft_win[i];
        imag[i] = 0.0f;
    }
    fft_compute(real, imag, FFT_N);

    float sample_rate = 44100.0f;
    float min_freq = 20.0f;
    float max_freq = 20000.0f;

    for (int b = 0; b < num_mags; b++) {
        float f_low = (b == 0) ? min_freq : min_freq * powf(max_freq / min_freq, (float)(b - 0.5f) / (num_mags - 1));
        float f_high = (b == num_mags - 1) ? max_freq : min_freq * powf(max_freq / min_freq, (float)(b + 0.5f) / (num_mags - 1));

        int bin_low  = (int)(f_low  * FFT_N / sample_rate);
        int bin_high = (int)(f_high * FFT_N / sample_rate);
        if (bin_low < 1) bin_low = 1;
        if (bin_high >= FFT_N / 2) bin_high = FFT_N / 2 - 1;

        float sum = 0.0f;
        int count = 0;
        for (int k = bin_low; k <= bin_high; k++) {
            sum += sqrtf(real[k] * real[k] + imag[k] * imag[k]);
            count++;
        }
        mags[b] = (count > 0) ? sum / count : 0.0f;
    }

    float max_mag = 0.0001f;
    for (int b = 0; b < num_mags; b++)
        if (mags[b] > max_mag) max_mag = mags[b];

    for (int b = 0; b < num_mags; b++)
        mags[b] = mags[b] / max_mag;
}

/* ------------------------------------------------------------------ */
/*  PulseAudio thread                                                  */
/* ------------------------------------------------------------------ */
static pa_mainloop     *pa_ml   = NULL;
static pa_context      *pa_ctx  = NULL;
static pa_stream       *pa_str  = NULL;
static pthread_t        pa_thr;
static volatile int     pa_done = 0;
static volatile int     pa_ok   = 0;

const char *audio_source_name = NULL;

static void pa_state_cb(pa_context *c, void *ud) {
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            *(int*)ud = 1;
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            *(int*)ud = -1;
            break;
        default: break;
    }
}

static void pa_stream_cb(pa_stream *s, size_t len, void *ud) {
    (void)s; (void)ud;
    const void *data;
    if (pa_stream_peek(s, &data, &len) < 0) return;
    if (data) {
        int frames = len / sizeof(float);
        ring_write((const float*)data, frames);
    }
    pa_stream_drop(s);
}

static void pa_source_cb(pa_context *c, const pa_source_info *i, int eol, void *ud) {
    pa_stream **sp = (pa_stream**)ud;
    if (eol || *sp) return;
    if (i->monitor_of_sink == PA_INVALID_INDEX) return;

    audio_source_name = i->name;
    printf("[audio] monitor source: %s\n", i->name);
    printf("[audio] description:     %s\n", i->description);

    pa_sample_spec ss;
    ss.format   = PA_SAMPLE_FLOAT32LE;
    ss.rate     = 44100;
    ss.channels = 1;

    *sp = pa_stream_new(c, "equalizer capture", &ss, NULL);
    if (!*sp) { fprintf(stderr, "[audio] pa_stream_new failed\n"); return; }

    pa_stream_set_read_callback(*sp, pa_stream_cb, NULL);
    pa_buffer_attr ba;
    ba.maxlength  = (uint32_t)-1;
    ba.tlength    = (uint32_t)-1;
    ba.prebuf     = (uint32_t)-1;
    ba.minreq     = (uint32_t)-1;
    ba.fragsize   = 4096;

    pa_stream_connect_record(*sp, i->name, &ba, PA_STREAM_NOFLAGS);
}

static int pa_find_monitor(pa_context *c) {
    pa_operation *o = pa_context_get_source_info_list(c, pa_source_cb, &pa_str);
    if (!o) return 0;
    while (pa_operation_get_state(o) == PA_OPERATION_RUNNING)
        pa_mainloop_iterate(pa_ml, 0, NULL);
    pa_operation_unref(o);
    return (pa_str != NULL);
}

static void *pa_thread(void *arg) {
    (void)arg;
    pa_ml = pa_mainloop_new();
    if (!pa_ml) return NULL;

    pa_ctx = pa_context_new(pa_mainloop_get_api(pa_ml), "equalizer");
    if (!pa_ctx) { pa_mainloop_free(pa_ml); return NULL; }

    int ready = 0;
    pa_context_set_state_callback(pa_ctx, pa_state_cb, &ready);
    if (pa_context_connect(pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        pa_context_unref(pa_ctx);
        pa_mainloop_free(pa_ml);
        return NULL;
    }

    while (ready == 0) pa_mainloop_iterate(pa_ml, 1, NULL);
    if (ready < 0) {
        pa_context_disconnect(pa_ctx);
        pa_context_unref(pa_ctx);
        pa_mainloop_free(pa_ml);
        return NULL;
    }

    printf("[audio] connected to PulseAudio\n");

    if (!pa_find_monitor(pa_ctx)) {
        fprintf(stderr, "[audio] no monitor source found, trying default source\n");
        pa_sample_spec ss;
        ss.format   = PA_SAMPLE_FLOAT32LE;
        ss.rate     = 44100;
        ss.channels = 1;
        pa_str = pa_stream_new(pa_ctx, "equalizer capture", &ss, NULL);
        if (pa_str) {
            pa_stream_set_read_callback(pa_str, pa_stream_cb, NULL);
            pa_buffer_attr ba;
            ba.maxlength  = (uint32_t)-1;
            ba.tlength    = (uint32_t)-1;
            ba.prebuf     = (uint32_t)-1;
            ba.minreq     = (uint32_t)-1;
            ba.fragsize   = 4096;
            pa_stream_connect_record(pa_str, NULL, &ba, PA_STREAM_NOFLAGS);
        }
    }

    if (pa_str) pa_ok = 1;
    else fprintf(stderr, "[audio] failed to create stream\n");

    while (!pa_done && pa_ok)
        pa_mainloop_iterate(pa_ml, 1, NULL);

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Generated audio fallback                                          */
/* ------------------------------------------------------------------ */
static int    fallback_active = 0;
static double fb_phase1 = 0.0, fb_phase2 = 0.0, fb_phase3 = 0.0;
static float  fb_noise_buf[512];
static int    fb_noise_pos = 0;

static void fallback_fill(float *buf, int n) {
    for (int i = 0; i < n; i++) {
        fb_phase1 += 0.008;
        fb_phase2 += 0.022;
        fb_phase3 += 0.003;
        float s = sinf(fb_phase1) * 0.3f
                + sinf(fb_phase2) * 0.15f
                + sinf(fb_phase3) * 0.4f;
        if (fb_noise_pos >= 512) {
            for (int j = 0; j < 512; j++)
                fb_noise_buf[j] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
            fb_noise_pos = 0;
        }
        s += fb_noise_buf[fb_noise_pos++] * 0.05f;
        buf[i] = s * 0.5f;
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
int audio_init(void) {
    if (!fft_initialised) fft_init();

    if (pthread_create(&pa_thr, NULL, pa_thread, NULL) == 0) {
        for (int i = 0; i < 50; i++) {
            if (pa_ok) { printf("[audio] live capture ready\n"); return 1; }
            if (pa_done) break;
            usleep(50000);
        }
    }

    printf("[audio] PulseAudio unavailable, using generated tones\n");
    fallback_active = 1;
    return 1;
}

void audio_cleanup(void) {
    if (pa_ok) {
        pa_done = 1;
        pthread_join(pa_thr, NULL);
        if (pa_str) { pa_stream_disconnect(pa_str); pa_stream_unref(pa_str); }
        if (pa_ctx) { pa_context_disconnect(pa_ctx); pa_context_unref(pa_ctx); }
        if (pa_ml)  pa_mainloop_free(pa_ml);
    }
}

int audio_get_frequencies(float *bars, int count) {
    if (count > AUDIO_NUM_BARS) count = AUDIO_NUM_BARS;

    if (fallback_active) {
        float samples[FFT_N];
        fallback_fill(samples, FFT_N);
        float mags[AUDIO_NUM_BARS];
        compute_magnitudes(samples, mags, AUDIO_NUM_BARS);
        for (int i = 0; i < count; i++)
            bars[i] = mags[i];
        return 1;
    }

    float samples[FFT_N];
    if (!ring_read(samples, FFT_N)) return 0;

    float mags[AUDIO_NUM_BARS];
    compute_magnitudes(samples, mags, AUDIO_NUM_BARS);
    for (int i = 0; i < count; i++)
        bars[i] = mags[i];
    return 1;
}
