/* TrimUI Brick, Brick Hammer and Brick Pro front-end: SDL2 as shipped with
 * TrimUI OS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768
 * screen). The 640x480 pages are drawn as on the Miyoo and scaled 1.6x to
 * fill the screen.
 *
 * The four cores: the voices and the effects are made on cores 2 and 3, a
 * block apart (ahead.c); the screen and the system have cores 0 and 1, and
 * MIDI runs on core 1. Memory is locked, so the sound never waits for a page
 * to be read back in.
 *
 * Built twice: JD_BRICKPRO adds the Brick Pro's two analog sticks. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "ahead.h"
#include "engine.h"
#include "gfx.h"
#include "log.h"
#include "midi.h"
#include "sync.h"
#include "ui.h"

#ifdef JD_BRICKPRO
#define DEVICE_NAME "TrimUI Brick Pro"
#else
#define DEVICE_NAME "TrimUI Brick"
#endif

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

#define SAVE_PATH "jaffedrone.sav"

static volatile sig_atomic_t got_signal;
static void on_signal(int s) { got_signal = s; }

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

/* ------------------------------------------------------------- audio */

static SDL_AudioDeviceID dev;
static char dev_name[128]; /* "" is the system's own output */
static int rate, bufsz;
static void lock(void) { SDL_LockAudioDevice(dev); }
static void unlock(void) { SDL_UnlockAudioDevice(dev); }

/* the audio callback's timing for the log, as on the Miyoo */
static AudioTiming timing;
static uint64_t clock0_us, made_us;
static int use_ahead;

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    uint64_t t0 = now_us();
    static uint64_t prev_t0;
    if (prev_t0 && t0 - prev_t0 > timing.gap_max_us)
        timing.gap_max_us = (uint32_t)(t0 - prev_t0);
    prev_t0 = t0;
    if (!clock0_us)
        clock0_us = t0;
    else if (timing.buffer_us) {
        uint64_t real = t0 - clock0_us;
        if (real > made_us + 3ULL * timing.buffer_us) {
            timing.late++;
            clock0_us = t0 - made_us;
        }
    }
    made_us += timing.buffer_us;
    timing.calls++;
    if (use_ahead) {
        ahead_read((int16_t *)stream, len / 4);
        return;
    }
    engine_render((int16_t *)stream, len / 4);
    uint32_t spent = (uint32_t)(now_us() - t0);
    timing.us_sum += spent;
    if (spent > timing.us_max)
        timing.us_max = spent;
}

static int is_usb_name(const char *n)
{
    return n && (strcasestr(n, "usb") || strcasestr(n, "uac") || strcasestr(n, "dac"));
}

/* JD_AUDIO picks the output: "auto" (a USB device when one is plugged in,
 * else the speaker and headphones), "internal", or part of a device's name */
static const char *pick_output(void)
{
    const char *want = getenv("JD_AUDIO");
    if (want && !strcasecmp(want, "internal"))
        return NULL;
    int n = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (!name)
            continue;
        if (want && *want && strcasecmp(want, "auto") ? strcasestr(name, want) != NULL : is_usb_name(name))
            return name;
    }
    return NULL;
}

/* the device opens paused; start: let it run at once (a device that comes
 * later, while the sound is already being made) */
static int open_output(const char *name, int start)
{
    if (dev) {
        SDL_CloseAudioDevice(dev);
        dev = 0;
    }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)bufsz;
    want.callback = audio_cb;
    /* the engine always makes 16-bit stereo at its own rate: SDL converts
     * for a device that wants something else */
    dev = SDL_OpenAudioDevice(name, 0, &want, &have, 0);
    if (!dev && name) {
        jdlog("audio: could not open %s (%s), using the speaker", name, SDL_GetError());
        name = NULL;
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
    if (!dev) {
        fprintf(stderr, "jaffedrone: audio: %s\n", SDL_GetError());
        return 0;
    }
    snprintf(dev_name, sizeof dev_name, "%s", name ? name : "");
    clock0_us = made_us = 0;
    jdlog("audio out: %s, %d Hz, %d frames a buffer", name ? name : "speaker / headphones", rate, bufsz);
    if (start)
        SDL_PauseAudioDevice(dev, 0);
    return 1;
}

/* a USB device came or went: follow it (unless JD_AUDIO names one) */
static void audio_hotplug(void)
{
    const char *want = getenv("JD_AUDIO");
    if (want && *want && strcasecmp(want, "auto"))
        return;
    const char *name = pick_output();
    if (!strcmp(name ? name : "", dev_name))
        return;
    open_output(name, 1);
    char msg[48];
    snprintf(msg, sizeof msg, "AUDIO: %.38s", name ? name : "SPEAKER");
    ui_notice(msg);
}

/* ------------------------------------------------------------- input */

/* the TrimUI's buttons, as SDL sees them (TrimUI OS, NextUI and MinUI
 * agree): the d-pad is hat 0, L2 and R2 are axes 2 and 5 */
enum { JB_B = 0, JB_A = 1, JB_Y = 2, JB_X = 3, JB_L1 = 4, JB_R1 = 5, JB_SELECT = 6, JB_START = 7, JB_MENU = 8,
       JB_L3 = 9, JB_R3 = 10 };
static int joy_map[32];

static void joy_map_default(void)
{
    for (int i = 0; i < 32; i++)
        joy_map[i] = -1;
    joy_map[JB_B] = BTN_B, joy_map[JB_A] = BTN_A, joy_map[JB_Y] = BTN_Y, joy_map[JB_X] = BTN_X;
    joy_map[JB_L1] = BTN_L1, joy_map[JB_R1] = BTN_R1;
    joy_map[JB_SELECT] = BTN_SELECT, joy_map[JB_START] = BTN_START, joy_map[JB_MENU] = BTN_MENU;
    /* JD_JOYMAP=A=1,B=0,...: another firmware's numbering */
    const char *m = getenv("JD_JOYMAP");
    static const char *const names[BTN_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y",
                                                  "L1", "R1", "L2", "R2", "SELECT", "START", "MENU"};
    while (m && *m) {
        char key[16];
        int num;
        if (sscanf(m, "%15[^=]=%d", key, &num) == 2 && num >= 0 && num < 32)
            for (int b = 0; b < BTN_COUNT; b++)
                if (!strcasecmp(key, names[b])) {
                    for (int i = 0; i < 32; i++)
                        if (joy_map[i] == b)
                            joy_map[i] = -1;
                    joy_map[num] = b;
                }
        m = strchr(m, ',');
        if (m)
            m++;
    }
}

static int map_key(SDL_Keycode k)
{
    switch (k) { /* a keyboard, for trying it on a computer */
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_SPACE: return BTN_A;
    case SDLK_LCTRL: return BTN_B;
    case SDLK_LSHIFT: return BTN_X;
    case SDLK_LALT: return BTN_Y;
    case SDLK_e: return BTN_L1;
    case SDLK_t: return BTN_R1;
    case SDLK_TAB: return BTN_L2;
    case SDLK_BACKSPACE: return BTN_R2;
    case SDLK_RCTRL: return BTN_SELECT;
    case SDLK_RETURN: return BTN_START;
    case SDLK_ESCAPE: return BTN_MENU;
    default: return -1;
    }
}

static int hat_was;
static void on_hat(int v)
{
    static const int bits[4] = {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT};
    static const int btn[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
    for (int i = 0; i < 4; i++)
        if ((v ^ hat_was) & bits[i])
            ui_button(btn[i], (v & bits[i]) != 0);
    hat_was = v;
}

/* L2 and R2 are axes: pressed past halfway, let go below a quarter */
static int trig_down[2];
static void on_trigger(int which, int value)
{
    int down = trig_down[which] ? value > 8192 : value > 16384;
    if (down != trig_down[which]) {
        trig_down[which] = down;
        ui_button(which ? BTN_R2 : BTN_L2, down);
    }
}

#ifdef JD_BRICKPRO
/* The Brick Pro's sticks: live expression, springing back when let go.
 *   left stick   up: bow the selected string by hand, harder the further
 *                it goes (a wind: blow); left / right: bend it, up to two
 *                semitones (JD_BEND sets the range)
 *   right stick  left / right: TONE darker / brighter, two octaves either
 *                way; up / down: more / less reverb
 *   L3           pluck the selected string;  R3  the next chord
 * Axes (as on the TrimUI Smart Pro): 0, 1 left x, y; 3, 4 right x, y. */
#define AXIS_LX 0
#define AXIS_LY 1
#define AXIS_RX 3
#define AXIS_RY 4
static float stick[5]; /* -1 .. 1, centre dead zone removed */
static int stick_bowing = -1, stick_bent = -1;
static float bend_range = 2.f;

static float deadzone(int v)
{
    float x = v / 32767.f, dz = 0.12f;
    if (x > -dz && x < dz)
        return 0.f;
    x = x > 0.f ? (x - dz) / (1.f - dz) : (x + dz) / (1.f - dz);
    return x > 1.f ? 1.f : x < -1.f ? -1.f : x;
}

static void sticks_apply(void)
{
    int s = ui_selected_string();
    float up = -stick[AXIS_LY];
    ui_lock();
    /* bowing: from a light touch (past the dead zone) to as hard as R1 and a little over */
    if (up > 0.f) {
        if (stick_bowing != s) {
            if (stick_bowing >= 0)
                engine_bow(stick_bowing, 0), engine_bow_pressure(stick_bowing, 1.f);
            stick_bowing = s;
            engine_bow(s, 1);
        }
        engine_bow_pressure(s, 0.15f + 1.05f * up * up);
    }
    else if (stick_bowing >= 0) {
        engine_bow(stick_bowing, 0);
        engine_bow_pressure(stick_bowing, 1.f);
        stick_bowing = -1;
    }
    if (stick_bent >= 0 && stick_bent != s)
        engine_express_bend(stick_bent, 0.f);
    engine_express_bend(s, stick[AXIS_LX] * bend_range);
    stick_bent = s;
    engine_express_tone(stick[AXIS_RX] * 2.f);
    engine_express_space(-stick[AXIS_RY]);
    ui_unlock();
}
#endif

static void open_joysticks(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickOpen(i))
            jdlog("controller %d: %s, %d buttons, %d axes, %d hats", i, SDL_JoystickNameForIndex(i),
                  SDL_JoystickNumButtons(SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(i))),
                  SDL_JoystickNumAxes(SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(i))),
                  SDL_JoystickNumHats(SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(i))));
}

static void on_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (!e->key.repeat)
            ui_button(map_key(e->key.keysym.sym), e->type == SDL_KEYDOWN);
        break;
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP: {
        int b = e->jbutton.button, down = e->type == SDL_JOYBUTTONDOWN;
        int m = b < 32 ? joy_map[b] : -1;
        if (m >= 0)
            ui_button(m, down);
#ifdef JD_BRICKPRO
        else if (b == JB_L3 && down) {
            ui_lock();
            engine_pluck(ui_selected_string());
            ui_unlock();
        }
        else if (b == JB_R3 && down)
            ui_button(BTN_SELECT, 1), ui_button(BTN_SELECT, 0);
#endif
        else if (down) /* worth knowing on a new firmware */
            jdlog("button %d (not used)", b);
        break;
    }
    case SDL_JOYHATMOTION:
        if (e->jhat.hat == 0)
            on_hat(e->jhat.value);
        break;
    case SDL_JOYAXISMOTION:
        if (e->jaxis.axis == 2)
            on_trigger(0, e->jaxis.value);
        else if (e->jaxis.axis == 5)
            on_trigger(1, e->jaxis.value);
#ifdef JD_BRICKPRO
        else if (e->jaxis.axis < 5) {
            stick[e->jaxis.axis] = deadzone(e->jaxis.value);
            sticks_apply();
        }
#endif
        break;
    case SDL_JOYDEVICEADDED:
        open_joysticks();
        break;
    case SDL_AUDIODEVICEADDED:
    case SDL_AUDIODEVICEREMOVED:
        if (!e->adevice.iscapture)
            audio_hotplug();
        break;
    case SDL_QUIT:
        got_signal = 1;
        break;
    }
}

/* ------------------------------------------------------------- cores */

static int ncores(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }

static void pin_self(int lo, int hi)
{
    cpu_set_t cs;
    CPU_ZERO(&cs);
    for (int c = lo; c <= hi; c++)
        CPU_SET(c, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fprintf(stderr, "jaffedrone v%s for %s\n", JD_VERSION, DEVICE_NAME);
    jdlog_set_clock(now_us() / 1000);
    jdlog_open("jaffedrone_log.txt", "jaffedrone.cfg");
    song_default();
    if (song_load(SAVE_PATH))
        fprintf(stderr, "jaffedrone: restored %s\n", SAVE_PATH);

    rate = env_int("JD_RATE", 48000);
    bufsz = env_int("JD_BUFFER", 512); /* the A53s have the time: half the Miyoo's delay */
    int cores = ncores();
    /* JD_SPLIT=0: one thread makes all the sound, as on the Miyoo */
    int split = env_int("JD_SPLIT", 1) && cores >= 2;
    int pin = env_int("JD_PIN", 1) && cores >= 4;

    /* 1 GB: the whole program stays in memory, so the sound never waits for
     * a page (the engine's buffers are a few megabytes) */
    if (env_int("JD_MLOCK", 1) && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "jaffedrone: could not lock memory\n");

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, getenv("JD_SCALE") ? getenv("JD_SCALE") : "linear");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "jaffedrone: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    SDL_DisplayMode dm;
    int sw = 1024, sh = 768;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0)
        sw = dm.w, sh = dm.h;
    SDL_Window *win = SDL_CreateWindow("Jaffedrone", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, sw, sh,
                                       SDL_WINDOW_SHOWN | (env_int("JD_WINDOWED", 0) ? 0 : SDL_WINDOW_FULLSCREEN));
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (win && !ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                               SCREEN_W, SCREEN_H) : NULL;
    if (!tex) {
        fprintf(stderr, "jaffedrone: video: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(ren, &ri);
        SDL_GetRendererOutputSize(ren, &sw, &sh);
        jdlog("%s: screen %dx%d, drawn by %s, %d cores", DEVICE_NAME, sw, sh, ri.name, cores);
    }
    /* JD_ROTATE=90/180/270, for a firmware whose screen is turned */
    int rot = env_int("JD_ROTATE", 0);
    int side = rot == 90 || rot == 270;
    float scale = side ? fminf((float)sw / SCREEN_H, (float)sh / SCREEN_W) : fminf((float)sw / SCREEN_W, (float)sh / SCREEN_H);
    SDL_Rect dst = {0, 0, (int)(SCREEN_W * scale + 0.5f), (int)(SCREEN_H * scale + 0.5f)};
    dst.x = (sw - dst.w) / 2, dst.y = (sh - dst.h) / 2;

    joy_map_default();
    open_joysticks();
#ifdef JD_BRICKPRO
    bend_range = (float)env_int("JD_BEND", 2);
#endif

    engine_init(rate);
    engine_set_playing(1);

    /* JD_AHEAD: frames made ahead of the device (0: made in its request);
     * JD_CHUNK: the pieces they are made in */
    int ahead = env_int("JD_AHEAD", bufsz);
    int chunk = env_int("JD_CHUNK", 128);
    int have_audio = open_output(pick_output(), 0);
    if (have_audio && ahead > 0) {
        if (split)
            use_ahead = ahead_start_split(rate, chunk, ahead, 1, pin ? 2 : -1, pin ? 3 : -1);
        if (!use_ahead)
            use_ahead = ahead_start(engine_render, rate, chunk, ahead, 1);
        /* the first buffer made before the device asks for it */
        for (int i = 0; use_ahead && i < 100 && ahead_ready() < ahead; i++)
            SDL_Delay(2);
    }
    if (have_audio) {
        timing.buffer_us = (uint32_t)(1000000.0 * bufsz / rate);
        jdlog("sound made %s; voices %s %s %s %s %s, scale %s",
              !use_ahead ? "in the device's request"
              : ahead_split() ? (pin ? "on two cores (voices on 2, effects on 3)" : "on two threads")
                              : "ahead by its own thread",
              engine_voice_name(song.sp[0][SP_VOICE]), engine_voice_name(song.sp[1][SP_VOICE]),
              engine_voice_name(song.sp[2][SP_VOICE]), engine_voice_name(song.sp[3][SP_VOICE]),
              engine_voice_name(song.sp[4][SP_VOICE]), engine_scale_name(song.gp[GP_SCALE]));
        ui_lock = use_ahead ? ahead_lock : lock;
        ui_unlock = use_ahead ? ahead_unlock : unlock;
        timing.rt = use_ahead ? ahead_rt() : 0;
        SDL_PauseAudioDevice(dev, 0);
    }
    else
        fprintf(stderr, "jaffedrone: no audio: %s\n", SDL_GetError());
    /* the screen and the system keep cores 0 and 1, below the sound */
    if (pin)
        pin_self(0, 1);
    if (setpriority(PRIO_PROCESS, 0, 10) != 0)
        fprintf(stderr, "jaffedrone: could not lower the screen's priority\n");

    ui_init();
    sync_init();
    /* the clock out leaves as the sound is heard: the engine runs the render
     * ahead and the device's buffer ahead of it */
    midi_set_latency((float)((use_ahead ? ahead : 0) + bufsz) / rate);
    if (env_int("JD_MIDI", 1))
        midi_start(pin ? 1 : -1);
    Uint32 last = SDL_GetTicks(), last_save = last, next_draw = last;
    static uint32_t fb[SCREEN_W * SCREEN_H];
    while (!ui_quit_requested() && !got_signal) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            on_event(&e);
        char note[48];
        while (midi_take_notice(note, sizeof note))
            ui_notice(note);
        Uint32 now = SDL_GetTicks();
        ui_tick((int)(now - last));
        last = now;
        if ((Sint32)(now - next_draw) < 0) {
            SDL_Delay(2);
            continue;
        }
        sync_poll(now_us());
        if (use_ahead) {
            AheadStats as;
            ahead_take_stats(&as);
            timing.us_sum += as.us_sum;
            if (as.us_max_buf > timing.us_max)
                timing.us_max = as.us_max_buf;
            timing.late += as.short_calls;
        }
        jdlog_tick(now_us() / 1000, &timing);

        ui_draw(fb);
        SDL_UpdateTexture(tex, NULL, fb, SCREEN_W * 4);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopyEx(ren, tex, NULL, &dst, rot, NULL, SDL_FLIP_NONE);
        SDL_RenderPresent(ren);

        if (now - last_save > 15000) {
            last_save = now;
            if (ui_take_changed())
                song_save(SAVE_PATH);
        }
        Uint32 spent = SDL_GetTicks() - now;
        timing.ui_frames++;
        timing.ui_ms_sum += spent;
        if (spent > timing.ui_ms_max)
            timing.ui_ms_max = spent;
        /* 30 frames a second: the screen has cores of its own here */
        Uint32 frame = 33;
        next_draw = now + (spent + 2 < frame ? frame : spent + 2);
        SDL_Delay(2);
    }

    song_save(SAVE_PATH);
    jdlog_set_clock(now_us() / 1000);
    midi_stop();
    jdlog_close();
    sync_close();
    if (dev)
        SDL_CloseAudioDevice(dev);
    ahead_stop();
    SDL_Quit();
    return 0;
}
