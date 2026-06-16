#include "renderer.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

static SDL_Window   *win    = NULL;
static SDL_Renderer *rend   = NULL;
static int log_w, log_h, running = 1;

static SDL_Texture  *full_tex = NULL;
static SDL_Texture  *blur_tex = NULL;
static int blur_w, blur_h;

static TTF_Font *fnt_small = NULL;
static TTF_Font *fnt_time  = NULL;

static const char *mode_names[]  = { "POMODORO", "TIMER", "CHRONO" };
static const char *phase_names[] = { "WORK", "SHORT BREAK", "LONG BREAK" };

#define C(r,g,b,a) do { SDL_SetRenderDrawColor(rend,r,g,b,a); } while(0)

static void hsv_to_rgb(float h, float s, float v, Uint8 *r, Uint8 *g, Uint8 *b) {
    h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
    float c = v * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r1, g1, b1;
    if (hp < 1.0f)      { r1 = c; g1 = x; b1 = 0; }
    else if (hp < 2.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (hp < 3.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (hp < 4.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; g1 = 0; b1 = c; }
    else                { r1 = c; g1 = 0; b1 = x; }
    float m = v - c;
    *r = (Uint8)((r1 + m) * 255.0f);
    *g = (Uint8)((g1 + m) * 255.0f);
    *b = (Uint8)((b1 + m) * 255.0f);
}

static void tx(int x, int y, const char *text, TTF_Font *f, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    if (!text || !text[0]) return;
    SDL_Color c = {r, g, b, a};
    SDL_Surface *surf = TTF_RenderText_Blended(f, text, c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    if (tex) {
        SDL_Rect dst = {x, y, surf->w, surf->h};
        SDL_RenderCopy(rend, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static int tw(const char *text, TTF_Font *f) {
    int w;
    TTF_SizeText(f, text, &w, NULL);
    return w;
}

static void format_time(char *buf, int len, int total_seconds) {
    int h = total_seconds / 3600;
    int m = (total_seconds % 3600) / 60;
    int s = total_seconds % 60;
    if (h > 0)
        snprintf(buf, len, "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, len, "%02d:%02d", m, s);
}

static void format_end_at(char *buf, int len, int remaining_seconds) {
    time_t t = time(NULL) + remaining_seconds;
    struct tm *tm = localtime(&t);
    int hour = tm->tm_hour;
    const char *ampm = "AM";
    if (hour >= 12) { ampm = "PM"; if (hour > 12) hour -= 12; }
    if (hour == 0) hour = 12;
    snprintf(buf, len, "ends at %d:%02d %s", hour, tm->tm_min, ampm);
}

static void reload_fonts(float scale) {
    if (fnt_small) TTF_CloseFont(fnt_small);
    if (fnt_time)  TTF_CloseFont(fnt_time);
    int ss = (int)(11 * scale + 0.5f); if (ss < 11) ss = 11;
    int ts = (int)(72 * scale + 0.5f); if (ts < 72) ts = 72;
    fnt_small = TTF_OpenFont("assets/DejaVuSansMono.ttf", ss);
    if (!fnt_small) fnt_small = TTF_OpenFont("/usr/share/fonts/TTF/DejaVuSansMono.ttf", ss);
    fnt_time  = TTF_OpenFont("assets/DejaVuSansMono-Bold.ttf", ts);
    if (!fnt_time)  fnt_time  = TTF_OpenFont("/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf", ts);
}

int renderer_init(int logical_w, int logical_h, const char *title) {
    log_w = logical_w;
    log_h = logical_h;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return 0;
    }

    win = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        logical_w, logical_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        TTF_Quit(); SDL_Quit();
        return 0;
    }

    rend = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rend) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit();
        return 0;
    }

    /* no logical size — we handle scaling manually */

    full_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET, logical_w, logical_h);
    int pw = logical_w * 0.55f;
    int ph = logical_h * 0.52f;
    blur_w = pw / 5; if (blur_w < 2) blur_w = 2;
    blur_h = ph / 5; if (blur_h < 2) blur_h = 2;
    blur_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET, blur_w, blur_h);

    reload_fonts(1.0f);
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    return 1;
}

void renderer_cleanup(void) {
    if (fnt_time)  TTF_CloseFont(fnt_time);
    if (fnt_small) TTF_CloseFont(fnt_small);
    if (blur_tex) SDL_DestroyTexture(blur_tex);
    if (full_tex) SDL_DestroyTexture(full_tex);
    if (rend) SDL_DestroyRenderer(rend);
    if (win)  SDL_DestroyWindow(win);
    TTF_Quit(); SDL_Quit();
}

int renderer_poll_events(void) { return running; }

void renderer_draw(const float *bars, int count, int is_live, const TimerState *ts, const TrackInfo *track) {
    int out_w, out_h;
    SDL_GetRendererOutputSize(rend, &out_w, &out_h);
    float scale = fminf((float)out_w / log_w, (float)out_h / log_h);
    int view_w = (int)(log_w * scale);
    int view_h = (int)(log_h * scale);
    int view_x = (out_w - view_w) / 2;
    int view_y = (out_h - view_h) / 2;

    static int last_w = 0, last_h = 0;
    if (out_w != last_w || out_h != last_h) {
        last_w = out_w; last_h = out_h;
        reload_fonts(scale);
    }

    int num_bars = count < 64 ? count : 64;

    /* ====== STEP 1 — render bars to logical-size offscreen ====== */
    SDL_SetRenderTarget(rend, full_tex);
    C(6, 6, 8, 255); SDL_RenderClear(rend);

    float bar_spacing = (float)log_w / num_bars;
    float bar_w = bar_spacing * 0.6f;
    float gap = bar_spacing - bar_w;
    int bar_bottom = log_h - 18;

    for (int i = 0; i < num_bars; i++) {
        float val = bars[i];
        if (val < 0) val = 0;
        if (val > 1) val = 1;
        float bh = val * bar_bottom * 0.85f;
        if (bh < 0.5f) bh = 0.5f;
        float bx = i * bar_spacing + gap / 2.0f;
        float by = bar_bottom - bh;
        Uint8 r, g, b;
        hsv_to_rgb(160.0f + i * 2.8f, 0.55f + val * 0.3f, 0.35f + val * 0.35f, &r, &g, &b);
        C(r, g, b, 180);
        SDL_RenderFillRect(rend, &(SDL_Rect){(int)(bx+0.5f), (int)(by+0.5f), (int)(bar_w+0.5f), (int)(bh+0.5f)});
    }
    for (int i = 0; i < num_bars; i++) {
        float val = bars[i];
        if (val < 0.05f) continue;
        float px = i * bar_spacing + gap / 2.0f;
        float py = bar_bottom - val * bar_bottom * 0.85f;
        Uint8 r, g, b;
        hsv_to_rgb(160.0f + i * 2.8f, 0.8f, 0.85f, &r, &g, &b);
        C(r, g, b, 150);
        SDL_RenderFillRect(rend, &(SDL_Rect){(int)(px+0.5f), (int)(py-1+0.5f), (int)(bar_w+0.5f), 2});
    }

    /* ====== STEP 2 — composite to screen (linear = smooth bars) ====== */
    SDL_SetRenderTarget(rend, NULL);
    C(6, 6, 8, 255); SDL_RenderClear(rend);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderCopy(rend, full_tex, NULL, &(SDL_Rect){view_x, view_y, view_w, view_h});

    /* ====== STEP 3 — blur panel at logical coords, then scale to screen ====== */
    int pw_l = log_w * 0.55f;
    int ph_l = log_h * 0.52f;
    int px_l = (log_w - pw_l) / 2;
    int py_l = (log_h - ph_l) / 2 - 10;

    SDL_SetRenderTarget(rend, blur_tex);
    C(0, 0, 0, 0); SDL_RenderClear(rend);
    SDL_RenderCopy(rend, full_tex, &(SDL_Rect){px_l, py_l, pw_l, ph_l}, NULL);

    int px_p = view_x + (int)(px_l * scale);
    int py_p = view_y + (int)(py_l * scale);
    int pw_p = (int)(pw_l * scale);
    int ph_p = (int)(ph_l * scale);

    SDL_SetRenderTarget(rend, NULL);
    SDL_RenderCopy(rend, blur_tex, NULL, &(SDL_Rect){px_p, py_p, pw_p, ph_p});
    C(0, 0, 0, 100);
    SDL_RenderFillRect(rend, &(SDL_Rect){px_p, py_p, pw_p, ph_p});

    /* ====== STEP 4 — UI overlay ====== */
    int accent_hue = 160 + ts->mode * 60;
    Uint8 ar, ag, ab;
    hsv_to_rgb(accent_hue, 0.7f, 0.6f, &ar, &ag, &ab);
    C(ar, ag, ab, 150);
    SDL_RenderFillRect(rend, &(SDL_Rect){px_p + 2, py_p + 1, pw_p - 4, 2});

    int tab_y = py_p + (int)(12 * scale);
    int area_w = pw_p - (int)(24 * scale);
    int tws[3];
    int total_w = 0;
    for (int m = 0; m < 3; m++) { tws[m] = tw(mode_names[m], fnt_small); total_w += tws[m]; }
    int tgap = (area_w - total_w) / 4; if (tgap < 8) tgap = 8;
    int cx = px_p + (int)(12 * scale) + tgap;
    for (int m = 0; m < 3; m++) {
        if (m == ts->mode) {
            tx(cx, tab_y, mode_names[m], fnt_small, ar, ag, ab, 255);
            C(ar, ag, ab, 120);
            SDL_RenderFillRect(rend, &(SDL_Rect){cx - 2, tab_y + (int)(13 * scale), tws[m] + 4, 2});
        } else {
            tx(cx, tab_y, mode_names[m], fnt_small, 170, 175, 180, 200);
        }
        cx += tws[m] + tgap;
    }

    /* time string */
    char time_str[24];
    if (ts->insert_mode && ts->mode == 1 && ts->input_buf[0]) {
        int len = strlen(ts->input_buf);
        int sec = (len >= 2) ? (ts->input_buf[len-2]-'0')*10 + (ts->input_buf[len-1]-'0')
                             : (ts->input_buf[len-1]-'0');
        int min = 0;
        for (int i = 0; i < len - 2; i++) min = min * 10 + (ts->input_buf[i]-'0');
        if (len == 1) min = 0;
        int total_s = min * 60 + sec;
        if (total_s > 86400) total_s = 86400;
        format_time(time_str, sizeof(time_str), total_s);
    } else {
        int display_s = (ts->mode == 1)
            ? ((ts->duration - ts->elapsed) / 1000)
            : (ts->elapsed / 1000);
        if (display_s < 0) display_s = 0;
        format_time(time_str, sizeof(time_str), display_s);
    }

    Uint8 tr=230, tg=235, tb=240, ta=255;
    if (ts->paused)           { tr=120; tg=122; tb=125; ta=160; }
    else if (ts->insert_mode) { tr=255; tg=210; tb=100; ta=255; }
    else if (ts->mode == 0) {
        int pi = ts->pomo_phase;
        if (pi == 0)      { tr=140; tg=255; tb=190; }
        else if (pi == 1) { tr=255; tg=230; tb=120; }
        else              { tr=255; tg=170; tb=110; }
    }

    int tw_time = tw(time_str, fnt_time);
    int lx = px_p + (pw_p - tw_time) / 2;
    int ly = tab_y + (int)(30 * scale);
    tx(lx, ly, time_str, fnt_time, tr, tg, tb, ta);

    /* progress bar */
    int pb_y = ly + (int)(90 * scale);
    int pb_w = (int)(pw_p * 0.7f);
    int pb_h = (int)(5 * scale);
    int pb_x = px_p + (pw_p - pb_w) / 2;
    C(40, 40, 45, 120);
    SDL_RenderFillRect(rend, &(SDL_Rect){pb_x, pb_y, pb_w, pb_h});

    float prog = 0;
    if (ts->mode == 0) {
        int dur = (ts->pomo_phase == 0) ? 1500000 : (ts->pomo_phase == 1) ? 300000 : 900000;
        if (dur > 0) prog = (float)ts->elapsed / dur;
    } else if (ts->mode == 1 && ts->duration > 0) {
        prog = (float)ts->elapsed / ts->duration;
    }
    if (prog > 0 && ts->mode != 2) {
        if (prog > 1) prog = 1;
        int fw = (int)(pb_w * prog);
        if (fw > 0) {
            Uint8 pr2, pg2, pb2;
            if (ts->mode == 0 && ts->pomo_phase == 0)
                hsv_to_rgb(140,0.7f,0.6f, &pr2,&pg2,&pb2);
            else
                hsv_to_rgb(160+ts->mode*60,0.6f,0.5f, &pr2,&pg2,&pb2);
            C(pr2, pg2, pb2, 220);
            SDL_RenderFillRect(rend, &(SDL_Rect){pb_x, pb_y, fw, pb_h});
        }
    }

    /* ends-at display */
    if ((ts->mode == 0 || ts->mode == 1) && ts->running && !ts->paused) {
        int rem_s = 0;
        if (ts->mode == 0) {
            int dur = (ts->pomo_phase == 0) ? 1500000 : (ts->pomo_phase == 1) ? 300000 : 900000;
            rem_s = (dur - ts->elapsed) / 1000;
        } else {
            rem_s = (ts->duration - ts->elapsed) / 1000;
        }
        if (rem_s > 0 && rem_s <= 86400) {
            char end_buf[32];
            format_end_at(end_buf, sizeof(end_buf), rem_s);
            int ew = tw(end_buf, fnt_small);
            int ex = px_p + (pw_p - ew) / 2;
            int ey = pb_y + pb_h + (int)(4 * scale);
            tx(ex, ey, end_buf, fnt_small, 140, 145, 150, 200);
        }
    }

    /* paused badge */
    if (ts->paused && !ts->insert_mode) {
        const char *pt = "PAUSED";
        int ptw = tw(pt, fnt_small);
        int ptx = pb_x + (pb_w - ptw) / 2;
        int pty = pb_y - (int)(18 * scale);
        tx(ptx, pty, pt, fnt_small, 200, 200, 210, 200);
        C(200, 200, 210, 80);
        SDL_RenderDrawLine(rend, ptx, pty + (int)(12 * scale), ptx + ptw, pty + (int)(12 * scale));
    }

    /* labels */
    int lab_y = pb_y + (int)(16 * scale);
    if (ts->mode == 0) {
        tx(px_p + (int)(12 * scale), lab_y, phase_names[ts->pomo_phase], fnt_small, tr, tg, tb, 235);
        char buf[32];
        snprintf(buf, sizeof(buf), "POMO %d/4", ts->pomo_cycle + 1);
        int bw = tw(buf, fnt_small);
        tx(px_p + pw_p - bw - (int)(12 * scale), lab_y, buf, fnt_small, 180, 185, 190, 200);
    } else if (ts->mode == 1) {
        if (ts->insert_mode) {
            tx(px_p + (int)(12 * scale), lab_y, "INSERT TIME", fnt_small, 255, 210, 100, 235);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d DIGITS", (int)strlen(ts->input_buf));
            int bw = tw(buf, fnt_small);
            tx(px_p + pw_p - bw - (int)(12 * scale), lab_y, buf, fnt_small, 180, 185, 190, 180);
        } else {
            tx(px_p + (int)(12 * scale), lab_y, "COUNTDOWN", fnt_small, 180, 185, 190, 200);
        }
    } else {
        tx(px_p + (int)(12 * scale), lab_y, "STOPWATCH", fnt_small, 180, 185, 190, 200);
        if (ts->lap_count > 0) {
            char buf[32];
            int last = ts->lap_times[ts->lap_count-1] / 1000;
            int prev = (ts->lap_count >= 2) ? ts->lap_times[ts->lap_count-2] / 1000 : 0;
            snprintf(buf, sizeof(buf), "LAP %d  +%ds", ts->lap_count, last - prev);
            int bw = tw(buf, fnt_small);
            tx(px_p + pw_p - bw - (int)(12 * scale), lab_y, buf, fnt_small, 170, 200, 230, 210);
        }
    }

    /* hints */
    int hint_y = lab_y + (int)(20 * scale);
    if (ts->insert_mode) {
        const char *h1 = "0-9: Type   BACKSPACE: Delete   ENTER: Set   ESC: Cancel";
        if (tw(h1, fnt_small) > pw_p - (int)(24 * scale))
            h1 = "Type digits (right-aligned)   ENTER: Set";
        int hw = tw(h1, fnt_small);
        tx(px_p + (pw_p - hw) / 2, hint_y, h1, fnt_small, 255, 210, 100, 220);
    } else {
        const char *h1 = "SPACE: Start/Pause   R: Reset   H/L: Mode   Q: Quit";
        if (tw(h1, fnt_small) > pw_p - (int)(24 * scale))
            h1 = "SPACE: Start/Pause   R: Reset   H/L: Mode";
        int hw = tw(h1, fnt_small);
        tx(px_p + (pw_p - hw) / 2, hint_y, h1, fnt_small, 160, 165, 170, 220);
    }

    if (ts->mode == 1 && !ts->insert_mode) {
        const char *h2 = "+/-: Minutes   SHIFT+/-: Seconds   I: Type";
        if (tw(h2, fnt_small) > pw_p - (int)(24 * scale))
            h2 = "+/-: Minutes   I: Type";
        int hw = tw(h2, fnt_small);
        tx(px_p + (pw_p - hw) / 2, hint_y + (int)(14 * scale), h2, fnt_small, 140, 145, 150, 180);
    }
    if (ts->mode == 2) {
        const char *h3 = "L: Lap";
        int hw = tw(h3, fnt_small);
        tx(px_p + (pw_p - hw) / 2, hint_y + (int)(14 * scale), h3, fnt_small, 140, 145, 150, 180);
    }

    /* ====== now-playing bar ====== */
    int np_h = (int)(26 * scale);
    int np_y = view_y + view_h - np_h;
    C(4, 4, 6, 220);
    SDL_RenderFillRect(rend, &(SDL_Rect){view_x, np_y, view_w, np_h});
    C(40, 42, 48, 50);
    SDL_RenderDrawLine(rend, view_x, np_y, view_x + view_w, np_y);

    if (track && track->valid && track->title[0]) {
        char np[512];
        int p = 0;
        if (track->artist[0]) {
            int alen = strlen(track->artist);
            memcpy(np + p, track->artist, alen); p += alen;
            memcpy(np + p, " - ", 3); p += 3;
        }
        int tlen = strlen(track->title);
        if (p + tlen < (int)sizeof(np) - 1) {
            memcpy(np + p, track->title, tlen); p += tlen;
        }
        if (track->album[0]) {
            int alen = strlen(track->album);
            if (p + alen + 4 < (int)sizeof(np) - 1) {
                memcpy(np + p, "  (", 3); p += 3;
                memcpy(np + p, track->album, alen); p += alen;
                np[p++] = ')';
            }
        }
        np[p] = 0;

        int max_w = view_w - (int)(40 * scale);
        while (tw(np, fnt_small) > max_w && p > 4) np[--p] = 0;
        if (tw(np, fnt_small) > max_w) {
            snprintf(np, sizeof(np), "%s", track->title);
            while (tw(np, fnt_small) > max_w && strlen(np) > 4) np[strlen(np)-1] = 0;
        }

        int nw = tw(np, fnt_small);
        tx(view_x + (view_w - nw) / 2, np_y + (np_h - (int)(11 * scale)) / 2, np, fnt_small, 200, 210, 220, 255);
    } else {
        const char *idle = "waiting for music...";
        int iw = tw(idle, fnt_small);
        tx(view_x + (view_w - iw) / 2, np_y + (np_h - (int)(11 * scale)) / 2, idle, fnt_small, 80, 85, 90, 180);
    }

    /* LIVE indicator */
    if (is_live) {
        int lx = view_x + view_w - (int)(60 * scale);
        int ly = view_y + (int)(8 * scale);
        tx(lx, ly, "LIVE", fnt_small, 80, 255, 130, 230);
        C(80, 255, 130, 180);
        SDL_RenderFillRect(rend, &(SDL_Rect){lx - 6, ly + 2, 4, 4});
    }

    SDL_RenderPresent(rend);
}
