/* Link's age and reloading the map (gz's "age" and "reload scene"). The game keeps the age in
 * Sequence_IsChildhood and most outdoor maps come in a _Child and an _Adult version, so a new age
 * takes effect when a map loads: the age switch only records it, the reload applies it. */
#ifndef AGE_H
#define AGE_H

#include <stdbool.h>

enum { AGE_CHILD, AGE_ADULT };

int age_current(void);   /* the game's, or -1 if unknown */
int age_wanted(void);    /* what the next reload gives (the game's until the switch is changed) */
void age_want(int age);
void age_frame(void);      /* frame hook, after warp_frame */
void age_reload_map(void); /* reload the map at the start point Link last arrived at, at age_wanted */
/* Load map `name` (any age's version, as named) at `last_start` with silent arrival (no arrival event or
 * camera), without altering the current age (memfile.c loads the snapshot's GameData). False if no start point
 * is known or the game refused the warp request immediately. */
bool age_warp_to_silent(const char *name, const char *last_start, bool have_pos);
/* whether map `name` has a version for `age` (X_Child <-> X_Adult; maps without an age in the name: both) */
bool age_map_has_version(const char *name, int age);

#endif
