/* debug > actors' placer (placer.h).
 *
 * The spot is kept on the ground with the game's own raycast on the layer Link stands on (voidmap_ground_cast):
 * from a little above the spot down, so it climbs steps and slopes and drops off ledges, and failing that from high
 * above, for a cliff it's been pushed into. Where there's no ground at all (over the void, off the map's collision)
 * it keeps its height and turns orange: an actor made there falls, or floats if it doesn't.
 *
 * Two modes: placing an actor chosen to spawn (placer_start), or looking at the actors with nothing to spawn
 * (placer_look). The view is the free camera's (freecam.c), steered from here: it looks at the spot from `dist`
 * away, `tilt` above level, along `yaw`. The controls, with the menu closed (the game sees none of them, as with
 * the free camera), shown along the bottom in the game's control guide with its button glyphs (gfx_guide; in gz's
 * style, in the panel at the top left that says what's under the beam):
 *   left stick   the spot, across the screen as the camera faces       X held   faster
 *   d-up/down    the spot up and down: it keeps that height across     L3  back to Link, on the ground
 *   R3           hide the bar, the beam and the panel, and show them again (freecam_ui_hidden)
 *   right stick  the camera round the spot, and up and down            ZL / ZR  further, nearer
 *   d-left/right the actors of the kind shown near the spot, nearest first, one after another: the spot locks on to
 *                each and the camera glides to it and follows it, smoothed, so one that moves isn't jerky
 *   Y            kill the actor locked on to, or the one the beam is over (any kind), or despawn it if it won't
 *                die (debug > actors > kill / despawn)
 *   B            done: the camera as it was
 *   placing:     L / R the actor's facing (15 degrees a press), A make it here (again as often as wanted)
 *   looking:     L / R the kind shown: all, enemies, npcs, objects (and on to the nearest of it); A go to: Link
 *                to the actor, or to the spot if there's none (load position's way), looking on
 * An actor locked on to, or under the beam, turns the ring red. The left stick lets go of the actor, the spot
 * carrying on from where it is.
 *   exploring:   L / R the kind of place (only those the map has); A go to; Y take Link into the rooms on / off
 *                (shown in the bar; placer_take_link). On, going to a dungeon room takes Link into it, which wakes
 *                it; B puts him back where he was (or where A last put him)
 * The module's own commands are off meanwhile (ootgz.c's command_context), but for the menu's (R + right stick
 * down): L / R are presses, not held. A script's input (remote.h) steers it too. */
#include "placer.h"

#include <stdio.h>
#include <string.h>

#include "actorsmenu.h"
#include "airwall.h"
#include "enemyhp.h"
#include "freecam.h"
#include "data.h"
#include "gamedata.h"
#include "gfx.h"
#include "guardview.h"
#include "interactview.h"
#include "input.h"
#include "lang.h"
#include "link.h"
#include "menu.h"
#include "menus.h" /* menu_root */
#include "position.h"
#include "sys.h"
#include "voidmap.h"
#include "watches.h"

float sinf(float x), cosf(float x), atan2f(float y, float x); /* from sdk */

#define PI 3.14159265f
#define DEADZONE 4000
#define MOVE_RATE 0.012f     /* of the camera's distance, a frame at full tilt (10 m away: 3.6 m a second) */
#define FAST 4.0f            /* X held */
#define SWING_SPEED 0.045f   /* radians a frame */
#define TILT_MIN 0.17f       /* about 10 degrees above level ... */
#define TILT_MAX 1.45f       /* ... to about 83 */
#define TILT_START 0.75f     /* at least this when taken from the game's camera (about 43 degrees) */
#define DIST_MIN 2.0f
#define DIST_MAX 60.0f
#define DIST_START 10.0f
#define ZOOM_RATE 1.03f      /* a frame */
#define TURN_STEP 15.0f      /* degrees a press of L / R */
#define RAISE_SPEED 0.05f    /* metres a frame that d-up / d-down move it up or down (X: faster) */
#define LOOK_ABOVE 0.8f      /* the camera looks at this far above the spot */
#define START_AHEAD 3.0f     /* metres in front of Link */
#define START_LOOK 120.0f    /* the free camera's view searched this far for ground */
#define STEP_UP 2.5f         /* the ground looked for from this far above the spot ... */
#define DROP 80.0f           /* ... to this far below; */
#define CLIMB 80.0f          /* then from this far above, down to STEP_UP */
#define LINK_NEAR 0.65f      /* the ray may hit Link's own body: within this of him, his feet are the ground */
#define FLAT_NY 0.35f        /* the ring lies on the ground unless it's steeper than this (normal's y) */
#define FOLLOW 0.2f          /* of the way to the spot the camera's focus goes each frame, locked on to an actor ... */
#define GLIDE 0.45f          /* ... and otherwise (steps in the ground's height, letting go of an actor) */
#define CYCLE_RADIUS 40.0f   /* d-left/right: the actors this near the spot */
#define CYCLE_MAX 48
#define CYCLE_READ 160       /* read, nearest first, before the kinds that can't be killed are left out */
#define HOVER_ACROSS 1.0f    /* an actor this near the spot across, ... */
#define HOVER_UP 2.5f        /* ... and this near up and down, is under the beam */
#define HOVER_READ 24
#define PANEL_COL 2          /* menu.c's ITEM_COL */

#define RING_OUT 0.55f
#define RING_IN 0.36f
#define RING_SEGMENTS 24
#define RING_LIFT 0.03f
#define BEAM_HEIGHT 5.0f
#define BEAM_WIDTH 0.035f
#define ARROW_BASE 0.66f
#define ARROW_TIP 0.98f
#define ARROW_HALF 0.20f

bool placer_active;
bool placer_take_link = true;

enum { MODE_PLACE, MODE_LOOK, MODE_EXPLORE };
static int mode;
static char name[64];         /* placing: the actor to make */

/* the kinds d-left/right go through (looking: L / R change it) */
enum { SHOW_ALL = PLACER_ALL, SHOW_ENEMIES = PLACER_ENEMIES, SHOW_NPCS = PLACER_NPCS, SHOW_OBJECTS = PLACER_OBJECTS,
       N_SHOWS = PLACER_KINDS };
const char *const placer_kind_names[PLACER_KINDS] = { "all kinds", "enemies", "npcs", "objects" };
#define show_names placer_kind_names
static int show;
static uintptr_t hover;       /* not locked on: the actor under the beam, or 0 */
static char hover_name[64];
static actor_near_t hover_read[HOVER_READ];

/* the explorer: the map's rooms, grottos and interiors (data_places, tools/places.py) that d-left/right go through,
 * of the kind ZL / ZR choose */
enum { EXPLORE_ALL, EXPLORE_ROOMS, EXPLORE_GROTTOS, EXPLORE_INTERIORS, N_EXPLORES };
static const char *const explore_names[N_EXPLORES] = { "places", "rooms", "grottos", "interiors" };
#define PLACES_MAX 256
#define INSIDE_DIST 2.5f  /* an interior or grotto: the camera this near the spot, ... */
#define INSIDE_TILT 0.6f  /* ... and this low (about 34 degrees), so it's inside with it, under the roof */
#define ROOM_DIST 6.0f    /* a dungeon room: nearer, under its ceiling, but further back (rooms are bigger) */
static int explore_kind;
static int place_at = -1;               /* the place the spot is on (data_places' index), or -1 */
static uint16_t places[PLACES_MAX];     /* the list, nearest the spot first */
static int n_places, places_at;         /* places_at: where in it the spot is (-1: not in it yet) */
static bool places_made;
static const airmap_t *places_map;      /* the map it was made in */
/* A dungeon wakes its rooms (makes their actors: enemies, chests' contents, switches) only for where Link is, not for
 * the camera, and writing its current room (GameData DungeonRoom) doesn't do it (FINDINGS 9.33). So going to a room
 * takes Link into it, the room waking as he lands; B puts him back where he was (home), unless A took him somewhere
 * of the user's own since, which becomes home. */
static float home[3];
static bool home_known, away; /* away: the explorer moved him from home */
static float absf(float v) { return v < 0 ? -v : v; }

/* the room Link is to be in: moved there once no load is under way. A move from one room to the next doesn't always
 * take (the Forest Temple's room 3 to room 4, three times running, while home to room 4 and room 3 to home did): so a
 * try that didn't is followed by one by way of home (there, then on to the room) */
#define WAKE_TRIES 3 /* the room, home, the room */
#define WAKE_NEAR 1.5f
static float wake_at[3];
static bool wake_wanted;
static int wake_tries;

static void wake_frame(void)
{
  if (!wake_wanted || position_busy())
    return;
  vec3f_t link;
  if (link_drawn_pos(&link) && absf(link.x - wake_at[0]) < WAKE_NEAR && absf(link.y - wake_at[1]) < WAKE_NEAR &&
      absf(link.z - wake_at[2]) < WAKE_NEAR) {
    wake_wanted = false; /* he's there: the room's awake */
    return;
  }
  if (wake_tries >= WAKE_TRIES) {
    wake_wanted = false;
    log_printf("placer: Link couldn't be moved into the room after %d tries", wake_tries);
    gfx_notice("couldn't take Link into the room to wake it");
    return;
  }
  wake_tries++;
  if (wake_tries % 2 == 0 && home_known && !(absf(link.x - home[0]) < WAKE_NEAR && absf(link.z - home[2]) < WAKE_NEAR)) {
    log_printf("placer: by way of home into the room");
    position_goto_quiet(home[0], home[1], home[2]); /* the next try goes on from there */
    return;
  }
  if (position_goto_quiet(wake_at[0], wake_at[1], wake_at[2]))
    away = true;
}

static float spot[3], normal[3] = { 0, 1, 0 };
static bool grounded;
static bool raised;           /* d-up/down moved it up or down: it keeps that height across (until L3, or a step) */
static float below_y;         /* raised: the ground under it, for the line down to it (below_known) */
static bool below_known;
static float facing;          /* the actor's, degrees: 0 faces +z (actor_spawn's) */
static float yaw, tilt, dist; /* the camera: looking along yaw (0 = +z), tilt above level, dist from the spot */
static uint32_t held_before;  /* buttons held last frame: a press is one that wasn't (all, after the menu) */
static int made;
static float focus[3];        /* what the camera looks at: the spot, smoothed */

static uintptr_t target;      /* the actor locked on to, or 0 */
static char target_name[64];
static actor_near_t cycle[CYCLE_MAX], cycle_read[CYCLE_READ];
static int n_cycle, cycle_at; /* the actors d-left/right step through, made on the first press; cycle_at: -1 none */
static bool cycle_made;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float root(float x) /* the sdk exports no sqrtf */
{
  float r;
  __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
  return r;
}

static float axis(int32_t v) { return v > DEADZONE || v < -DEADZONE ? (float)v / 32767.0f : 0; }

/* --- the ground ------------------------------------------------------------------------------------- */

static bool cast(float x0, float y0, float z0, float x1, float y1, float z1, float pos[3], float n[3])
{
  const float from[3] = { x0, y0, z0 }, to[3] = { x1, y1, z1 };
  return voidmap_ground_cast(from, to, pos, n);
}

/* the ground under (x, z) near height y: the spot moves there, grounded; or keeps y, not grounded */
static void settle(float x, float y, float z)
{
  float pos[3], n[3];
  bool ok = cast(x, y + STEP_UP, z, x, y - DROP, z, pos, n) || cast(x, y + CLIMB, z, x, y + STEP_UP, z, pos, n);
  vec3f_t link;
  if (ok && link_drawn_pos(&link) && (x - link.x) * (x - link.x) + (z - link.z) * (z - link.z) < LINK_NEAR * LINK_NEAR &&
      pos[1] > link.y + 0.25f) { /* on Link's head: under him, or his feet */
    if (!cast(x, link.y + 0.15f, z, x, link.y - DROP, z, pos, n))
      pos[1] = link.y, n[0] = 0, n[1] = 1, n[2] = 0;
  }
  spot[0] = x, spot[2] = z;
  grounded = ok;
  if (ok) {
    spot[1] = pos[1];
    memcpy(normal, n, sizeof(normal));
  } else {
    spot[1] = y;
    normal[0] = 0, normal[1] = 1, normal[2] = 0;
  }
}

/* --- the view ------------------------------------------------------------------------------------- */

static void view(void)
{
  float k = target || place_at >= 0 ? FOLLOW : GLIDE;
  for (int i = 0; i < 3; i++)
    focus[i] += (spot[i] - focus[i]) * k;
  double at[3] = { focus[0], focus[1] + LOOK_ABOVE, focus[2] };
  float level = cosf(tilt) * dist;
  double eye[3] = { at[0] - sinf(yaw) * level, at[1] + sinf(tilt) * dist, at[2] - cosf(yaw) * level };
  freecam_steer(eye, at);
}

/* --- the actors near the spot (d-left/right) --------------------------------------------------------- */

static bool starts_with(const char *s, const char *prefix)
{
  for (; *prefix; s++, prefix++)
    if (*s != *prefix)
      return false;
  return true;
}

/* the kinds debug > actors > kill can take: enemies by their death, NPCs and things deleted; not Link's own
 * companions, which go with him and are always nearest (B would delete Navi), nor what NPCs hold, which stands where
 * they do and was stepped to in their place */
static bool kind_of(const char *n, int kind)
{
  if (starts_with(n, "Npc_Navi") || starts_with(n, "Item_Ocarina")) /* not menu text */
    return false;
  if (starts_with(n, "Npc_Equipment")) /* what an NPC holds (a guard's spear): it goes with its NPC; not menu text */
    return false;
  bool enemy = starts_with(n, "Enemy_"), npc = starts_with(n, "Npc_") || starts_with(n, "Animal_"), /* animals with */
       object = starts_with(n, "Obj_") || starts_with(n, "Item_");                                      /* the npcs */
  return kind == SHOW_ENEMIES ? enemy : kind == SHOW_NPCS ? npc : kind == SHOW_OBJECTS ? object : enemy || npc || object;
}

/* what d-left/right go through: looking, the kind shown (L / R); placing, all of them (L / R turn the actor) */
static int listed(void) { return mode == MODE_LOOK ? show : SHOW_ALL; }

/* not locked on: the nearest actor of the kind shown right under the beam */
static void find_hover(void)
{
  hover = 0;
  int n = actors_near_point(spot, HOVER_UP, hover_read, HOVER_READ);
  float best = HOVER_ACROSS * HOVER_ACROSS;
  for (int i = 0; i < n; i++) {
    if (!hover_read[i].name || !kind_of(hover_read[i].name, SHOW_ALL)) /* under the beam: any kind */
      continue;
    float dx = hover_read[i].pos[0] - spot[0], dz = hover_read[i].pos[2] - spot[2];
    if (dx * dx + dz * dz < best) {
      best = dx * dx + dz * dz;
      hover = hover_read[i].actor;
      snprintf(hover_name, sizeof(hover_name), "%s", hover_read[i].name);
    }
  }
}

static void make_cycle(void)
{
  int n = actors_near_point(spot, CYCLE_RADIUS, cycle_read, CYCLE_READ);
  n_cycle = 0;
  for (int i = 0; i < n && n_cycle < CYCLE_MAX; i++)
    if (cycle_read[i].name && kind_of(cycle_read[i].name, listed()))
      cycle[n_cycle++] = cycle_read[i];
  cycle_at = -1;
  cycle_made = true;
  log_printf("placer: %d actors (%s) to step through within %.0f m (of %d running)", n_cycle, show_names[listed()],
             (double)CYCLE_RADIUS, n);
  for (int i = 0; i < n_cycle; i++)
    log_printf("placer:   %d %s %.1f m", i + 1, cycle[i].name, (double)cycle[i].distance);
}

static void let_go(void)
{
  if (target)
    log_printf("placer: let go of %s", target_name);
  target = 0;
}

/* the next (dir 1) or previous (-1) actor of the list that's still running, locked on to */
static void step(int dir)
{
  if (!cycle_made)
    make_cycle();
  if (!n_cycle) {
    let_go();
    return; /* the panel says there are none */
  }
  for (int tries = 0; tries < n_cycle; tries++) {
    cycle_at = cycle_at < 0 ? (dir > 0 ? 0 : n_cycle - 1) : (cycle_at + dir + n_cycle) % n_cycle;
    actor_read_t a;
    if (!actor_read(cycle[cycle_at].actor, &a) || !a.running)
      continue;
    target = cycle[cycle_at].actor;
    raised = false;
    snprintf(target_name, sizeof(target_name), "%s", a.name);
    memcpy(spot, a.pos, sizeof(spot));
    normal[0] = 0, normal[1] = 1, normal[2] = 0;
    grounded = true;
    hover = 0;
    log_printf("placer: locked on to %s %#lx at %.2f %.2f %.2f", target_name, (unsigned long)target, (double)a.pos[0],
               (double)a.pos[1], (double)a.pos[2]);
    return;
  }
  let_go();
  n_cycle = 0; /* none of them is left: the panel says so */
}

/* --- the explorer's places ------------------------------------------------------------------------- */

static bool same_text(const char *a, const char *b)
{
  for (; *a && *a == *b; a++, b++)
    ;
  return *a == *b;
}

static const airmap_t *map_places(void)
{
  const char *name = gd_get_str(GD_STR_MAP);
  if (!name || !*name)
    return 0;
  for (int i = 0; i < data_n_placemaps; i++)
    if (same_text(data_placemaps[i].map, name))
      return &data_placemaps[i];
  return 0;
}

static float place_distance_sq(int index)
{
  const float *p = data_places[index].pos;
  float dx = p[0] - spot[0], dy = p[1] - spot[1], dz = p[2] - spot[2];
  return dx * dx + dy * dy + dz * dz;
}

static bool of_kind(int index, int kind)
{
  return kind == EXPLORE_ALL || data_places[index].kind == kind - 1;
}

/* how many places of a kind (EXPLORE_*) the map has: L / R skip the kinds it has none of */
static int kind_count(const airmap_t *map, int kind)
{
  int n = 0;
  for (int i = 0; map && i < map->n; i++)
    n += of_kind(map->first + i, kind);
  return n;
}

/* the map's places of the kind chosen: its rooms in their numbers' order (places.json has them so), then the rest
 * nearest the spot first */
static void make_places(void)
{
  places_map = map_places();
  n_places = 0;
  places_at = -1;
  places_made = true;
  if (!places_map)
    return;
  int n_rooms = 0;
  for (int i = 0; i < places_map->n && n_places < PLACES_MAX; i++) {
    int index = places_map->first + i;
    if (!of_kind(index, explore_kind))
      continue;
    if (data_places[index].kind == PLACE_ROOM) { /* in order, before the rest */
      for (int k = n_places; k > n_rooms; k--)
        places[k] = places[k - 1];
      places[n_rooms++] = (uint16_t)index;
      n_places++;
      continue;
    }
    int at = n_places++;
    float d = place_distance_sq(index);
    for (; at > n_rooms && place_distance_sq(places[at - 1]) > d; at--)
      places[at] = places[at - 1];
    places[at] = (uint16_t)index;
  }
  log_printf("placer: %d %s in %s", n_places, explore_names[explore_kind], places_map->map);
}

/* the next (dir 1) or previous (-1) place: the spot on its floor, the camera gliding there */
static void place_step(int dir)
{
  if (!places_made || places_map != map_places())
    make_places();
  if (!n_places) {
    place_at = -1;
    return; /* the panel says there are none */
  }
  places_at = places_at < 0 ? (dir > 0 ? 0 : n_places - 1) : (places_at + dir + n_places) % n_places;
  place_at = places[places_at];
  raised = false;
  const place_t *p = &data_places[place_at];
  settle(p->pos[0], p->pos[1], p->pos[2]); /* its floor if its collision is there (it's loaded round Link) */
  if (p->kind == PLACE_ROOM && placer_take_link) { /* Link into it, the room waking round him (wake_frame) */
    memcpy(wake_at, spot, sizeof(wake_at));
    wake_wanted = true;
    wake_tries = 0;
    log_printf("placer: Link into %s, to wake it", p->label);
  }
  if (p->kind == PLACE_INTERIOR || p->kind == PLACE_GROTTO) /* from above, only its roof or the ground over it */
    dist = INSIDE_DIST, tilt = INSIDE_TILT;
  else if (p->kind == PLACE_ROOM)
    dist = ROOM_DIST, tilt = INSIDE_TILT;
  log_printf("placer: on %s at %.2f %.2f %.2f", p->label, (double)p->pos[0], (double)p->pos[1], (double)p->pos[2]);
}

/* --- starting and stopping ------------------------------------------------------------------------ */

static bool begin(int new_mode, const char *actor_name)
{
  if (placer_active) /* from one mode straight to the other: the camera stays */
    placer_stop();
  bool flying = freecam_on;
  double eye[3], at[3];
  if (!freecam_steer_begin(eye, at)) /* says why */
    return false;
  mode = new_mode;
  snprintf(name, sizeof(name), "%s", actor_name);
  float dx = (float)(at[0] - eye[0]), dy = (float)(at[1] - eye[1]), dz = (float)(at[2] - eye[2]);
  float len = root(dx * dx + dy * dy + dz * dz);
  if (len < 1e-4f)
    dx = 0, dy = -1, dz = 0, len = 1;
  dx /= len, dy /= len, dz /= len;
  float pos[3], n[3];
  vec3f_t link;
  double fx = 0, fz = 1;
  bool start_set = false;
  if (flying && cast((float)eye[0], (float)eye[1], (float)eye[2], (float)eye[0] + dx * START_LOOK,
                     (float)eye[1] + dy * START_LOOK, (float)eye[2] + dz * START_LOOK, pos, n)) {
    settle(pos[0], pos[1], pos[2]); /* where the free camera looks */
    start_set = true;
  } else if (link_drawn_pos(&link)) {
    link_facing(&fx, &fz);
    settle((float)(link.x + fx * START_AHEAD), link.y, (float)(link.z + fz * START_AHEAD));
    start_set = true;
  }
  if (!start_set) /* no ground in view and no Link: under the camera */
    settle((float)eye[0], (float)eye[1], (float)eye[2]);
  /* the camera as it was, swung round the spot; from the game's camera, higher (looking down, as The Sims) */
  float ex = (float)eye[0] - spot[0], ey = (float)eye[1] - spot[1] - LOOK_ABOVE, ez = (float)eye[2] - spot[2];
  float across = root(ex * ex + ez * ez);
  yaw = across > 0.01f ? atan2f(-ex, -ez) : atan2f(dx, dz);
  tilt = clampf(atan2f(ey, across), TILT_MIN, TILT_MAX);
  dist = clampf(root(ex * ex + ey * ey + ez * ez), DIST_MIN, DIST_MAX);
  if (!flying) {
    tilt = tilt < TILT_START ? TILT_START : tilt;
    dist = DIST_START;
  }
  facing = yaw * (180.0f / PI) + 180.0f; /* towards the camera, so its front is seen */
  held_before = ~0u;                     /* what's held now (the menu's select) isn't a press */
  made = 0;
  target = hover = 0;
  raised = false;
  cycle_made = false;
  place_at = -1;
  places_made = false;
  n_cycle = 0;
  memcpy(focus, spot, sizeof(focus));
  freecam_ui_hidden = false; /* everything shown at first */
  placer_active = true;
  view();
  log_printf("placer: %s at %.2f %.2f %.2f (%s), camera %.1f m away",
             mode == MODE_EXPLORE ? "exploring" : mode == MODE_LOOK ? "looking" : name,
             (double)spot[0], (double)spot[1], (double)spot[2], grounded ? "on the ground" : "no ground", (double)dist);
  return true;
}

bool placer_start(const char *actor_name) { return begin(MODE_PLACE, actor_name); }
bool placer_explore(void)
{
  explore_kind = EXPLORE_ALL;
  vec3f_t link;
  home_known = link_drawn_pos(&link);
  home[0] = link.x, home[1] = link.y, home[2] = link.z;
  away = false;
  wake_wanted = false;
  return begin(MODE_EXPLORE, "");
}

bool placer_look(int kind)
{
  show = kind >= 0 && kind < N_SHOWS ? kind : SHOW_ALL; /* the menu's choice each time, not the last one's */
  return begin(MODE_LOOK, "");
}

void placer_stop(void)
{
  if (!placer_active)
    return;
  placer_active = false;
  freecam_steer_end();
  if (!menu_current)
    gfx_clear(); /* its panel (the menu's own, when it's open) */
  log_printf("placer: done %s (%d made)", mode == MODE_EXPLORE ? "exploring" : mode == MODE_LOOK ? "looking" : name,
             made);
}

/* --- its panel: what's under the beam, and the controls as the game's button glyphs --------------------- */

/* a hint line as the grid gets it, for the panel's width: in the game's style its button words take two cells (their
 * glyphs' width; the line itself is drawn from the hint), the rest as it is; gz's style shows the line as it is */
static bool word_is(const char *word, const char *s, int n)
{
  int k = 0;
  while (k < n && word[k] && word[k] == s[k])
    k++;
  return k == n && !word[k];
}

static void grid_line(const char *src, char *dst, int len)
{
  static const char *const words[] = { "LS", "RS", "L3", "R3", "X", "Y", "A", "B", "d-up/down", "d-left/right", "L/R",
                                       "ZL/ZR" };
  int n = 0;
  while (*src && n < len - 1) {
    const char *end = src;
    while (*end && *end != ' ')
      end++;
    int w = (int)(end - src), cells = w;
    if (gfx_style == GFX_STYLE_GAME)
      for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (word_is(words[i], src, w))
          cells = words[i][0] != 'd' && words[i][1] == '/' ? 4 : words[i][2] == '/' ? 4 : 2; /* L/R, ZL/ZR: two */
    for (int k = 0; k < cells && n < len - 1; k++)
      dst[n++] = cells == w ? src[k] : 0x7F; /* 0x7F: drawn as nothing, holds the width */
    for (src = end; *src == ' ' && n < len - 1; src++)
      dst[n++] = ' ';
  }
  dst[n] = 0;
}

/* a door's close countdown in the watches (the door close watch, W_DOOR_CLOSE): on for this door */
static bool door_watched(uintptr_t actor)
{
  return actor && interactview_door_watched() == actor && watch_has(W_DOOR_CLOSE);
}

static void panel(void)
{
  if (freecam_ui_hidden) { /* R3: none of it, for a clear picture */
    gfx_clear();
    return;
  }
  char title[96], under[96], stats[64], list[96];
  if (mode == MODE_EXPLORE)
    snprintf(title, sizeof(title), "%s", T("explorer"));
  else if (mode == MODE_LOOK)
    snprintf(title, sizeof(title), "%s", T("browse actors"));
  else
    snprintf(title, sizeof(title), TF("place %s"), name);
  actor_read_t a;
  uintptr_t at = target ? target : hover;
  stats[0] = 0; /* its stats on the line under its name (kept, empty, so the panel doesn't jump) */
  if (mode == MODE_EXPLORE) { /* "room 3: deku baba x2, chest x2": its name, and what's in it under it */
    const char *label = place_at >= 0 ? T(data_places[place_at].label) : T("no place chosen");
    int cut = 0;
    while (label[cut] && !(label[cut] == ':' && label[cut + 1] == ' '))
      cut++;
    snprintf(under, sizeof(under), "%.*s", cut, label);
    if (label[cut])
      snprintf(stats, sizeof(stats), "%s", label + cut + 2);
    at = 0;
  } else if (at && actor_read(at, &a)) {
    snprintf(under, sizeof(under), "%s", a.name);
    if (a.killable) /* none for one that can't be killed: its life is the default 100 */
      snprintf(stats, sizeof(stats), a.invincible ? TF("life %d/%d, invincible") : TF("life %d/%d"), (int)a.life,
               (int)a.max_life);
  } else {
    snprintf(under, sizeof(under), "%s", grounded ? T("nothing under the beam") : T("no ground under the beam"));
  }
  char count[64] = ""; /* exploring: where in the list, on its own line under the type */
  if (mode == MODE_EXPLORE) {
    snprintf(list, sizeof(list), TF("type: %s"), explore_kind == EXPLORE_ALL ? T("all") : T(explore_names[explore_kind]));
    if (places_made && !n_places)
      snprintf(count, sizeof(count), "%s", T("none in this map"));
    else if (place_at >= 0 && places_at >= 0)
      snprintf(count, sizeof(count), TF("%d of %d"), places_at + 1, n_places);
  } else if (cycle_made && !n_cycle)
    snprintf(list, sizeof(list), TF("no %s within %d m"), T(show_names[listed()]), (int)CYCLE_RADIUS);
  else if (target && cycle_at >= 0 && listed() == SHOW_ALL)
    snprintf(list, sizeof(list), TF("%d of %d near"), cycle_at + 1, n_cycle);
  else if (target && cycle_at >= 0)
    snprintf(list, sizeof(list), TF("%d of %d %s near"), cycle_at + 1, n_cycle, T(show_names[listed()]));
  else
    snprintf(list, sizeof(list), "%s", T(show_names[listed()]));
  /* the controls: in the game's style, one line in its control guide across the bottom centre (gfx_guide); in gz's,
   * three short lines in the panel */
  static const char *const guide[3] = { /* MODE_PLACE, MODE_LOOK, MODE_EXPLORE: moving on top, acting under it */
    "LS move  d-up/down height  RS camera  ZL/ZR zoom  X faster  L3 back to Link\n"
    "d-left/right next  L/R turn  A spawn  Y kill/despawn  R3 hide controls  B done",
    "LS move  d-up/down height  RS camera  ZL/ZR zoom  X faster  L3 back to Link\n"
    "d-left/right next  L/R kind  A go to  Y kill/despawn  R3 hide controls  B done",
    "LS move  d-up/down height  RS camera  ZL/ZR zoom  X faster  L3 back to Link\n"
    "d-left/right next  L/R kind  A go to  Y take Link: %s  R3 hide controls  B done", /* on / off */
  };
  static const char *const hints[3][3] = { /* short: the panel is as wide as its widest line */
    { "LS move  d-up/down height  L3 back to Link  R3 hide controls", "RS camera  ZL/ZR zoom  X faster  d-left/right next",
      "L/R turn  A spawn  Y kill/despawn  B done" },
    { "LS move  d-up/down height  L3 back to Link  R3 hide controls", "RS camera  ZL/ZR zoom  X faster  d-left/right next",
      "L/R kind  A go to  Y kill/despawn  B done" },
    { "LS move  d-up/down height  L3 back to Link  R3 hide controls", "RS camera  ZL/ZR zoom  X faster  d-left/right next",
      "L/R kind  A go to  Y take Link: %s  B done" },
  };
  bool in_guide = gfx_style == GFX_STYLE_GAME;
  const char *const *h = hints[mode];
  /* browsing, on a guard: its vision and path too */
  static const char *const guard_guide = "LS move  d-up/down height  RS camera  ZL/ZR zoom  X faster  L3 back to Link\n"
                                         "d-left/right next  L/R kind  A go to  Y kill/despawn  minus actor options  "
                                         "R3 hide controls  B done";
  static const char *const guard_hints[3] = { "LS move  d-up/down height  L3 back to Link  R3 hide controls",
                                              "RS camera  ZL/ZR zoom  X faster  d-left/right next",
                                              "L/R kind  A go to  Y kill  minus actor options  B done" };
  bool on_guard = mode == MODE_LOOK && at && (guardview_has_path(at) || interactview_has(at)); /* it has options */
  if (on_guard)
    h = guard_hints;
  const char *take = placer_take_link ? T("on") : T("off"); /* exploring: Y take Link, as it is now */
  char hint[GFX_HINT_LEN], last[96], line_text[GFX_HINT_LEN]; /* last: one short line */
  snprintf(last, sizeof(last), mode == MODE_EXPLORE ? TF(h[2]) : "%s", mode == MODE_EXPLORE ? take : T(h[2]));
  snprintf(hint, sizeof(hint), "%s\n%s\n%s", T(h[0]), T(h[1]), last);
  if (in_guide) {
    snprintf(line_text, sizeof(line_text), mode == MODE_EXPLORE ? TF(guide[mode]) : "%s",
             mode == MODE_EXPLORE ? take : on_guard ? T(guard_guide) : T(guide[mode]));
    gfx_guide(line_text);
  }
  gfx_begin();
  gfx_text(0, 0, GFX_TITLE, title);
  gfx_row(0, GFX_ROW_TITLE);
  gfx_row(1, GFX_ROW_GAP);
  gfx_text(PANEL_COL, 2, at ? GFX_NORMAL : GFX_DIM, under);
  gfx_row(2, GFX_ROW_LINE);
  /* its own switches (its options, minus), one under another with their checkboxes as the menu draws them */
  char check[64];
  const char *labels[2 + IV_PARTS + 1];
  bool on[2 + IV_PARTS + 1];
  int n_checks = 0;
  if (mode == MODE_LOOK && at) {
    if (guardview_is_guard(at)) {
      labels[n_checks] = T("vision"), on[n_checks++] = guardview_own(at, GV_VISION);
      labels[n_checks] = T("patrol path"), on[n_checks++] = guardview_own(at, GV_PATH);
    } else if (guardview_has_path(at)) { /* a townsperson, a dog, a platform: the rail it walks */
      labels[n_checks] = T("path"), on[n_checks++] = guardview_own(at, GV_PATH);
    }
    if (interactview_has(at)) /* talk radius, z-target talk, target break line; a door's door close */
      for (int part = 0; part < IV_PARTS; part++)
        if (part != IV_DOOR || interactview_is_door(at))
          labels[n_checks] = interactview_part_label(at, part), on[n_checks++] = interactview_own(at, part);
    if (interactview_is_door(at)) {
      labels[n_checks] = T("close timer watch"), on[n_checks++] = door_watched(at);
    }
  }
  int row = 3;
  if (stats[0] || !n_checks) { /* its stats (an empty line kept for one with none, so the panel doesn't jump) */
    gfx_text(PANEL_COL, row, GFX_NORMAL, stats[0] ? stats : "\x7F"); /* 0x7F: an empty line that's still a line */
    gfx_row(row++, GFX_ROW_LINE);
  }
  {
    for (int i = 0; i < n_checks; i++, row++) {
      snprintf(check, sizeof(check), "%c%c %s", on[i] ? GFX_CHECK_ON : GFX_CHECK_OFF, GFX_WIDE_REST, labels[i]);
      gfx_text(PANEL_COL, row, GFX_NORMAL, check);
      gfx_row(row, GFX_ROW_LINE);
    }
  }
  gfx_text(PANEL_COL, row, GFX_DIM, list);
  gfx_row(row++, GFX_ROW_LINE);
  if (mode == MODE_EXPLORE) { /* the count's line, kept when empty so the panel doesn't jump */
    gfx_text(PANEL_COL, row, GFX_DIM, count[0] ? count : "\x7F");
    gfx_row(row++, GFX_ROW_LINE);
  }
  /* where it is: the actor on or under the beam, else the cursor (the beam's spot), under a heading of its own */
  float where[3];
  bool on_actor = mode != MODE_EXPLORE && at && actor_read(at, &a);
  memcpy(where, on_actor ? a.pos : spot, sizeof(where));
  row = gfx_position(PANEL_COL, row, on_actor ? T("position") : T("cursor"), where);
  if (in_guide) {
    gfx_end();
    return;
  }
  gfx_row(row++, GFX_ROW_GAP);
  for (int i = 0; i < 3; i++, row++) { /* the hint's lines on the grid too: its width, and gz's own style draws them */
    char line[GFX_COLS + 1];
    grid_line(i == 2 ? last : T(h[i]), line, sizeof(line));
    gfx_text(PANEL_COL, row, GFX_DIM, line);
    gfx_row(row, GFX_ROW_HINT);
  }
  gfx_hint(hint);
  gfx_end();
}

uintptr_t placer_selected(void) { return placer_active && mode == MODE_LOOK ? (target ? target : hover) : 0; }

bool placer_spot(float out[3])
{
  if (!placer_active)
    return false;
  memcpy(out, spot, sizeof(spot));
  return true;
}

/* --- an actor's options (minus while browsing, on a guard or anything Link can talk to or act on) ----------- */

static menu_t m_guard;
static uintptr_t menu_guard;  /* the actor it's for */
static char menu_title[64];   /* its name */
static char menu_actions[IV_PARTS][40]; /* "talk radius", "z-target talk", "target break line" */

static bool get_guard_vision(menu_item_t *it) { return guardview_own(menu_guard, GV_VISION); }
static bool get_guard_path(menu_item_t *it) { return guardview_own(menu_guard, GV_PATH); }

static void set_guard_vision(menu_item_t *it, bool on)
{
  if (guardview_own(menu_guard, GV_VISION) != on)
    guardview_toggle(menu_guard, GV_VISION);
}

static void set_guard_path(menu_item_t *it, bool on)
{
  if (guardview_own(menu_guard, GV_PATH) != on)
    guardview_toggle(menu_guard, GV_PATH);
}

static bool get_door_watch(menu_item_t *it) { return door_watched(menu_guard); }

static void set_door_watch(menu_item_t *it, bool on)
{
  if (on) {
    interactview_door_watch(menu_guard);
    watch_add(W_DOOR_CLOSE); /* already there: it moves to this door */
    watches_shown = true;    /* so it's seen */
    log_printf("placer: door close watch on");
    return;
  }
  for (int i = 0; i < watch_count; i++)
    if (watch_list[i].id == W_DOOR_CLOSE)
      watch_remove(i--);
  interactview_door_watch(0);
  log_printf("placer: door close watch off");
}

/* "open door" while it's shut, "close door" while it's open (its panel's pose): opens it by itself, as for an NPC, or
 * shuts it at once whoever is in the way (interactview.c); the menu can stay open, the door's AI runs. Its label
 * follows the door (door_action_label, kept up by placer_frame). */
static menu_item_t *door_action;

static const char *door_action_label(uintptr_t door)
{
  return interactview_door_is_open(door) == 1 ? "close door" : "open door";
}

static void act_door(menu_item_t *it)
{
  bool open = interactview_door_is_open(menu_guard) == 1;
  log_printf("placer: %s (turned %.1f deg)", open ? "close door" : "open door",
             (double)interactview_door_angle(menu_guard));
  if (open)
    interactview_door_close_now(menu_guard);
  else if (interactview_door_open(menu_guard))
    gfx_notice(T("opening the door"));
}

/* its interaction range's parts: the item's data is which (IV_*) */
static bool get_interact(menu_item_t *it) { return interactview_own(menu_guard, (int)it->data); }

static void set_interact(menu_item_t *it, bool on)
{
  if (interactview_own(menu_guard, (int)it->data) != on)
    interactview_toggle(menu_guard, (int)it->data);
}

void placer_menu_build(void) { menu_init(&m_guard, menu_title, 2 + IV_PARTS + 4); }

/* this actor's own switches, each for it alone (whatever scene > environment says): its items made again for it */
static void open_guard_menu(uintptr_t actor)
{
  actor_read_t a;
  if (!actor_read(actor, &a))
    return;
  menu_guard = actor;
  snprintf(menu_title, sizeof(menu_title), "%s", a.name);
  menu_reset(&m_guard);
  door_action = 0;
  m_guard.selected = 0;
  menu_item_t *item;
  if (guardview_is_guard(actor)) {
    item = menu_add_check(&m_guard, "vision", get_guard_vision, set_guard_vision, 0);
    if (item) {
      item->hint = "this guard's sight: where it catches link";
      item->log_label = "its vision:";
    }
    item = menu_add_check(&m_guard, "patrol path", get_guard_path, set_guard_path, 0);
    if (item) {
      item->hint = "the path this guard walks";
      item->log_label = "its patrol path:";
    }
  } else if (guardview_has_path(actor)) {
    item = menu_add_check(&m_guard, "path", get_guard_path, set_guard_path, 0);
    if (item) {
      item->hint = "the path it walks (the rail the map gives it)";
      item->log_label = "its path:";
    }
  }
  if (interactview_has(actor)) { /* talk radius, z-target talk, target break line: each its own */
    static const char *const hints[IV_PARTS] = { "where link can talk to it (facing it)",
                                                 "how far it reaches while z-targeting",
                                                 "dashed: where each breaks as link walks away",
                                                 "where it's held open: leave it and it closes 135 frames later" };
    for (int part = 0; part < IV_PARTS; part++) {
      if (part == IV_DOOR && !interactview_is_door(actor))
        continue;
      snprintf(menu_actions[part], sizeof(menu_actions[part]), "%s", interactview_part_label(actor, part));
      item = menu_add_check(&m_guard, menu_actions[part], get_interact, set_interact, part);
      if (item) {
        item->hint = hints[part];
        item->log_label = ""; /* interactview.c logs it */
      }
    }
  }
  if (interactview_is_door(actor)) {
    item = menu_add_check(&m_guard, "close timer watch", get_door_watch, set_door_watch, 0);
    if (item) {
      item->hint = "door close timer in the watches: frames until it shuts (135 while held open)";
      item->log_label = ""; /* logged above */
    }
    door_action = menu_add_action(&m_guard, door_action_label(actor), act_door, 0);
    if (door_action) {
      door_action->hint = "L: it opens by itself, or it shuts at once, even with link holding it open";
      door_action->log_label = "";
    }
  }
  menu_add_return(&m_guard); /* back to browsing: the menu closes, the browser is still on */
  if (!menu_current)
    menu_open(&menu_root);
  menu_goto(&m_guard); /* no parent: B or return closes the menu, back to the browser */
  log_printf("placer: %s's options", a.name);
}

/* --- each frame ------------------------------------------------------------------------------------ */

void placer_frame(bool menu_open)
{
  if (door_action && menu_current == &m_guard) /* "open door" / "close door", as the door is now */
    door_action->label = door_action_label(menu_guard);
  if (!placer_active)
    return;
  if (!freecam_steering()) { /* the free camera was turned off (its bind or the menu) */
    placer_active = false;
    if (!menu_open)
      gfx_clear();
    log_printf("placer: the free camera was turned off (%d made)", made);
    return;
  }
  vec3f_t link;
  if (!link_drawn_pos(&link)) { /* a map change, the title screen */
    placer_stop();
    return;
  }
  if (menu_open) {
    held_before = ~0u; /* what closes it isn't a press here */
    view();
    return;
  }
  int32_t lx, ly, rx, ry;
  input_sticks(&lx, &ly, &rx, &ry);
  if (input_remote_active && input_remote_sticks[0])
    lx = input_remote_stick[0], ly = input_remote_stick[1];
  if (input_remote_active && input_remote_sticks[1])
    rx = input_remote_stick[2], ry = input_remote_stick[3];
  uint32_t held = input_held() | (input_remote_active ? input_remote_buttons : 0);
  uint32_t pressed = held & ~held_before;
  held_before = held;

  /* the actor locked on to: the spot goes with it, until it dies or goes */
  if (target) {
    actor_read_t a;
    if (actor_read(target, &a) && a.running) {
      memcpy(spot, a.pos, sizeof(spot));
    } else {
      log_printf("placer: %s has gone", target_name);
      target = 0;
    }
  }
  if (mode == MODE_EXPLORE)
    wake_frame();
  if (pressed & BTN_B) {
    bool give_back = mode == MODE_EXPLORE && away && home_known;
    wake_wanted = false;
    placer_stop();
    if (give_back && !position_busy() && position_goto(home[0], home[1], home[2])) /* back where he was */
      log_printf("placer: Link back to %.3f %.3f %.3f", (double)home[0], (double)home[1], (double)home[2]);
    return;
  }
  if (mode == MODE_EXPLORE && pressed & BTN_Y) { /* take Link into the rooms, or not (the bar says which) */
    placer_take_link = !placer_take_link;
    log_printf("placer: take Link into rooms %s", placer_take_link ? "on" : "off");
    if (placer_take_link && place_at >= 0 && data_places[place_at].kind == PLACE_ROOM) {
      memcpy(wake_at, spot, sizeof(wake_at)); /* into the room the camera's on, now */
      wake_wanted = true;
      wake_tries = 0;
    } else if (!placer_take_link) {
      wake_wanted = false;
      if (away && home_known && !position_busy() && position_goto_quiet(home[0], home[1], home[2]))
        away = false; /* not taken along any more: back where he was */
    }
  }
  if (mode == MODE_LOOK && pressed & BTN_MINUS) { /* its options (a guard's vision and path, its interaction range) */
    uintptr_t actor = target ? target : hover;
    if (actor && (guardview_has_path(actor) || interactview_has(actor)))
      open_guard_menu(actor);
  }
  if (mode != MODE_EXPLORE && pressed & BTN_Y) { /* the actor locked on to, or under the beam */
    uintptr_t victim = target ? target : hover;
    if (!victim)
      gfx_notice("nothing under the beam");
    else if (actorsmenu_kill(victim)) {
      let_go(); /* the spot stays where it died; d-left/right go on through the same list */
      hover = 0;
    }
  }
  if (pressed & BTN_DRIGHT) /* right: on up the list (a room's number up), left: back down */
    mode == MODE_EXPLORE ? place_step(1) : step(1);
  else if (pressed & BTN_DLEFT)
    mode == MODE_EXPLORE ? place_step(-1) : step(-1);
  /* exploring: L / R the kind of place, and on to the first of it */
  if (mode == MODE_EXPLORE && pressed & (BTN_L | BTN_R)) { /* the next kind the map has any of */
    const airmap_t *map = map_places();
    for (int tries = 0; tries < N_EXPLORES; tries++) {
      explore_kind = (explore_kind + (pressed & BTN_R ? 1 : N_EXPLORES - 1)) % N_EXPLORES;
      if (kind_count(map, explore_kind))
        break;
    }
    place_at = -1;
    places_made = false;
    place_step(1);
  }
  /* looking: L / R the kind shown, and on to the nearest of it */
  if (mode == MODE_LOOK && pressed & (BTN_L | BTN_R)) {
    show = (show + (pressed & BTN_R ? 1 : N_SHOWS - 1)) % N_SHOWS;
    let_go();
    cycle_made = false;
    step(1);
  }
  /* the camera: round the spot, up and down, nearer and further (freecam.c's sense: right turns right, up looks up) */
  yaw -= axis(rx) * SWING_SPEED;
  tilt = clampf(tilt - axis(ry) * SWING_SPEED, TILT_MIN, TILT_MAX);
  if (held & BTN_ZR) /* zoom: ZR in, ZL out */
    dist = clampf(dist / ZOOM_RATE, DIST_MIN, DIST_MAX);
  if (held & BTN_ZL)
    dist = clampf(dist * ZOOM_RATE, DIST_MIN, DIST_MAX);
  /* the spot up and down (d-up / d-down): it keeps that height across until L3 drops it to the ground */
  float boost = held & BTN_X ? FAST : 1.0f;
  float rise = (held & BTN_DUP ? 1.0f : 0) - (held & BTN_DDOWN ? 1.0f : 0);
  if (rise) {
    if (target)
      let_go();
    place_at = -1;
    raised = true;
    grounded = false;
    spot[1] += rise * RAISE_SPEED * boost;
    normal[0] = 0, normal[1] = 1, normal[2] = 0;
  }
  if (pressed & BTN_RSTICK) /* hide or show the bar, the beam and the panel */
    freecam_ui_hidden = !freecam_ui_hidden;
  if (pressed & BTN_LSTICK) { /* back to Link: the spot on the ground where he stands, the camera gliding there */
    vec3f_t link;
    if (link_drawn_pos(&link)) {
      if (target)
        let_go();
      place_at = -1;
      raised = false;
      cycle_made = places_made = false; /* the next d-left/right: what's near him */
      settle(link.x, link.y, link.z);
    }
  }
  /* the spot: as the camera faces (freecam.c's moves) */
  float speed = dist * MOVE_RATE * boost;
  float fwd = axis(ly) * speed, side = axis(lx) * speed;
  if (fwd || side) {
    if (target) {
      let_go();
    }
    cycle_made = false; /* the next d-left/right: the actors (or places) near where the spot is then */
    place_at = -1;
    places_made = false;
    float sy = sinf(yaw), cy = cosf(yaw);
    if (raised) /* across at its height */
      spot[0] += sy * fwd - cy * side, spot[2] += cy * fwd + sy * side;
    else
      settle(spot[0] + sy * fwd - cy * side, spot[1], spot[2] + cy * fwd + sy * side);
  } else if (!grounded && !target && !raised) {
    settle(spot[0], spot[1], spot[2]); /* ground may have come (streamed in) */
  }
  /* placing: the actor's facing, a step a press of L / R */
  if (mode == MODE_PLACE && pressed & BTN_L)
    facing += TURN_STEP;
  if (mode == MODE_PLACE && pressed & BTN_R)
    facing -= TURN_STEP;
  /* raised: the ground under it, for the line down to it */
  below_known = false;
  if (raised) {
    float pos[3], n[3];
    below_known = cast(spot[0], spot[1], spot[2], spot[0], spot[1] - DROP, spot[2], pos, n);
    below_y = pos[1];
  }
  facing = facing >= 360.0f ? facing - 360.0f : facing < 0 ? facing + 360.0f : facing;
  if (!target && mode != MODE_EXPLORE)
    find_hover();
  else if (mode == MODE_EXPLORE)
    hover = 0;
  /* browsing: the beam left resting on an actor locks on to it, so the camera follows it as it walks (a guard on
   * its patrol); the stick lets go again */
  if (mode == MODE_LOOK && !target && hover && !fwd && !side && !rise) {
    actor_read_t a;
    if (actor_read(hover, &a) && a.running) {
      target = hover;
      raised = false;
      snprintf(target_name, sizeof(target_name), "%s", hover_name);
      hover = 0;
      log_printf("placer: locked on to %s %#lx under the beam", target_name, (unsigned long)target);
    }
  }

  if (mode != MODE_PLACE && pressed & BTN_A) { /* go to: Link to the actor, or the spot; it goes on (B ends it) */
    uintptr_t there = target ? target : hover;
    actor_read_t a;
    float to[3];
    const char *what = "the beam";
    if (there && actor_read(there, &a)) {
      memcpy(to, a.pos, sizeof(to));
      what = a.name;
    } else {
      memcpy(to, spot, sizeof(to)); /* over nothing: where the beam stands (in the air if there's no ground) */
    }
    if (position_goto(to[0], to[1], to[2])) {
      log_printf("placer: go to %s at %.3f %.3f %.3f", what, (double)to[0], (double)to[1], (double)to[2]);
      memcpy(home, to, sizeof(home)); /* where the user put him: B leaves him here */
      home_known = true;
      away = false;
      wake_wanted = false;
    }
  }
  if (mode == MODE_PLACE && pressed & BTN_A) {
    float at[3] = { spot[0], spot[1], spot[2] };
    if (actorsmenu_spawn_at(name, at, facing))
      made++;
  }
  view();
  panel();
}

/* --- the beam, ring and arrow ------------------------------------------------------------------------ */

static struct {
  float (*ends)[2][3];
  uint8_t *colours;
  int n, max;
} out;

static void line(const float a[3], const float b[3], uint8_t colour)
{
  if (out.n >= out.max)
    return;
  memcpy(out.ends[out.n][0], a, sizeof(float) * 3);
  memcpy(out.ends[out.n][1], b, sizeof(float) * 3);
  out.colours[out.n++] = colour;
}

/* a point on the ground round the spot: (dx, dz) across, lifted onto the ground's slope */
static void on_ground(float dx, float dz, float p[3])
{
  float ny = normal[1] > FLAT_NY ? normal[1] : 1, nx = normal[1] > FLAT_NY ? normal[0] : 0,
        nz = normal[1] > FLAT_NY ? normal[2] : 0;
  p[0] = spot[0] + dx;
  p[1] = spot[1] + RING_LIFT - (nx * dx + nz * dz) / ny;
  p[2] = spot[2] + dz;
}

int placer_lines(float ends[][2][3], uint8_t colours[], int max)
{
  if (!placer_active || freecam_ui_hidden)
    return 0;
  out.ends = ends, out.colours = colours, out.n = 0, out.max = max;
  uint8_t colour = target || hover ? AW_PLACER_TARGET : grounded ? AW_PLACER : AW_PLACER_AIR;
  /* the beam: three lines close together, for its width */
  for (int i = 0; i < 3; i++) {
    float a = (float)i * (2 * PI / 3);
    float bottom[3] = { spot[0] + sinf(a) * BEAM_WIDTH, spot[1], spot[2] + cosf(a) * BEAM_WIDTH };
    float top[3] = { bottom[0], spot[1] + BEAM_HEIGHT, bottom[2] };
    line(bottom, top, colour);
  }
  /* raised: a line down to the ground under it, and a cross where it meets it */
  if (raised && below_known) {
    float top[3] = { spot[0], spot[1], spot[2] }, ground[3] = { spot[0], below_y, spot[2] };
    line(top, ground, AW_PLACER_AIR);
    for (int i = 0; i < 2; i++) {
      float a[3] = { spot[0] - (i ? 0 : RING_IN), below_y, spot[2] - (i ? RING_IN : 0) };
      float b[3] = { spot[0] + (i ? 0 : RING_IN), below_y, spot[2] + (i ? RING_IN : 0) };
      line(a, b, AW_PLACER_AIR);
    }
  }
  /* the rings */
  for (int r = 0; r < 2; r++) {
    float radius = r ? RING_IN : RING_OUT, prev[3], first[3];
    for (int i = 0; i <= RING_SEGMENTS; i++) {
      float a = (float)i * (2 * PI / RING_SEGMENTS), p[3];
      if (i == RING_SEGMENTS)
        memcpy(p, first, sizeof(p));
      else
        on_ground(sinf(a) * radius, cosf(a) * radius, p);
      if (i)
        line(prev, p, colour);
      else
        memcpy(first, p, sizeof(first));
      memcpy(prev, p, sizeof(prev));
    }
  }
  if (mode != MODE_PLACE)
    return out.n;
  /* the arrow outside them: the way the actor will face */
  float f = facing * (PI / 180.0f), fx = sinf(f), fz = cosf(f), tip[3], left[3], right[3];
  on_ground(fx * ARROW_TIP, fz * ARROW_TIP, tip);
  on_ground(fx * ARROW_BASE - fz * ARROW_HALF, fz * ARROW_BASE + fx * ARROW_HALF, left);
  on_ground(fx * ARROW_BASE + fz * ARROW_HALF, fz * ARROW_BASE - fx * ARROW_HALF, right);
  line(left, tip, colour);
  line(right, tip, colour);
  line(left, right, colour);
  return out.n;
}
