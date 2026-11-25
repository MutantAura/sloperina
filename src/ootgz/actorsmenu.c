/* debug > actors (actorsmenu.h): gz's actors menu for this game.
 *
 * browse current actors: every actor in the actor manager, any state, in folders by kind (with their counts),
 * filtered by a piece of their names; a folder groups its actors under their names, or sorts them by distance. L on
 * one opens it: where it is and its state, go to it, spawn one like it (actor_spawn: the game's create request), kill
 * it (actor_kill: the game's death request, or its delete request for an NPC or thing with no death). spawn actor by
 * name or type makes any enemy, NPC, object or item the game has a pack for (actornames.c), loading it if it must.
 *
 * The lists are copies: an actor in them may have gone since, so the chosen one is looked for again in the actor
 * manager (actor_read) every time it's shown or used, and is never touched once it isn't there. */
#include "actorsmenu.h"

#include <stdio.h>
#include <string.h>

#include "actornames.h"
#include "age.h"
#include "enemyhp.h"
#include "gfx.h"
#include "link.h"
#include "osk.h"
#include "position.h"
#include "sys.h"

#define ALL_MAX 1536     /* browse: the actors read (a busy map has about 850) */
#define FOLDER_ROWS 120  /* a folder's rows */
#define FOLDER_HEADS 60  /* ... and its names over them */
#define FILTER_MAX 31

static menu_t m_actors, m_browse, m_folder, m_chosen, m_spawn, m_common;

/* what a list shows, by the start of the actor's name */
enum { KIND_ALL, KIND_ENEMIES, KIND_NPCS, KIND_OBJECTS, KIND_SCENERY, KIND_OTHER, N_KINDS };
static const char *const kind_names[N_KINDS] = { "all", "enemies", "npcs", "objects", "scenery", "other" };

static uintptr_t chosen;
static char chosen_name[64];

/* --- names ------------------------------------------------------------------------------------- */

static bool starts_with(const char *s, const char *prefix)
{
  for (; *prefix; s++, prefix++)
    if (*s != *prefix)
      return false;
  return true;
}

static int kind_of(const char *name)
{
  if (starts_with(name, "Enemy_"))
    return KIND_ENEMIES;
  if (starts_with(name, "Npc_"))
    return KIND_NPCS;
  if (starts_with(name, "Obj_") || starts_with(name, "Item_"))
    return KIND_OBJECTS;
  if (starts_with(name, "Land_"))
    return KIND_SCENERY;
  return KIND_OTHER;
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

static int compare_names(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return (unsigned char)*a - (unsigned char)*b;
}

/* --- the chosen actor: its own menu, opened from a folder's row ---------------------------------- */

static void fill_folder(void);

static void choose(const actor_near_t *row)
{
  chosen = row->actor;
  snprintf(chosen_name, sizeof(chosen_name), "%s", row->name ? row->name : "?");
  log_printf("actors: chose %s (%#lx), %.2f m away", chosen_name, (unsigned long)chosen, (double)row->distance);
}

static void act_goto(menu_item_t *it)
{
  actor_read_t a;
  if (!chosen) {
    gfx_notice("choose an actor first");
    return;
  }
  if (!actor_read(chosen, &a)) {
    gfx_notice("that actor has gone");
    return;
  }
  if (position_goto(a.pos[0], a.pos[1], a.pos[2])) {
    log_printf("actors: go to %s at %.3f %.3f %.3f", a.name, (double)a.pos[0], (double)a.pos[1], (double)a.pos[2]);
    menu_close(); /* so the move can be watched */
  }
}

static void act_kill(menu_item_t *it)
{
  if (!chosen) {
    gfx_notice("choose an actor first");
    return;
  }
  switch (actor_kill(chosen)) {
    case KILL_DEATH:
      gfx_notice("killed");
      break;
    case KILL_DEATH_REFUSED:
      gfx_notice("it won't die that way (a boss, or already dying)");
      break;
    case KILL_DELETED:
      gfx_notice("deleted (it had no death of its own)");
      break;
    case KILL_DELETE_REFUSED:
      gfx_notice("the game refused to delete it");
      break;
    case KILL_NOT_ALLOWED:
      gfx_notice("not that one: it can't die, and only npcs and things are deleted");
      break;
    case KILL_GONE:
      gfx_notice("that actor has gone");
      break;
    case KILL_UNKNOWN:
      gfx_notice("kill: not known for this game version");
      break;
  }
}

/* spawn: one more of the chosen actor's name, through the create request's handle, so the actor made is known
 * exactly (for remove spawned, and to make a horse one's own) */
#define SPAWN_AHEAD 2.0     /* metres in front of Link */
#define SPAWN_WAIT 600      /* frames to wait for it (its pack may have to load) */
#define OWN_WAIT 120        /* frames for a horse to start running, to be made one's own */
#define SPAWNED_MAX 64      /* remove spawned: the actors made here, kept while they're in the map */
#define CALM_FRAMES 60      /* an own horse's rodeo bit is cleared this long after (its setup sets it again) */

float atan2f(float y, float x); /* from sdk */

static struct {
  char name[64];
  int frames;
  uintptr_t handle; /* the game writes the request's status here: it stays put until made or failed */
  bool own_horse;   /* epona (owned): made one's own horse once it runs */
} spawning;
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

/* the game asked to make `name` in front of Link, facing him; spawn_frame follows the request */
static void spawn_named(const char *name, bool own_horse)
{
  vec3f_t link;
  double fx = 0, fz = 1;
  if (spawning.frames) {
    gfx_notice("still waiting for the last one");
    return;
  }
  if (!link_drawn_pos(&link)) {
    gfx_notice("spawn: Link not found");
    return;
  }
  link_facing(&fx, &fz);
  float at[3] = { (float)(link.x + fx * SPAWN_AHEAD), (float)link.y, (float)(link.z + fz * SPAWN_AHEAD) };
  float yaw = atan2f((float)fx, (float)fz) * (180.0f / 3.14159265f) + 180.0f; /* facing Link */
  snprintf(spawning.name, sizeof(spawning.name), "%s", name);
  spawning.own_horse = own_horse;
  uint32_t result;
  if (!actor_spawn_handle(spawning.name, at, yaw, &spawning.handle, &result)) {
    char text[96];
    snprintf(text, sizeof(text), "the game refused to make it (%u)", (unsigned)result);
    gfx_notice(text);
    return;
  }
  spawning.frames = 1;
  gfx_notice("spawning");
  menu_close(); /* so it can be watched */
}

static void act_spawn(menu_item_t *it)
{
  if (!chosen) {
    gfx_notice("choose an actor first");
    return;
  }
  spawn_named(chosen_name, false);
}

/* epona (owned): once she runs, registered as the owned horse and her rodeo bit cleared (FINDINGS 9.33) */
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
  if (!spawning.frames)
    return;
  uintptr_t made;
  spawn_poll_t now = actor_spawn_poll(&spawning.handle, &made);
  if (now == SPAWN_MADE) {
    log_printf("actors: %s made after %d frames: %#lx", spawning.name, spawning.frames, (unsigned long)made);
    remember(made);
    spawning.frames = 0;
    if (spawning.own_horse)
      owning = made, own_left = OWN_WAIT;
    else
      gfx_notice("spawned");
  } else if (now == SPAWN_FAILED) {
    log_printf("actors: %s: the request failed after %d frames", spawning.name, spawning.frames);
    gfx_notice("the game couldn't make it");
    spawning.frames = 0;
  } else if (++spawning.frames > SPAWN_WAIT) {
    actor_spawn_cancel(&spawning.handle);
    log_printf("actors: %s not made in %d frames", spawning.name, SPAWN_WAIT);
    gfx_notice("it wasn't made in time");
    spawning.frames = 0;
  }
}

/* the chosen actor, as it is now: three lines */
static void text_chosen(menu_item_t *it, char *buf, int len)
{
  actor_read_t a;
  bool there = chosen && actor_read(chosen, &a);
  if (!chosen) {
    snprintf(buf, len, it->data == 0 ? "none chosen" : "");
  } else if (!there) {
    snprintf(buf, len, it->data == 0 ? "%s: gone" : "", chosen_name);
  } else if (it->data == 0) {
    snprintf(buf, len, "%s", a.name);
  } else if (it->data == 1) {
    if (a.distance_known)
      snprintf(buf, len, "%.2f m away at %.2f %.2f %.2f", (double)a.distance, (double)a.pos[0], (double)a.pos[1],
               (double)a.pos[2]);
    else
      snprintf(buf, len, "at %.2f %.2f %.2f", (double)a.pos[0], (double)a.pos[1], (double)a.pos[2]);
  } else if (a.has_life) {
    snprintf(buf, len, "state %u%s, life %d/%d", (unsigned)a.state, a.running ? "" : " (not running)", (int)a.life,
             (int)a.max_life);
  } else {
    snprintf(buf, len, "state %u%s, no life", (unsigned)a.state, a.running ? "" : " (not running)");
  }
}

/* the chosen actor's menu: its lines, go to, spawn one like it, kill, return */
static void fill_chosen(void)
{
  m_chosen.title = chosen_name;
  menu_reset(&m_chosen);
  for (int line = 0; line < 3; line++)
    menu_add_text(&m_chosen, text_chosen, line);
  menu_end_group(&m_chosen);
  menu_item_t *item = menu_add_action(&m_chosen, "go to", act_goto, 0);
  if (item)
    item->hint = "L: Link to this actor (load position's way; the saved position stays)";
  item = menu_add_action(&m_chosen, "spawn one like it", act_spawn, 0);
  if (item)
    item->hint = "L: the game makes another of this actor, in front of Link (its pack loads if it must)";
  item = menu_add_action(&m_chosen, "kill", act_kill, 0);
  if (item)
    item->hint = "L: the game's death request, as a fall into the void; npcs and things with none are deleted";
  menu_add_return(&m_chosen);
  menu_refresh(&m_chosen);
}

/* --- browse: every actor, in folders ------------------------------------------------------------ */

static actor_near_t all[ALL_MAX];
static int n_all, all_more;
static int kind_count[N_KINDS]; /* the actors of each kind whose names have the filter */
static char filter[FILTER_MAX + 1];
static char filter_label[FILTER_MAX + 24];
static char folder_labels[N_KINDS][40];

static int folder_kind;
static int sort_choice; /* 0: grouped by name, 1: by distance */
static const char *const sort_names[] = { "by name", "by distance" };
static uint16_t order[ALL_MAX];
static int n_order;
static actor_near_t frows[FOLDER_ROWS];
static char frow_labels[FOLDER_ROWS][72];
static char fhead_labels[FOLDER_HEADS][72];
static int n_frows, n_fmore;

static bool wanted(const actor_near_t *a, int kind)
{
  return (kind == KIND_ALL || kind_of(a->name) == kind) && has_piece(a->name, filter);
}

static void fill_browse(void);

static void count_kinds(void)
{
  for (int k = 0; k < N_KINDS; k++)
    kind_count[k] = 0;
  for (int i = 0; i < n_all; i++)
    if (has_piece(all[i].name, filter)) {
      kind_count[KIND_ALL]++;
      kind_count[kind_of(all[i].name)]++;
    }
}

/* every actor read again */
static void browse_refresh(void)
{
  n_all = actors_all(all, ALL_MAX, &all_more);
  count_kinds();
  fill_browse();
  fill_folder();
}

static void act_browse_refresh(menu_item_t *it) { browse_refresh(); }

static void filter_typed(const char *text, intptr_t ctx)
{
  snprintf(filter, sizeof(filter), "%s", text ? text : "");
  log_printf("actors: browse filter \"%s\"", filter);
  count_kinds();
  fill_browse();
}

static void act_filter(menu_item_t *it)
{
  osk_open("name has (empty: any)", filter, FILTER_MAX, filter_typed, 0);
}

static void text_loaded(menu_item_t *it, char *buf, int len)
{
  if (all_more)
    snprintf(buf, len, "%d actors read (%d more not)", n_all, all_more);
  else
    snprintf(buf, len, "%d actors loaded, %d with the name", n_all, kind_count[KIND_ALL]);
}

static void fill_browse(void)
{
  menu_reset(&m_browse);
  menu_item_t *item = menu_add_action(&m_browse, "refresh", act_browse_refresh, 0);
  if (item)
    item->hint = "L: read every actor again (also on entering this menu)";
  snprintf(filter_label, sizeof(filter_label), "name has: %s", filter[0] ? filter : "anything");
  item = menu_add_action(&m_browse, filter_label, act_filter, 0);
  if (item) {
    item->hint = "L: type a piece of the names to show (any case; empty: all)";
    item->log_label = "";
  }
  menu_add_text(&m_browse, text_loaded, 0);
  menu_add_heading(&m_browse, "folders");
  for (int k = 0; k < N_KINDS; k++) {
    snprintf(folder_labels[k], sizeof(folder_labels[k]), "%s (%d)", kind_names[k], kind_count[k]);
    item = menu_add_submenu(&m_browse, folder_labels[k], &m_folder);
    if (item) {
      item->data = k;
      item->log_label = "";
    }
  }
  menu_end_group(&m_browse);
  menu_add_return(&m_browse);
  menu_refresh(&m_browse);
}

/* the folder's order: grouped by name, nearest first in a group; or nearest first */
static bool before(const actor_near_t *a, const actor_near_t *b)
{
  if (sort_choice == 0) {
    int c = compare_names(a->name, b->name);
    if (c)
      return c < 0;
  }
  return a->distance < b->distance;
}

static void sort_order(void)
{
  for (int gap = n_order / 2; gap > 0; gap /= 2) /* Shell's sort: a few hundred at most */
    for (int i = gap; i < n_order; i++) {
      uint16_t v = order[i];
      int j = i;
      for (; j >= gap && before(&all[v], &all[order[j - gap]]); j -= gap)
        order[j] = order[j - gap];
      order[j] = v;
    }
}

static int get_folder(menu_item_t *it) { return folder_kind; }
static void set_folder(menu_item_t *it, int choice)
{
  folder_kind = choice;
  fill_folder();
}
static int get_sort(menu_item_t *it) { return sort_choice; }
static void set_sort(menu_item_t *it, int choice)
{
  sort_choice = choice;
  fill_folder();
}

static void text_folder(menu_item_t *it, char *buf, int len)
{
  if (!n_frows)
    snprintf(buf, len, filter[0] ? "none with \"%s\" in the name" : "none", filter);
  else if (n_fmore)
    snprintf(buf, len, "%d shown, %d more", n_frows, n_fmore);
  else
    snprintf(buf, len, "%d %s", n_frows, sort_choice ? "nearest first" : "by name, nearest first in each");
}

/* L on a row: that actor's own menu */
static void act_open_row(menu_item_t *it)
{
  if (it->data < 0 || it->data >= n_frows)
    return;
  choose(&frows[it->data]);
  fill_chosen();
  m_chosen.parent = &m_folder;
  menu_goto(&m_chosen);
}

/* the folder's items: the actors of its kind with the filter in their names, in the order chosen */
static void fill_folder(void)
{
  n_order = 0;
  for (int i = 0; i < n_all; i++)
    if (wanted(&all[i], folder_kind))
      order[n_order++] = (uint16_t)i;
  sort_order();

  m_folder.title = kind_names[folder_kind];
  menu_reset(&m_folder);
  menu_item_t *item = menu_add_option(&m_folder, "folder", N_KINDS, kind_names, get_folder, set_folder, 0);
  if (item)
    item->hint = "which kind (the folders of browse)";
  item = menu_add_option(&m_folder, "sort", 2, sort_names, get_sort, set_sort, 0);
  if (item)
    item->hint = "grouped under their names, or all nearest first";
  menu_add_text(&m_folder, text_folder, 0);
  n_frows = n_fmore = 0;
  int n_heads = 0;
  for (int i = 0; i < n_order; i++) {
    const actor_near_t *a = &all[order[i]];
    bool new_name = sort_choice == 0 && (i == 0 || compare_names(a->name, all[order[i - 1]].name) != 0);
    if (new_name) {
      int in_group = 1;
      while (i + in_group < n_order && !compare_names(all[order[i + in_group]].name, a->name))
        in_group++;
      if (n_heads >= FOLDER_HEADS || n_frows >= FOLDER_ROWS) {
        n_fmore += n_order - i;
        break;
      }
      snprintf(fhead_labels[n_heads], sizeof(fhead_labels[n_heads]), "%s (%d)", a->name, in_group);
      menu_add_heading(&m_folder, fhead_labels[n_heads++]);
    }
    if (n_frows >= FOLDER_ROWS) {
      n_fmore++;
      continue;
    }
    frows[n_frows] = *a;
    const char *mark = a->actor == chosen ? "> " : "  ";
    if (sort_choice == 0)
      snprintf(frow_labels[n_frows], sizeof(frow_labels[n_frows]), "%s%6.1f m  state %u", mark, (double)a->distance,
               (unsigned)a->state);
    else
      snprintf(frow_labels[n_frows], sizeof(frow_labels[n_frows]), "%s%6.1f  %s", mark, (double)a->distance, a->name);
    item = menu_add_action(&m_folder, frow_labels[n_frows], act_open_row, n_frows);
    if (item) {
      item->hint = "L: open it: where it is, go to, spawn one like it, kill";
      item->log_label = "";
    }
    n_frows++;
  }
  m_folder.pinned = 3; /* folder, sort and the count stay at the top */
  menu_end_group(&m_folder);
  menu_add_return(&m_folder);
  m_folder.pinned_end = 1;
  menu_refresh(&m_folder);
}

/* --- spawn actor by name or type: the actors the game has a pack for (actornames.c) ------------------- */

#define SPAWN_ROWS 40

static char spawn_filter[FILTER_MAX + 1];
static char spawn_filter_label[FILTER_MAX + 24];
static int spawn_rows[SPAWN_ROWS], n_spawn_rows, n_spawn_matches;
static int spawn_kind = KIND_ALL;

static void fill_spawn(void);

static void spawn_filter_typed(const char *text, intptr_t ctx)
{
  snprintf(spawn_filter, sizeof(spawn_filter), "%s", text ? text : "");
  fill_spawn();
}

static void act_spawn_filter(menu_item_t *it)
{
  osk_open("actor name has", spawn_filter, FILTER_MAX, spawn_filter_typed, 0);
}

static void act_spawn_row(menu_item_t *it) { spawn_named(actor_names[spawn_rows[it->data]], false); }

static int get_spawn_kind(menu_item_t *it) { return spawn_kind; }
static void set_spawn_kind(menu_item_t *it, int choice)
{
  spawn_kind = choice;
  fill_spawn();
}

static void text_spawn_count(menu_item_t *it, char *buf, int len)
{
  if (!n_spawn_matches && spawn_filter[0])
    snprintf(buf, len, "no %s with \"%s\" in the name", kind_names[spawn_kind], spawn_filter);
  else if (!n_spawn_matches)
    snprintf(buf, len, "none");
  else if (n_spawn_matches > n_spawn_rows)
    snprintf(buf, len, "%d match: the first %d (type more of the name)", n_spawn_matches, n_spawn_rows);
  else
    snprintf(buf, len, "%d match", n_spawn_matches);
}

static void fill_spawn(void)
{
  n_spawn_rows = n_spawn_matches = 0;
  for (int i = 0; i < actor_names_count; i++)
    if ((spawn_kind == KIND_ALL || kind_of(actor_names[i]) == spawn_kind) && has_piece(actor_names[i], spawn_filter)) {
      if (n_spawn_rows < SPAWN_ROWS)
        spawn_rows[n_spawn_rows++] = i;
      n_spawn_matches++;
    }
  menu_reset(&m_spawn);
  snprintf(spawn_filter_label, sizeof(spawn_filter_label), "name has: %s", spawn_filter[0] ? spawn_filter : "anything");
  menu_item_t *item = menu_add_action(&m_spawn, spawn_filter_label, act_spawn_filter, 0);
  if (item) {
    item->hint = "L: type a piece of the name (any case), e.g. wolfos, tsubo, rupee";
    item->log_label = "";
  }
  item = menu_add_option(&m_spawn, "type", N_KINDS, kind_names, get_spawn_kind, set_spawn_kind, 0);
  if (item)
    item->hint = "by the start of their names: Enemy_, Npc_, Obj_ and Item_, Land_, the rest";
  menu_add_text(&m_spawn, text_spawn_count, 0);
  for (int i = 0; i < n_spawn_rows; i++) {
    item = menu_add_action(&m_spawn, actor_names[spawn_rows[i]], act_spawn_row, i);
    if (item) {
      item->hint = "L: the game makes it in front of Link, facing him (its pack loads if it must)";
      item->log_label = "";
    }
  }
  m_spawn.pinned = 3; /* name has, type and the count stay at the top */
  m_spawn.pinned_end = 1;
  menu_add_return(&m_spawn);
  menu_refresh(&m_spawn);
}

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
} commons[] = {
  { "animals", animals, N(animals), 0 },
  { "enemies", enemies, N(enemies), 0 },
  { "characters", characters, N(characters), "spawns might affect flags/save states/etc" },
  { "objects", objects, N(objects), 0 },
  { "pickups", pickups, N(pickups), 0 },
};
#define N_COMMONS N(commons)
static menu_t m_common_list[N_COMMONS];

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

static void fill_common(void)
{
  for (int g = 0; g < N_COMMONS; g++) {
    menu_t *m = &m_common_list[g];
    menu_init(m, commons[g].title, commons[g].count + 2);
    for (int i = 0; i < commons[g].count; i++) {
      menu_item_t *item = menu_add_action(m, commons[g].list[i].label, act_common, (intptr_t)&commons[g].list[i]);
      if (item)
        item->hint = commons[g].list[i].own_horse
                         ? "L: epona in front of link, as your own horse, so she can be ridden"
                         : "L: the game makes it in front of Link, facing him (its pack loads if it must)";
    }
    menu_add_return(m);
    if (commons[g].note)
      menu_add_text(m, text_note, (intptr_t)commons[g].note);
    menu_add_submenu(&m_common, commons[g].title, m);
  }
  menu_add_return(&m_common);
}

/* --- building, and entering ----------------------------------------------------------------------- */

menu_t *actorsmenu_build(void)
{
  menu_init(&m_actors, "actors", 5);
  menu_init(&m_common, "quick spawn", N_COMMONS + 1);
  fill_common();
  menu_init(&m_spawn, "spawn actor by name or type", SPAWN_ROWS + 5);
  fill_spawn();
  menu_init(&m_browse, "browse current actors", N_KINDS + 6);
  menu_init(&m_folder, "all", 3 + FOLDER_ROWS + FOLDER_HEADS + 3);
  menu_init(&m_chosen, "actor", 8);
  fill_browse();
  fill_folder();
  fill_chosen();
  menu_item_t *item = menu_add_submenu(&m_actors, "browse current actors", &m_browse);
  if (item)
    item->hint = "every actor loaded in the scene, in folders by kind: open one to go to it, copy it or kill it";
  item = menu_add_submenu(&m_actors, "spawn actor by name or type", &m_spawn);
  if (item)
    item->hint = "any enemy, npc, object or item the game has: it loads what it needs";
  item = menu_add_submenu(&m_actors, "quick spawn", &m_common);
  if (item)
    item->hint = "short lists by kind: animals (epona as your own horse), enemies, characters, objects, pickups";
  item = menu_add_action(&m_actors, "remove spawned", act_remove_spawned, 0);
  if (item)
    item->hint = "L: delete every actor spawned here that's still in this map";
  menu_add_return(&m_actors);
  return &m_actors;
}

void actorsmenu_frame(void)
{
  spawn_frame();
  static menu_t *was;
  if (menu_current != was) {
    if (menu_current == &m_browse && was != &m_folder)
      browse_refresh(); /* entered: the actors as they are now */
    else if (menu_current == &m_folder && was == &m_browse && m_browse.selected < m_browse.n_items) {
      folder_kind = (int)m_browse.items[m_browse.selected].data; /* the folder it was entered by */
      fill_folder();
    } else if (menu_current == &m_folder && was == &m_chosen) {
      fill_folder(); /* the chosen row's mark */
    }
  }
  was = menu_current;
}
