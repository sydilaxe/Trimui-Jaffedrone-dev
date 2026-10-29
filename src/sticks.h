/* The Brick Pro's two analog sticks and the lights around them, worked out
 * apart from SDL so the test harness can check them.
 *
 * A stick seldom reaches the far end of its axis, and a round gate lets it
 * reach only about 70% of each axis on a diagonal. So each direction of
 * each axis learns how far this stick really goes, and the round gate is
 * stretched to a square: every control reaches its whole range, straight
 * or on a diagonal. */
#pragma once
#include <stdint.h>

enum { STK_LX, STK_LY, STK_RX, STK_RY, STK_AXES };

/* forget what was learned; reach: how far a stick is first taken to go
 * (a fraction of the axis, 0.3 .. 1), until it shows it goes further */
void sticks_reset(float reach);
void sticks_axis(int axis, int raw); /* an axis moved: -32768 .. 32767 */
/* the sticks now, -1 .. 1 each (up is negative, as SDL has it): the
 * centre's dead zone removed, the stick's reach stretched to full, the
 * diagonals reaching the corners */
void sticks_read(float out[STK_AXES]);
float sticks_reach(int axis, int dir); /* what has been learned (dir 0: negative, 1: positive) */

/* how hard the left stick bows, from how far up it is (0 .. 1): 0 for not
 * at all, then a light touch (0.1) to the engine's hardest (1.5, half
 * again as hard as R1) */
float stick_bow_pressure(float up);
/* a control played from a stick: at the centre the song's value (set),
 * pushed fully one way its max, the other way its min. *held is the value
 * playing (or below min when none yet); it moves only once the stick is
 * most of the way to the next step, so a stepped control (HARM) does not
 * flicker between two steps. Returns the value to play. */
int stick_control(int *held, int set, int min, int max, float amt);

/* the lights: an intensity (0 .. 1) as a colour of that brightness, with
 * a curve so that halfway looks about half as bright */
uint32_t stick_led_colour(uint32_t rgb, float intensity);
/* dir: the lights' folder (NULL: /sys/class/led_anim). 1 if the sticks'
 * lights were found there; what they showed is kept for leds_close. */
int leds_open(const char *dir);
/* show a colour on each stick's light (written only when it changes) */
void leds_show(uint32_t left, uint32_t right);
void leds_close(void); /* put back what they showed before */
