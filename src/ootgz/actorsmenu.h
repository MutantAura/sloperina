/* debug > actors: the running actors near Link, nearest first (gz's actors menu); choose one, then go to it, copy it or kill
 * it; browse every actor in the scene by kind; spawn any enemy, npc, object or item by name. */
#ifndef ACTORSMENU_H
#define ACTORSMENU_H

#include <stdint.h>

#include "menu.h"

menu_t *actorsmenu_build(void); /* from menus_build: the menu, for debug's submenu item */
void actorsmenu_frame(void);    /* each frame, game thread: reads the list again when the menu is entered */

/* game thread: the game asked to make `name` at `at`, facing yaw degrees (0: +z), then watched for until it appears
 * (with notices); false, with a notice, if the game refused it. The actor placer's A (placer.c). */
bool actorsmenu_spawn_at(const char *name, const float at[3], float yaw);
/* game thread: debug > actors > kill / despawn on `actor`, with its notices: the game's death request (enemyhp.h
 * actor_kill), and where it's refused, or taken but the actor's still running half a second later, despawned
 * (actor_despawn); true if it was killed or despawned. The actor placer's Y. */
bool actorsmenu_kill(uintptr_t actor);

#endif
