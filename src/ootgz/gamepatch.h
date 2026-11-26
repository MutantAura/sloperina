/* Changes to the game's own files, made at run time and switchable. Beta: it edits what the game reads and keeps in
 * memory (FINDINGS 9.35). A feature adds patches at module init (lank.c is one; remake.h shows changes that need
 * Link to be made again) and switches them with gamepatch_set; nothing else in the module needs to know of it.
 *
 * A patch names a file of the game's RomFS (a .zs file, usually a pack) and, optionally, a member of it (a SARC
 * archive's), and a fix that changes those bytes in place, never their size. Two ways in:
 *  - Reads: main's nn::fs file imports are wrapped. While a patch on a file is on, opening that file reads it here
 *    (decompressed with the game's dictionary), runs the fixes and answers the game's reads with the result, as one
 *    zstd frame of stored blocks (no dictionary, no compressor needed).
 *  - Copies the game keeps: the game holds on to what it took from a file and remakes things from that copy, so a
 *    file read before a switch stays as it was. A patch with an anchor (a 4-byte word at a 4-byte aligned place in
 *    its bytes, the same with the patch on or off) gets, when it's switched, its fix run at once on the copies found
 *    before, and a search of the process's memory, a piece each frame, that runs it round each anchor in regions up
 *    to max_region bytes; the copies the fix recognises are remembered for the next switch.
 * Add patches at module init (lank.c is one), before gamepatch_hook_game. */
#ifndef GAMEPATCH_H
#define GAMEPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gamepatch gamepatch_t;
struct gamepatch {
  const char *file;   /* full path, e.g. GAME_ROM "/Pack/Actor/Player_Child.pack.zs" */
  const char *member; /* the archive member to change, or 0 for the whole (decompressed) file */
  /* Make `data` (len bytes: the member, or memory round an anchor) agree with `on`, in place and the same size.
   * Returns how many changes it made (0: it already did), or -1 if `data` isn't the patch's (memory holds
   * anything: change only what's recognised). */
  int (*fix)(const gamepatch_t *patch, uint8_t *data, size_t len, bool on);
  uint32_t anchor;      /* 0: no search for kept copies */
  size_t before, after; /* how much memory round an anchor the fix is given */
  size_t max_region;    /* the search reads only memory regions up to this size (0: any); most memory is in a few
                         * large regions, so this decides how long a search takes */
  intptr_t user;        /* for the fix */
  bool on;              /* gamepatch_set's */
  bool read;            /* the game has opened the file since it started: a copy may be kept */
  uintptr_t known[4];   /* anchors of copies found (0: none) */
};

#define GAMEPATCH_MAX 8

/* at module init: wrap main's nn::fs file imports (open, size, read, close); 0, or why not */
const char *gamepatch_hook_game(uintptr_t main_base);
/* whether the wrappers are in: without them nothing can be changed */
bool gamepatch_available(void);
/* register a patch (static storage; off to start with); false when GAMEPATCH_MAX are in */
bool gamepatch_add(gamepatch_t *patch);
/* switch a patch: the game's next read of its file follows at once, copies it keeps once the search reaches them */
void gamepatch_set(gamepatch_t *patch, bool on);
/* a search for kept copies is under way */
bool gamepatch_searching(void);
/* game thread, once a frame: the next piece of the search */
void gamepatch_frame(void);

/* `data` as one zstd frame of stored blocks (single segment, the content size in its header, no dictionary), in a
 * block from aligned_alloc (free it); 0 if there's no memory */
uint8_t *gamepatch_stored_frame(const uint8_t *data, size_t size, size_t *size_out);

#endif
