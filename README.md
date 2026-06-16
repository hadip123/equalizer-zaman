# Equalizer Zaman

> **AI-generated project** — This entire codebase was written collaboratively with an AI coding assistant (opencode / deepseek-v4). The architecture, implementation, and documentation were produced through iterative human–AI pair programming.



A retro-style audio spectrum equalizer that reacts to system audio and doubles as a fullscreen timer overlay — Pomodoro, countdown timer, and chronometer with lap recording.

Originally prototyped as an HTML/CSS/JS canvas app, then rewritten in C/SDL2 for native performance. Integrated into the `zaman` timer application as a desktop background.

## Screenshot

```
  ▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌
  ▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▐▌
  ▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▌▐▐▌
  ┌─────────────────────────────────────┐
  │  POMODORO    TIMER    CHRONO        │
  │            ─────────────────         │
  │                                     │
  │              25:00                  │
  │           ████████░░░░             │
  │                                     │
  │  WORK                  POMO 1/4    │
  │  SPACE: Start/Pause   R: Reset      │
  └─────────────────────────────────────┘
  ─── ♫ Artist – Title (Album) ───
```

## Architecture

```
equalizer-zaman/
├── src/
│   ├── main.c              Entry point, event loop, timer logic
│   ├── audio.c / audio.h   PulseAudio capture + FFT
│   ├── renderer.c / .h     SDL2 window, scaling, text, bars
│   └── mpris.c / mpris.h   D-Bus MPRIS track metadata
├── assets/
│   ├── DejaVuSansMono.ttf        UI font (regular)
│   └── DejaVuSansMono-Bold.ttf   UI font (bold, timer digits)
├── Makefile
├── .gitignore
└── README.md
```

### Modules

#### `audio` — PulseAudio capture + FFT

- Opens the PulseAudio monitor source (system audio loopback) using `pa_simple_new`.
- Captures 16-bit PCM samples at 44.1 kHz in a 1024-sample ring buffer.
- A worker thread applies a Hann window and runs an FFT every ~23ms.
- Magnitudes are summed into 64 log-spaced frequency bins and normalized to 0–1.
- Output: `float bars[64]` updated atomically via a spin-lock-free ring.

#### `renderer` — SDL2 display

- Creates a resizable window with a fixed **960×540 logical viewport**.
- The viewport is **letterboxed** (centered with black bars) to maintain aspect ratio.
- **Bars**: rendered to an offscreen texture at logical size, then copied to screen with **linear filtering** (smooth gradients).
- **Text**: rendered directly at native resolution using **SDL2_ttf** with font sizes scaled by the viewport ratio — no blur, no double-scaling.
- **Blur effect**: the overlay panel region is copied from the bar texture into a 5× downscaled target, then upscaled back to screen (cheap frosted-glass).
- **Now-playing bar**: dark bar at the bottom with `Artist – Title (Album)` from MPRIS.

#### `mpris` — D-Bus MPRIS metadata

- Connects to the session D-Bus and listens for `org.mpris.MediaPlayer2.Player` signals.
- Queries `Metadata` for `xesam:title`, `xesam:artist`, `xesam:album`.
- Updates atomically via a spin lock.

#### `main` — Event loop & timer state machine

- **Pomodoro**: 25 min work / 5 min short break / 15 min long break, 4-cycle counter.
- **Timer**: countdown with +/− minute/second adjustment, insert mode (press I, type digits right-to-left).
- **Chronometer**: stopwatch with lap recording (L key).
- Max duration: 24 hours.
- **End-time estimate**: when a timer is running, "ends at HH:MM AM/PM" is shown below the progress bar.
- Keyboard: `Space` start/pause, `R` reset, `H`/`L` cycle mode, `I` insert mode, `Q`/`Escape` quit.

## Dependencies

| Package       | Purpose                          |
|---------------|----------------------------------|
| SDL2          | Windowing, input, rendering      |
| SDL2_ttf      | TTF font rendering               |
| PulseAudio    | System audio capture (monitor)   |
| dbus-1        | MPRIS media player metadata      |
| libm          | FFT math, `sqrtf`, `sinf`        |

### Install (Debian/Ubuntu)

```sh
sudo apt install libsdl2-dev libsdl2-ttf-dev libpulse-dev libdbus-1-dev build-essential
```

### Build

```sh
make
./zaman
```

## Controls

| Key          | Action                            |
|--------------|-----------------------------------|
| `Space`      | Start / Pause timer               |
| `R`          | Reset current timer               |
|              | *(shows estimated end time when running)* |
| `H` / `L`    | Cycle mode (Pomodoro → Timer → Chrono) |
| `+` / `-`    | Adjust minutes (Timer mode)       |
| `Shift+`+/-  | Adjust seconds (Timer mode)       |
| `I`          | Insert mode — type digits (Timer) |
| `Backspace`  | Delete last digit (insert mode)   |
| `Enter`      | Confirm digits (insert mode)      |
| `Esc`        | Cancel insert mode / Quit         |
| `L`          | Record lap (Chronometer mode)     |
| `Q` / `Esc`  | Quit                              |

## Development History

1. **HTML prototype** — Canvas-based equalizer bars with requestAnimationFrame, Web Audio API for mic input.
2. **C/SDL2 rewrite** — SDL2 window, PulseAudio monitor capture, FFT in worker thread.
3. **Timer integration** — Merged with `zaman` timer app as a desktop background.
4. **Blur panel** — Downscale/upscale render targets for frosted-glass overlay effect.
5. **Font system** — Replaced custom 5×7 bitmap font (missing many ASCII characters) with SDL2_ttf + DejaVu Sans Mono.
6. **Native-resolution text** — Removed `SDL_RenderSetLogicalSize` to avoid blurry text; manual viewport scaling keeps bars smooth and text sharp.
7. **Modular structure** — Split into `src/` and `assets/` directories with clean module boundaries.

## Known Issues

- Audio capture uses PulseAudio's `pa_simple` API (blocking reads). The FFT thread may drop frames under heavy load.
- MPRIS detection polls every 2 seconds rather than listening to PropertiesChanged signals (simpler, but not instant).
- The blur effect is a cheap 5× nearest-neighbor downscale, not a proper Gaussian. It creates a frosted-glass look but isn't physically accurate.
