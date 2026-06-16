#include "audio.h"
#include "renderer.h"
#include "mpris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>

#define SMOOTH    0.22f
#define PEAK_DROP 0.004f

/* timer constants */
#define POMO_WORK_MS   (25 * 60 * 1000)
#define POMO_SHORT_MS  ( 5 * 60 * 1000)
#define POMO_LONG_MS   (15 * 60 * 1000)
#define DEFAULT_TIMER_MS (5 * 60 * 1000)
#define MAX_TIMER_MS     (24 * 60 * 60 * 1000)

typedef enum { MODE_POMODORO, MODE_TIMER, MODE_CHRONO } TimerMode;

typedef struct {
    TimerMode mode;
    int running;
    int paused;
    Uint32 last_tick;

    int elapsed;        /* current session elapsed ms (for renderer) */
    int duration;       /* current session total ms (for renderer) */

    /* pomodoro */
    int pomo_cycle;     /* completed work sessions this cycle (0-3) */
    int pomo_phase;     /* 0=work, 1=short_break, 2=long_break */
    int phase_elapsed;  /* ms into current phase */

    /* timer */
    int timer_duration; /* ms for countdown */
    int insert_mode;
    char input_buf[8];
    int input_len;

    /* chronometer */
    int chrono_elapsed;
    int lap_times[MAX_LAPS];
    int lap_count;
} AppState;

static const int   phase_durations[] = { POMO_WORK_MS, POMO_SHORT_MS, POMO_LONG_MS };

static void reset_phase(AppState *s) {
    s->phase_elapsed = 0;
    if (s->mode == MODE_POMODORO)
        s->duration = phase_durations[s->pomo_phase];
    else if (s->mode == MODE_TIMER)
        s->duration = s->timer_duration;
    else
        s->duration = 0;
}

static void init_state(AppState *s) {
    s->mode          = MODE_POMODORO;
    s->running       = 0;
    s->paused        = 0;
    s->last_tick     = SDL_GetTicks();
    s->pomo_cycle    = 0;
    s->pomo_phase    = 0;
    s->phase_elapsed = 0;
    s->timer_duration = DEFAULT_TIMER_MS;
    s->insert_mode = 0;
    s->input_buf[0] = 0;
    s->input_len = 0;
    s->chrono_elapsed = 0;
    s->lap_count     = 0;
    reset_phase(s);
}

static void start_stop(AppState *s) {
    if (!s->running) {
        s->running = 1;
        s->paused = 0;
        s->last_tick = SDL_GetTicks();
    } else if (!s->paused) {
        s->paused = 1;
    } else {
        s->paused = 0;
        s->last_tick = SDL_GetTicks();
    }
}

static void reset(AppState *s) {
    s->running = 0;
    s->paused = 0;
    s->lap_count = 0;
    s->chrono_elapsed = 0;
    reset_phase(s);
}

static void complete_phase(AppState *s) {
    if (s->mode == MODE_POMODORO) {
        if (s->pomo_phase == 0) {
            s->pomo_cycle++;
            if (s->pomo_cycle >= 4) {
                s->pomo_cycle = 0;
                s->pomo_phase = 2;
            } else {
                s->pomo_phase = 1;
            }
        } else {
            s->pomo_phase = 0;
        }
        s->running = 0;
        s->paused = 0;
        reset_phase(s);
    } else if (s->mode == MODE_TIMER) {
        s->running = 0;
        s->paused = 0;
    }
}

static void update_timer(AppState *s) {
    if (!s->running || s->paused) return;

    Uint32 now = SDL_GetTicks();
    int dt = (int)(now - s->last_tick);
    if (dt > 1000) dt = 1000; /* cap to avoid spiral on lag */
    s->last_tick = now;

    if (s->mode == MODE_POMODORO) {
        s->phase_elapsed += dt;
        if (s->phase_elapsed >= phase_durations[s->pomo_phase]) {
            s->phase_elapsed = phase_durations[s->pomo_phase];
            complete_phase(s);
        }
        s->elapsed = s->phase_elapsed;
    } else if (s->mode == MODE_TIMER) {
        s->phase_elapsed += dt;
        if (s->phase_elapsed >= s->timer_duration) {
            s->phase_elapsed = s->timer_duration;
            complete_phase(s);
        }
        s->elapsed = s->phase_elapsed;
    } else {
        s->chrono_elapsed += dt;
        s->elapsed = s->chrono_elapsed;
    }
}

int main(void) {
    printf("equalizer — zaman edition\n");
    printf("=========================\n\n");

    if (!renderer_init(960, 640, "zaman")) {
        fprintf(stderr, "failed to initialize renderer\n");
        return 1;
    }

    if (!audio_init()) {
        fprintf(stderr, "failed to initialize audio\n");
        renderer_cleanup();
        return 1;
    }

    mpris_init();

    float raw_bars[AUDIO_NUM_BARS];
    float smooth_bars[AUDIO_NUM_BARS];
    float peak_bars[AUDIO_NUM_BARS];
    float peak_vel[AUDIO_NUM_BARS];

    for (int i = 0; i < AUDIO_NUM_BARS; i++) {
        smooth_bars[i] = 0.0f;
        peak_bars[i]   = 0.0f;
        peak_vel[i]    = 0.0f;
    }

    AppState state;
    init_state(&state);

    int is_live = 1;
    int no_data_count = 0;
    int frame = 0;
    TrackInfo track = {0};
    while (renderer_poll_events()) {
        /* --- MPRIS track info poll (every ~30 frames) --- */
        if (frame % 30 == 0) {
            mpris_poll(&track);
        }
        frame++;

        /* --- keyboard --- */
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto done;
            if (e.type != SDL_KEYDOWN) continue;
            SDL_Keycode k = e.key.keysym.sym;

            if (k == SDLK_q || k == SDLK_ESCAPE) goto done;

            /* --- INSERT MODE (Timer digit entry) --- */
            if (state.insert_mode) {
                if (k >= SDLK_0 && k <= SDLK_9) {
                    if (state.input_len < 6) {
                        state.input_buf[state.input_len++] = '0' + (k - SDLK_0);
                        state.input_buf[state.input_len] = 0;
                    }
                    continue;
                }
                if (k == SDLK_BACKSPACE) {
                    if (state.input_len > 0) {
                        state.input_buf[--state.input_len] = 0;
                    }
                    continue;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (state.input_len > 0) {
                        /* parse right-to-left: last 2 = seconds, rest = minutes */
                        int sec = 0, min = 0;
                        if (state.input_len >= 2)
                            sec = (state.input_buf[state.input_len - 2] - '0') * 10
                                + (state.input_buf[state.input_len - 1] - '0');
                        else
                            sec = state.input_buf[state.input_len - 1] - '0';

                        for (int i = 0; i < state.input_len - 2; i++)
                            min = min * 10 + (state.input_buf[i] - '0');
                        if (state.input_len == 1) min = 0;

                        int total_ms = (min * 60 + sec) * 1000;
                        if (total_ms < 1000) total_ms = 1000;
                        if (total_ms > MAX_TIMER_MS) total_ms = MAX_TIMER_MS;
                        state.timer_duration = total_ms;
                        if (!state.running) reset_phase(&state);
                    }
                    state.insert_mode = 0;
                    state.input_buf[0] = 0;
                    state.input_len = 0;
                    continue;
                }
                if (k == SDLK_ESCAPE) {
                    state.insert_mode = 0;
                    state.input_buf[0] = 0;
                    state.input_len = 0;
                    continue;
                }
                continue; /* ignore all other keys in insert mode */
            }

            /* --- NORMAL MODE --- */
            if (k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                start_stop(&state);
                continue;
            }
            if (k == SDLK_r) { reset(&state); continue; }

            if (k == SDLK_l || k == SDLK_RIGHT) {
                if (!state.running) {
                    state.mode = (state.mode + 1) % 3;
                    reset(&state);
                }
                continue;
            }
            if (k == SDLK_h || k == SDLK_LEFT) {
                if (!state.running) {
                    state.mode = (state.mode + 2) % 3;
                    reset(&state);
                }
                continue;
            }

            if (state.mode == MODE_TIMER && !state.running) {
                if (k == SDLK_i) {
                    state.insert_mode = 1;
                    state.input_buf[0] = 0;
                    state.input_len = 0;
                    continue;
                }
                if (k == SDLK_EQUALS || k == SDLK_PLUS) {
                    int mod = (e.key.keysym.mod & KMOD_SHIFT) ? 1000 : 60000;
                    state.timer_duration += mod;
                    if (state.timer_duration > MAX_TIMER_MS) state.timer_duration = MAX_TIMER_MS;
                    if (!state.running) reset_phase(&state);
                    continue;
                }
                if (k == SDLK_MINUS) {
                    int mod = (e.key.keysym.mod & KMOD_SHIFT) ? 1000 : 60000;
                    state.timer_duration -= mod;
                    if (state.timer_duration < 1000) state.timer_duration = 1000;
                    if (!state.running) reset_phase(&state);
                    continue;
                }
            }

            if (state.mode == MODE_CHRONO && state.running && !state.paused) {
                if (k == SDLK_l) {
                    if (state.lap_count < MAX_LAPS) {
                        state.lap_times[state.lap_count++] = state.chrono_elapsed;
                    }
                    continue;
                }
            }
        }

        /* --- update timer --- */
        update_timer(&state);

        /* --- audio --- */
        int got_data = audio_get_frequencies(raw_bars, AUDIO_NUM_BARS);

        if (!got_data) {
            no_data_count++;
            if (no_data_count > 120) is_live = 0;
        } else {
            no_data_count = 0;
            is_live = 1;
            for (int i = 0; i < AUDIO_NUM_BARS; i++) {
                float val = raw_bars[i];
                if (val < 0.0f) val = 0.0f;
                if (val > 1.0f) val = 1.0f;
                smooth_bars[i] += (val - smooth_bars[i]) * SMOOTH;
                if (smooth_bars[i] >= peak_bars[i]) {
                    peak_bars[i] = smooth_bars[i];
                    peak_vel[i] = 0.001f;
                } else {
                    peak_vel[i] += PEAK_DROP;
                    peak_bars[i] -= peak_vel[i];
                    if (peak_bars[i] < 0.0f) peak_bars[i] = 0.0f;
                }
            }
        }

        float display_bars[AUDIO_NUM_BARS];
        for (int i = 0; i < AUDIO_NUM_BARS; i++) {
            float p = peak_bars[i];
            if (p > smooth_bars[i] + 0.015f)
                display_bars[i] = p;
            else
                display_bars[i] = smooth_bars[i];
        }

        /* --- timer state for renderer --- */
        TimerState ts;
        ts.mode    = state.mode;
        ts.running = state.running;
        ts.paused  = state.paused;
        ts.elapsed = state.mode == MODE_CHRONO ? state.chrono_elapsed : state.phase_elapsed;
        ts.duration = state.duration;
        ts.pomo_cycle  = state.pomo_cycle;
        ts.pomo_phase  = state.pomo_phase;
        ts.lap_count   = state.lap_count;
        ts.insert_mode = state.insert_mode;
        snprintf(ts.input_buf, sizeof(ts.input_buf), "%s", state.input_buf);
        for (int i = 0; i < state.lap_count; i++)
            ts.lap_times[i] = state.lap_times[i];

        renderer_draw(display_bars, AUDIO_NUM_BARS, is_live, &ts, &track);
    }

done:
    printf("shutting down...\n");
    mpris_cleanup();
    audio_cleanup();
    renderer_cleanup();
    return 0;
}
