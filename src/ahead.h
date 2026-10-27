/* Rendering ahead: the sound is made by a thread of its own, a buffer ahead
 * of the audio device, which only copies it out. The Miyoo's driver asks for
 * two buffers back to back and then waits; made inside its request, both had
 * to be ready in one buffer's time, which left the sound half the processor
 * it seemed to have. Made ahead, steadily, it has nearly all of it, for one
 * buffer more of delay. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t short_calls, short_frames; /* requests not ready in time (a dropout), and the frames missing */
    uint32_t us_sum, us_max_buf; /* time spent making sound; the slowest piece, scaled to a buffer */
} AheadStats;

/* start the thread: render(out, frames) is called in pieces of `chunk`
 * frames, keeping `ahead` frames ready. rt asks for real-time priority. */
int ahead_start(void (*render)(int16_t *, int), int rate, int chunk, int ahead, int rt);
void ahead_stop(void);
int ahead_rt(void); /* the thread got real-time priority */
/* copy `frames` stereo frames out, waiting briefly for them if they are
 * still being made; whatever is missing is silence */
void ahead_read(int16_t *out, int frames);
int ahead_ready(void); /* frames made and waiting */
/* hold the sound still while its settings change (the thread's lock) */
void ahead_lock(void);
void ahead_unlock(void);
/* take the counters, and reset them */
void ahead_take_stats(AheadStats *s);
