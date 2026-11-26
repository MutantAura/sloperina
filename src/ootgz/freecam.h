/* debug > free camera: the picture taken off Link and flown anywhere with the sticks, the world placed and
 * streamed around it the game's own way, as for an event's camera (FINDINGS 9.30). */
#ifndef FREECAM_H
#define FREECAM_H

#include <stdbool.h>

extern bool freecam_on;
bool freecam_set(bool on);          /* false, with a notice, if it can't be turned on now */
void freecam_frame(bool menu_open); /* each frame, game thread: its bind, the sticks, the world */

/* The actor placer's view (placer.c): on (if it wasn't), with the eye and look-at it has now; false if there's no
 * camera. Then the placer's eye and look-at each frame, the sticks left alone, until the end: off again, or flying
 * on from there if it was on before. Turning the free camera off meanwhile ends the steering (freecam_steering). */
bool freecam_steer_begin(double eye_now[3], double at_now[3]);
void freecam_steer(const double eye[3], const double at[3]);
void freecam_steer_end(void);
bool freecam_steering(void);
/* R3 in the free camera, the explorer or the actor placer: their own drawing hidden (controls bar, beam, panel) */
extern bool freecam_ui_hidden;

#endif
