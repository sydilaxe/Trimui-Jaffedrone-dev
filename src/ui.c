/* Jaffedrone UI. The look borrows from Norman Jaffe's houses: long sloping
 * rooflines, chamfered angular masses, cedar and sandstone against slate,
 * one copper edge catching the light. */
#include "ui.h"
#include "logo_data.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "gfx.h"
#include "log.h"
#include "sync.h"

#define C_BG 0x14161b
#define C_PANEL 0x1c1f27
#define C_PANEL2 0x282c37
#define C_LINE 0x3a3f4c
#define C_TEXT 0xf2ebdd
#define C_DIM 0x7d7f8a
#define C_COPPER 0xb3875c
#define C_CEDAR 0x7f6553
#define C_SAND 0xd9c3a0
#define C_SEA 0x7fd1a8
#define C_RED 0xff5a50
#define C_OFF 0x3a3f4c
#define C_CEDARWOOD 0xb26a3e /* the ceiling boards */
#define C_SLATE 0x4a525e      /* the wordmark */
#define C_SLATE_D 0x2c323c
#define HEADER_H 46
#define BODY_Y 58
#define FOOTER_Y 456

static void nop(void) {}
void (*ui_lock)(void) = nop;
void (*ui_unlock)(void) = nop;

static const uint32_t scol[NS] = {0xc9a67c, 0xad7f61, 0xd9c3a0, 0x9fb7a4, 0x7f9aa6};
static const char *const page_names[PAGE_COUNT] = {"PLAY", "SOUND", "COMPOSE", "MIX", "SONG"};
static int page, cur_s;
static int ring;  /* PLAY: the selected ring of the selected string */
static int quiet; /* PLAY: the quiet view, strings and rings only */
static int l2_used, r2_used;
static int bowing = -1; /* the string bowed by hand (R1 held) */
static int latch_on(void) { return song.gp[GP_PLAYMODE] == PLAY_LATCH; }
static void set_playmode(int latch);
static int harm_s = -1; /* LATCH: the string sounding a harmonic while B, or L1 and R1, are held */
static int harm_both;   /* ... started by both shoulders: it ends when either lets go */
static int l1_s; /* the string L1 or L2 plays */
static int start_used;  /* START took part in a page change */
/* some button drivers send a quick release and press while a button is
 * held: a bow survives a gap this short, and a pluck is not repeated */
#define REPEAT_GAP_MS 70
static int bow_release_ms, l1_up_ms = -1000;
static int held[BTN_COUNT], hold_ms[BTN_COUNT], rep_ms[BTN_COUNT], rep_n[BTN_COUNT];
static int quit, changed, now_ms, toast_ms;
static char toast[48];

static int wrap(int x, int n) { return ((x % n) + n) % n; }
static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---------------------------------------------------------------- log
 * What the player does, for the log: every message shown is an event;
 * parameter changes are merged while a button repeats; presses, page time,
 * adjustments and hand playing are counted for the session summary. */
static const char *const btn_names[BTN_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y",
                                                 "L1", "R1", "L2", "R2", "SELECT", "START", "MENU"};
static int presses[BTN_COUNT], page_ms[8], hand_plucks, hand_bow_ms, bow_start_ms, chords_seen;
static uint32_t last_chords;
#define NADJ 48
static struct {
    char name[32];
    int n;
} adj[NADJ];
static int nadj;
static char pend_name[32];
static int pend_from, pend_to, pend_ms = -1;

static void count_adjust(const char *name)
{
    for (int i = 0; i < nadj; i++)
        if (!strcmp(adj[i].name, name)) {
            adj[i].n++;
            return;
        }
    if (nadj < NADJ) {
        snprintf(adj[nadj].name, sizeof adj[nadj].name, "%s", name);
        adj[nadj++].n = 1;
    }
}

static void flush_param(void)
{
    if (pend_ms < 0)
        return;
    jdlog("param %s %d -> %d", pend_name, pend_from, pend_to);
    pend_ms = -1;
}

/* a parameter moved; repeats of the same one merge into one line */
static void log_param(const char *name, int from, int to)
{
    count_adjust(name);
    if (pend_ms >= 0 && strcmp(pend_name, name))
        flush_param();
    if (pend_ms < 0) {
        snprintf(pend_name, sizeof pend_name, "%s", name);
        pend_from = from;
    }
    pend_to = to;
    pend_ms = 0;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast, sizeof toast, fmt, ap);
    va_end(ap);
    toast_ms = 1400;
    jdlog("%s", toast);
}

void ui_summary(void (*out)(const char *line))
{
    char l[160];
    int n = 0;
    flush_param();
    n = snprintf(l, sizeof l, "presses:");
    for (int b = 0; b < BTN_COUNT; b++)
        if (presses[b] && n < (int)sizeof l - 16)
            n += snprintf(l + n, sizeof l - n, " %s %d", btn_names[b], presses[b]);
    out(l);
    n = snprintf(l, sizeof l, "time on pages:");
    for (int p = 0; p < PAGE_COUNT; p++)
        n += snprintf(l + n, sizeof l - n, " %s %.0f s", page_names[p], page_ms[p] / 1000.0);
    out(l);
    /* the five most adjusted */
    n = snprintf(l, sizeof l, "most adjusted:");
    for (int k = 0; k < 5; k++) {
        int best = -1;
        for (int i = 0; i < nadj; i++)
            if (adj[i].n > 0 && (best < 0 || adj[i].n > adj[best].n))
                best = i;
        if (best < 0)
            break;
        n += snprintf(l + n, sizeof l - n, " %s %d", adj[best].name, adj[best].n);
        adj[best].n = -adj[best].n; /* taken */
    }
    for (int i = 0; i < nadj; i++)
        adj[i].n = adj[i].n < 0 ? -adj[i].n : adj[i].n;
    out(l);
    snprintf(l, sizeof l, "hand: %d plucks or tongued notes, %.1f s bowed or blown; %d chord changes", hand_plucks,
             hand_bow_ms / 1000.0, chords_seen);
    out(l);
}

static int splash_ms; /* the start-up screen, until a button or this runs out */
void ui_init(void) { splash_ms = 2500; }
int ui_quit_requested(void) { return quit; }
int ui_take_changed(void)
{
    int c = changed;
    changed = 0;
    return c;
}

/* ------------------------------------------------------------- controls */

static void set_scale(int sc)
{
    if (song.gp[GP_KEYLOCK]) {
        say("KEY LOCKED");
        return;
    }
    engine_set_scale(wrap(sc, engine_nscales()));
    say("%s", engine_scale_name(song.gp[GP_SCALE]));
}

/* ---------------------------------------------------------------- cells
 *
 * The SOUND, COMPOSE and MIX pages are laid out as cells, each one
 * parameter; the d-pad moves to the nearest cell in its direction. */
enum { CK_S, CK_G, CK_L, CK_F, CK_SLOT }; /* string, global, LFO, effect, preset slot */
typedef struct {
    short x, y, w, h;
    char kind, a, b;
} Cell;
#define MAXC 64
static Cell cells[MAXC];
static int ncell, sel_cell[PAGE_COUNT];

static void cell(int x, int y, int w, int h, int kind, int a, int b)
{
    if (ncell < MAXC)
        cells[ncell++] = (Cell){(short)x, (short)y, (short)w, (short)h, (char)kind, (char)a, (char)b};
}

/* SOUND: per-string tone above, the shared resonance below */
static const int sound_rows[9] = {SP_NOTE, SP_HARM, SP_BOW, SP_DECAY, SP_BRIGHT, SP_FOLD, SP_STIFF, SP_POS, SP_BUZZ};
#define SOUND_Y 118
#define SOUND_RH 29
#define ENGINE_Y 84
#define BAND_Y (SOUND_Y + 9 * SOUND_RH + 4)
/* COMPOSE: the progression on the left, its controls on the right */
static const int comp_a[7] = {GP_SCALE, GP_ROOT, GP_MODE, GP_CHANGES, GP_CHANCE, GP_GLIDE, GP_BPM};
static const int comp_b[7] = {GP_MOTION, GP_HOME, GP_COLOR, GP_EVOLVE, GP_RELATE, GP_DRIFT, GP_SYNC};
#define COMP_X 268
#define COMP_CW 184
#define LFO_Y 318
/* MIX: a strip per string (with AURA: playing or listening), master, then
 * AURA, delay, reverb and binaural */
#define MIX_FX_Y 276
#define MIX_FXC_Y 344
#define MIX_FXW 160

/* ------------------------------------------------------------- presets
 * Ten slots, each a whole song (sound, rings, tuning, the progression
 * memory) in its own file. What each holds is read back for the tiles. */
static char preset_dir[400] = ".";
static struct {
    int used, scale, winds, percs, bpm, prog_n;
} slot_info[NPRESET];
static int slots_read;

void ui_set_dir(const char *dir) { snprintf(preset_dir, sizeof preset_dir, "%s", dir); slots_read = 0; }

static void preset_path(int i, char *b, int n) { snprintf(b, n, "%s/preset_%02d.jds", preset_dir, i + 1); }

static void read_slots(void)
{
    for (int i = 0; i < NPRESET; i++) {
        char p[512];
        Song t;
        preset_path(i, p, sizeof p);
        slot_info[i].used = song_peek(p, &t);
        if (slot_info[i].used) {
            slot_info[i].scale = t.gp[GP_SCALE];
            slot_info[i].winds = engine_wind_count(&t);
            slot_info[i].percs = engine_perc_count(&t);
            slot_info[i].bpm = t.gp[GP_BPM];
            slot_info[i].prog_n = t.prog_n;
        }
    }
    slots_read = 1;
}

static void preset_save(int i)
{
    char p[512];
    preset_path(i, p, sizeof p);
    if (song_save(p))
        say("SAVED TO PRESET %d", i + 1);
    else
        say("COULD NOT SAVE PRESET %d", i + 1);
    slots_read = 0;
}

static void preset_load(int i)
{
    char p[512];
    preset_path(i, p, sizeof p);
    /* the Wi-Fi and output settings belong to the room, not the preset */
    int keep[5] = {song.gp[GP_SYNC], song.gp[GP_OUTPUT], song.gp[GP_CLKSIDE], song.gp[GP_PPQN], song.gp[GP_PULSE]};
    if (song_load(p)) {
        song.gp[GP_SYNC] = keep[0];
        for (int k = 0; k < 4; k++)
            song.gp[GP_OUTPUT + k] = keep[1 + k];
        engine_set_bpm((float)song.gp[GP_BPM]);
        say("PRESET %d", i + 1);
    }
    else
        say("PRESET %d IS EMPTY", i + 1);
}

static void preset_clear(int i)
{
    char p[512];
    preset_path(i, p, sizeof p);
    remove(p);
    say("CLEARED PRESET %d", i + 1);
    slots_read = 0;
}

#define SLOT_Y 82
#define PROGT_Y 230
#define SONGC_Y 304

static void layout(int pg)
{
    ncell = 0;
    const int cw = SCREEN_W / NS;
    switch (pg) {
    case PAGE_SOUND:
        for (int s = 0; s < NS; s++) /* each voice: a string, or one of the winds */
            cell(s * cw + 3, ENGINE_Y, cw - 6, 30, CK_S, s, SP_VOICE);
        for (int r = 0; r < 9; r++)
            for (int s = 0; s < NS; s++)
                cell(s * cw + 3, SOUND_Y + r * SOUND_RH, cw - 6, SOUND_RH - 3, CK_S, s, sound_rows[r]);
        cell(3, BAND_Y, cw - 6, 32, CK_F, 0, FX_SYMP);
        cell(cw + 3, BAND_Y, cw - 6, 32, CK_F, 0, FX_BODY);
        cell(2 * cw + 3, BAND_Y, cw - 6, 32, CK_F, 0, FX_BTYPE);
        cell(3 * cw + 3, BAND_Y, cw - 6, 32, CK_G, 0, GP_MATERIAL); /* the winds' bore */
        cell(4 * cw + 3, BAND_Y, cw - 6, 32, CK_G, 0, GP_CUTOFF);
        for (int i = 0; i < 5; i++)
            cell(i * cw + 3, BAND_Y + 36, cw - 6, 34, CK_F, 0, FX_SCOUNT + i);
        break;
    case PAGE_COMPOSE:
        for (int r = 0; r < 7; r++) {
            cell(COMP_X, BODY_Y + r * 34, COMP_CW - 6, 31, CK_G, 0, comp_a[r]);
            cell(COMP_X + COMP_CW, BODY_Y + r * 34, COMP_CW - 6, 31, CK_G, 0, comp_b[r]);
        }
        for (int l = 0; l < NLFO; l++)
            for (int p = 0; p < NLP; p++)
                cell(COMP_X + l * COMP_CW, LFO_Y + p * 33, COMP_CW - 6, 30, CK_L, l, p);
        break;
    case PAGE_SONG:
        for (int i = 0; i < NPRESET; i++)
            cell(5 + (i % 5) * 127, SLOT_Y + (i / 5) * 62, 122, 56, CK_SLOT, i, 0);
        {
            static const int rows[3][3] = {{GP_PROG, GP_ATTACK, GP_RELEASE},
                                           {GP_KEYLOCK, GP_ROOT, GP_MODALITY},
                                           {GP_SCALE, GP_CHANGES, GP_PLAYMODE}};
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    cell(3 + c * 213, SONGC_Y + r * 37, 207, 34, CK_G, 0, rows[r][c]);
            /* the output: stereo, or mono + clock pulses for other gear */
            for (int c = 0; c < 4; c++)
                cell(3 + c * 160, SONGC_Y + 3 * 37, 154, 34, CK_G, 0, GP_OUTPUT + c);
        }
        break;
    case PAGE_MIX:
        for (int s = 0; s < NS; s++) {
            cell(s * cw + 3, BODY_Y + 42, cw - 6, 31, CK_S, s, SP_LEVEL);
            cell(s * cw + 3, BODY_Y + 76, cw - 6, 31, CK_S, s, SP_PAN);
            cell(s * cw + 3, BODY_Y + 110, cw - 6, 31, CK_S, s, latch_on() ? SP_RELEASE : SP_GATE);
            cell(s * cw + 3, BODY_Y + 144, cw - 6, 31, CK_S, s, SP_LISTEN);
        }
        cell(3, BODY_Y + 180, cw - 6, 31, CK_G, 0, GP_MASTER);
        cell(cw + 3, BODY_Y + 180, cw - 6, 31, CK_F, 0, FX_SLEVEL);
        cell(2 * cw + 3, BODY_Y + 180, cw - 6, 31, CK_G, 0, GP_SUB);
        cell(3 * cw + 3, BODY_Y + 180, cw - 6, 31, CK_G, 0, GP_SUBDIV);
        cell(4 * cw + 3, BODY_Y + 180, cw - 6, 31, CK_G, 0, GP_SUBRES);
        /* in the order the sound passes through them: AIR (at the instruments),
         * then delay, reverb and binaural */
        cell(4, MIX_FXC_Y, MIX_FXW - 8, 33, CK_F, 0, FX_AIR);
        for (int c = 0; c < 3; c++)
            for (int p = 0; p < 3; p++)
                cell((c + 1) * MIX_FXW + 4, MIX_FXC_Y + p * 36, MIX_FXW - 8, 33, CK_F, 0, c * 3 + p);
        break;
    }
}

static int dummy_val;
static const ParamDef slot_def = {"PRESET", 0, 1};
static int *cell_ref(const Cell *c, const ParamDef **d)
{
    switch (c->kind) {
    case CK_SLOT: *d = &slot_def; return &dummy_val;
    case CK_S: *d = sparam_def(c->b); return &song.sp[(int)c->a][(int)c->b];
    case CK_G: *d = gparam_def(c->b); return &song.gp[(int)c->b];
    case CK_L: *d = lparam_def(c->b); return &song.lfo[(int)c->a][(int)c->b];
    default: *d = fparam_def(c->b); return &song.fx[(int)c->b];
    }
}

static void cell_format(const Cell *c, char *v, int n)
{
    switch (c->kind) {
    case CK_SLOT: v[0] = 0; (void)n; break;
    case CK_S: sparam_format(c->a, c->b, v, n); break;
    case CK_G: gparam_format(c->b, v, n); break;
    case CK_L: lparam_format(c->a, c->b, v, n); break;
    default: fparam_format(c->b, v, n); break;
    }
}

/* the nearest cell from the selected one in a direction */
static void nav(int dx, int dy)
{
    layout(page);
    int cur = sel_cell[page] < ncell ? sel_cell[page] : 0, best = cur;
    float cx = cells[cur].x + cells[cur].w / 2.f, cy = cells[cur].y + cells[cur].h / 2.f, bc = 1e9f;
    for (int i = 0; i < ncell; i++) {
        float ex = cells[i].x + cells[i].w / 2.f - cx, ey = cells[i].y + cells[i].h / 2.f - cy;
        float along = ex * dx + ey * dy, across = fabsf(dx ? ey : ex);
        if (along < 4.f)
            continue;
        float cost = along + 3.f * across;
        if (cost < bc)
            bc = cost, best = i;
    }
    sel_cell[page] = best;
}

static void adjust(int dir, int repeats)
{
    layout(page);
    if (sel_cell[page] >= ncell)
        return;
    const Cell *c = &cells[sel_cell[page]];
    const ParamDef *d;
    int *v = cell_ref(c, &d);
    int step = (repeats > 12 && d->max - d->min > 30) ? 4 : 1;
    int nv = clampi(*v + dir * step, d->min, d->max);
    if (c->kind == CK_S && c->b == SP_VOICE && held[BTN_R2]) {
        /* R2 + Y / X on VOICE: the previous / next family, strings, winds,
         * struck, starting at its first instrument */
        r2_used = 1;
        if (repeats)
            return;
        int fam = *v == 0 ? 0 : *v <= NEXCITE ? 1 : 2;
        fam = (fam + dir + 3) % 3;
        nv = fam == 0 ? 0 : fam == 1 ? 1 : VOICE_PERC(0);
    }
    if (nv == *v)
        return;
    {
        char nm[32];
        if (c->kind == CK_S)
            snprintf(nm, sizeof nm, "%d %s", c->a + 1, sparam_name(c->a, c->b));
        else if (c->kind == CK_L)
            snprintf(nm, sizeof nm, "LFO%d %s", c->a + 1, d->name);
        else
            snprintf(nm, sizeof nm, "%s", d->name);
        log_param(nm, *v, nv);
    }
    if (c->kind == CK_SLOT)
        return;
    if (song.gp[GP_KEYLOCK] && c->kind == CK_G && (c->b == GP_SCALE || c->b == GP_ROOT || c->b == GP_MODALITY)) {
        say("KEY LOCKED");
        return;
    }
    if (c->kind == CK_G && c->b == GP_PROG) {
        engine_prog_mode(nv);
        say("%s", nv == PROG_RECORD ? "RECORDING CHORDS: EACH CHANGE IS KEPT" : nv == PROG_PLAY ? "PLAYING THE KEPT CHORDS"
                                                                                               : "FREE: THE ALGORITHM CHOOSES");
        return;
    }
    if (c->kind == CK_G && c->b == GP_SCALE) {
        set_scale(nv);
        return;
    }
    *v = nv;
    if (c->kind == CK_G && c->b == GP_BPM)
        engine_set_bpm((float)nv);
    if (c->kind == CK_G && c->b == GP_SYNC)
        engine_set_rate(1.0);
    if (c->kind == CK_G && c->b == GP_PLAYMODE) {
        set_playmode(nv == PLAY_LATCH);
        return;
    }
    if (c->kind == CK_S && c->b == SP_VOICE)
        say("VOICE %d: %s", c->a + 1,
            nv == 0 ? "A STRING: BOW AND PLUCK" : nv <= NEXCITE ? "A WIND: BREATH AND TONGUE" : "STRUCK: MALLET AND BOW");
    if (c->kind == CK_G && c->b == GP_MODE)
        say("%s", nv == MODE_MONO ? "MONO: ALL STRINGS FOLLOW STRING 1" : "CHORD: EACH STRING ITS OWN NOTE");
    if (c->kind == CK_G && c->b == GP_OUTPUT)
        say("%s", nv == OUT_MONO_CLOCK ? "MONO AUDIO + CLOCK OUT" : "STEREO OUT");
    if (c->kind == CK_G && c->b == GP_PPQN)
        say("%d PPQN%s", clk_ppqn_value(nv), clk_ppqn_value(nv) == 2 ? ": POCKET OPERATOR / VOLCA" : clk_ppqn_value(nv) == 24 ? ": DIN SYNC" : "");
}

/* the string of the selected cell, or -1 */
static int cell_string(void)
{
    layout(page);
    return sel_cell[page] < ncell && cells[sel_cell[page]].kind == CK_S ? cells[sel_cell[page]].a : -1;
}

/* PLAY: change one field of the selected ring and show the pattern */
static void ring_show(void)
{
    const int *P = song.sp[cur_s];
    static const char *const rn[6] = {"1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BARS"};
    if (!P[RING_P(ring, 0)])
        say("%d %s OFF%s", cur_s + 1, ring ? "BOW" : "PLUCK", ring ? " (BOW HOLDS)" : "");
    else
        say("%d %s E(%d,%d) >%d %s", cur_s + 1, ring ? "BOW" : "PLUCK", P[RING_P(ring, 2)], P[RING_P(ring, 1)],
            P[RING_P(ring, 3)], rn[P[SP_RATE]]);
}

static void ring_edit(int field, int dir)
{
    int *P = song.sp[cur_s];
    if (field == SP_RATE)
        P[SP_RATE] = clampi(P[SP_RATE] + dir, 0, 5);
    else {
        int id = RING_P(ring, field);
        const ParamDef *d = sparam_def(id);
        if (field == 3)
            P[id] = wrap(P[id] + dir, P[RING_P(ring, 1)]);
        else
            P[id] = clampi(P[id] + dir, d->min, field == 2 ? P[RING_P(ring, 1)] : d->max);
        if (field == 1 && P[RING_P(ring, 2)] > P[id])
            P[RING_P(ring, 2)] = P[id];
        P[RING_P(ring, 0)] = 1; /* editing a ring switches it on */
    }
    ring_show();
}

static void play_action(int b)
{
    int l2 = held[BTN_L2], r2 = held[BTN_R2];
    if (l2) {
        l2_used = 1;
        /* as on RadioScan: steps and step length on the d-pad */
        switch (b) {
        case BTN_UP: ring_edit(1, +1); return;
        case BTN_DOWN: ring_edit(1, -1); return;
        case BTN_RIGHT: ring_edit(SP_RATE, -1); return; /* right = faster */
        case BTN_LEFT: ring_edit(SP_RATE, +1); return;
        case BTN_X: ring_edit(2, +1); return; /* more pulses */
        case BTN_Y: ring_edit(3, -1); return; /* rotate back */
        case BTN_A: /* mute or unmute the string (it fades) */
            song.on[cur_s] = !song.on[cur_s];
            say("STRING %d %s", cur_s + 1, song.on[cur_s] ? "ON" : "MUTED");
            return;
        }
    }
    if (r2) {
        r2_used = 1;
        switch (b) {
        case BTN_LEFT: set_scale(song.gp[GP_SCALE] - 1); return;
        case BTN_RIGHT: set_scale(song.gp[GP_SCALE] + 1); return;
        case BTN_UP: song.gp[GP_CUTOFF] = clampi(song.gp[GP_CUTOFF] + 2, 0, 100); say("TONE %d", song.gp[GP_CUTOFF]); return;
        case BTN_DOWN: song.gp[GP_CUTOFF] = clampi(song.gp[GP_CUTOFF] - 2, 0, 100); say("TONE %d", song.gp[GP_CUTOFF]); return;
        case BTN_X:
        case BTN_Y: { /* the chance each pulse of the selected ring plays */
            int *c = &song.sp[cur_s][ring ? SP_BCHANCE : SP_PCHANCE];
            *c = clampi(*c + (b == BTN_X ? 10 : -10), 0, 100);
            say("%d %s CHANCE %d%%", cur_s + 1, ring ? "BOW" : "PLUCK", *c);
            return;
        }
        case BTN_A: song.fx[FX_BODY] = clampi(song.fx[FX_BODY] + 5, 0, 100); say("BODY %d", song.fx[FX_BODY]); return;
        case BTN_B: song.fx[FX_BODY] = clampi(song.fx[FX_BODY] - 5, 0, 100); say("BODY %d", song.fx[FX_BODY]); return;
        }
    }
    /* the rings play like HI5's pattern cells and RadioScan's rhythms:
     * A on/off, B fewer pulses (wrapping back to all), X rotate, Y a new
     * pattern; the d-pad picks the string and the ring */
    int *P = song.sp[cur_s];
    if (latch_on()) /* LATCH: up / down RELEASE, X / Y bow ATTACK, B (held) a harmonic, A lets all go */
        switch (b) {
        case BTN_X:
        case BTN_Y: {
            char atk[16];
            P[SP_LATK] = clampi(P[SP_LATK] + (b == BTN_X ? 3 : -3), 0, 100);
            sparam_format(cur_s, SP_LATK, atk, sizeof atk);
            say("%d BOW ATTACK %s", cur_s + 1, atk);
            return;
        }
        case BTN_B: /* while held: the next harmonic up (a wind overblows) */
            if (harm_s != cur_s) {
                if (harm_s >= 0)
                    engine_overblow(harm_s, 0);
                harm_s = cur_s;
                engine_overblow(cur_s, 1);
                if (!live.latch[cur_s])
                    say("%d IS NOT DRONING: L1 OR R1 FIRST", cur_s + 1);
            }
            return;
        case BTN_UP:
        case BTN_DOWN: {
            char rel[16];
            P[SP_RELEASE] = clampi(P[SP_RELEASE] + (b == BTN_UP ? 3 : -3), 0, 100);
            sparam_format(cur_s, SP_RELEASE, rel, sizeof rel);
            say("%d RELEASE %s", cur_s + 1, rel);
            return;
        }
        case BTN_A:
            engine_unlatch_all();
            say("ALL LET GO");
            return;
        }
    switch (b) {
    case BTN_LEFT: cur_s = wrap(cur_s - 1, NS); break;
    case BTN_RIGHT: cur_s = wrap(cur_s + 1, NS); break;
    case BTN_UP: ring = RING_PLUCK; ring_show(); break;
    case BTN_DOWN: ring = RING_BOW; ring_show(); break;
    case BTN_A:
        P[RING_P(ring, 0)] = !P[RING_P(ring, 0)];
        ring_show();
        break;
    case BTN_B: {
        int pu = P[RING_P(ring, 2)];
        P[RING_P(ring, 2)] = pu > 1 ? pu - 1 : P[RING_P(ring, 1)];
        P[RING_P(ring, 0)] = 1;
        ring_show();
        break;
    }
    case BTN_X: ring_edit(3, +1); break;
    case BTN_Y:
        engine_euclid_random(cur_s, ring);
        ring_show();
        break;
    }
}

/* LATCH: start or let go of string s, and say so */
static void do_latch(int s, int how)
{
    if (!live.playing)
        engine_set_playing(1);
    int st = engine_latch(s, how), wnd = engine_voice_wind(s), prc = engine_voice_perc(s);
    char rel[16];
    sparam_format(s, SP_RELEASE, rel, sizeof rel);
    if (st)
        say("%d %s, DRONING", s + 1,
            st == LATCH_HAMMER ? (wnd ? "FLUTTER TONGUED" : "MALLET DROPPED")
            : st == LATCH_PLUCK ? (wnd ? "TONGUED" : prc ? "STRUCK" : "PLUCKED")
                                : (wnd ? "BLOWN" : "BOWED"));
    else
        say("%d LET GO: %s", s + 1, rel);
    if (jdlog_level() == LOG_DETAIL)
        jdlog("latch voice %d %s", s + 1, st == LATCH_PLUCK ? "pluck" : st == LATCH_HAMMER ? "strike" : st == LATCH_BOW ? "bow" : "let go");
}

/* on PLAY, at once: L1 plucks (a wind: a tongued note), L2 drops the
 * dulcimer mallet (a wind: a flutter tongue) */
static void fire_l1(int tap)
{
    int s = l1_s;
    if (latch_on()) {
        do_latch(s, tap ? LATCH_HAMMER : LATCH_PLUCK);
        return;
    }
    if (tap)
        engine_hammer(s);
    else
        engine_pluck(s);
    hand_plucks++;
    if (jdlog_level() == LOG_DETAIL)
        jdlog("hand %s voice %d", tap ? (engine_voice_wind(s) ? "flutter tongue" : "mallet drop")
                                      : (engine_voice_wind(s) ? "tongue" : "pluck"), s + 1);
}

static void set_playmode(int latch)
{
    song.gp[GP_PLAYMODE] = latch ? PLAY_LATCH : PLAY_RINGS;
    engine_unlatch_all(); /* every string starts silent, or back to its rings */
    changed = 1;
    say("%s", latch ? "LATCH: L1 PLUCKS, R1 BOWS, AGAIN LETS GO" : "RINGS: THE RHYTHMS PLAY");
}

static void action(int b, int repeats)
{
    switch (b) {
    case BTN_L1:
    case BTN_R1:
        /* START + L1 / R1 changes page everywhere; L1 / R1 alone changes
         * page on the edit pages, and plays the selected string on PLAY:
         * L1 plucks, R1 bows while held */
        if (held[BTN_START] || page != PAGE_PLAY) {
            start_used |= held[BTN_START];
            page = wrap(page + (b == BTN_R1 ? 1 : -1), PAGE_COUNT);
        }
        else if (latch_on()) { /* LATCH: L1 plucks (a tap strikes), R1 bows; again, it lets go */
            if (repeats)
                return;
            if (held[b == BTN_L1 ? BTN_R1 : BTN_L1]) { /* both shoulders, either order: harmonic / overblow */
                if (harm_s >= 0 && harm_s != cur_s)
                    engine_overblow(harm_s, 0);
                harm_s = cur_s, harm_both = 1;
                engine_overblow(cur_s, 1);
                return;
            }
            if (b == BTN_R1)
                do_latch(cur_s, LATCH_BOW);
            else /* L1: plucks it on at once (on a droning string, lets go) */
                l1_s = cur_s, fire_l1(0);
        }
        else if (b == BTN_L1) {
            if (bowing >= 0) /* already bowing: L1 overblows / touches a harmonic */
                engine_overblow(bowing, 1);
            else if (!repeats) /* a pluck, at once */
                l1_s = cur_s, fire_l1(0);
        }
        else {
            bowing = cur_s;
            bow_start_ms = now_ms;
            if (jdlog_level() == LOG_DETAIL)
                jdlog("hand %s voice %d%s", engine_voice_wind(cur_s) ? "blow" : "bow", cur_s + 1,
                      held[BTN_L1] ? (engine_voice_wind(cur_s) ? ", overblown" : ", harmonic") : "");
            engine_bow(cur_s, 1);
            /* R1 added while L1 is held: overblow (wind) or a harmonic (strings) */
            engine_overblow(cur_s, held[BTN_L1]);
        }
        return;
    case BTN_START: return; /* acts on release, unless used for a page change */
    case BTN_SELECT:
        if (held[BTN_START]) { /* START + SELECT: the log's level */
            start_used = 1;
            jdlog_set_level((jdlog_level_raw() + 1) % NLOGLEVEL);
            say("LOG %s", jdlog_level_name(jdlog_level_raw()));
            return;
        }
        engine_next_chord();
        changed = 1;
        if (!quiet)
            say("%s  %s", engine_scale_name(song.gp[GP_SCALE]), engine_chord_name());
        return;
    case BTN_MENU: quit = 1; return;
    case BTN_L2:
    case BTN_R2: return;
    case BTN_A:
        if (held[BTN_START]) { /* START + A: PLAY MODE RINGS or LATCH */
            start_used = 1;
            if (!repeats)
                set_playmode(!latch_on());
            return;
        }
        break;
    }
    changed = 1;
    if (page == PAGE_PLAY) {
        play_action(b);
        return;
    }
    int cs = cell_string();
    if (page == PAGE_SONG) {
        layout(page);
        const Cell *c = sel_cell[page] < ncell ? &cells[sel_cell[page]] : NULL;
        if (c && c->kind == CK_SLOT && (b == BTN_A || b == BTN_X || b == BTN_Y)) {
            if (b == BTN_A)
                preset_load(c->a);
            else if (b == BTN_X)
                preset_save(c->a);
            else if (held[BTN_R2]) {
                r2_used = 1;
                preset_clear(c->a);
            }
            return;
        }
        if (c && c->kind == CK_G && c->b == GP_PROG && b == BTN_Y && held[BTN_R2]) {
            r2_used = 1;
            song.prog_n = song.prog_pos = 0;
            song.gp[GP_PROG] = PROG_FREE;
            say("PROGRESSION CLEARED");
            return;
        }
    }
    switch (b) {
    case BTN_Y: adjust(-1, repeats); return;
    case BTN_X: adjust(+1, repeats); return;
    case BTN_UP: nav(0, -1); return;
    case BTN_DOWN: nav(0, 1); return;
    case BTN_LEFT: nav(-1, 0); return;
    case BTN_RIGHT: nav(1, 0); return;
    case BTN_A: /* mute the string the cell belongs to */
        if (cs >= 0) {
            song.on[cs] = !song.on[cs];
            say("STRING %d %s", cs + 1, song.on[cs] ? "ON" : "MUTED");
        }
        return;
    case BTN_B: /* pluck that string, or the one selected on PLAY */
        engine_pluck(cs >= 0 ? cs : cur_s);
        return;
    }
}

static int repeatable(int b)
{
    if (page == PAGE_PLAY && !held[BTN_L2] && !held[BTN_R2] && (b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT))
        return 0;
    return b <= BTN_RIGHT || b == BTN_X || b == BTN_Y;
}

void ui_button(int b, int down)
{
    if (b < 0 || b >= BTN_COUNT || held[b] == down)
        return;
    if (splash_ms > 0) { /* any press only clears the start-up screen */
        if (down)
            splash_ms = 0;
        return;
    }
    held[b] = down;
    hold_ms[b] = rep_ms[b] = rep_n[b] = 0;
    if (b == BTN_L2 && down)
        l2_used = 0;
    if (b == BTN_R2 && down)
        r2_used = 0;
    /* L2 on PLAY: the dulcimer mallet, dropped on the selected string at once
     * (held, L2 still edits the rings with the d-pad); START + L2 toggles the
     * quiet view */
    if (b == BTN_L2 && down && page == PAGE_PLAY) {
        if (held[BTN_START]) {
            start_used = 1;
            quiet = !quiet;
            return;
        }
        l1_s = cur_s;
        ui_lock();
        fire_l1(1);
        ui_unlock();
    }
    if (!down && harm_s >= 0 && (b == BTN_B ? !harm_both : harm_both && (b == BTN_L1 || b == BTN_R1))) {
        /* LATCH: letting go of B (or of either shoulder) ends the harmonic */
        harm_both = 0;
        ui_lock();
        engine_overblow(harm_s, 0);
        ui_unlock();
        harm_s = -1;
        return;
    }
    if (b == BTN_R1 && !down && bowing >= 0) {
        bow_release_ms = REPEAT_GAP_MS; /* stops in ui_tick unless R1 comes back */
        return;
    }
    if (b == BTN_R1 && down && bowing >= 0) {
        bow_release_ms = 0; /* still bowing the same string */
        return;
    }
    if (b == BTN_L1 && page == PAGE_PLAY && !held[BTN_START]) {
        if (!down && bowing >= 0) { /* letting go of L1: back to the plain bow */
            ui_lock();
            engine_overblow(bowing, 0);
            ui_unlock();
        }
        if (!down)
            l1_up_ms = now_ms;
        else if (now_ms - l1_up_ms < REPEAT_GAP_MS)
            return; /* a glitch, not a new pluck */
    }
    if (b == BTN_START) {
        if (down)
            start_used = 0;
        else if (!start_used) { /* a plain tap: start or stop the drone */
            ui_lock();
            engine_set_playing(!live.playing);
            ui_unlock();
            say("%s", live.playing ? "DRONE ON" : "DRONE OFF");
        }
        return;
    }
    if (down) {
        int pg = page;
        presses[b]++;
        if (jdlog_level() == LOG_DETAIL)
            jdlog("press %s%s%s on %s", held[BTN_L2] && b != BTN_L2 ? "L2+" : "", held[BTN_R2] && b != BTN_R2 ? "R2+" : "",
                  btn_names[b], page_names[page]);
        ui_lock();
        action(b, 0);
        ui_unlock();
        if (page != pg)
            jdlog("page %s", page_names[page]);
    }
}

void ui_tick(int ms)
{
    now_ms += ms;
    if (splash_ms > 0)
        splash_ms -= ms;
    page_ms[page] += ms;
    if (pend_ms >= 0 && (pend_ms += ms) > 700)
        flush_param();
    if (live.chords != last_chords) {
        last_chords = live.chords;
        chords_seen++;
        jdlog("chord %s (%s)", engine_chord_name(), engine_scale_name(song.gp[GP_SCALE]));
    }
    if (bow_release_ms > 0 && (bow_release_ms -= ms) <= 0 && bowing >= 0 && !held[BTN_R1]) {
        hand_bow_ms += now_ms - bow_start_ms;
        if (jdlog_level() == LOG_DETAIL)
            jdlog("hand release voice %d after %.1f s", bowing + 1, (now_ms - bow_start_ms) / 1000.0);
        ui_lock();
        engine_bow(bowing, 0);
        engine_overblow(bowing, 0);
        ui_unlock();
        bowing = -1;
    }
    if (toast_ms > 0)
        toast_ms -= ms;
    for (int b = 0; b < BTN_COUNT; b++) {
        if (!held[b] || !repeatable(b))
            continue;
        hold_ms[b] += ms;
        if (hold_ms[b] < 300)
            continue;
        rep_ms[b] += ms;
        int interval = hold_ms[b] > 1500 ? 35 : 75;
        while (rep_ms[b] >= interval) {
            rep_ms[b] -= interval;
            ui_lock();
            action(b, ++rep_n[b]);
            ui_unlock();
        }
    }
}

/* -------------------------------------------------------- shape helpers */

/* a mass whose top edge slopes: left corner at y, right corner at y + drop */
static void roof(int x, int y, int w, int h, int drop, uint32_t c)
{
    int top = drop > 0 ? y + drop : y, bot = y + h;
    gfx_fill(x, top, w, bot - top, c);
    if (drop > 0)
        gfx_tri(x, y, x + w, y + drop, x, y + drop, c);
    else if (drop < 0)
        gfx_tri(x, y, x + w, y + drop, x + w, y, c);
}

/* a block with its top-right corner cut off */
static void chamfer(int x, int y, int w, int h, int cut, uint32_t c)
{
    gfx_fill(x, y + cut, w, h - cut, c);
    gfx_fill(x, y, w - cut, cut, c);
    gfx_tri(x + w - cut, y, x + w, y + cut, x + w - cut, y + cut, c);
}

/* a parallelogram whose top is shifted right by skew */
static void para(int x, int y, int w, int h, int skew, uint32_t c)
{
    gfx_fill(x + skew, y, w - skew, h, c);
    gfx_tri(x, y + h, x + skew, y, x + skew, y + h, c);
    gfx_tri(x + w, y, x + w + skew, y, x + w, y + h, c);
}

/* a thin diagonal line as two triangles */
static void slant(int x0, int y0, int x1, int y1, int t, uint32_t c)
{
    gfx_tri(x0, y0, x1, y1, x1, y1 + t, c);
    gfx_tri(x0, y0, x0, y0 + t, x1, y1 + t, c);
}

/* a straight line of thickness t at any angle, as a quad */
static void line(float x0, float y0, float x1, float y1, float t, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.5f)
        return;
    float nx = -dy / l * t * 0.5f, ny = dx / l * t * 0.5f;
    gfx_tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    gfx_tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

/* a diamond (a square on its corner) */
static void diamond(float x, float y, float r, uint32_t c)
{
    gfx_tri(x, y - r, x + r, y, x, y + r, c);
    gfx_tri(x, y - r, x - r, y, x, y + r, c);
}

/* a parameter row: name left, value right, bar along the bottom */
static void param_row(int x, int y, int w, int h, int sel, int on, const char *name, const char *val,
                      float t, int bipolar, uint32_t col)
{
    chamfer(x, y, w, h, 7, sel ? gfx_mix(C_PANEL2, col, 0.18f) : C_PANEL);
    if (sel)
        gfx_fill(x, y, 3, h, col);
    if (gfx_text_w(name, FONT_SMALL) + gfx_text_w(val, FONT_SMALL) + 24 <= w) /* a long value hides the name */
        gfx_text(x + 9, y + 2, name, sel ? col : C_DIM, FONT_SMALL);
    gfx_text(x + w - 9 - gfx_text_w(val, FONT_SMALL), y + 2, val, on ? C_TEXT : C_DIM, FONT_SMALL);
    int bx = x + 9, bw = w - 18, by = y + h - 8;
    gfx_fill(bx, by, bw, 4, C_LINE);
    if (bipolar) {
        int mid = bx + bw / 2, end = bx + (int)(bw * t);
        gfx_fill(end < mid ? end : mid, by, abs(end - mid) + 1, 4, on ? col : C_DIM);
    }
    else
        gfx_fill(bx, by, (int)(bw * t), 4, on ? col : C_DIM);
}

/* a vibrating string: a standing wave whose amplitude follows the meter */
/* what a voice is, for drawing: 0 a string, 1 a wind, 2 struck */
static int vkind_of(int s) { return engine_voice_perc(s) ? 2 : engine_voice_wind(s) ? 1 : 0; }

static void draw_string_big(int x0, int x1, int y, float amp, float ph, uint32_t col, int on, float big, int kind)
{
    int w = x1 - x0;
    int wind = kind == 1;
    /* in decibels, so a quiet string still visibly moves: -54 dB still,
     * -10 dB the full swing */
    float db = amp > 1e-6f ? 20.f * log10f(amp) : -120.f;
    float a = fminf(1.f, fmaxf(0.f, (db + 54.f) / 44.f)) * 11.f * big;
    if (kind == 2) {
        /* struck: a metal bar resting on two cords at its nodes (22% and
         * 78% along), flexing in its first mode, the ends swinging against
         * the middle */
        uint32_t bc = on ? col : C_OFF;
        int th = (int)(2 * big) + 2, py2 = 0;
        for (int i = 0; i <= w; i++) {
            float t = (float)i / w, sh = cosf(6.2831853f * (t - 0.5f));
            int yy = y + (int)(sh * sinf(ph * 2.f) * a * 0.6f);
            if (i > 0) {
                int lo = yy < py2 ? yy : py2, hi = yy < py2 ? py2 : yy;
                gfx_fill(x0 + i, lo - th / 2, 1, hi - lo + th, bc);
            }
            py2 = yy;
        }
        for (int k = 0; k < 2; k++) { /* the cords under the nodes */
            float xn = x0 + w * (k ? 0.776f : 0.224f);
            gfx_tri(xn - 5 * big, y + th + 7 * big, xn + 5 * big, y + th + 7 * big, xn, y + th, on ? gfx_mix(C_BG, col, 0.5f) : C_OFF);
        }
        return;
    }
    int py = y;
    for (int i = 0; i <= w; i++) {
        float t = (float)i / w;
        float s = sinf(t * 3.14159265f) * sinf(t * 6.2831853f * 2.f + ph) * a;
        int yy = y + (int)s;
        if (i > 0) {
            int lo = yy < py ? yy : py, hi = yy < py ? py : yy;
            gfx_fill(x0 + i, lo, 1, hi - lo + 2, on ? col : C_OFF);
        }
        py = yy;
    }
    if (wind) {
        /* a bore: two walls, closed at the mouthpiece, open at the bell */
        int hw = (int)(9 * big) + 4;
        uint32_t wc = on ? gfx_mix(C_BG, col, 0.55f) : C_OFF;
        gfx_fill(x0, y - hw, w, 2, wc);
        gfx_fill(x0, y + hw, w, 2, wc);
        gfx_fill(x0 - 3, y - hw, 3, 2 * hw + 2, on ? col : C_OFF);
        gfx_tri(x1, y - hw, x1 + 6, y - hw - 5, x1, y - hw + 2, wc);
        gfx_tri(x1, y + hw + 2, x1 + 6, y + hw + 7, x1, y + hw, wc);
        return;
    }
    /* end posts */
    gfx_fill(x0 - 2, y - 7, 3, 15, on ? col : C_OFF);
    gfx_fill(x1, y - 7, 3, 15, on ? col : C_OFF);
}

static void draw_string(int x0, int x1, int y, float amp, float ph, uint32_t col, int on, int kind)
{
    draw_string_big(x0, x1, y, amp, ph, col, on, 1.f, kind);
}

/* ---------------------------------------------------------------- pages */

/* the wordmark (tools/mklogo.py): solid block letters whose tops follow the
 * house's two roofs, h pixels high, with a fine darker edge */
static void draw_logo_a(float x, float y, float h, float edge, uint32_t fill, uint32_t col, int alpha)
{
    float k = h / LOGO_H, xy[2 * LOGO_MAXPT];
    for (int l = 0; l < LOGO_NLETTER; l++) {
        int a = logo_start[l], n = logo_start[l + 1] - a;
        for (int i = 0; i < n; i++)
            xy[2 * i] = x + logo_pt[a + i][0] * k, xy[2 * i + 1] = y + logo_pt[a + i][1] * k;
        gfx_poly_a(xy, n, fill, alpha);
        for (int i = 0, j = n - 1; i < n; j = i++) /* the edge also smooths the fill's steps */
            gfx_line(xy[2 * j], xy[2 * j + 1], xy[2 * i], xy[2 * i + 1], edge, col);
    }
}

static void draw_logo(float x, float y, float h, float edge, uint32_t fill, uint32_t col)
{
    draw_logo_a(x, y, h, edge, fill, col, 255);
}

static void draw_header(void)
{
    int x0_tabs;
    gfx_fill(0, 0, SCREEN_W, HEADER_H, C_PANEL);
    /* the roofline: one copper edge across the band */
    slant(150, HEADER_H - 3, SCREEN_W, HEADER_H - 16, 3, C_COPPER);
    /* the wordmark on a cedar board, its top cut to the two roofs */
    {
        float h = 26.f, k = h / LOGO_H, lx = 14.f, ly = 9.f, w = LOGO_W * k;
        float xy[10] = {0, 0, lx + LOGO_VALLEY_X * k, ly - 5 + LOGO_VALLEY_Y * k, lx + w + 12, 0, lx + w + 4, HEADER_H - 6, 0,
                        HEADER_H - 6};
        xy[1] = ly - 5 + (LOGO_ROOF_L - (LOGO_VALLEY_Y - LOGO_ROOF_L) * lx / (LOGO_VALLEY_X * k)) * k;
        xy[5] = ly - 5 + LOGO_ROOF_R * k;
        gfx_poly(xy, 5, C_CEDARWOOD);
        draw_logo(lx, ly, h, 1.f, C_SLATE, C_SLATE_D);
        x0_tabs = (int)(lx + w + 22);
    }
    int x = x0_tabs;
    for (int p = 0; p < PAGE_COUNT; p++) {
        int w = gfx_text_w(page_names[p], FONT_SMALL) + 12;
        para(x, 10, w, 24, 6, p == page ? C_COPPER : C_PANEL2);
        gfx_text(x + 9, 11, page_names[p], p == page ? 0x000000 : C_DIM, FONT_SMALL);
        x += w + 6;
    }
    char s[24], nm[8];
    note_name(song.gp[GP_ROOT], nm, sizeof nm);
    snprintf(s, sizeof s, "%s %d", nm, song.gp[GP_BPM]);
    int bw = gfx_text_w(s, FONT_LARGE);
    gfx_text(SCREEN_W - 10 - bw, 4, s, C_SAND, FONT_LARGE);
    int ix = SCREEN_W - 10 - bw - 30;
    if (live.playing) {
        float ph = 1.f - fmodf(now_ms / 1000.f * engine_bpm() / 60.f, 1.f);
        gfx_disc(ix + 8, 24, 5.f + 4.f * ph * ph, C_SEA);
    }
    else
        gfx_ring(ix + 8, 24, 7.f, 2.f, C_DIM);
    if (jdlog_level() != LOG_OFF)
        gfx_text(ix - 16 - gfx_text_w("LOG", FONT_SMALL), 24, "LOG", jdlog_level() == LOG_DETAIL ? C_SEA : C_DIM, FONT_SMALL);
    if (song.gp[GP_SYNC] != SYNC_OFF) {
        const SyncStatus *ss = sync_status();
        int linked = ss->leading || ss->have_leader;
        const char *t = ss->leading ? "LEAD" : ss->have_leader ? "FOLLOW" : "SYNC";
        int w = gfx_text_w(t, FONT_SMALL) + 16;
        para(ix - w - 14, 12, w, 22, 6, linked ? C_SEA : C_PANEL2);
        gfx_text(ix - w - 14 + 11, 13, t, linked ? 0x000000 : C_DIM, FONT_SMALL);
    }
}

static void draw_footer(void)
{
    static const char *const hints[PAGE_COUNT] = {
        "A:ON/OFF B:FEWER X:ROTATE Y:NEW L1:PLUCK R1:BOW L2:EDIT R2:SOUND",
        "D-PAD:MOVE Y:- X:+ A:MUTE B:PLUCK L1/R1:PAGE SEL:CHORD",
        "D-PAD:MOVE Y:- X:+ B:PLUCK L1/R1:PAGE SEL:NEXT CHORD",
        "D-PAD:MOVE Y:- X:+ A:MUTE B:PLUCK L1/R1:PAGE SEL:CHORD",
        "PRESET A:LOAD X:SAVE R2+Y:CLEAR  PROG R2+Y:CLEAR  SEL:CHORD",
    };
    const char *h = hints[page];
    int vs = page == PAGE_PLAY ? cur_s : cell_string(); /* the voice under the cursor */
    int cw_ = vs >= 0 ? engine_voice_wind(vs) : 0, cp_ = vs >= 0 ? engine_voice_perc(vs) : 0;
    if (page != PAGE_PLAY && page != PAGE_SONG && cw_)
        h = page == PAGE_COMPOSE ? "D-PAD:MOVE Y:- X:+ B:TONGUE L1/R1:PAGE SEL:NEXT CHORD"
                                 : "D-PAD:MOVE Y:- X:+ A:MUTE B:TONGUE L1/R1:PAGE SEL:CHORD";
    if (page != PAGE_PLAY && page != PAGE_SONG && cp_)
        h = page == PAGE_COMPOSE ? "D-PAD:MOVE Y:- X:+ B:STRIKE L1/R1:PAGE SEL:NEXT CHORD"
                                 : "D-PAD:MOVE Y:- X:+ A:MUTE B:STRIKE R2+Y/X:FAMILY SEL:CHORD";
    else if (page == PAGE_SOUND && vs >= 0)
        h = cw_ ? "D-PAD:MOVE Y:- X:+ A:MUTE B:TONGUE R2+Y/X:FAMILY SEL:CHORD"
                : "D-PAD:MOVE Y:- X:+ A:MUTE B:PLUCK R2+Y/X:FAMILY SEL:CHORD";
    if (page == PAGE_PLAY && latch_on())
        h = cw_   ? "L1:TONGUE L2:FLUTTER R1:BLOW B:OVERBLOW Y/X:ATK ^v:REL"
            : cp_ ? "L1:STRIKE L2:DROP R1:BOW B:HARMONIC Y/X:ATK ^v:REL A:ALL GO"
                  : "L1:PLUCK L2:DROP R1:BOW B:HARMONIC Y/X:ATK ^v:REL A:ALL GO";
    else if (page == PAGE_PLAY && cp_)
        h = "A:ON/OFF B:FEWER Y:NEW L1:STRIKE L2:DROP R1:BOW L1+R1:HARM";
    else if (page == PAGE_PLAY && cw_)
        h = "A:ON/OFF B:FEWER Y:NEW L1:TONGUE L2:FLUTTER R1:BLOW L1+R1:OVER";
    else if (page == PAGE_PLAY)
        h = "A:ON/OFF B:FEWER Y:NEW L1:PLUCK L2:DROP R1:BOW L1+R1:HARM";
    if (page == PAGE_PLAY && held[BTN_L2])
        h = "L2+ ^v:STEPS <>:RATE X:MORE PULSES Y:ROTATE BACK A:MUTE";
    else if (page == PAGE_PLAY && held[BTN_R2])
        h = "R2+ X/Y:RING CHANCE <>:SCALE ^v:TONE A/B:BODY";
    gfx_fill(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, C_PANEL);
    gfx_fill(0, FOOTER_Y, 4, SCREEN_H - FOOTER_Y, C_COPPER);
    gfx_text(12, FOOTER_Y + 2, h, C_DIM, FONT_SMALL);
}

static void draw_toast(void)
{
    if (toast_ms <= 0)
        return;
    int w = gfx_text_w(toast, FONT_SMALL) + 36, h = 30;
    int x = (SCREEN_W - w) / 2, y = FOOTER_Y - h - 10;
    chamfer(x, y, w, h, 10, C_PANEL2);
    gfx_fill(x, y, 3, h, C_COPPER);
    gfx_text(x + 16, y + 6, toast, C_TEXT, FONT_SMALL);
}

/* the colour of a cell: its string's, or its section's */
static uint32_t cell_col(const Cell *c)
{
    if (c->kind == CK_S)
        return scol[(int)c->a];
    if (c->kind == CK_L)
        return c->a ? C_SAND : C_COPPER;
    if (c->kind == CK_F && c->b >= FX_SCOUNT)
        return C_SEA;
    if (c->kind == CK_F && c->b >= FX_SYMP)
        return C_SAND;
    if (c->kind == CK_G && c->b == GP_MATERIAL)
        return C_SAND;
    if (c->kind == CK_G && c->b >= GP_SUB)
        return C_CEDAR + 0x303030;
    if (page == PAGE_COMPOSE && c->kind == CK_G && c->b >= GP_MOTION)
        return C_SEA;
    if (page == PAGE_MIX && c->kind == CK_F && c->b < FX_SYMP)
        return c->b < 3 ? C_COPPER : c->b < 6 ? C_SAND : C_SEA;
    return c->kind == CK_G && c->b == GP_CUTOFF ? C_SAND : C_COPPER;
}

static void draw_cells(void)
{
    layout(page);
    for (int i = 0; i < ncell; i++) {
        const Cell *c = &cells[i];
        if (c->kind == CK_SLOT)
            continue; /* drawn as tiles by draw_song */
        const ParamDef *d;
        int *v = cell_ref(c, &d);
        char val[24];
        cell_format(c, val, sizeof val);
        float t = (float)(*v - d->min) / (d->max - d->min);
        int on = c->kind == CK_S ? song.on[(int)c->a] : 1;
        if (c->kind == CK_F && c->b > FX_SCOUNT && song.fx[FX_SCOUNT] == 0)
            on = 0;
        if (c->kind == CK_S && c->b == SP_NOTE && song.gp[GP_MODE] == MODE_MONO && c->a > 0)
            on = 0;
        if (c->kind == CK_G && c->b == GP_MATERIAL && !engine_wind_count(&song))
            on = 0; /* no winds, no bore */
        if (c->kind == CK_G && (c->b == GP_SUBDIV || c->b == GP_SUBRES) && !song.gp[GP_SUB])
            on = 0;
        if (c->kind == CK_G && c->b == GP_MODALITY && song.gp[GP_SCALE] != 0)
            on = 0; /* modes are for EQUAL */
        if (c->kind == CK_G && song.gp[GP_KEYLOCK] && (c->b == GP_SCALE || c->b == GP_ROOT || c->b == GP_MODALITY))
            on = 0; /* locked */
        if (c->kind == CK_G && c->b > GP_OUTPUT && song.gp[GP_OUTPUT] != OUT_MONO_CLOCK)
            on = 0; /* no clock in stereo */
        if (c->kind == CK_S && c->b == SP_PAN && song.gp[GP_OUTPUT] == OUT_MONO_CLOCK)
            on = 0; /* mono + clock plays every voice from the centre */
        param_row(c->x, c->y, c->w, c->h, i == sel_cell[page], on, c->kind == CK_S ? sparam_name(c->a, c->b) : d->name, val, t,
                  d->min < 0, cell_col(c));
    }
}

/* the five strings as a header: a long roof falling across them */
static void string_heads(int y0, int h)
{
    const int cw = SCREEN_W / NS;
    int cs = cell_string();
    for (int s = 0; s < NS; s++) {
        int x = s * cw + 3, w = cw - 6, on = song.on[s];
        uint32_t col = scol[s];
        roof(x, y0, w, h, 4, s == cs ? gfx_mix(C_PANEL2, col, 0.14f) : C_PANEL);
        char nm[24], dn[16];
        engine_degree_name(song.gp[GP_MODE] == MODE_MONO ? song.sp[0][SP_NOTE] : song.sp[s][SP_NOTE], dn, sizeof dn);
        snprintf(nm, sizeof nm, "%d %s", s + 1, dn);
        gfx_text(x + 8, y0 + 1, nm, on ? col : C_DIM, FONT_SMALL);
        if (!on)
            gfx_fill(x + w - 26, y0 + 12, 18, 4, C_RED);
        if (h > 30)
            draw_string_big(x + 12, x + w - 12, y0 + h - 9, on ? live.meter[s] : 0.f, live.phase[s], col, on, 0.6f, vkind_of(s));
        else /* a short header: the string as a line that brightens as it sounds */
            gfx_fill(x + 70, y0 + 11, w - 80, 2, on ? gfx_mix(C_PANEL2, col, 0.3f + fminf(0.7f, live.meter[s] * 4.f)) : C_OFF);
    }
}

static void draw_sound(void)
{
    string_heads(BODY_Y, ENGINE_Y - BODY_Y - 3);
    /* the resonance band: one copper roofline over body and sympathetic strings */
    slant(0, BAND_Y - 2, SCREEN_W, BAND_Y - 4, 2, C_COPPER);
    draw_cells();
    /* the sympathetic strings' meters, small posts along the bottom row */
    int n = song.fx[FX_SCOUNT];
    for (int k = 0; k < n; k++) {
        int h = (int)(fminf(1.f, engine_symp_meter(k) * 6.f) * 10.f);
        gfx_fill(SCREEN_W - 10 - (n - k) * 5, BAND_Y + 36 + 30 - h, 3, h + 1, C_SEA);
    }
}

static void draw_compose(void)
{
    /* the scale as a polygon: each degree at its true angle in the octave;
     * the chord joined inside it, the sympathetic strings as dots */
    int px = 6, pw = 252;
    roof(px, BODY_Y, pw, FOOTER_Y - 6 - BODY_Y, 20, C_PANEL);
    slant(px, BODY_Y, px + pw, BODY_Y + 20, 2, C_COPPER);
    gfx_text(px + 10, BODY_Y + 22, engine_scale_name(song.gp[GP_SCALE]), C_DIM, FONT_SMALL);
    int n = engine_scale_n();
    float cx = px + pw / 2.f, cy = BODY_Y + 150, r = 74.f, vx[12], vy[12];
    for (int i = 0; i < n; i++) {
        float a = -1.5707963f + 6.2831853f * engine_scale_cents(i) / 1200.f;
        vx[i] = cx + r * cosf(a);
        vy[i] = cy + r * sinf(a);
    }
    for (int i = 0; i < n; i++)
        line(vx[i], vy[i], vx[(i + 1) % n], vy[(i + 1) % n], 1.f, C_LINE);
    int has[12] = {0}, order[12], no = 0;
    for (int s = 0; s < NS; s++)
        if (song.on[s]) {
            int d = song.gp[GP_MODE] == MODE_MONO ? song.sp[0][SP_NOTE] : song.sp[s][SP_NOTE];
            has[((d % n) + n) % n] |= 1 << s;
        }
    for (int i = 0; i < n; i++)
        if (has[i])
            order[no++] = i;
    uint32_t fill = gfx_mix(C_BG, C_COPPER, 0.35f + 0.3f * (live.master_meter > 0.5f));
    for (int k = 1; k + 1 < no; k++)
        gfx_tri(vx[order[0]], vy[order[0]], vx[order[k]], vy[order[k]], vx[order[k + 1]], vy[order[k + 1]], fill);
    for (int k = 0; no >= 2 && k < no; k++)
        line(vx[order[k]], vy[order[k]], vx[order[(k + 1) % no]], vy[order[(k + 1) % no]], 2.f, C_COPPER);
    int sym[12] = {0};
    for (int k = 0; k < song.fx[FX_SCOUNT]; k++) {
        int d = engine_symp_degree(k);
        sym[((d % n) + n) % n]++;
    }
    for (int i = 0; i < n; i++) {
        diamond(vx[i], vy[i], has[i] ? 6.f : 3.f, has[i] ? C_TEXT : C_DIM);
        /* the strings on a degree step inward, the sympathetic strings outward */
        for (int s = 0, j = 0; s < NS; s++)
            if (has[i] & (1 << s))
                diamond(cx + (vx[i] - cx) * (0.84f - 0.12f * j), cy + (vy[i] - cy) * (0.84f - 0.12f * j), 4.f, scol[s]), j++;
        for (int k = 0; k < sym[i]; k++)
            gfx_disc(cx + (vx[i] - cx) * (1.12f + 0.07f * k), cy + (vy[i] - cy) * (1.12f + 0.07f * k), 2.f, C_SEA);
        char nm[12];
        engine_pc_name(i, nm, sizeof nm);
        float lx = cx + (vx[i] - cx) * 1.5f, ly = cy + (vy[i] - cy) * 1.42f;
        if (n <= 8 || has[i])
            gfx_text((int)lx - gfx_text_w(nm, FONT_SMALL) / 2, (int)ly - 11, nm, has[i] ? C_SAND : C_DIM, FONT_SMALL);
    }
    /* the chord now, and the ones before it fading down the roof */
    const char *cn = engine_chord_name();
    gfx_text((int)cx - gfx_text_w(cn, FONT_LARGE) / 2, BODY_Y + 254, cn[0] ? cn : "-", C_SAND, FONT_LARGE);
    for (int i = 1; i < 4; i++) {
        const char *h = engine_chord_history(i);
        if (!h[0])
            break;
        int y = BODY_Y + 288 + (i - 1) * 22;
        para(px + 16 + i * 6, y + 4, 10, 12, 3, gfx_mix(C_PANEL, C_COPPER, 0.9f - 0.15f * i));
        gfx_text(px + 36 + i * 6, y, h, gfx_mix(C_PANEL, C_DIM, 1.f - 0.12f * i), FONT_SMALL);
    }
    static const int bars_of[6] = {0, 1, 2, 4, 8, 16};
    int bars = bars_of[song.gp[GP_CHANGES]];
    if (bars) {
        int done = (int)((live.beat / 4) % (uint32_t)bars), bw = bars > 8 ? 9 : 16;
        for (int i = 0; i < bars; i++)
            para(px + 14 + i * (bw + 4), FOOTER_Y - 26, bw, 12, 3, i <= done ? C_COPPER : C_PANEL2);
    }
    /* the controls: harmony and time, the algorithm, then two LFOs */
    for (int l = 0; l < NLFO; l++) {
        int x = COMP_X + l * COMP_CW;
        char t[12];
        snprintf(t, sizeof t, "LFO %d", l + 1);
        gfx_text(x + 4, LFO_Y - 22, t, l ? C_SAND : C_COPPER, FONT_SMALL);
        int bw = COMP_CW - 70;
        gfx_fill(x + 60, LFO_Y - 13, bw, 4, C_PANEL2);
        gfx_fill(x + 60, LFO_Y - 13, (int)(bw * live.lfo_val[l]), 4, song.lfo[l][LP_DEPTH] ? (l ? C_SAND : C_COPPER) : C_LINE);
    }
    draw_cells();
}

static void draw_mix(void)
{
    const int cw = SCREEN_W / NS;
    int cs = cell_string();
    for (int s = 0; s < NS; s++) {
        int x = s * cw + 3, w = cw - 6, on = song.on[s];
        uint32_t col = scol[s];
        chamfer(x, BODY_Y, w, 38, 8, s == cs ? gfx_mix(C_PANEL2, col, 0.14f) : C_PANEL);
        char nm[8];
        snprintf(nm, sizeof nm, "%d", s + 1);
        gfx_text(x + 8, BODY_Y, nm, on ? col : C_DIM, FONT_SMALL);
        if (!on)
            gfx_fill(x + 24, BODY_Y + 9, 18, 4, C_RED);
        /* the meter, placed where the string sits in the stereo field */
        int mw = w - 16, mx = x + 8, my = BODY_Y + 26;
        gfx_fill(mx, my, mw, 5, C_PANEL2);
        int lv = (int)(mw * fminf(1.f, sqrtf(live.meter[s] * 4.f)) * (on ? 1 : 0));
        gfx_fill(mx, my, lv, 5, col);
        int pp = mx + mw / 2 + (song.gp[GP_OUTPUT] == OUT_MONO_CLOCK ? 0 : song.sp[s][SP_PAN] * mw / 100);
        gfx_fill(pp - 1, my - 4, 3, 13, C_TEXT);
    }
    int mm = (int)(628 * fminf(1.f, live.master_meter));
    gfx_fill(6, BODY_Y + 214, 628, 3, C_PANEL2);
    gfx_fill(6, BODY_Y + 214, mm, 3, live.master_meter > 0.95f ? C_RED : C_SEA);

    /* the panels, left to right: AURA, DELAY, REVERB, BINAURAL (c: which one,
     * 3 being AURA) */
    static const char *const names[4] = {"DELAY", "REVERB", "BINAURAL", "AURA"};
    static const int col_of[4] = {1, 2, 3, 0};
    for (int c = 0; c < 4; c++) {
        int k = col_of[c], x = k * MIX_FXW + 4, w = MIX_FXW - 8;
        uint32_t col = c == 0 ? C_COPPER : c == 1 ? C_SAND : c == 2 ? C_SEA : C_TEXT;
        roof(x, MIX_FX_Y + k * 3, w, MIX_FXC_Y - 4 - MIX_FX_Y - k * 3, 6, C_PANEL);
        gfx_text(x + 10, MIX_FX_Y + k * 3 + 3, names[c], col, FONT_SMALL);
        int gx = x + 12, gy = MIX_FX_Y + 32, gw = w - 24, gh = 26;
        if (c == 3) { /* the stage: players (discs) sounding to listeners (rings) */
            float a = song.fx[FX_AIR] / 100.f;
            for (int s = 0; s < NS; s++)
                for (int l = 0; l < NS; l++)
                    if (a > 0.f && song.sp[s][SP_LISTEN] == AIR_SENDS && song.sp[l][SP_LISTEN] == AIR_LISTENS && song.on[s] &&
                        song.on[l]) {
                        float x0 = gx + gw * (0.5f + song.sp[s][SP_PAN] / 100.f), x1 = gx + gw * (0.5f + song.sp[l][SP_PAN] / 100.f);
                        gfx_line(x0, gy + gh * 0.3f, x1, gy + gh * 0.75f, 1.f, gfx_mix(C_PANEL2, scol[s], 0.3f + 0.6f * a));
                    }
            for (int s = 0; s < NS; s++) {
                float px = gx + gw * (0.5f + song.sp[s][SP_PAN] / 100.f);
                if (song.sp[s][SP_LISTEN] == AIR_LISTENS)
                    gfx_ring(px, gy + gh * 0.75f, 4.5f, 1.6f, song.on[s] ? scol[s] : C_DIM);
                else if (song.sp[s][SP_LISTEN] == AIR_SENDS)
                    gfx_disc(px, gy + gh * 0.3f, 4.5f, song.on[s] ? scol[s] : C_DIM);
                else /* out of the air */
                    gfx_fill((int)px - 4, (int)(gy + gh * 0.3f), 8, 2, C_DIM);
            }
            continue;
        }
        if (c == 0) { /* echoes dying away */
            float fb = song.fx[FX_DFB] / 100.f, a = 1.f;
            for (int i = 0; i < 6; i++) {
                int h = (int)(gh * a * (0.3f + 0.7f * song.fx[FX_DMIX] / 100.f));
                gfx_fill(gx + i * (gw / 6), gy + gh - h, gw / 6 - 6, h, i ? gfx_mix(C_PANEL2, col, a) : col);
                a *= 0.35f + 0.6f * fb;
            }
        }
        else if (c == 1) { /* a wedge of decay */
            int len = (int)(gw * (0.3f + 0.7f * song.fx[FX_VTIME] / 100.f));
            gfx_tri(gx, gy, gx + len, gy + gh, gx, gy + gh, gfx_mix(C_PANEL2, col, 0.3f + 0.6f * song.fx[FX_VSEND] / 100.f));
        }
        else { /* two waves, left and right, sliding past each other at the beat */
            float hz = live.bin_hz, d = song.fx[FX_BINDEPTH] / 100.f;
            float drift = hz > 0.f ? 6.2831853f * fmodf(now_ms * 0.001f * hz, 1.f) * d : 0.f;
            for (int w = 0; w < 2; w++) {
                int py = 0;
                uint32_t wc = hz > 0.f ? (w ? col : gfx_mix(col, C_SAND, 0.5f)) : C_LINE;
                for (int i = 0; i <= gw; i++) {
                    float t = (float)i / gw * 6.2831853f * 3.f + (w ? drift : 0.f);
                    int yy = gy + gh / 2 + (int)(sinf(t) * gh * 0.32f) + (w ? 0 : 0);
                    if (i)
                        gfx_fill(gx + i, yy < py ? yy : py, 1, abs(yy - py) + 2, wc);
                    py = yy;
                }
            }
            gfx_text(x + w - 14 - gfx_text_w(hz > 0.f ? "L  R" : "OFF", FONT_SMALL), MIX_FX_Y + k * 3 + 3,
                     hz > 0.f ? "L  R" : "OFF", C_DIM, FONT_SMALL);
        }
    }
    draw_cells();
}

/* SONG: ten preset tiles, the progression memory, then its controls */
static void draw_song(void)
{
    if (!slots_read)
        read_slots();
    layout(page);
    gfx_text(8, BODY_Y, "PRESETS", C_DIM, FONT_SMALL);
    for (int i = 0; i < ncell; i++) {
        const Cell *c = &cells[i];
        if (c->kind != CK_SLOT)
            continue;
        int k = c->a, sel = i == sel_cell[page], used = slot_info[k].used;
        uint32_t col = used ? C_COPPER : C_LINE;
        chamfer(c->x, c->y, c->w, c->h, 10, sel ? gfx_mix(C_PANEL2, C_COPPER, 0.25f) : C_PANEL);
        if (sel)
            gfx_fill(c->x, c->y, 3, c->h, C_COPPER);
        char t[32];
        snprintf(t, sizeof t, "%d", k + 1);
        gfx_text(c->x + 8, c->y + 2, t, used ? C_SAND : C_DIM, FONT_SMALL);
        if (used) {
            snprintf(t, sizeof t, "%.10s", engine_scale_name(slot_info[k].scale));
            gfx_text(c->x + 30, c->y + 2, t, C_TEXT, FONT_SMALL);
            int w = slot_info[k].winds, pc = slot_info[k].percs, st_ = NS - w - pc; /* how the five voices are split */
            if (w == NS || pc == NS || st_ == NS)
                snprintf(t, sizeof t, "%s %d", w ? "WIND" : pc ? "STRUCK" : "STRING", slot_info[k].bpm);
            else if (!pc)
                snprintf(t, sizeof t, "%dS+%dW %d", st_, w, slot_info[k].bpm);
            else
                snprintf(t, sizeof t, "%dS%dW%dP %d", st_, w, pc, slot_info[k].bpm);
            gfx_text(c->x + 8, c->y + 28, t, C_DIM, FONT_SMALL);
            if (slot_info[k].prog_n) /* a mark for a kept progression */
                para(c->x + c->w - 22, c->y + 32, 12, 12, 3, col);
        }
        else
            gfx_text(c->x + 30, c->y + 2, "-", C_DIM, FONT_SMALL);
    }
    /* the progression memory: up to sixteen chords; the one playing ringed */
    int pm = song.gp[GP_PROG];
    char hd[48];
    snprintf(hd, sizeof hd, "PROGRESSION  %d/%d  %s", song.prog_n, NPROG, pm == PROG_RECORD ? "RECORDING" : pm == PROG_PLAY ? "PLAYING" : "");
    gfx_text(8, PROGT_Y - 24, hd, pm == PROG_RECORD ? C_RED : pm == PROG_PLAY ? C_SEA : C_DIM, FONT_SMALL);
    for (int k = 0; k < NPROG; k++) {
        int x = 5 + (k % 8) * 79, y = PROGT_Y + (k / 8) * 34, here = pm == PROG_PLAY && k == song.prog_pos;
        para(x, y, 72, 30, 5, k < song.prog_n ? (here ? gfx_mix(C_PANEL2, C_SEA, 0.5f) : C_PANEL2) : C_PANEL);
        if (k < song.prog_n) {
            char t[12];
            snprintf(t, sizeof t, "%.7s", song.prog_name[k]);
            gfx_text(x + 8, y + 4, t, here ? C_TEXT : C_SAND, FONT_SMALL);
        }
    }
    draw_cells();
}

/* a Euclidean ring as geometry: the steps are the corners of a polygon, the
 * pulses are joined into a second polygon inside it, which lights on a hit.
 * glow (0..1) fills it: the flash for plucks, the bow's swell for bows. */
static void draw_ring(float cx, float cy, float r, const int *P, int rg, int cur, float glow, uint32_t col, int sel,
                      int dim)
{
    int on = P[RING_P(rg, 0)] && !dim, steps = P[RING_P(rg, 1)], pulses = P[RING_P(rg, 2)], rot = P[RING_P(rg, 3)];
    uint32_t edge = on ? gfx_mix(C_PANEL2, col, 0.45f) : C_LINE;
    float vx[16], vy[16];
    for (int i = 0; i < steps; i++) {
        float a = -1.5707963f + 6.2831853f * i / steps;
        vx[i] = cx + r * cosf(a);
        vy[i] = cy + r * sinf(a);
    }
    if (sel) /* a square frame, chamfered, around the selected ring */
        gfx_rect((int)(cx - r - 10), (int)(cy - r - 10), (int)(2 * r + 20), (int)(2 * r + 20), 1, col);
    /* the pulse polygon, filled as a fan */
    int px[16], np = 0;
    for (int i = 0; i < steps; i++)
        if (euclid_hit(steps, pulses, rot, i))
            px[np++] = i;
    /* a ring that is off still lights when the string is played by hand */
    uint32_t fill = on ? gfx_mix(C_BG, col, 0.12f + 0.55f * glow)
                       : glow > 0.03f && !dim ? gfx_mix(C_BG, col, 0.5f * glow) : gfx_mix(C_BG, C_LINE, 0.5f);
    if (np >= 3)
        for (int k = 1; k + 1 < np; k++)
            gfx_tri(vx[px[0]], vy[px[0]], vx[px[k]], vy[px[k]], vx[px[k + 1]], vy[px[k + 1]], fill);
    for (int k = 0; np >= 2 && k < np; k++) {
        int a = px[k], b = px[(k + 1) % np];
        if (np == 2 && k == 1)
            break;
        line(vx[a], vy[a], vx[b], vy[b], 2.f, on ? gfx_mix(fill, col, 0.6f) : C_LINE);
    }
    if (np == 1 && on)
        line(cx, cy, vx[px[0]], vy[px[0]], 2.f, gfx_mix(fill, col, 0.6f));
    /* the outer polygon */
    if (steps >= 2)
        for (int i = 0; i < steps; i++) {
            int j = (i + 1) % steps;
            if (steps == 2 && i == 1)
                break;
            line(vx[i], vy[i], vx[j], vy[j], 1.f, edge);
        }
    /* corners: pulses solid, rests small, the current step ringed */
    for (int i = 0; i < steps; i++) {
        int hit = euclid_hit(steps, pulses, rot, i);
        if (hit)
            diamond(vx[i], vy[i], 5.f, on ? col : C_DIM);
        else
            diamond(vx[i], vy[i], 2.5f, edge);
        if (on && i == cur && live.playing) {
            diamond(vx[i], vy[i], 8.f, C_TEXT);
            diamond(vx[i], vy[i], 6.f, hit ? col : C_BG);
        }
    }
}

/* LATCH: a board with a shed-roof top, like the house's walls, filled to
 * how open the string's gate is; its state and RELEASE under it */
static void draw_latch(int s, int x, int y, int w, int h, uint32_t col, int sel, int on)
{
    float slope = 14.f, x1 = x + w, yb = y + h, yt = y;
    float lv = fminf(1.f, fmaxf(0.f, live.bow_env[s]));
    int st = live.latch[s], wnd = engine_voice_wind(s), prc = engine_voice_perc(s);
    if (lv > 0.003f) {
        float yf = yb - lv * (yb - yt);
        float pts[10];
        int n = 0;
#define PT(a, b) (pts[2 * n] = (a), pts[2 * n + 1] = (b), n++)
        PT(x, yb);
        if (yf >= yt + slope) /* below the roof: a flat top */
            PT(x, yf), PT(x1, yf);
        else if (yf >= yt) { /* in the roof: the left part under its slope */
            float xc = x + (yt + slope - yf) / slope * w;
            PT(x, yt + slope), PT(xc, yf), PT(x1, yf);
        }
        else
            PT(x, yt + slope), PT(x1, yt);
        PT(x1, yb);
#undef PT
        gfx_poly(pts, n, gfx_mix(C_BG, col, (st ? 0.25f : 0.12f) + 0.35f * lv));
    }
    uint32_t edge = !on ? C_OFF : st ? col : sel ? gfx_mix(C_BG, col, 0.7f) : gfx_mix(C_BG, col, 0.4f);
    float th = st ? 2.f : 1.3f;
    gfx_line(x, yb, x, yt + slope, th, edge);
    gfx_line(x, yt + slope, x1, yt, th, edge);
    gfx_line(x1, yt, x1, yb, th, edge);
    gfx_line(x, yb, x1, yb, th, edge);
    const char *word = st && s == harm_s ? (wnd ? "OVERBLOWN" : "HARMONIC")
                       : st == LATCH_HAMMER ? (wnd ? "HARD" : prc ? "HARD" : "STRUCK")
                       : st == LATCH_PLUCK ? (wnd ? "TONGUED" : prc ? "STRUCK" : "PLUCKED")
                       : st == LATCH_BOW ? (wnd ? "BLOWN" : "BOWED")
                       : lv > 0.02f      ? "FADING"
                                         : (wnd ? "L1 / R1" : "L1 / R1");
    gfx_text(x + w / 2 - gfx_text_w(word, FONT_SMALL) / 2, (int)(yb - h * 0.45f), word, st ? C_TEXT : C_DIM, FONT_SMALL);
    char rel[16], atk[16], lb[24];
    sparam_format(s, SP_RELEASE, rel, sizeof rel);
    sparam_format(s, SP_LATK, atk, sizeof atk);
    snprintf(lb, sizeof lb, "ATK %s", atk);
    gfx_text(x + w / 2 - gfx_text_w(lb, FONT_SMALL) / 2, (int)yb + 4, lb, sel ? col : C_DIM, FONT_SMALL);
    snprintf(lb, sizeof lb, "REL %s", rel);
    gfx_text(x + w / 2 - gfx_text_w(lb, FONT_SMALL) / 2, (int)yb + 22, lb, sel ? col : C_DIM, FONT_SMALL);
}

static void draw_play(void)
{
    const int cw = SCREEN_W / NS;
    /* the chord band: one long roof, the scale, the chord, and the bars to
     * the next change as a row of tilted blocks */
    roof(0, BODY_Y - 6, SCREEN_W, 44, -10, C_PANEL);
    slant(0, BODY_Y + 38, SCREEN_W, BODY_Y + 28, 2, C_COPPER);
    gfx_text(12, BODY_Y, engine_scale_name(song.gp[GP_SCALE]), C_DIM, FONT_SMALL);
    const char *cn = engine_chord_name();
    if (cn[0])
        gfx_text((SCREEN_W - gfx_text_w(cn, FONT_LARGE)) / 2, BODY_Y - 8, cn, C_SAND, FONT_LARGE);
    static const int bars_of[6] = {0, 1, 2, 4, 8, 16};
    int bars = bars_of[song.gp[GP_CHANGES]];
    if (bars) {
        int done = (int)((live.beat / 4) % (uint32_t)bars), bw = bars > 8 ? 7 : 12;
        for (int i = 0; i < bars; i++) {
            int x = SCREEN_W - 14 - (bars - i) * (bw + 3);
            para(x, BODY_Y + 2, bw, 14, 3, i <= done ? C_COPPER : C_PANEL2);
        }
    }

    int top = BODY_Y + 46;
    for (int s = 0; s < NS; s++) {
        int x = s * cw + 3, w = cw - 6, sel = s == cur_s, on = song.on[s];
        uint32_t col = scol[s];
        const int *P = song.sp[s];
        int drop = 5, t = top + s * drop;
        roof(x, t, w, FOOTER_Y - 6 - t, drop, sel ? gfx_mix(C_BG, col, 0.10f) : C_PANEL);
        if (sel)
            gfx_fill(x, FOOTER_Y - 9, w, 3, col);
        char nm[24], dn[16];
        int note = song.gp[GP_MODE] == MODE_MONO ? song.sp[0][SP_NOTE] : P[SP_NOTE];
        engine_degree_name(note, dn, sizeof dn);
        snprintf(nm, sizeof nm, "%d %s", s + 1, dn);
        gfx_text(x + 8, t + drop + 2, nm, on ? col : C_DIM, FONT_SMALL);
        if (!on) /* muted: a red bar across the header */
            gfx_fill(x + w - 30, t + drop + 10, 20, 4, C_RED);
        /* the string itself, brighter while bowed by hand */
        int sy = t + drop + 42;
        draw_string(x + 12, x + w - 12, sy, on ? live.meter[s] : 0.f, live.phase[s],
                    gfx_mix(col, C_TEXT, 0.6f * live.hand_env[s] + 0.4f * live.flash[s][RING_PLUCK]), on, vkind_of(s));
        if (latch_on()) { /* LATCH: one board that fills while the string drones */
            draw_latch(s, x + 14, sy + 26, w - 28, FOOTER_Y - 62 - (sy + 26), col, sel, on);
            continue;
        }
        float r = 38.f, cx = x + w / 2.f;
        float pcy = sy + 20 + r + 12, bcy = pcy + 2 * r + 34;
        draw_ring(cx, pcy, r, P, RING_PLUCK, live.step[s][RING_PLUCK], live.flash[s][RING_PLUCK], col,
                  sel && ring == RING_PLUCK, !on);
        draw_ring(cx, bcy, r, P, RING_BOW, live.step[s][RING_BOW], P[SP_BON] ? live.bow_env[s] : live.hand_env[s], col,
                  sel && ring == RING_BOW, !on);
        /* when the bow ring is off the bow holds: a steady bar across the ring */
        if (!P[SP_BON] && on && P[SP_BOW] > 0)
            gfx_fill((int)(cx - r * 0.6f), (int)bcy - 1, (int)(r * 1.2f), 3, gfx_mix(C_BG, col, 0.5f));
        char lb[24];
        int wnd = engine_voice_wind(s), prc = engine_voice_perc(s);
        const char *pw = wnd ? "TONGUE" : prc ? "STRIKE" : "PLUCK";
        if (P[SP_PCHANCE] < 100)
            snprintf(lb, sizeof lb, "%s %d%%", pw, P[SP_PCHANCE]);
        else
            snprintf(lb, sizeof lb, "%s", pw);
        gfx_text((int)cx - gfx_text_w(lb, FONT_SMALL) / 2, (int)(pcy + r + 4), lb, sel && ring == 0 ? col : C_DIM, FONT_SMALL);
        snprintf(lb, sizeof lb, P[SP_BCHANCE] < 100 ? (wnd ? "BREATH %d%%" : "BOW %d%%") : (wnd ? "BREATH" : "BOW"),
                 P[SP_BCHANCE]);
        gfx_text((int)cx - gfx_text_w(lb, FONT_SMALL) / 2, (int)(bcy + r + 4), lb, sel && ring == 1 ? col : C_DIM, FONT_SMALL);
    }
}

/* the quiet view: five strings across the room under one long roof, each
 * with its rings small at the end; no words but the chord */
static void draw_quiet(void)
{
    slant(0, 70, SCREEN_W, 30, 3, gfx_mix(C_BG, C_COPPER, 0.6f));
    for (int s = 0; s < NS; s++) {
        int y = 120 + s * 72, on = song.on[s];
        const int *P = song.sp[s];
        draw_string_big(40, SCREEN_W - 150, y, on ? live.meter[s] : 0.f, live.phase[s], scol[s], on, 2.4f, vkind_of(s));
        draw_ring(SCREEN_W - 110, y, 20.f, P, RING_PLUCK, live.step[s][RING_PLUCK], live.flash[s][RING_PLUCK], scol[s], 0, !on);
        draw_ring(SCREEN_W - 50, y, 20.f, P, RING_BOW, live.step[s][RING_BOW], P[SP_BON] ? live.bow_env[s] : live.hand_env[s],
                  scol[s], 0, !on);
    }
    const char *cn = engine_chord_name();
    gfx_text((SCREEN_W - gfx_text_w(cn, FONT_SMALL)) / 2, SCREEN_H - 34, cn, C_DIM, FONT_SMALL);
}

/* the start-up screen: the house's roofline (splash.jdi, from
 * tools/mksplash.py) with the wordmark large on the siding below the
 * valley, its own two roofs answering the house's */
#define SPLASH_LOGO_H 92.f /* the wordmark's height on the start-up screen (read by tools/mksplash.py) */
static uint8_t *splash_px;
static uint32_t splash_pal[256];
static int splash_tried;

static void splash_load(void)
{
    splash_tried = 1;
    char p[3][512];
    snprintf(p[0], sizeof p[0], "%s/splash.jdi", preset_dir);
    snprintf(p[1], sizeof p[1], "splash.jdi");
    snprintf(p[2], sizeof p[2], "dist/Jaffedrone/splash.jdi");
    for (int i = 0; i < 3 && !splash_px; i++) {
        FILE *f = fopen(p[i], "rb");
        if (!f)
            continue;
        unsigned char hd[8], pal[768];
        if (fread(hd, 1, 8, f) == 8 && !memcmp(hd, "JDI1", 4) && (hd[4] | hd[5] << 8) == SCREEN_W &&
            (hd[6] | hd[7] << 8) == SCREEN_H && fread(pal, 1, 768, f) == 768) {
            for (int c = 0; c < 256; c++)
                splash_pal[c] = (uint32_t)pal[3 * c] << 16 | pal[3 * c + 1] << 8 | pal[3 * c + 2];
            splash_px = malloc(SCREEN_W * SCREEN_H);
            if (splash_px && fread(splash_px, 1, SCREEN_W * SCREEN_H, f) != SCREEN_W * SCREEN_H) {
                free(splash_px);
                splash_px = NULL;
            }
        }
        fclose(f);
    }
}

static void draw_splash(uint32_t *fb)
{
    if (!splash_tried)
        splash_load();
    /* centred on the screen, in the siding below the valley; the photo is
     * placed so the house's valley meets the wordmark's (tools/mksplash.py) */
    float h = SPLASH_LOGO_H, k = h / LOGO_H;
    float x = (SCREEN_W - LOGO_W * k) / 2, y = 262.f;
    if (splash_px)
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
            fb[i] = splash_pal[splash_px[i]];
    else { /* no picture: the two roofs drawn */
        gfx_clear(0x9db4f0);
        float xy[10] = {0, 40, 325, 248, 640, 92, 640, 480, 0, 480};
        gfx_poly(xy, 5, 0xdcdcd8);
    }
    draw_logo_a(x, y, h, 1.4f, C_SLATE, C_SLATE_D, 210); /* 82%: painted on, the boards faintly through */
    char v[24];
    snprintf(v, sizeof v, "v%s", JD_VERSION);
    gfx_text((SCREEN_W - gfx_text_w(v, FONT_SMALL)) / 2, (int)(y + h + 16), v, C_SLATE, FONT_SMALL);
}

void ui_draw(uint32_t *fb)
{
    gfx_target(fb);
    gfx_clear(C_BG);
    if (splash_ms > 0) {
        draw_splash(fb);
        return;
    }
    if (splash_px) { /* done with the picture */
        free(splash_px);
        splash_px = NULL;
    }
    if (page == PAGE_PLAY && quiet) {
        draw_quiet();
        draw_toast();
        return;
    }
    draw_header();
    switch (page) {
    case PAGE_PLAY: draw_play(); break;
    case PAGE_SOUND: draw_sound(); break;
    case PAGE_COMPOSE: draw_compose(); break;
    case PAGE_SONG: draw_song(); break;
    default: draw_mix(); break;
    }
    draw_footer();
    draw_toast();
}
