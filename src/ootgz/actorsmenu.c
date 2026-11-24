/* debug > actors (actorsmenu.h): gz's actors menu for this game. The running actors near Link, nearest first,
 * read through actors.c when the menu is entered or refresh is chosen; one of them chosen with L, then gone to.
 *
 * The list is a copy: an actor in it may have gone since, so the chosen one is looked for again in the actor
 * manager (actor_read) every time it's shown or used, and is never touched once it isn't there. */
#include "actorsmenu.h"

#include <stdio.h>
#include <string.h>

#include "enemyhp.h"
#include "gfx.h"
#include "position.h"
#include "sys.h"

#define ROWS 24     /* the list's rows */
#define WALK_MAX 96 /* the nearest this many are read, then filtered by kind */

static menu_t m_actors;

static const char *const range_names[] = { "5 m", "10 m", "20 m", "50 m", "100 m" };
static const float ranges[] = { 5, 10, 20, 50, 100 };
static int range_choice = 2;

/* what the list shows, by the start of the actor's name */
enum { KIND_ALL, KIND_ENEMIES, KIND_NPCS, KIND_OBJECTS, KIND_SCENERY, KIND_OTHER, N_KINDS };
static const char *const kind_names[N_KINDS] = { "all", "enemies", "npcs", "objects", "scenery", "other" };
static int kind_choice = KIND_ALL;

static actor_near_t found[WALK_MAX];
static actor_near_t rows[ROWS];
static int n_rows, n_more; /* shown, and the matches that didn't fit */
static char row_labels[ROWS][72];

static uintptr_t chosen;
static char chosen_name[64];

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

static void fill(void);

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
  const actor_near_t *row = &rows[it->data];
  chosen = row->actor;
  snprintf(chosen_name, sizeof(chosen_name), "%s", row->name ? row->name : "?");
  log_printf("actors: chose %s (%#lx), %.2f m away", chosen_name, (unsigned long)chosen, (double)row->distance);
  fill(); /* its row's mark */
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

static void text_count(menu_item_t *it, char *buf, int len)
{
  if (!n_rows)
    snprintf(buf, len, "none within %s", range_names[range_choice]);
  else if (n_more)
    snprintf(buf, len, "the nearest %d (%d more further away)", n_rows, n_more);
  else
    snprintf(buf, len, "%d within %s, nearest first", n_rows, range_names[range_choice]);
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

/* the menu's items, from the list as it is */
static void fill(void)
{
  menu_reset(&m_actors);
  menu_item_t *item = menu_add_action(&m_actors, "refresh", act_refresh, 0);
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
      item->hint = "L: choose it (below: where it is, go to)";
      item->log_label = "";
    }
  }
  int list_end = m_actors.n_items;
  for (int line = 0; line < 3; line++)
    menu_add_text(&m_actors, text_chosen, line);
  item = menu_add_action(&m_actors, "go to", act_goto, 0);
  if (item)
    item->hint = "L: Link to the chosen actor (load position's way; the saved position stays)";
  menu_add_return(&m_actors);
  m_actors.pinned = 4; /* refresh, range, show and the count stay at the top */
  m_actors.pinned_end = m_actors.n_items - list_end; /* the chosen one, go to and return at the bottom */
  menu_refresh(&m_actors);
}

menu_t *actorsmenu_build(void)
{
  menu_init(&m_actors, "actors", ROWS + 12);
  fill();
  return &m_actors;
}

void actorsmenu_frame(void)
{
  static menu_t *was;
  if (menu_current == &m_actors && was != &m_actors)
    refresh(); /* entered: the actors as they are now */
  was = menu_current;
}
