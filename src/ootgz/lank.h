/* Lank and Lonk, beta (link > lank / lonk mode): Lank is child Link playing adult Link's animation sets, Lonk adult
 * Link playing the child's, so the model takes the other age's proportions, as in the cutscenes played at the wrong
 * age. Two game file patches (gamepatch.h), one on each age's Player pack; a mode (nothing, lank, lonk) is chosen,
 * then applied: that age's patch goes on, and the map Link is in is reloaded with him at the mode's age, whatever
 * age the map was made for (remake.h), which makes him again. Not kept in the settings: a beta that edits the game's
 * files in memory starts off. FINDINGS 9.35. */
#ifndef LANK_H
#define LANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { LANK_CHILD, LANK_ADULT };

/* the modes; LANK_MODES is how many */
enum { LANK_MODE_OFF, LANK_MODE_LANK, LANK_MODE_LONK, LANK_MODES };
extern const char *const lank_mode_names[LANK_MODES];

/* at module init, before gamepatch_hook_game */
void lank_init(void);
/* the link menu's "lank / lonk mode (beta)" submenu (left out if the file hooks couldn't be put in) */
struct menu;
void lank_menu_add(struct menu *link_menu);
int lank_mode_chosen(void);
void lank_mode_choose(int mode);
/* Make the chosen mode the one in force and reload the map with Link as it has him (remake.h); warps > override
 * age is set to the mode's age (and put back by nothing). True once the reload is on its way. */
bool lank_apply(void);
int lank_mode_applied(void);

/* In `data`, the `age`'s AnimationParam string table (its key ModelProjectName, then the sets' names: the child's six,
 * Demo, 01 to 04 and Cba; the adult's the same six plus 02_H and Player_Dark): give the six the other age's prefix
 * when `on`, their own when not. How many names it changed (0: they already were), or -1 when it isn't that age's
 * table. (lank.c's patch fix; here for the host tests.) */
int lank_swap_names(uint8_t *data, size_t len, int age, bool on);

#endif
