/* Memfiles (memfiles_v2): save and load snapshots using the game's own GameData binary serializer,
 * ResetGimmick (fast same-map reload), clean silent area reload, and direct AI camera traversal. */
#ifndef OOTGZ_MEMFILE_H
#define OOTGZ_MEMFILE_H

#include <stdbool.h>

#define MEMFILE_SLOTS 4

extern bool memfile_session;     /* restore unsaved session fields (memfilefields.h) too */
extern bool memfile_fast_reload; /* reset actors in place via ResetGimmick on same map & age */

int         memfile_slot(void);
void        memfile_set_slot(int s);
bool        memfile_save(int s);
bool        memfile_load(int s); /* true when the load started */
void        memfile_frame(void); /* frame hook (after age_frame) */
bool        memfile_busy(void);
void        memfile_slot_text(int s, char *buf, int len); /* for the memfiles menu */
const char *memfile_status(void);                         /* current save/load line, or "" */

#endif
