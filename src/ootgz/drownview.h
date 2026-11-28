/* Drowning (FINDINGS 9.47): the actors that drown in deep water and come back (cuccos, dogs, horses and Epona), their
 * drown timer watch (watches.h W_DROWN) and where they come back, drawn with the air walls' lines (airwall.c). */
#ifndef DROWNVIEW_H
#define DROWNVIEW_H

#include <stdbool.h>
#include <stdint.h>

/* what its AI does when it drowns: none, or the cucco's, the dog's or the horse's way (game thread; looked up again
 * every second) */
enum { DV_NONE, DV_CUCCO, DV_DOG, DV_HORSE };
int drownview_kind(uintptr_t actor);
static inline bool drownview_has(uintptr_t actor) { return drownview_kind(actor) != 0; }

/* where it comes back: a cucco where it last landed safely (its blackboard's WarpPos) or else its home, a dog at
 * home, a horse where it last landed (if that spot's safe, else the nearest safe one, found when it drowns) */
enum { DV_AT_NONE, DV_AT_HOME, DV_AT_WARP, DV_AT_LANDING, DV_AT_SAFE_SPOT /* none to show: not landed yet */ };
int drownview_respawn(uintptr_t actor, float out[3]);

/* the drown watches (watches.h W_DROWN_STATE, W_DROWN, W_FADE_OUT, W_RESPAWN): the actor they're for (0: none;
 * forgotten when it goes), and their values: DW_STATE in words ("idle", "drowning" or a cucco's "in the water",
 * "looking for land", "jump 2 of 3"; "fading out", "hidden", "finding a safe spot", "respawned", "jumped out"), and in
 * frames DW_DROWN until it starts to fade out (a cucco: its step's), DW_FADE the fade out (its length before it, what's
 * left during it), DW_RESPAWN until it's put back; "-" when idle */
enum { DW_DROWN, DW_FADE, DW_RESPAWN, DW_STATE };
void drownview_watch(uintptr_t actor);
uintptr_t drownview_watched(void);
void drownview_text(int which, char *buf, int len);

/* each actor's own switch (debug > actors > browse, its options): its respawn point drawn; forgotten when it goes */
bool drownview_own(uintptr_t actor);
void drownview_toggle(uintptr_t actor);
bool drownview_active(void);

/* game thread, once a frame, before airwall_frame */
void drownview_frame(void);
/* game thread: the respawn points' lines (from, to) and colours (AW_RESPAWN); how many */
#define DROWNVIEW_MAX_LINES 192
int drownview_lines(float ends[][2][3], uint8_t colours[], int max);

/* debugging: the actor's drowning module, counters and respawn point to the log (remote "aidump") */
void drownview_dump(uintptr_t actor);

#endif
