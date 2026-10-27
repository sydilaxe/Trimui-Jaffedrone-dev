/* Pages, controls and drawing. Platform code feeds button edges and time. */
#pragma once
#include <stdint.h>

enum {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_X, BTN_Y,
    BTN_L1, BTN_R1, BTN_L2, BTN_R2,
    BTN_SELECT, BTN_START, BTN_MENU,
    BTN_COUNT
};

enum { PAGE_PLAY, PAGE_SOUND, PAGE_COMPOSE, PAGE_MIX, PAGE_SONG, PAGE_COUNT };

/* Set by the platform to serialise UI edits against the audio callback. */
extern void (*ui_lock)(void);
extern void (*ui_unlock)(void);

void ui_init(void);
void ui_set_dir(const char *dir); /* where presets are kept (default: here) */
void ui_button(int btn, int down);
void ui_tick(int ms);
void ui_draw(uint32_t *fb);
int ui_quit_requested(void);
int ui_take_changed(void);
/* the session in a few lines (presses, pages, adjustments), for the log */
void ui_summary(void (*out)(const char *line));
