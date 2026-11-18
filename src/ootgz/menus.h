/* The menu tree (port of ootmenu.pyw's build_menus). */
#ifndef MENUS_H
#define MENUS_H

#include "menu.h"

extern menu_t menu_root;
void menus_build(void);
void menus_splash(void); /* gz's start-up splash */

/* defined in ootgz.c */
extern bool frame_advance, frame_advance_in_pause, pause_display;
/* the watches, the input display and the log are hidden while the game's pause screen or map is open */
extern bool hide_in_game_menus;
extern int frame_advance_button; /* 0 L, 1 R */
/* frame advance from elsewhere (the macro menu), game thread: pause turns frame advance on if it's off and
 * pauses as a tap would (this frame is let through, held from the next); run lets it run again (from a menu
 * item while held, it leaves the hold once the menu's done) */
void fa_pause_request(void);
void fa_run_request(void);
bool fa_is_paused(void);

void watches_sync(void);         /* rebuild the watches menu if the list changed */
void menus_research_frame(void); /* debug > pause research: logging, every frame */

#endif
