/* debug > actors (actorsmenu.h): gz's actors menu for this game.
 *
 * The menu itself: the running actors near Link, nearest first, read through actors.c when the menu is entered or
 * refresh is chosen. browse: every actor in the actor manager, any state, in folders by kind (with their counts),
 * filtered by a piece of their names; a folder groups its actors under their names, or sorts them by distance.
 * In either list L chooses an actor, which can then be gone to or killed (actor_kill: the game's death request, or
 * its delete request for an NPC or thing with no death).
 *
 * The lists are copies: an actor in them may have gone since, so the chosen one is looked for again in the actor
 * manager (actor_read) every time it's shown or used, and is never touched once it isn't there. */
#include "actorsmenu.h"

#include <stdio.h>
#include <string.h>

#include "enemyhp.h"
#include "gfx.h"
#include "osk.h"
#include "position.h"
#include "sys.h"

#define ROWS 24          /* the near list's rows */
#define WALK_MAX 96      /* the nearest this many are read, then filtered by kind */
#define ALL_MAX 1536     /* browse: the actors read (a busy map has about 850) */
#define FOLDER_ROWS 120  /* a folder's rows */
#define FOLDER_HEADS 60  /* ... and its names over them */
#define FILTER_MAX 31

static menu_t m_actors, m_browse, m_folder;

static const char *const range_names[] = { "5 m", "10 m", "20 m", "50 m", "100 m" };
static const float ranges[] = { 5, 10, 20, 50, 100 };
static int range_choice = 2;

/* what a list shows, by the start of the actor's name */
enum { KIND_ALL, KIND_ENEMIES, KIND_NPCS, KIND_OBJECTS, KIND_SCENERY, KIND_OTHER, N_KINDS };
static const char *const kind_names[N_KINDS] = { "all", "enemies", "npcs", "objects", "scenery", "other" };
static int kind_choice = KIND_ALL;

static actor_near_t found[WALK_MAX];
static actor_near_t rows[ROWS];
static int n_rows, n_more; /* shown, and the matches that didn't fit */
static char row_labels[ROWS][72];

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

/* --- the chosen actor: shown and used from both lists ------------------------------------------- */

static void fill(void);
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

/* the chosen actor, as it is now: three lines */
static void text_chosen(menu_item_t *it, char *buf, int len)
{
  actor_read_t a;
  bool there = chosen && actor_read(chosen, &a);
  if (!chosen) {
    snprintf(buf, len, it->data == 0 ? "chosen: none (L on a row)" : "");
  } else if (!there) {
    snprintf(buf, len, it->data == 0 ? "chosen: %s (gone)" : "", chosen_name);
  } else if (it->data == 0) {
    snprintf(buf, len, "chosen: %s", a.name);
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

/* under a list: the chosen actor's lines, go to, kill, return; how many items that was */
static int add_chosen_items(menu_t *menu)
{
  int before = menu->n_items;
  menu_end_group(menu);
  for (int line = 0; line < 3; line++)
    menu_add_text(menu, text_chosen, line);
  menu_item_t *item = menu_add_action(menu, "go to", act_goto, 0);
  if (item)
    item->hint = "L: Link to the chosen actor (load position's way; the saved position stays)";
  item = menu_add_action(menu, "kill", act_kill, 0);
  if (item)
    item->hint = "L: the game's death request, as a fall into the void; npcs and things with none are deleted";
  menu_add_return(menu);
  return menu->n_items - before;
}

/* --- the actors near Link ----------------------------------------------------------------------- */

/* the list read again: the nearest WALK_MAX within range, then those of the kind shown */
static void refresh(void)
{
  int n = actors_near(ranges[range_choice], found, WALK_MAX);
  n_rows = n_more = 0;
  for (int i = 0; i < n; i++) {
    if (kind_choice != KIND_ALL && kind_of(found[i].name) != kind_choice)
      continue;
    if (n_rows < ROWS)
      rows[n_rows++] = found[i];
    else
      n_more++;
  }
  fill();
}

static void act_refresh(menu_item_t *it) { refresh(); }

static int get_range(menu_item_t *it) { return range_choice; }
static void set_range(menu_item_t *it, int choice)
{
  range_choice = choice;
  refresh();
}
static int get_kind(menu_item_t *it) { return kind_choice; }
static void set_kind(menu_item_t *it, int choice)
{
  kind_choice = choice;
  refresh();
}

static void act_choose(menu_item_t *it)
{
  choose(&rows[it->data]);
  fill(); /* its row's mark */
}

static void text_count(menu_item_t *it, char *buf, int len)
{
  if (!n_rows)
    snprintf(buf, len, "none within %s", range_names[range_choice]);
  else if (n_more)
    snprintf(buf, len, "the nearest %d (%d more further away)", n_rows, n_more);
  else
    snprintf(buf, len, "%d within %s, nearest first", n_rows, range_names[range_choice]);
}

/* the menu's items, from the list as it is */
static void fill(void)
{
  menu_reset(&m_actors);
  menu_item_t *item = menu_add_submenu(&m_actors, "browse", &m_browse);
  if (item)
    item->hint = "every actor in the scene, in folders by kind, at any distance";
  item = menu_add_action(&m_actors, "refresh", act_refresh, 0);
  if (item)
    item->hint = "L: read the actors near Link again (also on entering this menu)";
  item = menu_add_option(&m_actors, "range", 5, range_names, get_range, set_range, 0);
  if (item)
    item->hint = "how far from Link to look";
  item = menu_add_option(&m_actors, "show", N_KINDS, kind_names, get_kind, set_kind, 0);
  if (item)
    item->hint = "by the start of their names: Enemy_, Npc_, Obj_ and Item_, Land_, the rest";
  menu_add_text(&m_actors, text_count, 0);
  for (int i = 0; i < n_rows; i++) {
    snprintf(row_labels[i], sizeof(row_labels[i]), "%s%5.1f  %s", rows[i].actor == chosen ? "> " : "  ",
             (double)rows[i].distance, rows[i].name ? rows[i].name : "?");
    item = menu_add_action(&m_actors, row_labels[i], act_choose, i);
    if (item) {
      item->hint = "L: choose it (below: where it is, go to, kill)";
      item->log_label = "";
    }
  }
  m_actors.pinned = 5; /* browse, refresh, range, show and the count stay at the top */
  m_actors.pinned_end = add_chosen_items(&m_actors);
  menu_refresh(&m_actors);
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
  if (menu_current == &m_folder)
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

static void act_choose_folder(menu_item_t *it)
{
  choose(&frows[it->data]);
  fill_folder(); /* its row's mark */
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
    item = menu_add_action(&m_folder, frow_labels[n_frows], act_choose_folder, n_frows);
    if (item) {
      item->hint = "L: choose it (below: where it is, go to, kill)";
      item->log_label = "";
    }
    n_frows++;
  }
  m_folder.pinned = 3; /* folder, sort and the count stay at the top */
  m_folder.pinned_end = add_chosen_items(&m_folder);
  menu_refresh(&m_folder);
}

/* --- building, and entering ----------------------------------------------------------------------- */

menu_t *actorsmenu_build(void)
{
  menu_init(&m_actors, "actors", ROWS + 14);
  menu_init(&m_browse, "browse", N_KINDS + 6);
  menu_init(&m_folder, "all", 3 + FOLDER_ROWS + FOLDER_HEADS + 8);
  fill_browse();
  fill_folder();
  fill(); /* last: its browse item makes it browse's parent */
  return &m_actors;
}

void actorsmenu_frame(void)
{
  static menu_t *was;
  if (menu_current != was) {
    if (menu_current == &m_actors && was != &m_browse)
      refresh(); /* entered: the actors as they are now */
    else if (menu_current == &m_browse && was != &m_folder)
      browse_refresh();
    else if (menu_current == &m_folder && m_browse.selected < m_browse.n_items) {
      folder_kind = (int)m_browse.items[m_browse.selected].data; /* the folder it was entered by */
      fill_folder();
    }
  }
  was = menu_current;
}
