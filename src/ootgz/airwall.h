/* Air walls (port of airwall.py): which walls are in the current map, whether Link is inside one,
 * and their outlines projected with the game's camera, handed to the render thread to draw: as lines
 * (overlayui.c, on the GPU) or as dots and short rectangles (nvn.c's cleared rectangles, gz's style). */
#ifndef AIRWALL_H
#define AIRWALL_H

#include <stdbool.h>
#include <stdint.h>

extern bool airwalls_shown; /* draw them: the kinds below that are on */
extern bool airwalls_push;  /* the ones that push Link off (he can't stand on them) */
extern bool airwalls_block; /* the ones that stop him (invisible walls) */
extern bool airwalls_all;   /* the kinds that don't touch Link on foot (enemies, NPCs, animals, camera...) */
extern bool airwalls_xray;  /* also where Link is in front of them (else hidden behind him) */

/* a piece of an outline: a rectangle of the visible picture, 0..65535 each way, colour index. A side
 * thinner than the line width (a dot, or a straight piece of a line) is widened to it when drawn. */
/* slots, ocarina and load: savepos.c's; void: voidmap.c's */
enum { AW_BLOCKS, AW_INSIDE, AW_OTHER, AW_PUSHES, AW_SLOT_A, AW_SLOT_B, AW_LOAD, AW_OCARINA, AW_OCARINA_LOAD,
       AW_VOID_GROUND, AW_VOID_DEADLY, AW_VOID_RING, AW_VOID_SHIFT_OK, AW_VOID_SHIFT_FAIL, AW_VOID_AREA,
       AW_VOID_BOUNDS, AW_HOOK, AW_HOOK_IDLE, AW_HOOK_AIM, AW_PLACER, AW_PLACER_AIR, AW_PLACER_TARGET, AW_GUARD_SEARCH,
       AW_GUARD_ALERTED, AW_GUARD_CAUGHT, AW_GUARD_OFF, AW_GUARD_SEARCH_FAR, AW_GUARD_ALERTED_FAR, AW_GUARD_CAUGHT_FAR,
       AW_GUARD_OFF_FAR, AW_GUARD_PATH, AW_TALK, AW_TALK_IN, AW_TALK_LOCK, AW_ACT, AW_ACT_IN, AW_ACT_LOCK,
       AW_DOOR, AW_DOOR_IN, AW_HIT_ATTACK, AW_HIT_INVINCIBLE, AW_HIT_LINK, AW_HIT_BUMP, AW_HIT_HURT, AW_HIT_PICKUP, AW_HIT_GUARD, AW_RESPAWN, AW_NCOLORS };
/* hooks: hsstore.c's; placer: placer.c's; guard: guardview.c's; talk / act: interactview.c's; hit: hitview.c's;
 * respawn: drownview.c's */
typedef struct { uint16_t x0, y0, x1, y1; uint8_t color; } aw_dot_t;

/* the same outlines as lines, for the GPU (the game's style): ends 0..1 across the visible picture, the
 * nearer end's distance from the camera (metres), the ends' depths (as the game's depth buffer has them,
 * moved toward the camera by a margin: overlayui.c hides the lines where Link is nearer), colour index */
/* scene: the scenery hides it too, wherever it's nearer (the void lines; the walls' only hide behind Link) */
typedef struct { float x0, y0, x1, y1, near, d0, d1; uint8_t color; bool scene; } aw_line_t;

/* a see-through face in the world, for the GPU: the parallelogram corner, corner + u, corner + v, corner + u + v; or
 * with triangle only the triangle corner, corner + u, corner + v */
typedef struct { float corner[3], u[3], v[3]; uint8_t color; bool scene, triangle; } aw_face_t;

/* game thread, once per frame: lines for the GPU, or (gpu false) rectangles */
void airwall_frame(bool gpu);
/* render thread: once a frame, before the calls after it, takes the newest frame the game thread made (publish.h);
 * airwall_release when done with it */
void airwall_take(void);
/* the opacity the render thread draws the lines and faces at: 0 to 1, fading as the game pauses and unpauses */
extern volatile float airwall_fade;
/* the hitboxes' (hitview.c) on top of it: they fade out whenever the game pauses (the item wheel too, whatever
 * hide world overlays over item wheel says), as what they show stops with the game and the wheel turns its camera */
extern volatile float airwall_hit_fade;
const aw_dot_t *airwall_published(int *n);
const aw_line_t *airwall_lines(int *n);
void airwall_release(void);
/* render thread: Link's box with those lines: the rectangle round him (0..1 across and down the visible
 * picture) and the planes just in front of him, behind him and under him (the ground, lifted a little), each
 * as its depth on the picture: p[0] x + p[1] y + p[2] (x, y 0..1; a plane's depth is linear on the picture).
 * What of the scene is in the rectangle, between the front and the back and over the ground is taken for
 * Link. False: he isn't on the picture */
bool airwall_link_box(float rect[4], float front[3], float back[3], float ground[3]);
/* render thread: whether the frame's depth is wanted whatever Link's box says: a line the scenery hides, or
 * any face (they're only drawn through the depth's layer) */
bool airwall_needs_depth(void);
/* render thread: the faces to draw under those lines, and the camera they and the lines were worked out with */
struct view_cam;
const aw_face_t *airwall_faces(int *n, const struct view_cam **cam);
void airwall_summary(char *buf, int len);     /* "12 walls, 3 within 60" */
void airwall_link_text(char *buf, int len);   /* the air wall watch: depth inside, or "outside" */

#endif
