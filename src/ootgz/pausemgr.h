/* The game's pause manager (FINDINGS 9.1): its pause types, each a request count; a type with a count above 0 stops
 * what its masks say. Raising one from outside works as the game's own request does. */
#ifndef PAUSEMGR_H
#define PAUSEMGR_H

#include <stdbool.h>
#include <stdint.h>

#define PAUSE_GAME 0xBA597383u   /* GamePause: the pause screen's and the item wheel's */
#define PAUSE_DEBUG 0x2D67E2DAu  /* DebugPause */
#define PAUSE_CLICK 0xFBAD2D6Au  /* ClickPause */

/* a type's ID from its name (MurmurHash3 of it) or hex; 0 if neither */
uint32_t pausemgr_id(const char *name_or_hex);
/* its request count (bit 31 left out), or -1 where the manager or the type can't be found */
int pausemgr_count(uint32_t id);
/* adds delta to its count (never below 0); false where it can't be found */
bool pausemgr_add(uint32_t id, int delta);
/* The world stopped while the game goes on drawing it and its camera runs (frame advance's camera tools, ootgz.c):
 * DebugPause raised, with one bit more than its own masks (FINDINGS 9.41: the third word's 0x2; DebugPause's own
 * stops the view too). Off: lowered, its masks as they were. False where the manager isn't found. */
bool pausemgr_freeze(bool on);
/* debugging: a type's three masks (what it stops) set; was: what they were (may be 0) */
bool pausemgr_set_masks(uint32_t id, const uint32_t masks[3], uint32_t was[3]);
/* remote "pausetype": the types and their counts, a line each */
int pausemgr_list(char *out, int len);

#endif
