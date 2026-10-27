#include "log.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "sync.h"
#include "ui.h"

static FILE *f;
static char path_[512], cfg_[512];
static int level = LOG_BASIC;
static uint64_t t_now, t_start, t_last_stat;
static long written;
static const char *const level_names[NLOGLEVEL] = {"OFF", "BASIC", "DETAIL"};

const char *jdlog_level_name(int l) { return level_names[l]; }
int jdlog_level(void) { return f ? level : LOG_OFF; }
int jdlog_level_raw(void) { return level; }
void jdlog_set_clock(uint64_t now_ms) { t_now = now_ms; }

static void rotate(void)
{
    if (!f || written < LOG_MAX_BYTES)
        return;
    char old[520];
    fclose(f);
    snprintf(old, sizeof old, "%s.1", path_);
    rename(path_, old);
    f = fopen(path_, "w");
    written = 0;
}

static void line(const char *tag, const char *fmt, va_list ap)
{
    if (!f || level == LOG_OFF)
        return;
    double t = (t_now - t_start) / 1000.0;
    written += fprintf(f, "%9.3f %-5s ", t, tag);
    written += vfprintf(f, fmt, ap);
    written += fprintf(f, "\n");
    rotate();
}

static void put(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line(tag, fmt, ap);
    va_end(ap);
}

void jdlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line("EVENT", fmt, ap);
    va_end(ap);
}

void jdlog_open(const char *path, const char *cfg_path)
{
    snprintf(path_, sizeof path_, "%s", path);
    snprintf(cfg_, sizeof cfg_, "%s", cfg_path ? cfg_path : "");
    if (cfg_[0]) {
        FILE *c = fopen(cfg_, "r");
        if (c) {
            int l;
            if (fscanf(c, "log=%d", &l) == 1 && l >= 0 && l < NLOGLEVEL)
                level = l;
            fclose(c);
        }
    }
    f = fopen(path_, "a");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    written = ftell(f);
    t_start = t_last_stat = t_now;
    put("START", "jaffedrone v%s, log %s", JD_VERSION, level_names[level]);
    fflush(f);
}

void jdlog_set_level(int l)
{
    if (l < 0 || l >= NLOGLEVEL)
        return;
    if (f && level != LOG_OFF && l == LOG_OFF)
        put("LOG", "off");
    level = l;
    if (f && l != LOG_OFF)
        put("LOG", "%s", level_names[l]);
    if (cfg_[0]) {
        FILE *c = fopen(cfg_, "w");
        if (c) {
            fprintf(c, "log=%d\n", l);
            fclose(c);
        }
    }
    if (f)
        fflush(f);
}

/* a number from a system file, or -1 (the processor's clock, temperature) */
static long read_num(const char *path)
{
    FILE *p = fopen(path, "r");
    long v = -1;
    if (p) {
        if (fscanf(p, "%ld", &v) != 1)
            v = -1;
        fclose(p);
    }
    return v;
}

/* the system's state: the processor's clock and temperature, the cores
 * running. A clock or core count that drops mid-session explains dropouts
 * that load alone does not. */
static void sys_state(char *out, int n)
{
    long khz = read_num("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    long temp = read_num("/sys/class/thermal/thermal_zone0/temp");
    char cores[16] = "?";
    FILE *p = fopen("/sys/devices/system/cpu/online", "r");
    if (p) {
        if (!fgets(cores, sizeof cores, p))
            strcpy(cores, "?");
        cores[strcspn(cores, "\n")] = 0;
        fclose(p);
    }
    if (temp > 1000)
        temp /= 1000; /* some kernels give millidegrees */
    snprintf(out, n, "cpu %ld MHz %ld C cores %s", khz > 0 ? khz / 1000 : -1, temp, cores);
}

/* status: audio load and lateness, levels, the leveller, voices; then hints */
static void status(AudioTiming *t, double secs)
{
    EngineStats s = stats; /* a snapshot; counters keep running */
    engine_stats_reset();
    double load = t && t->calls && t->buffer_us ? 100.0 * t->us_sum / t->calls / t->buffer_us : 0;
    double peak_load = t && t->buffer_us ? 100.0 * t->us_max / t->buffer_us : 0;
    double rms = s.frames ? sqrt(s.rms_acc / s.frames) : 0, lev = s.frames ? s.lev_acc / s.frames : 0;
    const SyncStatus *ss = sync_status();
    double ui_avg = t && t->ui_frames ? (double)t->ui_ms_sum / t->ui_frames : 0;
    char sys[64];
    sys_state(sys, sizeof sys);
    put("STAT", "load avg %.0f%% peak %.0f%% late %u gap max %.0f ms calls %.1f/s of %.1f | peak %.2f rms %.3f clips %u | leveller avg %.1f "
                "min %.1f | screen avg %.0f ms max %u ms | audio %s | %s | sync %s",
        load, peak_load, t ? t->late : 0, t ? t->gap_max_us / 1000.0 : 0, t && secs > 0 ? t->calls / secs : 0,
        t && t->buffer_us ? 1e6 / t->buffer_us : 0, s.peak, rms, s.clips, lev, s.lev_min, ui_avg,
        t ? t->ui_ms_max : 0, t && t->rt ? "realtime" : "normal", sys,
        ss->leading ? "lead" : ss->have_leader ? "follow" : "-");
    if (level == LOG_DETAIL)
        for (int v = 0; v < NS; v++) {
            if (!song.on[v] || s.f_target[v] <= 0.f)
                continue;
            s.f_meas[v] = engine_voice_pitch(v);
            double cents = s.f_meas[v] > 0.f ? 1200.0 * log2(s.f_meas[v] / s.f_target[v]) : 0;
            put("VOICE", "%d %s target %.1f Hz measured %.1f Hz (%+.0f c) meter %.3f breath %.2f corr %.3f", v + 1,
                engine_voice_name(song.sp[v][SP_VOICE]), s.f_target[v], s.f_meas[v], cents, s.meter[v],
                s.breath[v], s.corr[v]);
            if (s.f_meas[v] > 0.f && fabs(cents) > 30 && s.meter[v] > 0.02f)
                put("HINT", "voice %d sounds %+.0f cents off its note: try another REG, BREATH or EMBOUCH", v + 1, cents);
        }
    /* what to change */
    if (load > 60)
        put("HINT", "audio load %.0f%%: raise JD_BUFFER to 2048 or set JD_RATE=44100 in launch.sh", load);
    if (t && t->late)
        put("HINT", "%u audio dropouts (crackles): raise JD_BUFFER, or fewer sympathetic strings%s", t->late,
            t->ui_ms_max > 33 ? "; the screen is slow too (it slows to 20 frames a second under load)" : "");
    if (s.clips)
        put("HINT", "%u samples near full scale: MASTER above 80 drives the clipper; lower it for a cleaner sound", s.clips);
    int idle = song.gp[GP_PLAYMODE] == PLAY_LATCH; /* LATCH with every string let go is meant to be quiet */
    for (int v = 0; v < NS && idle; v++)
        idle = !live.latch[v];
    if (lev > 27.f && !idle && live.playing)
        put("HINT", "the sound is very quiet and is being lifted fully: raise LEVEL, BOW or BREATH, or TONE");
    if (t)
        memset(t, 0, offsetof(AudioTiming, buffer_us)); /* the counters; the buffer and rt stay */
    fflush(f);
}

void jdlog_tick(uint64_t now_ms, AudioTiming *t)
{
    t_now = now_ms;
    stats.detail = f && level == LOG_DETAIL;
    if (!f || level == LOG_OFF)
        return;
    uint64_t every = level == LOG_DETAIL ? 1000 : 10000;
    if (now_ms - t_last_stat >= every) {
        status(t, (now_ms - t_last_stat) / 1000.0);
        t_last_stat = now_ms;
    }
}

static void summary_line(const char *s) { put("SUM", "%s", s); }

void jdlog_close(void)
{
    if (!f)
        return;
    if (level != LOG_OFF) {
        put("END", "session %.0f s", (t_now - t_start) / 1000.0);
        ui_summary(summary_line);
    }
    fclose(f);
    f = NULL;
    stats.detail = 0;
}
