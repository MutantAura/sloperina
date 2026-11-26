/* Guard vision (FINDINGS 9.36): where the security guards (the Gerudo fortress's guards, Hyrule Castle's soldiers)
 * see Link and catch him, drawn on the ground round each, with the air walls' lines and faces (airwall.c). */
#ifndef GUARDVIEW_H
#define GUARDVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "airwall.h"

extern bool guardview_shown; /* scene > environment > guard vision */
extern bool guardview_paths; /* ... patrol paths: the rails the guards walk */

/* game thread, once a frame, before airwall_frame: finds the guards near Link and works out their sight */
void guardview_frame(void);
/* game thread: the outlines in the world, (from, to) and a colour (airwall.h AW_GUARD_*); how many */
#define GUARDVIEW_MAX_LINES 1400
int guardview_lines(float ends[][2][3], uint8_t colours[], int max);
/* game thread, after guardview_lines: the fills as faces (airwall.h); how many */
#define GUARDVIEW_MAX_FACES 1200
int guardview_faces(aw_face_t faces[], int max);
/* each guard's own switches (debug > actors > browse, its options): GV_VISION its sight, GV_PATH the rail it walks;
 * off until turned on, forgotten when it goes (a map change). guardview_shows: drawn now (its own switch, or the
 * menu's for every guard) */
enum { GV_VISION, GV_PATH };
bool guardview_is_guard(uintptr_t actor);
/* a bone's world matrix (3 rows of 4: the columns its axes, the last its position), as the game's AI gets it
 * (QueryModelGetBoneMatrix34f); false without that bone (or on a build without the calls) */
bool guardview_bone_matrix(uintptr_t actor, const char *bone, float m[12]);
/* a path to show: a guard's, or any actor's the layout links to a rail (guardview_rails_of: how many, 0 with none) */
bool guardview_has_path(uintptr_t actor);
int guardview_rails_of(uintptr_t actor);
bool guardview_own(uintptr_t actor, int what);
bool guardview_shows(uintptr_t actor, int what);
void guardview_toggle(uintptr_t actor, int what);
/* anything to draw: the menu's guard vision on, or a guard's own choice */
bool guardview_active(void);
/* the menu's line: "3 guards, 2 on duty   1 alerted" / "no guards near" */
void guardview_text(char *buf, int len);

#endif
