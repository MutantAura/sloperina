/* debug > free camera: the picture taken off Link and flown anywhere with the sticks, the world placed and
 * streamed around it the game's own way, as for an event's camera (FINDINGS 9.30). */
#ifndef FREECAM_H
#define FREECAM_H

#include <stdbool.h>

extern bool freecam_on;
bool freecam_set(bool on);          /* false, with a notice, if it can't be turned on now */
void freecam_frame(bool menu_open); /* each frame, game thread: its bind, the sticks, the world */

#endif
