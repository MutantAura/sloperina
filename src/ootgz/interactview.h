/* Interaction ranges (FINDINGS 9.37): where Link can talk to or act on the actors near him (an NPC's Talk, a sign's
 * Check, a door's OpenDoor...), drawn on the ground round each with the air walls' lines and faces (airwall.c). */
#ifndef INTERACTVIEW_H
#define INTERACTVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "airwall.h"

extern bool interactview_shown; /* scene > environment > actors > interaction ranges */
/* its parts: IV_RADIUS where A works (talk, check, open...), IV_ZTARGET the further reach while Z-targeting, IV_BREAK
 * where each holds to once taken up (dashed: where it breaks walking away), IV_DOOR a swinging door's: where anyone
 * holds it open (Link has to leave it for the door to close behind him); interactview_parts: the menu's options */
enum { IV_RADIUS, IV_ZTARGET, IV_BREAK, IV_DOOR, IV_PARTS };
extern bool interactview_parts[IV_PARTS];

/* game thread, once a frame, before airwall_frame */
void interactview_frame(void);
/* game thread: the outlines in the world, (from, to) and a colour (airwall.h AW_TALK* / AW_ACT*); how many */
#define INTERACTVIEW_MAX_LINES 900
int interactview_lines(float ends[][2][3], uint8_t colours[], int max);
/* game thread, after interactview_lines: the fills (airwall.h faces); how many */
#define INTERACTVIEW_MAX_FACES 900
int interactview_faces(aw_face_t faces[], int max);
/* the menu's line: "6 near, 2 in range" */
void interactview_text(char *buf, int len);

/* an actor Link can talk to or act on (the game's files say so), and its own switches (the actor browser's options),
 * one a part, off until turned on, forgotten when it goes; interactview_shows: drawn now (its own, or the menu's) */
bool interactview_has(uintptr_t actor);
bool interactview_is_door(uintptr_t actor); /* has the IV_DOOR part */
/* it has that part: IV_RADIUS a client A works through, IV_ZTARGET a lock-on one (a chest has none), IV_BREAK any, IV_DOOR
 * a swinging door */
bool interactview_has_part(uintptr_t actor, int part);
/* the door close watch (watches.h W_DOOR_CLOSE): the door it's for (0: none; forgotten when it goes), and its value:
 * frames left before the door shuts (CloseWaitFrame while it's held open), "closed" with no door event running */
void interactview_door_watch(uintptr_t door);
uintptr_t interactview_door_watched(void);
void interactview_door_text(char *buf, int len);
/* the door's actions, each once, then the door goes on as it does: open it (its attention sender reserved for a
 * moment by the door itself, as an NPC does: the game's own opening, wherever Link is), and shut it now, even with
 * Link (or anyone) in the half disc that holds it open (once its counts are found: set to 0, with nobody counted near
 * for a moment) */
bool interactview_door_open(uintptr_t door);
/* whether it's open now (its AI's own IsOpenDoor: true from when it starts opening until it starts shutting): 1, 0,
 * or -1 if that can't be read */
int interactview_door_is_open(uintptr_t door);
float interactview_door_angle(uintptr_t door); /* its panel turned from shut, degrees; -1 unknown */
void interactview_door_close_now(uintptr_t door);
bool interactview_own(uintptr_t actor, int part);
bool interactview_shows(uintptr_t actor, int part);
void interactview_toggle(uintptr_t actor, int part);
/* what it's called in the menus, for this actor ("talk radius", "z-target talk", "target break line"; a sign's "check radius") */
const char *interactview_part_label(uintptr_t actor, int part);
/* anything to draw: the menu's option on, or an actor's own choice */
bool interactview_active(void);
/* "talk", "check", "talk, open door"...: what A does near it (its non-lock-on clients' actions), for the menu */
void interactview_actions(uintptr_t actor, char *buf, int len);

#endif
