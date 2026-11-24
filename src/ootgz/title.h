/* Back to the game's title screen whatever it's doing, the way the game itself leaves play for it outside the pause
 * screen: an event ending with the result "Title" (FINDINGS 9.26). */
#ifndef TITLE_H
#define TITLE_H

#include <stdbool.h>

typedef enum {
  TITLE_IDLE,    /* nothing asked */
  TITLE_PENDING, /* asked: on the way (menus closing, a load finishing, an event ending) */
  TITLE_DONE,    /* Link is gone: the game is at (or fading to) its title screen */
  TITLE_FAILED,  /* didn't happen; title_status says why */
} title_status_t;

/* this build's offsets are known */
bool title_known(void);
/* game thread: start going back to the title screen; 0 if under way, else why not (English; gfx_notice translates) */
const char *title_start(void);
/* game thread, once a frame */
void title_frame(void);
/* on the way to the title screen: frame advance doesn't hold the game meanwhile (it has to run to leave play) */
bool title_going(void);
/* how the last title_start went; *text (may be 0): what happened, or why it failed */
title_status_t title_status(const char **text);

#endif
