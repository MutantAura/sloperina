/* Where Link voids out (FINDINGS §9.8): the game's ground map round Link, its deadly cells and the fence Link has
 * to stay in once there's no mapped ground underfoot, drawn in the world with the air walls' lines (airwall.c). */
#ifndef VOIDMAP_H
#define VOIDMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "airwall.h"

extern bool voidmap_shown;  /* draw it: the parts below that are on */
extern bool voidmap_ground; /* the edges of the mapped ground */
extern bool voidmap_deadly; /* the cells that void Link whatever is near */
extern bool voidmap_ring;   /* the fence round the ground point the game remembers */
extern bool voidmap_shift;  /* green / red ground: where the fence will or won't move */
extern bool voidmap_areas;  /* the areas placed in the map that void Link inside them (airwall.c draws them) */
extern bool voidmap_xray;   /* also where the scenery is in front of it (else hidden behind it) */

/* game thread, once a frame: logs each void the ground map causes, as its flag is set */
void voidmap_frame(void);
/* game thread: the lines in the world, (from, to) and a colour (airwall.h AW_VOID_*); how many */
#define VOIDMAP_MAX_LINES 900
int voidmap_lines(float ends[][2][3], uint8_t colours[], int max);
/* game thread, after voidmap_lines: the columns', fence's and shift ground's sides as faces (airwall.h); how many */
#define VOIDMAP_MAX_FACES 960
int voidmap_faces(aw_face_t faces[], int max);
/* the menu's line: "on the map", "off the map  6.8 of 8.0", "VOID  far from the ground" ... */
void voidmap_text(char *buf, int len);

#endif
