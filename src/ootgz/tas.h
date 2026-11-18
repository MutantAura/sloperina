/* The input macro (a first TAS tool): one macro of controller input, recorded frame by frame in memory and
 * played back through the game's own controller reads (input.c's wrappers). One input per frame, given to
 * every read the game makes in that frame; a frame runs from one frame hook to the next. Recording starts
 * with the game paused by frame advance, so it begins on a known frame; return to start puts Link back where
 * it began (position.c, a slot of the macro's own). The macro menu (menus.c) drives it. */
#ifndef TAS_H
#define TAS_H

#include <stdbool.h>
#include <stdint.h>

#include "input.h"

typedef struct {
  uint32_t buttons;        /* nn::hid NpadButton bits as the game saw them (input.h BTN_*), low 32 bits */
  int16_t  lx, ly, rx, ry; /* stick axes, the hid range fits in 16 bits */
} tas_frame_t;             /* 12 bytes */

#define TAS_FPS 60
#define TAS_MAX_FRAMES (TAS_FPS * 60 * 10) /* 10 minutes at 60 Hz */

typedef enum { TAS_IDLE, TAS_RECORDING, TAS_STOPPED, TAS_PLAYING } tas_state_t;

/* game thread: */
void tas_frame(void);          /* frame hook, once a frame, before input_update: commit / advance */
void tas_draw(void);           /* before gfx_frame (also in frame advance's held presents): the indicator */
void tas_record_start(void);   /* pauses the game first; recording starts on the next frame let through */
void tas_record_stop(void);
void tas_record_restart(void); /* while recording: discard the frames, Link back to the start, go on at 0 */
void tas_play(void);           /* from the cursor */
void tas_pause(void);          /* keeps the cursor */
void tas_rewind(void);         /* cursor to 0, Link back to the start */
uint32_t tas_length(void), tas_position(void);
tas_state_t tas_state(void);
bool tas_waiting(void);        /* Link being put back at the start: nothing recorded or played meanwhile */
extern bool tas_log_positions; /* log Link's position each macro frame ("tas: frame N x y z"): replay test */

/* input.c's wrappers, any thread: the `count` states of one read of overload `style` (NPAD_STYLE_*), after
 * input.c's own filtering. Records the first read of a frame (later reads get the same), or replaces the
 * states with the frame being played. */
void tas_filter(int style, npad_state_t *states, int count);

#endif
