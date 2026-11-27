/* Making Link again (beta; FINDINGS 9.35). The game makes Link only when his age changes or a file loads, and reads
 * what he's made of (his pack) then, so a change to it (a game file patch, gamepatch.h) shows from the next time.
 * remake_start does it now: it reloads the map Link is in, as it is named, at the age asked for (age.h's
 * age_reload_map_as, so in a map made for the other age too): one load when that changes his age, else two, through
 * the other age and back, after which he stands where he stood. Any feature that changes how Link is made can use
 * it; lank.c is one. */
#ifndef REMAKE_H
#define REMAKE_H

#include <stdbool.h>

/* Make Link again at `age` (AGE_CHILD / AGE_ADULT), once a search for kept copies of switched game files
 * (gamepatch.h) has ended and no event runs. False, with a notice, if it can't be started (no Link, or one is
 * under way). */
bool remake_start(int age);
/* started and not done yet */
bool remake_pending(void);
/* game thread, once a frame, after age_frame */
void remake_frame(void);

#endif
