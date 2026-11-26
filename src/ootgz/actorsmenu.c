/* debug > actors (actorsmenu.h): gz's actors menu for this game.
 *
 * browse currently loaded actors: the free camera over a beam, stepping from actor to actor (placer.c), killing
 * them (actorsmenu_kill: the game's death request, or its delete request for an NPC or thing with no death) and
 * going to them. quick spawn makes the likeliest of each kind, or any enemy, NPC, object or item the game has a
 * pack for (actornames.c, its "all" lists), loading it if it must: through the game's create request, placed with
 * the sticks or in front of Link. */
#include "actorsmenu.h"

#include <stdio.h>
#include <string.h>

#include "actornames.h"
#include "age.h"
#include "enemyhp.h"
#include "gfx.h"
#include "lang.h"
#include "link.h"
#include "osk.h"
#include "placer.h"
#include "position.h"
#include "sys.h"

#define FILTER_MAX 31

static menu_t m_actors, m_common;

/* --- names ------------------------------------------------------------------------------------- */

static bool starts_with(const char *s, const char *prefix)
{
  for (; *prefix; s++, prefix++)
    if (*s != *prefix)
      return false;
  return true;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c; }

/* `piece` anywhere in `name`, any case; "" is in every name */
static bool has_piece(const char *name, const char *piece)
{
  for (const char *start = name; *start; start++) {
    int k = 0;
    while (piece[k] && lower(start[k]) == lower(piece[k]))
      k++;
    if (!piece[k])
      return true;
  }
  return !*piece;
}

/* a death the game took, watched: an actor still running this many frames later (an NPC whose AI pays it no heed)
 * is despawned */
#define DEATH_WATCH 30
static struct {
  uintptr_t actor;
  int frames;
} death_watch;

static void despawn(uintptr_t actor, const char *why)
{
  switch (actor_despawn(actor)) {
    case KILL_DELETED:
      gfx_notice(why);
      break;
    case KILL_DELETE_REFUSED:
      gfx_notice("the game refused to despawn it");
      break;
    case KILL_GONE:
      break; /* gone after all */
    default:
      gfx_notice("not that one: only enemies, npcs and things");
      break;
  }
}

static void death_watch_frame(void)
{
  if (!death_watch.actor || --death_watch.frames > 0)
    return;
  actor_read_t a;
  if (actor_read(death_watch.actor, &a) && a.running) {
    log_printf("actors: %s still running %d frames after its death request: despawned", a.name, DEATH_WATCH);
    despawn(death_watch.actor, "despawned (it wouldn't die)");
  }
  death_watch.actor = 0;
}

bool actorsmenu_kill(uintptr_t actor)
{
  actor_kill_t result = actor_kill(actor);
  switch (result) {
    case KILL_DEATH:
      gfx_notice("killed");
      death_watch.actor = actor; /* despawned if it pays the death no heed */
      death_watch.frames = DEATH_WATCH;
      break;
    case KILL_DEATH_REFUSED: /* a boss, or one whose death ignores this reason: despawned */
    case KILL_NOT_ALLOWED:
      despawn(actor, "despawned (it won't die that way)");
      return true;
    case KILL_DELETED:
      gfx_notice("despawned (it has no death of its own)");
      break;
    case KILL_DELETE_REFUSED:
      gfx_notice("the game refused to despawn it");
      break;
    case KILL_GONE:
      gfx_notice("that actor has gone");
      break;
    case KILL_UNKNOWN:
      gfx_notice("kill: not known for this game version");
      break;
  }
  return result == KILL_DEATH || result == KILL_DELETED;
}

/* spawn: placed with the sticks first (placer.c), or straight away in front of Link; each through the create
 * request's handle, so the actor made is known exactly (for remove spawned, and to make a horse one's own). The
 * placer can ask again before the last one is made, so a few requests are followed at once. */
#define SPAWN_AHEAD 2.0     /* metres in front of Link */
#define SPAWN_WAIT 600      /* frames to wait for it (its pack may have to load) */
#define SPAWN_SLOTS 8       /* requests followed at once */
#define OWN_WAIT 120        /* frames for a horse to start running, to be made one's own */
#define SPAWNED_MAX 64      /* remove spawned: the actors made here, kept while they're in the map */
#define CALM_FRAMES 60      /* an own horse's rodeo bit is cleared this long after (its setup sets it again) */

float atan2f(float y, float x); /* from sdk */

static int spawn_where; /* 0: placed with the sticks, 1: in front of Link */
static const char *const spawn_where_names[] = { "placed with the sticks", "in front of Link" };

static struct {
  char name[64];
  int frames;       /* since asked; 0: the slot is free */
  uintptr_t handle; /* the game writes the request's status here: it stays put until made or failed */
  bool own_horse;   /* epona (owned): made one's own horse once it runs */
} spawning[SPAWN_SLOTS];
static bool placing_own_horse; /* the placer's actor is epona (owned): each one it makes becomes one's own */
static uintptr_t spawned[SPAWNED_MAX];
static int n_spawned;
static uintptr_t owning, calming;
static int own_left, calm_left;

static void remember(uintptr_t actor)
{
  actor_read_t a;
  int n = 0;
  for (int i = 0; i < n_spawned; i++) /* only those still in the map */
    if (actor_read(spawned[i], &a))
      spawned[n++] = spawned[i];
  n_spawned = n;
  if (n_spawned < SPAWNED_MAX)
    spawned[n_spawned++] = actor;
}

static bool spawn_request(const char *name, const float at[3], float yaw, bool own_horse)
{
  int slot = 0;
  while (slot < SPAWN_SLOTS && spawning[slot].frames)
    slot++;
  if (slot == SPAWN_SLOTS) {
    gfx_notice("still waiting for the last ones");
    return false;
  }
  snprintf(spawning[slot].name, sizeof(spawning[slot].name), "%s", name);
  spawning[slot].own_horse = own_horse;
  uint32_t result;
  if (!actor_spawn_handle(spawning[slot].name, at, yaw, &spawning[slot].handle, &result)) {
    char text[96];
    snprintf(text, sizeof(text), "the game refused to make it (%u)", (unsigned)result);
    gfx_notice(text);
    return false;
  }
  spawning[slot].frames = 1;
  gfx_notice("spawning");
  return true;
}

bool actorsmenu_spawn_at(const char *name, const float at[3], float yaw)
{
  return spawn_request(name, at, yaw, placing_own_horse);
}

/* the game asked to make `name`: placed first, or in front of Link, facing him; spawn_frame follows the request */
static void spawn_named(const char *name, bool own_horse)
{
  if (spawn_where == 0) {
    placing_own_horse = own_horse;
    if (placer_start(name))
      menu_close(); /* to place it */
    return;
  }
  vec3f_t link;
  double fx = 0, fz = 1;
  if (!link_drawn_pos(&link)) {
    gfx_notice("spawn: Link not found");
    return;
  }
  link_facing(&fx, &fz);
  float at[3] = { (float)(link.x + fx * SPAWN_AHEAD), (float)link.y, (float)(link.z + fz * SPAWN_AHEAD) };
  float yaw = atan2f((float)fx, (float)fz) * (180.0f / 3.14159265f) + 180.0f; /* facing Link */
  if (spawn_request(name, at, yaw, own_horse))
    menu_close(); /* so it can be watched */
}

/* epona (owned): once she runs, registered as the owned horse and her rodeo bit cleared (FINDINGS 9.34) */
static void own_frame(void)
{
  if (calm_left > 0) {
    calm_left--;
    actor_calm_horse(calming);
  }
  if (!own_left)
    return;
  actor_read_t a;
  if (!actor_read(owning, &a)) {
    own_left = 0;
    gfx_notice("the horse went before it was yours");
  } else if (a.running) {
    own_left = 0;
    bool owned = actor_own_horse(owning);
    actor_calm_horse(owning);
    calming = owning, calm_left = CALM_FRAMES;
    gfx_notice(owned ? "spawned: your horse" : "spawned, but the game didn't take it as your horse");
  } else if (--own_left == 0) {
    gfx_notice("spawned, but it didn't start in time to be yours");
  }
}

static void spawn_frame(void)
{
  own_frame();
  for (int i = 0; i < SPAWN_SLOTS; i++) {
    if (!spawning[i].frames)
      continue;
    uintptr_t made;
    spawn_poll_t now = actor_spawn_poll(&spawning[i].handle, &made);
    if (now == SPAWN_MADE) {
      log_printf("actors: %s made after %d frames: %#lx", spawning[i].name, spawning[i].frames, (unsigned long)made);
      remember(made);
      spawning[i].frames = 0;
      if (spawning[i].own_horse)
        owning = made, own_left = OWN_WAIT;
      else
        gfx_notice("spawned");
    } else if (now == SPAWN_FAILED) {
      log_printf("actors: %s: the request failed after %d frames", spawning[i].name, spawning[i].frames);
      gfx_notice("the game couldn't make it");
      spawning[i].frames = 0;
    } else if (++spawning[i].frames > SPAWN_WAIT) {
      actor_spawn_cancel(&spawning[i].handle);
      log_printf("actors: %s not made in %d frames", spawning[i].name, SPAWN_WAIT);
      gfx_notice("it wasn't made in time");
      spawning[i].frames = 0;
    }
  }
}

static int look_kind; /* PLACER_ALL ... */

static void act_look(menu_item_t *it)
{
  if (placer_look(look_kind))
    menu_close();
}

static int get_look_kind(menu_item_t *it) { return look_kind; }
static void set_look_kind(menu_item_t *it, int choice) { look_kind = choice; }

static int get_spawn_where(menu_item_t *it) { return spawn_where; }
static void set_spawn_where(menu_item_t *it, int choice) { spawn_where = choice; }

/* --- quick spawn: short lists of the likeliest ones, by kind ------------------------------- */

typedef struct {
  const char *label;
  const char *child, *adult; /* the actor for each age (adult 0: the same) */
  bool own_horse;
} common_t;

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

static const common_t animals[] = {
  { "epona (owned)", "Animal_Epona_Adult", 0, true },
  { "young epona", "Animal_Epona_Child", 0, false },
  { "horse", "Animal_Horse", 0, false },
  { "cucco", "Animal_Cucco", 0, false },
  { "bug", "Animal_Insect", 0, false },
  { "cow", "Animal_Cow", 0, false },
  { "dog", "Animal_Dog_White", 0, false },
  { "cat", "Animal_Cat", 0, false },
  { "frog", "Animal_Frog_Red", 0, false },
  { "butterfly", "Animal_Butterfly", 0, false },
  { "fish", "Animal_Fish_Bass", 0, false },
};
static const common_t enemies[] = {
  { "deku baba", "Enemy_Dekubaba", 0, false },     { "skulltula", "Enemy_Skulltula", 0, false },
  { "keese", "Enemy_Keese", 0, false },            { "octorok", "Enemy_Octorok", 0, false },
  { "stalchild", "Enemy_Stalchild", 0, false },    { "wolfos", "Enemy_Wolfos", 0, false },
  { "stalfos", "Enemy_Stalfos", 0, false },        { "lizalfos", "Enemy_Lizalfos", 0, false },
  { "redead", "Enemy_ReDead", 0, false },          { "like like", "Enemy_LikeLike", 0, false },
};
static const common_t characters[] = {
  { "saria", "Npc_Kokiri_Saria_Child", "Npc_Kokiri_Saria_Adult", false },
  { "mido", "Npc_Kokiri_Mido_Child", "Npc_Kokiri_Mido_Adult", false },
  { "malon", "Npc_Hylia_Malon_Child", "Npc_Hylia_Malon_Adult", false },
  { "talon", "Npc_Hylia_Talon_Child", "Npc_Hylia_Talon_Adult", false },
  { "zelda", "Npc_Hylia_Zelda_Child", "Npc_Hylia_Zelda_Adult", false },
  { "darunia", "Npc_Goron_Darunia_Child", "Npc_Goron_Darunia_Adult", false },
  { "ruto", "Npc_Zora_Ruto_Child_001", "Npc_Zora_Ruto_Adult_001", false },
  { "impa", "Npc_Hylia_Impa_Child", 0, false },
  { "sheik", "Npc_Hylia_Sheik_Adult", 0, false },
  { "kaepora gaebora", "Npc_Kaepora", 0, false },
};
static const common_t objects[] = {
  { "bomb", "Obj_Bomb", 0, false },                 { "bombchu", "Obj_Bombchu", 0, false },
  { "bomb flower", "Obj_BombFlower_A_01", 0, false }, { "pot", "Obj_Pot_A_01", 0, false },
  { "crate", "Obj_BrokenBox_A_01", 0, false },      { "push block", "Obj_MovableBox_A_01", 0, false },
  { "liftable rock", "Obj_LiftRock_A_01", 0, false }, { "sign", "Obj_SignBoard_A_01", 0, false },
  { "hookshot target", "Obj_HookShotTarget_A_01", 0, false },
  { "song of time block", "Obj_SongOfTimeBlock_A_01", 0, false },
};
static const common_t pickups[] = {
  { "heart", "Obj_Heart", 0, false },               { "fairy", "Obj_Fairy", 0, false },
  { "green rupee", "Obj_Rupee_Green", 0, false },   { "red rupee", "Obj_Rupee_Red", 0, false },
  { "deku stick", "Obj_DekuStick", 0, false },      { "deku nut", "Obj_DekuNut", 0, false },
  { "deku seeds", "Obj_DekuSeed", 0, false },       { "arrows", "Obj_ArrowBundle_Small", 0, false },
  { "magic jar", "Obj_MagicJar_Large", 0, false },
};

static const struct {
  const char *title;
  const common_t *list;
  int count;
  const char *note; /* a line under the list, or 0 */
  int all;          /* its full list under the picks (lists, below), or -1: actornames.c has no Animal_ */
  int more;         /* a submenu of another full list between them (character equipment), or -1 */
} commons[] = {
  { "animals", animals, N(animals), 0, -1, -1 },
  { "enemies", enemies, N(enemies), 0, 0, -1 },
  { "characters", characters, N(characters), "spawns might affect flags/save states/etc", 1, 4 },
  { "objects", objects, N(objects), 0, 2, -1 },
  { "pickups", pickups, N(pickups), 0, 3, -1 },
};
#define N_COMMONS N(commons)
static menu_t m_common_list[N_COMMONS], m_equipment;

/* the full lists: every actor whose name starts with prefix (actornames.c) but not with skip */
static const struct {
  const char *prefix, *skip;
  const char *heading; /* above it, or 0 (a menu of its own) */
  menu_t *menu;
} lists[] = {
  { "Enemy_", 0, "all enemies", &m_common_list[1] },
  { "Npc_", "Npc_Equipment_", "all characters", &m_common_list[2] }, /* the equipment has its own menu */
  { "Obj_", 0, "all objects", &m_common_list[3] },
  { "Item_", 0, "all items", &m_common_list[4] },
  { "Npc_Equipment_", 0, 0, &m_equipment },
};
#define N_LISTS N(lists)

static void act_common(menu_item_t *it)
{
  const common_t *c = (const common_t *)it->data;
  spawn_named(c->adult && age_current() == AGE_ADULT ? c->adult : c->child, c->own_horse);
}

static void act_remove_spawned(menu_item_t *it)
{
  int removed = 0;
  for (int i = 0; i < n_spawned; i++)
    removed += actor_remove(spawned[i]);
  n_spawned = 0;
  calm_left = own_left = 0;
  char text[64];
  snprintf(text, sizeof(text), "%d removed", removed);
  gfx_notice(text);
}

static void text_note(menu_item_t *it, char *buf, int len) { snprintf(buf, len, "%s", (const char *)it->data); }

/* --- each kind's full list (actornames.c): a window of ALL_ROWS on the names with its prefix and the filter's
 * piece, scrolled with the arrows above and below it (held: keeps going), the scroll bar beside it as the log's */

#define ALL_ROWS 5 /* as the watches and the log: the list and its arrows fit the menu with the cursor on them */
#define ALL_MATCH_MAX 1024 /* the most of a kind: objects, 997 */

static struct {
  char filter[FILTER_MAX + 1];
  char filter_label[FILTER_MAX + 24];
  uint16_t match[ALL_MATCH_MAX]; /* actor_names indices */
  int n, top;
} full[N_LISTS];

static void full_match(int g)
{
  full[g].n = 0;
  for (int i = 0; i < actor_names_count && full[g].n < ALL_MATCH_MAX; i++)
    if (starts_with(actor_names[i], lists[g].prefix) && !(lists[g].skip && starts_with(actor_names[i], lists[g].skip)) &&
        has_piece(actor_names[i], full[g].filter))
      full[g].match[full[g].n++] = (uint16_t)i;
  full[g].top = 0;
  snprintf(full[g].filter_label, sizeof(full[g].filter_label), "name has: %s",
           full[g].filter[0] ? full[g].filter : "anything");
}

static int full_rows(int g) { return full[g].n < ALL_ROWS ? full[g].n : ALL_ROWS; }

/* the window kept in range, and the scroll bar told where it is */
static void full_window(int g)
{
  int most = full[g].n > ALL_ROWS ? full[g].n - ALL_ROWS : 0;
  full[g].top = full[g].top < 0 ? 0 : full[g].top > most ? most : full[g].top;
  lists[g].menu->inner_items = full[g].n > full_rows(g) ? full[g].n : full_rows(g);
  lists[g].menu->inner_top = full[g].top;
}

static void fill_list_menu(int g);

static void full_filter_typed(const char *text, intptr_t g)
{
  snprintf(full[g].filter, sizeof(full[g].filter), "%s", text ? text : "");
  full_match((int)g);
  fill_list_menu((int)g); /* as many rows as it found, the arrows only if they're needed */
}

/* it->data: the list (lists) */
static void act_full_filter(menu_item_t *it)
{
  osk_open("actor name has", full[it->data].filter, FILTER_MAX, full_filter_typed, it->data);
}

/* it->data: the list * 2, + 1 for the arrow below; the line above is plain text when they all fit */
static void text_full_arrow(menu_item_t *it, char *buf, int len)
{
  int g = (int)(it->data >> 1);
  full_window(g);
  int last = full[g].top + ALL_ROWS < full[g].n ? full[g].top + ALL_ROWS : full[g].n;
  if (it->data & 1)
    snprintf(buf, len, "%c", GFX_ICON_DOWN);
  else if (!full[g].n)
    snprintf(buf, len, TF("none with \"%s\" in the name"), full[g].filter);
  else if (full[g].n <= ALL_ROWS)
    snprintf(buf, len, TF("%d found"), full[g].n);
  else
    snprintf(buf, len, "%c  %d-%d / %d", GFX_ICON_UP, full[g].top + 1, last, full[g].n);
}

static bool full_arrow_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  *start = 0;
  *len = 1; /* the arrow alone */
  return true;
}

static void full_scroll(menu_item_t *it, int part)
{
  int g = (int)(it->data >> 1);
  full[g].top += it->data & 1 ? 1 : -1;
  full_window(g);
}

/* it->data: the list * ALL_ROWS + the row */
static int full_index(menu_item_t *it)
{
  int g = (int)(it->data / ALL_ROWS), at = full[g].top + (int)(it->data % ALL_ROWS);
  return at < full[g].n ? full[g].match[at] : -1;
}

static void text_full_row(menu_item_t *it, char *buf, int len)
{
  int i = full_index(it);
  snprintf(buf, len, "%s", i >= 0 ? actor_names[i] : "");
}

static void act_full_row(menu_item_t *it)
{
  int i = full_index(it);
  if (i >= 0)
    spawn_named(actor_names[i], false);
}

static void add_full_arrow(menu_t *m, int g, int below)
{
  bool scrolls = full[g].n > ALL_ROWS;
  menu_item_t *arrow = scrolls ? menu_add_action(m, "", 0, g * 2 + below) : menu_add_text(m, 0, g * 2 + below);
  if (!arrow)
    return;
  arrow->text = text_full_arrow;
  arrow->indent = 2; /* as the names, the scroll bar left of them */
  arrow->log_label = "";
  if (!scrolls) /* the count alone: nothing to scroll */
    return;
  arrow->cursor = full_arrow_cursor;
  arrow->press = full_scroll;
  arrow->press_repeat = true;
  arrow->hint = below ? "L: scroll down the list (held: keeps going)" : "L: scroll up the list (held: keeps going)";
}

/* the filter, the count (the arrow up), a row for each name in the window and the arrow down */
static void add_full_list(menu_t *m, int g)
{
  if (lists[g].heading)
    menu_add_heading(m, lists[g].heading);
  menu_item_t *item = menu_add_action(m, full[g].filter_label, act_full_filter, g);
  if (item) {
    item->hint = "L: type a piece of the name (any case), e.g. wolfos, tsubo, rupee";
    item->log_label = "";
  }
  add_full_arrow(m, g, 0);
  m->inner_first = m->n_items;
  for (int row = 0; row < full_rows(g); row++) {
    item = menu_add_action(m, "", act_full_row, g * ALL_ROWS + row);
    if (item) {
      item->text = text_full_row;
      item->indent = 2; /* a level in from name has: room for the scroll bar */
      item->hint = "L: the game makes it, placed with the sticks or in front of Link (spawn where)";
      item->log_label = "";
    }
  }
  m->inner_rows = m->n_items - m->inner_first;
  /* the panel kept as wide as the longest name it can show (menu.c, gfx), found or not, so it doesn't change
   * size as the list scrolls or the filter changes */
  m->inner_widest = 0;
  m->inner_indent = 2;
  for (int i = 0; i < actor_names_count; i++)
    if (starts_with(actor_names[i], lists[g].prefix) && !(lists[g].skip && starts_with(actor_names[i], lists[g].skip)) &&
        (!m->inner_widest || strlen(actor_names[i]) > strlen(m->inner_widest)))
      m->inner_widest = actor_names[i];
  full_window(g);
  if (full[g].n > ALL_ROWS)
    add_full_arrow(m, g, 1);
}

/* a kind's menu: its picks, the list's submenu (character equipment), its full list, return and its note;
 * again when the filter changes (menu_reset: its items, in the room menu_init gave it) */
static void fill_kind(int c)
{
  menu_t *m = &m_common_list[c];
  menu_reset(m);
  for (int i = 0; i < commons[c].count; i++) {
    menu_item_t *item = menu_add_action(m, commons[c].list[i].label, act_common, (intptr_t)&commons[c].list[i]);
    if (item)
      item->hint = commons[c].list[i].own_horse
                       ? "L: epona as your own horse, so she can be ridden (placed or in front of Link: spawn where)"
                       : "L: the game makes it, placed with the sticks or in front of Link (spawn where)";
  }
  if (commons[c].more >= 0) {
    menu_item_t *item = menu_add_submenu(m, "character equipment", lists[commons[c].more].menu);
    if (item)
      item->hint = "the Npc_Equipment_ actors, kept out of all characters below";
  }
  if (commons[c].all >= 0)
    add_full_list(m, commons[c].all);
  menu_add_return(m);
  if (commons[c].note)
    menu_add_text(m, text_note, (intptr_t)commons[c].note);
}

/* the menu a full list is in, made again */
static void fill_list_menu(int g)
{
  for (int c = 0; c < N_COMMONS; c++)
    if (commons[c].all == g) {
      fill_kind(c);
      return;
    }
  menu_reset(lists[g].menu); /* a menu of its own (character equipment) */
  add_full_list(lists[g].menu, g);
  menu_add_return(lists[g].menu);
}

static void fill_common(void)
{
  for (int g = 0; g < N_LISTS; g++)
    full_match(g);
  for (int c = 0; c < N_COMMONS; c++) {
    if (commons[c].more >= 0) {
      menu_init(lists[commons[c].more].menu, "character equipment", ALL_ROWS + 4);
      fill_list_menu(commons[c].more);
    }
    menu_init(&m_common_list[c], commons[c].title,
              commons[c].count + 2 + (commons[c].all >= 0 ? ALL_ROWS + 4 : 0) + (commons[c].more >= 0));
    fill_kind(c);
    menu_add_submenu(&m_common, commons[c].title, &m_common_list[c]);
  }
  menu_add_return(&m_common);
}

/* --- building, and entering ----------------------------------------------------------------------- */

menu_t *actorsmenu_build(void)
{
  menu_init(&m_actors, "actors", 6);
  menu_init(&m_common, "quick spawn", N_COMMONS + 1);
  fill_common();
  menu_item_t *item = menu_add_action(&m_actors, "browse currently loaded actors", act_look, 0);
  if (item)
    item->hint = "L: the free camera over a beam: d-left/right go from actor to actor, A goes to one, Y kills or despawns";
  item = menu_add_option(&m_actors, "look at", PLACER_KINDS, placer_kind_names, get_look_kind, set_look_kind, 0);
  if (item) {
    item->hint = "which actors it steps through to start with (L/R change it while browsing)";
    item->indent = 2; /* browse's option, under it */
  }
  item = menu_add_submenu(&m_actors, "quick spawn", &m_common);
  if (item)
    item->hint = "by kind: the likeliest first, then every one the game has with a name filter (it loads what it needs)";
  item = menu_add_action(&m_actors, "remove spawned", act_remove_spawned, 0);
  if (item)
    item->hint = "L: delete every actor spawned here that's still in this map";
  item = menu_add_option(&m_actors, "spawn where", 2, spawn_where_names, get_spawn_where, set_spawn_where, 0);
  if (item)
    item->hint = "placed: a beam to move with the left stick, the camera on the right, A spawns, Y kills or despawns, B ends";
  menu_add_return(&m_actors);
  return &m_actors;
}

void actorsmenu_frame(void)
{
  spawn_frame();
  death_watch_frame();
}
