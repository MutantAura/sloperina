/* Making Link again (beta; FINDINGS 9.35). The game makes Link only when his age changes or a file loads, and reads
 * what he's made of (his pack) then, so a change to it (a game file patch, gamepatch.h) shows from the next time.
 * remake_request asks for that at the next reload map, which then goes through the other age and back: two loads,
 * after which his age, sword, shield and clothes are as before. Any feature that changes how Link is made can use
 * it; lank.c is one. */
#ifndef REMAKE_H
#define REMAKE_H

#include <stdbool.h>

/* ask (or stop asking) for Link to be made again at the next reload map */
void remake_request(bool on);
/* asked and not done yet (also while the two loads run) */
bool remake_pending(void);
/* age.c, at the start of a reload map, with the map and Link's age: may make it the first of the two loads */
void remake_reload_starting(const char *map, int age);
/* game thread, once a frame, after age_frame */
void remake_frame(void);

#endif
