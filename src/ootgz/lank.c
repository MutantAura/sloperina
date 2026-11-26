/* Lank and Lonk, beta (FINDINGS 9.35): two game file patches (gamepatch.h). Each age's pack has a
 * Component/AnimationParam whose AnimationResources name the age's animation sets; the patch gives the six sets both
 * ages have the other age's name (Child and Adult are the same length). The game reads the names when it makes Link
 * at that age, from the pack as read or as it kept it, so both are patched. The anchor is the first name of the
 * file's value strings ("Play", 4-byte aligned in a BYML string table), with the table's key a little before. The
 * copy the game makes Link from is the member alone, in a block of its own (64 KiB, seen at offset 0x124 of the
 * file), so the search reads only regions up to 1 MiB: about 1.1 GiB of 9.6. */
#include "lank.h"

#include <string.h>

#include "age.h"
#include "gamefile.h"
#include "gamepatch.h"
#include "gfx.h"
#include "lang.h"
#include "link.h"
#include "menu.h"
#include "remake.h"
#include "sys.h"

#define PREFIX_LEN 13 /* "Player_Child_", "Player_Adult_" */
static const char CHILD[] = "Player_Child_";
static const char ADULT[] = "Player_Adult_";
static const char KEY[] = "ModelProjectName";
static const char *const shared_sets[] = { "Demo_Animation", "01_Animation", "02_Animation",
                                           "03_Animation", "04_Animation", "Cba_Animation" };
static const char *const adult_only[] = { "Player_Adult_02_H_Animation", "Player_Dark_Animation" };
#define SHARED (int)(sizeof(shared_sets) / sizeof(*shared_sets))

/* whether the NUL-terminated `s` is at data[at] (within len) */
static bool string_at(const uint8_t *data, size_t len, size_t at, const char *s)
{
  size_t n = strlen(s) + 1;
  if (at > len || n > len - at)
    return false;
  for (size_t i = 0; i < n; i++)
    if (data[at + i] != (uint8_t)s[i])
      return false;
  return true;
}

static bool same_prefix(const uint8_t *data, const char *prefix)
{
  for (int i = 0; i < PREFIX_LEN; i++)
    if (data[i] != (uint8_t)prefix[i])
      return false;
  return true;
}

/* whether a shared set's name (either prefix) starts at data[at] */
static bool shared_name_at(const uint8_t *data, size_t len, size_t at)
{
  if (at > len || len - at < PREFIX_LEN || (!same_prefix(data + at, CHILD) && !same_prefix(data + at, ADULT)))
    return false;
  for (int k = 0; k < SHARED; k++)
    if (string_at(data, len, at + PREFIX_LEN, shared_sets[k]))
      return true;
  return false;
}

int lank_swap_names(uint8_t *data, size_t len, int age, bool on)
{
  size_t key = 0;
  while (key < len && !string_at(data, len, key, KEY))
    key++;
  if (key >= len)
    return -1;
  /* the strings after the key: the shared names, and whether the adult's own are among them */
  size_t names[2 * SHARED];
  int n = 0;
  bool adult_list = false;
  for (size_t at = key + sizeof(KEY); at < len; at++) {
    if (data[at - 1] != 0)
      continue; /* only where a string starts */
    if (shared_name_at(data, len, at)) {
      if (n == 2 * SHARED)
        return -1; /* more than one table's worth: not what this expects */
      names[n++] = at;
    }
    for (size_t k = 0; k < sizeof(adult_only) / sizeof(*adult_only); k++)
      adult_list |= string_at(data, len, at, adult_only[k]);
  }
  if (n != SHARED || (adult_list ? LANK_ADULT : LANK_CHILD) != age)
    return -1;
  const char *want = (age == LANK_CHILD) == on ? ADULT : CHILD; /* the child's list swapped, or the adult's not */
  int changed = 0;
  for (int i = 0; i < n; i++)
    if (!same_prefix(data + names[i], want)) {
      memcpy(data + names[i], want, PREFIX_LEN);
      changed++;
    }
  return changed;
}

static int fix(const gamepatch_t *patch, uint8_t *data, size_t len, bool on)
{
  return lank_swap_names(data, len, (int)patch->user, on);
}

#define ANCHOR 0x79616C50u /* "Play", read as a little-endian word */

static gamepatch_t patches[2] = {
  { .file = GAME_ROM "/Pack/Actor/Player_Child.pack.zs",
    .member = "Component/AnimationParam/Player_Child.engine__component__AnimationParam.bgyml",
    .fix = fix, .anchor = ANCHOR, .before = 128, .after = 320, .max_region = 1u << 20,
    .user = LANK_CHILD },
  { .file = GAME_ROM "/Pack/Actor/Player_Adult.pack.zs",
    .member = "Component/AnimationParam/Player.engine__component__AnimationParam.bgyml",
    .fix = fix, .anchor = ANCHOR, .before = 128, .after = 320, .max_region = 1u << 20,
    .user = LANK_ADULT },
};

const char *const lank_mode_names[LANK_MODES] = { "off", "lank & lonk" };

static int chosen, applied;

void lank_init(void)
{
  gamepatch_add(&patches[0]);
  gamepatch_add(&patches[1]);
}

int lank_mode_chosen(void)
{
  return chosen;
}

void lank_mode_choose(int mode)
{
  if (mode >= 0 && mode < LANK_MODES)
    chosen = mode;
}

int lank_mode_applied(void)
{
  return applied;
}

/* the map reloaded at once (link > scene interaction > reload map), so Link is made again with the files as patched */
static void reload_now(void)
{
  log_printf("lank / lonk: reloading the map");
  age_reload_map();
  menu_close(); /* so the reload can be watched */
}

void lank_apply(void)
{
  if (!gamepatch_available())
    return;
  if (chosen == applied) {
    if (remake_pending())
      reload_now(); /* applied, but not reloaded yet */
    else
      gfx_notice("already applied");
    return;
  }
  applied = chosen;
  bool swap = applied == LANK_MODE_SWAP;
  gamepatch_set(&patches[0], swap);
  gamepatch_set(&patches[1], swap);
  bool made = link_actor() != 0;
  remake_request(made); /* before a file is loaded, Link will be made with it anyway */
  log_printf("lank / lonk: %s applied", lank_mode_names[applied]);
  if (made)
    reload_now();
  else
    gfx_notice("applied: it shows when a file is loaded");
}

/* --- the menu ------------------------------------------------------------------------------------ */

static menu_t m_lank;

static int get_mode(menu_item_t *it) { return lank_mode_chosen(); }
static void set_mode(menu_item_t *it, int mode) { lank_mode_choose(mode); }
static void act_apply(menu_item_t *it) { lank_apply(); }

static void text_blank(menu_item_t *it, char *buf, int len)
{
  buf[0] = 0;
}

static void text_applied(menu_item_t *it, char *buf, int len)
{
  snprintf(buf, len, TF("applied: %s%s"), T(lank_mode_names[applied]),
           remake_pending() ? T(", once the map is reloaded") : "");
}

void lank_menu_add(menu_t *link_menu)
{
  if (!gamepatch_available())
    return; /* the file hooks couldn't be put in */
  menu_init(&m_lank, "lank / lonk mode (beta)", 6);
  menu_item_t *mode = menu_add_option(&m_lank, "mode", LANK_MODES, lank_mode_names, get_mode, set_mode, 0);
  if (mode)
    mode->hint = "lank & lonk: each age plays the other's animations (a tall child, a small adult)";
  menu_add_action(&m_lank, "apply", act_apply, 0); /* reloads the map itself */
  menu_add_return(&m_lank);
  menu_add_text(&m_lank, text_blank, 0);
  menu_add_text(&m_lank, text_applied, 0);
  menu_add_submenu(link_menu, "lank / lonk mode (beta)", &m_lank);
}
