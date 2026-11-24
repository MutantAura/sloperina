/* scene > time of day: the game's clock, shown and set (timeofday.c). */
#ifndef TIMEOFDAY_H
#define TIMEOFDAY_H

#include <stdbool.h>
#include <stdint.h>

#include "menu.h"

/* the seconds into the day (0..86399); false when there's no clock (not in play, or not known for this build) */
bool tod_get(uint32_t *seconds);
/* the clock set, as the game's ExecuteWmSetTime does; false when there's no clock */
bool tod_set(uint32_t seconds);
extern bool tod_frozen; /* the clock held at tod_frozen_at (set while ticked) */
/* game thread, once a frame: holds the clock while frozen */
void tod_frame(void);
/* the scene > time of day menu */
menu_t *tod_menu_build(void);

#endif
