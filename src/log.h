/* Jaffedrone log: a plain-text record for tuning the engine, following how
 * the instrument is played, and spotting audio problems.
 *
 *   BASIC   events (presses and what they did, parameter changes, pages,
 *           chords, hand playing) and a status line every 10 s
 *   DETAIL  the same, a status line every second, and each voice's pitch,
 *           breath and correction
 *
 * Status lines carry the audio callback's load and lateness, levels,
 * clipping and the leveller; HINT lines suggest what to change. At the end
 * of a session a summary of presses, pages and adjustments is written.
 * The file rotates to <path>.1 past LOG_MAX_BYTES. Nothing here runs in the
 * audio thread: the engine only fills counters (see EngineStats). */
#pragma once
#include <stdint.h>

enum { LOG_OFF, LOG_BASIC, LOG_DETAIL, NLOGLEVEL };
#define LOG_MAX_BYTES (2 * 1024 * 1024)

/* the audio callback's timing, kept by the platform */
typedef struct {
    uint32_t calls, late;    /* late: the device ran out of audio (a dropout) */
    uint32_t us_max, us_sum; /* time spent rendering */
    uint32_t ui_frames, ui_ms_sum, ui_ms_max; /* the screen: frames drawn and their time */
    uint32_t gap_max_us;     /* the longest wait between two callbacks */
    uint32_t buffer_us;      /* one buffer's worth of time */
    int rt;                  /* the audio thread runs at real-time priority */
} AudioTiming;

void jdlog_open(const char *path, const char *cfg_path); /* restores the level */
void jdlog_close(void);
int jdlog_level(void);     /* the level in effect (OFF without a file) */
int jdlog_level_raw(void); /* the level chosen */
void jdlog_set_level(int level); /* saved to the cfg file */
const char *jdlog_level_name(int level);
void jdlog(const char *fmt, ...); /* a timestamped event line */
/* call often from the main thread: writes status lines when they are due */
void jdlog_tick(uint64_t now_ms, AudioTiming *t);
void jdlog_set_clock(uint64_t now_ms); /* the time events are stamped with */
