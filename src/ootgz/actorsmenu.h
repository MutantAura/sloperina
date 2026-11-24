/* debug > actors: the running actors near Link, nearest first (gz's actors menu); choose one, then go to it or kill it. */
#ifndef ACTORSMENU_H
#define ACTORSMENU_H

#include "menu.h"

menu_t *actorsmenu_build(void); /* from menus_build: the menu, for debug's submenu item */
void actorsmenu_frame(void);    /* each frame, game thread: reads the list again when the menu is entered */

#endif
