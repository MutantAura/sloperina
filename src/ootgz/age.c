/* Age and map reload.
 *
 * Reload: the current map (Sequence_CurrentBanc) through the warp code's event request, at the start
 * point Link last arrived at (Sequence_ReservedNextStartPosName keeps it) if the map has it, else the
 * map's first entrance an exit leads to.
 *
 * New age: Sequence_IsChildhood, the map's other-age version (X_Child <-> X_Adult; maps without one,
 * like the dungeons, are shared), and the sword and shield swapped with PlayerStatus.AnotherEraEquip*,
 * which hold the other age's (the Temple of Time does the same). Adult-only tunics and boots are taken
 * off for a child. Nothing is applied to Link until the map loads.
 *
 * Override age (warps menu): a map whose GameBancParam has IsForceSwitchChildOrAdult sets the age on entry; the load
 * script (LoadBancImpl) and the scene setup (main+0x9B76A0 on 1.1.1) both skip that while
 * DebugParam.IsSetChildhoodManually is on, and the load script turns it off after reading it, so it is written
 * every frame from the warp until the new map has settled. */
#include "age.h"

#include "binds.h"
#include "data.h"
#include "gamedata.h"
#include "gfx.h"
#include "link.h"
#include "remake.h"
#include "sys.h"
#include "warp.h"

#define F_IS_CHILDHOOD 0xCEA220D3u /* Sequence_IsChildhood */
#define F_OTHER_WEAPON 0xA1F7EE2Bu /* PlayerStatus.AnotherEraEquipWeapon */
#define F_OTHER_SHIELD 0x2342B619u /* PlayerStatus.AnotherEraEquipShield */
#define F_SET_CHILD_MANUALLY 0x86724737u /* DebugParam.IsSetChildhoodManually */

/* how long the override stays on: frames after the game reserved the map (a load measured ~640 on 1.1.1),
 * and at most from the warp */
#define OVERRIDE_AGE_AFTER_RESERVED 900
#define OVERRIDE_AGE_MAX_FRAMES     2400

static int wanted_age = -1; /* -1: follow the game's */
static int override_age = -1; /* -1: disabled, the map decides */
static bool holding_override;
static uint32_t hold_started, hold_reserved; /* frames; hold_reserved 0 until the warp is done */

int age_current(void)
{
  bool child;
  if (!gd_get_bool(F_IS_CHILDHOOD, &child))
    return -1;
  return child ? AGE_CHILD : AGE_ADULT;
}

int age_wanted(void)
{
  return wanted_age >= 0 ? wanted_age : age_current();
}

void age_want(int age)
{
  wanted_age = age == AGE_ADULT ? AGE_ADULT : AGE_CHILD;
}

/* --- string helpers ---------------------------------------------------------------------------- */

static bool str_equal(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static bool starts_with(const char *str, const char *prefix)
{
  while (*prefix && *str == *prefix) {
    str++;
    prefix++;
  }
  return !*prefix;
}

static const char *or_unknown(const char *str)
{
  return str ? str : "?";
}

/* --- maps -------------------------------------------------------------------------------------- */

static const warp_map_t *find_map(const char *name)
{
  for (int i = 0; i < data_n_warp_maps; i++)
    if (str_equal(data_warp_maps[i].name, name))
      return &data_warp_maps[i];
  return 0;
}

/* The version of map `name` for `age`, into out: X_Child <-> X_Adult (X_Adult_Alternative -> X_Child).
 * False if the map has an age and its other version isn't known. */
static bool age_version(const char *name, int age, char *out, int len)
{
  const char *age_suffix = name;
  for (const char *p = name; *p; p++)
    if (starts_with(p, "_Child") || starts_with(p, "_Adult")) {
      age_suffix = p;
      break;
    }
  if (age_suffix == name) { /* no age in the name: shared by both */
    snprintf(out, len, "%s", name);
    return true;
  }
  bool is_child_map = starts_with(age_suffix, "_Child");
  if ((age == AGE_CHILD) == is_child_map) {
    snprintf(out, len, "%s", name);
    return true;
  }
  snprintf(out, len, "%.*s%s", (int)(age_suffix - name), name, age == AGE_CHILD ? "_Child" : "_Adult");
  return find_map(out) != 0;
}

bool age_map_has_version(const char *name, int age)
{
  char out[GD_STR_LEN];
  return age_version(name, age, out, sizeof(out));
}

/* The spawn to reload map at: the start point Link last arrived at, else an entrance an exit leads
 * to, else the map's first. */
static const spawn_t *find_reload_spawn(const warp_map_t *map, const char *last_start)
{
  const spawn_t *spawns = &data_spawns[map->first];
  if (last_start && last_start[0])
    for (int i = 0; i < map->n; i++)
      if (str_equal(spawns[i].pos, last_start))
        return &spawns[i];
  for (int i = 0; i < map->n; i++)
    if (spawns[i].starter)
      return &spawns[i];
  return map->n ? &spawns[0] : 0;
}

/* --- age --------------------------------------------------------------------------------------- */

static void swap_strings(uint32_t hash_a, uint32_t hash_b)
{
  const char *value_a = gd_get_str(hash_a);
  const char *value_b = gd_get_str(hash_b);
  if (!value_a || !value_b)
    return;
  char copy_a[GD_STR_LEN], copy_b[GD_STR_LEN];
  snprintf(copy_a, sizeof(copy_a), "%s", value_a);
  snprintf(copy_b, sizeof(copy_b), "%s", value_b);
  gd_set_str(hash_a, copy_b);
  gd_set_str(hash_b, copy_a);
}

static void take_off_adult_only(uint32_t slot)
{
  const char *item = gd_get_str(slot);
  if (item && starts_with(item, "Armor_Adult_"))
    gd_set_str(slot, "");
}

/* The age the save data had before a reload changed it, with the adult-only clothes that took off;
 * -1 once the reload has happened. The data is changed before the map loads, so a reload that never
 * happens has to put it back. */
static int undo_age = -1;
static char undo_tunic[GD_STR_LEN], undo_boots[GD_STR_LEN];

static void apply_age(int age)
{
  log_printf("age: %s (sword %s, shield %s)", age == AGE_CHILD ? "child" : "adult",
             or_unknown(gd_get_str(GD_STR_SWORD)), or_unknown(gd_get_str(GD_STR_SHIELD)));
  swap_strings(GD_STR_SWORD, F_OTHER_WEAPON);
  swap_strings(GD_STR_SHIELD, F_OTHER_SHIELD);
  if (age == AGE_CHILD) {
    take_off_adult_only(GD_STR_TUNIC);
    take_off_adult_only(GD_STR_BOOTS);
  }
  gd_set_bool(F_IS_CHILDHOOD, age == AGE_CHILD);
}

/* --- reload ------------------------------------------------------------------------------------ */

/* A map the warp list doesn't know. Static: warp_arm keeps the pointers until the map has changed. */
static warp_map_t unlisted_map;
static spawn_t unlisted_spawn;
static char unlisted_name[GD_STR_LEN], unlisted_pos[GD_STR_LEN];

static bool reload_target(const char *name, const char *last_start, bool allow_fallback_pos,
                          const warp_map_t **map_out, const spawn_t **spawn_out)
{
  const warp_map_t *map = find_map(name);
  const spawn_t *spawn = 0;
  if (map) {
    spawn = find_reload_spawn(map, last_start);
  } else if ((last_start && last_start[0]) || allow_fallback_pos) {
    /* a map the warps don't list (houses, shops...): its last start point, arriving as from a
     * warp song (no starter actor needed) */
    snprintf(unlisted_name, sizeof(unlisted_name), "%s", name);
    snprintf(unlisted_pos, sizeof(unlisted_pos), "%s", (last_start && last_start[0]) ? last_start : "StartPos_00");
    unlisted_map = (warp_map_t){ unlisted_name, unlisted_name, WC_OTHER, 0, 0, 0 };
    unlisted_spawn = (spawn_t){ unlisted_pos, unlisted_pos, "Dm_SY_BancChangeArrive", 0 };
    map = &unlisted_map;
    spawn = &unlisted_spawn;
  }
  *map_out = map;
  *spawn_out = spawn;
  return map && spawn;
}

void age_reload_map(void)
{
  const char *current_map = gd_get_str(GD_STR_MAP);
  const char *last_start = gd_get_str(GD_STR_SPAWN);
  int current_age = age_current();
  if (!current_map || !current_map[0] || current_age < 0) {
    gfx_notice("can't reload: game data not found");
    return;
  }
  remake_reload_starting(current_map, current_age); /* may make this the first of two loads (remake.h) */
  int age = age_wanted();
  char name[GD_STR_LEN];
  if (!age_version(current_map, age, name, sizeof(name))) {
    gfx_notice(age == AGE_CHILD ? "this map has no child version" : "this map has no adult version");
    return;
  }
  log_printf("reload: %s -> %s, last start point \"%s\", age %d -> %d", current_map, name,
             last_start ? last_start : "?", current_age, age);
  const warp_map_t *map;
  const spawn_t *spawn;
  if (!reload_target(name, last_start, false, &map, &spawn)) {
    gfx_notice("can't reload: no start point known for this map");
    return;
  }
  if (age != current_age) {
    if (undo_age < 0) {
      undo_age = current_age;
      snprintf(undo_tunic, sizeof(undo_tunic), "%s", gd_get_str(GD_STR_TUNIC) ? gd_get_str(GD_STR_TUNIC) : "");
      snprintf(undo_boots, sizeof(undo_boots), "%s", gd_get_str(GD_STR_BOOTS) ? gd_get_str(GD_STR_BOOTS) : "");
    } else if (age == undo_age) {
      undo_age = -1; /* a second reload, back to the age the map is loaded at */
    }
    apply_age(age);
  }
  wanted_age = -1; /* from now on, follow the game's again */
  warp_arm(map, spawn);
}

bool age_warp_to_silent(const char *name, const char *last_start, bool have_pos)
{
  const warp_map_t *map;
  const spawn_t *spawn;
  if (!name || !name[0] || !reload_target(name, last_start, have_pos, &map, &spawn))
    return false;
  log_printf("reload: %s, start point \"%s\" (memfile load)", name, spawn->pos);
  wanted_age = -1; /* the caller's game data says the age */
  undo_age = -1;
  return warp_arm_silent(map, spawn);
}

int age_override(void)
{
  return override_age;
}

void age_set_override(int age)
{
  override_age = age == AGE_CHILD || age == AGE_ADULT ? age : -1;
}

static void hold_override(bool on)
{
  if (holding_override == on)
    return;
  holding_override = on;
  gd_set_bool(F_SET_CHILD_MANUALLY, on);
  log_printf("override age: override %s", on ? "on" : "off");
}

void age_prepare_warp(void)
{
  if (override_age < 0) {
    hold_override(false);
    return;
  }
  int current_age = age_current();
  if (current_age < 0) {
    gfx_notice("override age: game data not found");
    return;
  }
  if (override_age != current_age) {
    if (undo_age < 0) {
      undo_age = current_age;
      snprintf(undo_tunic, sizeof(undo_tunic), "%s", gd_get_str(GD_STR_TUNIC) ? gd_get_str(GD_STR_TUNIC) : "");
      snprintf(undo_boots, sizeof(undo_boots), "%s", gd_get_str(GD_STR_BOOTS) ? gd_get_str(GD_STR_BOOTS) : "");
    } else if (override_age == undo_age) {
      undo_age = -1;
    }
    apply_age(override_age);
  }
  wanted_age = -1;
  hold_started = frame_counter();
  hold_reserved = 0;
  holding_override = false;
  hold_override(true);
}

/* keep the override on until the new map has settled, or the warp was given up */
static void override_age_frame(void)
{
  if (!holding_override)
    return;
  uint32_t now = frame_counter();
  if (warp_abandoned() || now - hold_started > OVERRIDE_AGE_MAX_FRAMES) {
    hold_override(false);
    return;
  }
  if (!hold_reserved && !warp_pending() && warp_requested())
    hold_reserved = now ? now : 1;
  if (hold_reserved && now - hold_reserved > OVERRIDE_AGE_AFTER_RESERVED) {
    hold_override(false);
    return;
  }
  gd_set_bool(F_SET_CHILD_MANUALLY, true); /* the load script turns it off once it has read it */
}

void age_frame(void)
{
  override_age_frame();
  if (cmd_pressed(CMD_RELOAD_MAP)) { /* unbound unless given buttons (settings > rebind commands) */
    gfx_logf("reload map (%s)", age_wanted() == AGE_ADULT ? "adult" : "child");
    age_reload_map();
  }
  if (undo_age < 0 || warp_pending())
    return;
  if (warp_abandoned() && age_current() >= 0 && age_current() != undo_age) {
    apply_age(undo_age);
    gd_set_str(GD_STR_TUNIC, undo_tunic);
    gd_set_str(GD_STR_BOOTS, undo_boots);
    gfx_notice("age change undone: the map didn't reload");
  }
  undo_age = -1;
}
