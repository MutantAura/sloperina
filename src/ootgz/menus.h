/* The menu tree (port of ootmenu.pyw's build_menus). */
#ifndef MENUS_H
#define MENUS_H

#include "menu.h"

extern menu_t menu_root;
void menus_build(void);
void menus_splash(void); /* gz's start-up splash */

/* defined in ootgz.c */
extern bool frame_advance, frame_advance_in_pause, pause_display;
/* the watches, the input display and the log are hidden while the game's pause screen or map is open,
 * and while its item wheel is */
extern bool hide_in_game_menus, hide_over_item_wheel;
/* the world overlays (air walls, void out, save positions, the hookshot's hook, enemy health) are hidden while the
 * game's pause screen or map is open, and while its item wheel is only with this (off: its quick opening and closing
 * would make them flash) */
extern bool hide_world_over_item_wheel;
/* the game paused in a way that hides the world overlays (see above) */
bool world_overlays_hidden(void);

void watches_sync(void);         /* rebuild the watches menu if the list changed */
void menus_research_frame(void); /* debug > pause research: logging, every frame */

#endif
