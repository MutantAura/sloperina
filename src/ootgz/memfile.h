/* Memfiles (memfiles_v2): save and load snapshots using the game's own GameData binary serializer,
 * ResetGimmick (fast same-map reload), clean silent area reload, and direct AI camera traversal. */
#ifndef OOTGZ_MEMFILE_H
#define OOTGZ_MEMFILE_H

#include <stdbool.h>

#define MEMFILE_SLOTS     5
#define MEMFILE_NAME_MAX  30 /* an exported memfile's name (its file: <name>.szm) */
#define MEMFILE_FILES_MAX 64 /* .szm files listed for import */
#define MEMFILE_EXT       ".szm"
#define MEMFILE_DIR_NAME  "memfiles" /* in sloperinaGZ/ on the SD card */

extern bool memfile_session;     /* restore unsaved session fields (memfilefields.h) too */
extern bool memfile_fast_reload; /* reset actors in place via ResetGimmick on same map & age */

int         memfile_slot(void);
void        memfile_set_slot(int s);
bool        memfile_save(int s);
bool        memfile_load(int s); /* true when the load started */
void        memfile_frame(void); /* frame hook (after age_frame) */
bool        memfile_busy(void);
void        memfile_scan_start(void); /* memfiles are in use: start the background walk the fast load needs */
void        memfile_slot_text(int s, char *buf, int len); /* for the memfiles menu */
const char *memfile_status(void);                         /* current save/load line, or "" */

/* sharing memfiles as .szm files in sloperinaGZ/memfiles (only when asked: nothing is written on its own) */
bool        memfile_used(int s);
const char *memfile_slot_map(int s);                      /* the slot's map (the export's first name), or "" */
bool        memfile_export(int s, const char *name);      /* write slot s as <name>.szm */
int         memfile_list_files(void);                     /* list the .szm files; how many */
const char *memfile_file_name(int i);                     /* from the last list, without .szm */
bool        memfile_import(int i, int s);                 /* read listed file i into slot s */

#endif
