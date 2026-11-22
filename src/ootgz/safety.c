/* The game's safety section (FINDINGS 9.14).
 *
 * GameSafetyMgr is one of the current scene's components (9.3, entry 28 each time it was looked at), told by its
 * vtable (z64hd vt_safety_mgr), as savepos.c finds AutoSaveMgr. It keeps the name hash of the GameSafetySetting it
 * has applied at +0x54 and that setting's switches packed into 16 bits at +0x50 (the same in both builds). */
#include "safety.h"

#include "sys.h"
#include "z64hd.h"

/* the scene and its components, as savepos.c reads them */
#define SCENE_CURRENT 0x1F0    /* [scene manager +this]: the current scene */
#define SCENE_COUNT 0x50       /* u32 */
#define SCENE_COMPONENTS 0x58  /* pointer to the components' pointers */
#define SCENE_COMPONENTS_MAX 64
#define SAFETY_ENTRY 28        /* where it was each time; looked for in the others too */
#define RESCAN_CALLS 30        /* a scene without it is looked through again after this many calls */

#define MGR_FLAGS 0x50         /* u16 */
#define MGR_NAME_HASH 0x54     /* u32 */

/* MurmurHash3_32 (seed 0) of each name: RSDB GameSafetySetting.Product.111, all 28 rows */
static const struct { uint32_t hash; const char *name; } settings[] = {
  { 0xEDB12E71, "Common_DisableWarp" },
  { 0x59F280EC, "Common_ShutterEnemy" },
  { 0x48DB8265, "Default" },
  { 0xE5D9EDDF, "Defeat_Ganon" },
  { 0x66E4A71E, "Defeat_Ganon_InBattle" },
  { 0xE8DAFE6C, "DoorOfTime" },
  { 0xE873CE22, "Get_Epona" },
  { 0xE4C584E6, "Get_StoneOfFire" },
  { 0x628DE7A6, "Get_StoneOfForest" },
  { 0x0AA38424, "Get_StoneOfWater" },
  { 0xDE5C4369, "Get_StoneOfWater_MiddleBoss" },
  { 0x900CA0A5, "Market_Item_Bottle_TutorialEvent" },
  { 0x3408E01B, "MiniGame" },
  { 0xBC5CF193, "MiniGame_PossibleWarp" },
  { 0x6DD278F5, "MiniGame_PossibleWarp_DiggingHole" },
  { 0xFAE899F0, "MiniGame_TimeStop" },
  { 0x533999F4, "SageOfFire" },
  { 0x5A2BE4B5, "SageOfForest" },
  { 0x4A5C7197, "SageOfForest_Step06" },
  { 0x408DE331, "SageOfShadow" },
  { 0x96F9FB4E, "SageOfShadow_BattleOnShip" },
  { 0xD7709AE7, "SageOfShadow_BeforeBossRoom" },
  { 0x6371358D, "SageOfSpirit" },
  { 0x5CDFDEBA, "SageOfWater" },
  { 0x9D4AD373, "SageOfWater_MiddleBoss" },
  { 0x81FBC769, "SubEvent_AllBossBattle" },
  { 0x7CAEC227, "SubEvent_GetStormSong" },
  { 0xBF657439, "SubEvent_GetStormSong_BeforeBossRoom" },
};

/* short, so a setting's flags fit a watch's value; in bit order */
static const struct { uint16_t bit; const char *word; } flag_words[] = {
  { SAFETY_MINIGAME, "mini" },                   /* not menu text */
  { SAFETY_ACTIVE, "on" },                       /* not menu text */
  { SAFETY_CHECK_AREA, "area" },                 /* not menu text */
  { SAFETY_DISABLE_SAVE, "save" },               /* not menu text */
  { SAFETY_DISABLE_WARP, "warp" },               /* not menu text */
  { SAFETY_DISABLE_AUTO_PLACEMENT, "place" },    /* not menu text */
  { SAFETY_DISABLE_TREASURE_BOX, "box" },        /* not menu text */
  { SAFETY_DISABLE_GRAB, "grab" },               /* not menu text */
  { SAFETY_DISABLE_BOOT, "boot" },               /* not menu text */
  { SAFETY_DISABLE_TIME, "time" },               /* not menu text */
  { SAFETY_SAVE_WHEN_END, "end" },               /* not menu text */
  { SAFETY_DISABLE_LOADING_PAUSE, "pause" },     /* not menu text */
  { SAFETY_NO_TEMP_SAVE_ERROR, "err" },          /* not menu text */
};

const char *safety_setting_name(uint32_t name_hash)
{
  for (unsigned i = 0; i < sizeof(settings) / sizeof(*settings); i++)
    if (settings[i].hash == name_hash)
      return settings[i].name;
  return 0;
}

void safety_flags_text(uint16_t flags, char *buf, int len)
{
  int used = 0;
  unsigned rest = flags;
  if (len > 0)
    buf[0] = 0;
  for (unsigned i = 0; i < sizeof(flag_words) / sizeof(*flag_words); i++) {
    if (!(flags & flag_words[i].bit))
      continue;
    used += snprintf(buf + used, used < len ? len - used : 0, "%s%s", used ? " " : "", flag_words[i].word);
    rest &= ~(unsigned)flag_words[i].bit;
  }
  if (rest) /* bits with no known meaning (1, 2, 4) */
    used += snprintf(buf + used, used < len ? len - used : 0, "%s+0x%x", used ? " " : "", rest);
  if (!used)
    snprintf(buf, len, "0");
}

static uintptr_t cached_scene, cached_mgr;
static int rescan_wait;

/* GameSafetyMgr of the current scene, or 0. The scene and the manager are kept while they stay the same. */
static uintptr_t safety_mgr(void)
{
  if (!z64hd_game || !z64hd_game->scene_mgr_ptr || !z64hd_game->vt_safety_mgr)
    return 0;
  uintptr_t scene_mgr = sys_ptr(MAIN(z64hd_game->scene_mgr_ptr));
  uintptr_t scene = scene_mgr ? sys_ptr(scene_mgr + SCENE_CURRENT) : 0;
  if (!scene)
    return 0;
  uintptr_t want_vtable = MAIN(z64hd_game->vt_safety_mgr);
  if (scene == cached_scene) {
    if (cached_mgr && sys_ptr(cached_mgr) == want_vtable)
      return cached_mgr;
    if (!cached_mgr && rescan_wait > 0) { /* not there a moment ago: don't look every call */
      rescan_wait--;
      return 0;
    }
  }
  cached_scene = scene;
  cached_mgr = 0;
  rescan_wait = RESCAN_CALLS;
  uint32_t count;
  uintptr_t components = sys_ptr(scene + SCENE_COMPONENTS);
  if (!components || !sys_read32(scene + SCENE_COUNT, &count) || count > SCENE_COMPONENTS_MAX)
    return 0;
  for (uint32_t k = 0; k <= count; k++) {
    uint32_t i = k == 0 ? SAFETY_ENTRY : k - 1; /* entry 28 first, then each of the others */
    if (i >= count || (k && i == SAFETY_ENTRY))
      continue;
    uintptr_t component = sys_ptr(components + i * sizeof(uintptr_t));
    if (component && sys_ptr(component) == want_vtable) {
      cached_mgr = component;
      log_printf("safety: GameSafetyMgr %#lx (scene entry %u)", (unsigned long)component, (unsigned)i);
      break;
    }
  }
  return cached_mgr;
}

bool safety_addresses(uintptr_t *flags, uintptr_t *name_hash)
{
  uintptr_t mgr = safety_mgr();
  if (!mgr)
    return false;
  *flags = mgr + MGR_FLAGS;
  *name_hash = mgr + MGR_NAME_HASH;
  return true;
}

bool safety_state(uint16_t *flags, uint32_t *name_hash)
{
  uintptr_t flags_addr, hash_addr;
  uint32_t packed;
  if (!safety_addresses(&flags_addr, &hash_addr) || !sys_read32(flags_addr, &packed) ||
      !sys_read32(hash_addr, name_hash))
    return false;
  *flags = (uint16_t)packed; /* the low half; little-endian */
  return true;
}
