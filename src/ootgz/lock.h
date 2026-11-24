/* Locks: Link's X (horizontal), Y (height) and Z (horizontal) held where the game writes them
 * (the patch's store stubs, z64hd.h). */
#ifndef LOCK_H
#define LOCK_H

#include <stdbool.h>

typedef enum { LOCK_X, LOCK_Y, LOCK_Z } lock_axis_t;

bool lock_available(void);             /* the build has the store stubs (z64hd_builds.h) */
bool lock_is_set(lock_axis_t axis);
void lock_set(lock_axis_t axis, bool on);
void lock_hold_set(bool on);
void lock_frame(void);                 /* the menu's locks let go when the map changes */
/* link > no void outs: the patch's stub makes the void-out decision return at once (FINDINGS 9.25). It
 * shares the lock stubs' word, so it lives here. */
bool lock_no_void_available(void);     /* the build has the decision's offset (z64hd_builds.h) */
bool lock_no_void_is_set(void);
void lock_no_void_set(bool on);

#endif
