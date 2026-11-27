/* Hitboxes (FINDINGS 9.39): the game's physics shapes round Link and the actors near him, read live from the Havok
 * worlds each frame and drawn with the air walls' lines and faces (airwall.c), in gz's hit view's colours: the attacks
 * that can hurt Link now (red, gz's AT), where Link and the actors can be hit (blue, gz's AC; purple while Link or a
 * foe is invincible) and the bodies that bump into him and his own (white, gz's OC). */
#ifndef HITVIEW_H
#define HITVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "airwall.h"

extern bool hitview_shown; /* scene > environment > hitboxes > show: the parts ticked under it */
/* only what's switched on (and a sensor with a contact collection): HV_ATTACK attack sensors whose hit mask has Link in
 * it; HV_BUMP solid character bodies that collide with Link (his own with the characters); HV_INVINCIBLE an invincible
 * Link's or fightable actor's target sensors; HV_HURT the other target sensors */
enum { HV_ATTACK, HV_BUMP, HV_INVINCIBLE, HV_HURT, HV_PARTS };
/* each part's style: off, its outline, or filled */
enum { HV_OFF, HV_OUTLINE, HV_FILL, HV_STYLES };
extern int hitview_style[HV_PARTS];
extern bool hitview_link_only; /* show: only Link's own (his body, his sword...) */
extern bool hitview_xray;      /* also through walls and scenery (else hidden by whatever is nearer) */
extern bool hitview_guard;     /* show: where Link's raised shield blocks from, while he guards (green) */
/* an actor's own switches (the actor browser's options), one per part (HV_ATTACK...): that part of its hitboxes shown
 * whatever the menu's show says, in the menu's style for it (filled where that's off); forgotten when it goes */
bool hitview_own(uintptr_t actor, int part);
void hitview_toggle(uintptr_t actor, int part);

/* game thread, once a frame, before airwall_frame */
void hitview_frame(void);
bool hitview_active(void);
/* game thread: the outlines (from, to) and colours (airwall.h AW_HIT_*); how many */
#define HITVIEW_MAX_LINES 1100
int hitview_lines(float ends[][2][3], uint8_t colours[], int max);
/* game thread, after hitview_lines: the fills; how many */
#define HITVIEW_MAX_FACES 2400
int hitview_faces(aw_face_t faces[], int max);
/* the menu's line: "4 attacks, 12 bodies near" */
void hitview_text(char *buf, int len);
/* debugging (remote hitlist): the shapes found last frame, a line each */
int hitview_list(char *out, int len);
/* debugging (remote "hitlist <name>"): every body of the actors with `name` in their name, drawn or not */
int hitview_list_all(const char *name, char *out, int len);

#endif
