/* debug > actors' placer: an actor chosen to spawn is placed first, as furniture is in The Sims on a console. The
 * free camera looks down on a spot on the ground, marked by a yellow beam and a ring with an arrow for the way the
 * actor will face; the left stick moves the spot over the ground, the right stick swings the camera round it, and
 * A has the game make the actor there. D-left/right go from one actor near it to the next, the camera following,
 * and Y kills the one locked on to or under the beam. Looking at actors is the same with nothing to place. Its
 * controls are shown at the top left in the game's button glyphs while it's on. */
#ifndef PLACER_H
#define PLACER_H

#include <stdbool.h>
#include <stdint.h>

extern bool placer_active;
/* the explorer's Y, take Link (settings explorer_take_link, on at first): going to a dungeon room takes Link into it,
 * which is what wakes it; off, only the camera goes and the room shows without its enemies */
extern bool placer_take_link;

/* place `name`, starting where the free camera looks if it's on, else in front of Link; closes nothing (the caller
 * closes the menu). False, with a notice, if there's no camera to look through. */
bool placer_start(const char *name);
/* the same with nothing to place (debug > actors > browse currently loaded actors): stepping from actor to actor, killing them;
 * d-left/right go through the actors of `kind` first (PLACER_ALL ...), which ZL / ZR change */
enum { PLACER_ALL, PLACER_ENEMIES, PLACER_NPCS, PLACER_OBJECTS, PLACER_KINDS };
extern const char *const placer_kind_names[PLACER_KINDS]; /* "all kinds", "enemies", "npcs", "objects" */
bool placer_look(int kind);
/* the same for places (the top menu's explorer): d-left/right go through the map's dungeon rooms, grottos and
 * interiors (data_places), ZL / ZR choose which, A goes there; no killing */
bool placer_explore(void);
void placer_stop(void); /* the camera back as it was */
/* browsing or placing: the actor the beam is on or locked on to (0: none, or not browsing), and the spot on the
 * ground the camera looks at (false: not browsing); for the overlays that draw round actors near Link, to draw round
 * these too */
uintptr_t placer_selected(void);
bool placer_spot(float out[3]);
/* from actorsmenu_build: an actor's options (minus while browsing, on a guard): its vision and patrol path */
void placer_menu_build(void);

/* each frame, game thread, before the overlays are built: the sticks and buttons while the menu is closed */
void placer_frame(bool menu_open);

/* game thread: the beam, ring and arrow as world lines, (from, to) and a colour (airwall.h AW_PLACER*); how many */
#define PLACER_MAX_LINES 96
int placer_lines(float ends[][2][3], uint8_t colours[], int max);

#endif
