#pragma once
#include <stdint.h>

#define MAX_LAPS 16

typedef struct {
    int mode;              /* 0=Pomodoro, 1=Timer, 2=Chronometer */
    int running;           /* 1=running */
    int paused;            /* 1=paused (timer still active but not counting) */
    int elapsed;           /* elapsed ms in current session */
    int duration;          /* total duration ms (for progress) */
    int pomo_cycle;        /* completed pomodoros (0-3) */
    int pomo_phase;        /* 0=work, 1=short_break, 2=long_break */
    int lap_times[MAX_LAPS];
    int lap_count;
    int insert_mode;       /* 1=typing timer duration */
    char input_buf[12];    /* raw digit string for insert */
} TimerState;

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    int  valid;
} TrackInfo;

int  renderer_init(int logical_w, int logical_h, const char *title);
void renderer_cleanup(void);
void renderer_draw(const float *bars, int count, int is_live, const TimerState *ts, const TrackInfo *track);
int  renderer_poll_events(void);
