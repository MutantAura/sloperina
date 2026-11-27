/* Hitboxes (FINDINGS 9.39): the game's physics shapes round Link and the actors near him, read live from the Havok
 * worlds each frame and drawn with the air walls' lines and faces (airwall.c): the attacks that can hurt Link (red),
 * the bodies that bump into him (grey; his own white), and over an actor that can be fought, or Link, while it's
 * invincible, its target shapes in blue. */
#ifndef HITVIEW_H
#define HITVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "airwall.h"

extern bool hitview_shown; /* scene > environment > hitboxes > show: the parts ticked under it */
/* HV_ATTACK attack sensors whose hit mask has Link in it; HV_BUMP solid character bodies that collide with Link (his
 * own with the characters); HV_INVINCIBLE an invincible Link's or fightable actor's target sensors; HV_HURT every
 * target sensor of Link and the characters (where they can be hit) while not invincible */
enum { HV_ATTACK, HV_BUMP, HV_INVINCIBLE, HV_HURT, HV_PARTS };
extern bool hitview_parts[HV_PARTS];

/* game thread, once a frame, before airwall_frame */
void hitview_frame(void);
bool hitview_active(void);
/* game thread: the outlines (from, to) and colours (airwall.h AW_HIT_*); how many */
#define HITVIEW_MAX_LINES 1100
int hitview_lines(float ends[][2][3], uint8_t colours[], int max);
/* game thread, after hitview_lines: the fills; how many */
#define HITVIEW_MAX_FACES 600
int hitview_faces(aw_face_t faces[], int max);
/* the menu's line: "4 attacks, 12 bodies near" */
void hitview_text(char *buf, int len);

#endif
