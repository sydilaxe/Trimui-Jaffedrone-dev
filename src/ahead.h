/* Rendering ahead: the sound is made by a thread of its own, a buffer ahead
 * of the audio device, which only copies it out. The Miyoo's driver asks for
 * two buffers back to back and then waits; made inside its request, both had
 * to be ready in one buffer's time, which left the sound half the processor
 * it seemed to have. Made ahead, steadily, it has nearly all of it, for one
 * buffer more of delay. On a device with cores to spare (the TrimUI Brick's
 * four), the voices and the effects are made on two cores (ahead_start_split). */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t short_calls, short_frames; /* requests not ready in time (a dropout), and the frames missing */
    uint32_t us_sum, us_max_buf; /* time spent making sound; the slowest piece, scaled to a buffer */
    uint32_t us_voices, us_effects; /* split: each stage's time (us_sum is the busier one) */
} AheadStats;

/* start the thread: render(out, frames) is called in pieces of `chunk`
 * frames, keeping `ahead` frames ready. rt asks for real-time priority. */
int ahead_start(void (*render)(int16_t *, int), int rate, int chunk, int ahead, int rt);
/* the same, in two stages on two threads: engine_voices and engine_effects
 * (see engine.h), a block apart. Each thread is pinned to its core (cv, ce;
 * -1 leaves it free). The sound is the same as engine_render's in the same
 * pieces. */
int ahead_start_split(int rate, int chunk, int ahead, int rt, int cv, int ce);
int ahead_split(void); /* running in two stages */
void ahead_stop(void);
int ahead_rt(void); /* the thread got real-time priority */
/* copy `frames` stereo frames out, waiting briefly for them if they are
 * still being made; whatever is missing is silence. Returns the frames
 * that were there. */
int ahead_read(int16_t *out, int frames);
int ahead_ready(void); /* frames made and waiting */
/* hold the sound still while its settings change (the thread's lock) */
void ahead_lock(void);
void ahead_unlock(void);
/* take the counters, and reset them */
void ahead_take_stats(AheadStats *s);
