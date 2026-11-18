/* gz's save / load position: R + d-up saves where Link is and which way he faces, R + d-down puts him
 * back there (menu closed; also link > save / load position). In the same map only. Link is moved
 * through his physics record, which levitate.c finds and keeps (link_record_*).
 * Other modules (the macro, tas.c) keep slots of their own, so they don't overwrite the player's. */
#ifndef POSITION_H
#define POSITION_H

#include <stdbool.h>

/* a saved position: Link's drawn position, his facing as a unit (fx, fz), and the map it's in */
typedef struct {
  bool saved;
  double x, y, z, fx, fz;
  char map[64];
} position_slot_t;

void position_save(void); /* the shortcuts' slot */
void position_load(void);
/* a slot of the caller's own. Save: true when saved (no "position saved" notice; failures say why).
 * Load: true when the load started (it settles over a few frames: position_busy) */
bool position_save_to(position_slot_t *slot);
bool position_load_from(const position_slot_t *slot);
bool position_busy(void);      /* a load waiting for the buttons, for Link's record, or settling */
bool position_load_ok(void);   /* the last load that finished put Link there */
bool position_in_map(const position_slot_t *slot); /* the slot is in this map (or the map isn't known) */
/* frame hook, after levitate_frame: the shortcuts, and a load in progress */
void position_frame(bool menu_open);

#endif
