/* The scene's actors: the enemies and their life, for enemyhp.c. Read from the game's disassembly
 * (FINDINGS §9.5).
 *
 * The actor manager *(main+actor_mgr_ptr) keeps the live actors in an intrusive red-black tree at
 * [mgr+0x38]: node N has left at +0x00, right at +0x08; each node sits at +8 in an entry E, which holds
 * the actor at +0x28 and the next entry with the same key at +0x38 (no -8). The game walks it in order
 * under the manager's lock; here it's read without the lock, so every pointer is checked and the walk
 * is capped (a tree changing under us costs at most a frame of wrong bars).
 *
 * An actor: its name (char *) at +0x20 ("Enemy_Wolfos"), its state at +0x2C (4 and 5: running; 6: an
 * enemy that hasn't appeared yet), its world position (3 floats) at +0x31C. Its Life component is
 * [[actor+0x270]+0x2B8] (vtable vt_life), with parameter objects (vtable vt_param, s32 value at +8)
 * for the current life at +0x1C10 and the max at +0x1C18 (QueryActorLife reads exactly these). */
#include "enemyhp.h"

#include <string.h>

#include "link.h"
#include "sys.h"
#include "z64hd.h"

float sqrtf(float x), sinf(float x), cosf(float x); /* from sdk */

#define MGR_TREE_ROOT 0x38
#define NODE_LEFT 0x00
#define NODE_RIGHT 0x08
#define ENTRY_NODE 0x08   /* the node's place in its entry */
#define ENTRY_ACTOR 0x28
#define ENTRY_NEXT 0x38
#define ENTRY_SIZE 0x40

#define ACTOR_NAME 0x20
#define ACTOR_STATE 0x2C
#define ACTOR_COMPONENTS 0x270
#define ACTOR_POS 0x31C
#define COMPONENTS_LIFE 0x2B8
#define LIFE_CURRENT 0x1C10
#define LIFE_MAX 0x1C18
#define PARAM_VALUE 0x8

/* states: below 2 not started (the game's walks skip them), 3 being set up, 4 and 5 running, 6 not yet
 * appeared (a Wolfos still in the ground, Stalfos before they drop, Poes before they show), 7..11 dying
 * or gone (FINDINGS §9.5) */
#define STATE_RUNNING 4
#define STATE_RUNNING_LAST 5

#define MAX_DEPTH 64
#define MAX_VISITS 4096
#define ENEMY_PREFIX "Enemy_"
#define BAR_HEIGHT 1.8f   /* metres above the actor's position; one height for all for now */
#define NEAR_LINK 30.0f   /* metres: enemies further from Link are left out (a dungeon keeps all its rooms') */
#define REPORT_FRAMES 300 /* a summary in the log this often while enemy health is on */

static bool readable(uintptr_t addr, size_t size)
{
  return addr && !(addr & 7) && sys_mapped(addr, size, SYS_PERM_R);
}

static bool starts_with(const char *s, const char *prefix)
{
  for (; *prefix; s++, prefix++)
    if (*s != *prefix)
      return false;
  return true;
}

#define MAIN_SIZE_MAX 0x5000000 /* main's image is smaller than this */

static bool in_main(uintptr_t addr)
{
  return addr >= z64hd_main && addr < z64hd_main + MAIN_SIZE_MAX;
}

/* why actor_life gave up (for the log) */
enum { LIFE_OK, NO_COMPONENTS, NO_LIFE, LIFE_UNREADABLE, NO_PARAMS, N_LIFE_RESULTS };

/* An actor's life, if it has a Life component. Its vtable isn't required to be vt_life (Link's): an
 * enemy's may be a kind of it. */
static int actor_life(uintptr_t actor, int32_t *life, int32_t *max)
{
  if (!readable(actor + ACTOR_COMPONENTS, 8))
    return NO_COMPONENTS;
  uintptr_t components = *(uintptr_t *)(actor + ACTOR_COMPONENTS);
  if (!readable(components + COMPONENTS_LIFE, 8))
    return NO_COMPONENTS;
  uintptr_t component = *(uintptr_t *)(components + COMPONENTS_LIFE);
  if (!component)
    return NO_LIFE;
  if (!readable(component, LIFE_MAX + 8))
    return LIFE_UNREADABLE;
  uintptr_t cur = *(uintptr_t *)(component + LIFE_CURRENT);
  uintptr_t top = *(uintptr_t *)(component + LIFE_MAX);
  /* QueryActorLife reads +8 of both without checking their kind; here they must at least be objects of
   * the game's (a vtable in main) */
  if (!readable(cur, PARAM_VALUE + 4) || !readable(top, PARAM_VALUE + 4) || !in_main(*(uintptr_t *)cur) ||
      !in_main(*(uintptr_t *)top))
    return NO_PARAMS;
  *life = *(int32_t *)(cur + PARAM_VALUE);
  *max = *(int32_t *)(top + PARAM_VALUE);
  return LIFE_OK;
}

/* --- the walk ------------------------------------------------------------------------------------ */

static struct {
  void (*visit)(uintptr_t actor); /* use_actor, or find_named's */
  enemy_t *out;
  float distance_sq[64]; /* each out[i]'s from Link, to keep the nearest when out is full */
  int max, n;
  int visits, actors, with_life, enemies;
  uintptr_t link;
  vec3f_t link_pos;
  /* enemies left out, and why (for the log): the first few of a walk */
  struct {
    uintptr_t actor;
    int why; /* SKIP_* */
    int life_result, life, max;
    uint32_t state;
    float distance;
  } skipped[8];
  int n_skipped;
} walk;

/* why an enemy got no bar */
enum { SKIP_LIFE, SKIP_STATE, SKIP_MAX, SKIP_FAR, N_SKIPS };
static const char *const skip_names[N_SKIPS] = { "no life", "not running", "no max life", "too far" };
static const char *const life_names[N_LIFE_RESULTS] = { "ok", "no components", "no life component",
                                                        "life unreadable", "no life values" };

static void note_skipped(uintptr_t actor, int why, int life_result, int32_t life, int32_t max, float distance)
{
  if (walk.n_skipped >= (int)(sizeof(walk.skipped) / sizeof(walk.skipped[0])))
    return;
  walk.skipped[walk.n_skipped].actor = actor;
  walk.skipped[walk.n_skipped].why = why;
  walk.skipped[walk.n_skipped].life_result = life_result;
  walk.skipped[walk.n_skipped].life = life;
  walk.skipped[walk.n_skipped].max = max;
  walk.skipped[walk.n_skipped].state = *(uint32_t *)(actor + ACTOR_STATE);
  walk.skipped[walk.n_skipped].distance = distance;
  walk.n_skipped++;
}

static float distance_to_link(uintptr_t actor)
{
  const float *pos = (const float *)(actor + ACTOR_POS);
  float dx = pos[0] - walk.link_pos.x, dy = pos[1] - walk.link_pos.y, dz = pos[2] - walk.link_pos.z;
  return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void use_actor(uintptr_t actor)
{
  walk.actors++;
  if (!readable(actor, ACTOR_POS + 12) || actor == walk.link)
    return;
  int32_t life = 0, max = 0;
  int result = actor_life(actor, &life, &max);
  uintptr_t name = *(uintptr_t *)(actor + ACTOR_NAME);
  bool enemy = name && sys_mapped(name, 8, SYS_PERM_R) && starts_with((const char *)name, ENEMY_PREFIX);
  if (result != LIFE_OK) {
    if (enemy)
      note_skipped(actor, SKIP_LIFE, result, 0, 0, distance_to_link(actor));
    return;
  }
  walk.with_life++;
  if (!enemy)
    return;
  uint32_t state = *(uint32_t *)(actor + ACTOR_STATE);
  if (state < STATE_RUNNING || state > STATE_RUNNING_LAST || max <= 0) {
    note_skipped(actor, max <= 0 ? SKIP_MAX : SKIP_STATE, result, life, max, distance_to_link(actor));
    return;
  }
  const float *pos = (const float *)(actor + ACTOR_POS);
  float dx = pos[0] - walk.link_pos.x, dy = pos[1] - walk.link_pos.y, dz = pos[2] - walk.link_pos.z;
  float distance_sq = dx * dx + dy * dy + dz * dz;
  if (distance_sq > NEAR_LINK * NEAR_LINK) {
    note_skipped(actor, SKIP_FAR, result, life, max, sqrtf(distance_sq));
    return;
  }
  walk.enemies++;
  int slot = walk.n;
  if (walk.n < walk.max) {
    walk.n++;
  } else { /* full: in place of the furthest, if this one is nearer */
    slot = 0;
    for (int i = 1; i < walk.n; i++)
      if (walk.distance_sq[i] > walk.distance_sq[slot])
        slot = i;
    if (walk.distance_sq[slot] <= distance_sq)
      return;
  }
  walk.distance_sq[slot] = distance_sq;
  enemy_t *e = &walk.out[slot];
  e->id = actor;
  e->pos[0] = pos[0];
  e->pos[1] = pos[1];
  e->pos[2] = pos[2];
  e->height = BAR_HEIGHT;
  e->life = life;
  e->max_life = max;
}

/* in order, as the game does; iterative, with a stack of right children still to visit */
static void walk_tree(uintptr_t root)
{
  uintptr_t stack[MAX_DEPTH];
  int depth = 0;
  uintptr_t node = root;
  while ((node || depth) && walk.visits < MAX_VISITS) {
    while (node && depth < MAX_DEPTH) {
      if (!readable(node, 16))
        return;
      stack[depth++] = node;
      node = *(uintptr_t *)(node + NODE_LEFT);
    }
    if (node) /* deeper than a balanced tree of this size can be: not a tree any more */
      return;
    node = stack[--depth];
    for (uintptr_t entry = node - ENTRY_NODE; entry && walk.visits < MAX_VISITS;
         entry = *(uintptr_t *)(entry + ENTRY_NEXT)) {
      walk.visits++;
      if (!readable(entry, ENTRY_SIZE))
        break;
      uintptr_t actor = *(uintptr_t *)(entry + ENTRY_ACTOR);
      if (actor)
        walk.visit(actor);
    }
    node = *(uintptr_t *)(node + NODE_RIGHT);
  }
}

/* --- reporting ----------------------------------------------------------------------------------- */

static int report_wait;

static void report(const enemy_t *enemies, int n)
{
  if (report_wait-- > 0)
    return;
  report_wait = REPORT_FRAMES;
  log_printf("actors: %d entries, %d actors, %d with life, %d enemies within %.0f m (%d kept)", walk.visits, walk.actors,
             walk.with_life, walk.enemies, (double)NEAR_LINK, n);
  for (int i = 0; i < n && i < 8; i++) {
    const enemy_t *e = &enemies[i];
    log_printf("actors: %s life %d/%d at %.2f, %.2f, %.2f (state %u)", *(const char **)(e->id + ACTOR_NAME),
               (int)e->life, (int)e->max_life, (double)e->pos[0], (double)e->pos[1], (double)e->pos[2],
               *(uint32_t *)(e->id + ACTOR_STATE));
  }
  /* the enemies left out, and why: a boss without a bar shows here */
  for (int i = 0; i < walk.n_skipped; i++) {
    const char *name = *(const char **)(walk.skipped[i].actor + ACTOR_NAME);
    log_printf("actors: skipped %s: %s (life %s %d/%d, state %u, %.1f m from Link)",
               name && sys_mapped((uintptr_t)name, 8, SYS_PERM_R) ? name : "?", skip_names[walk.skipped[i].why],
               life_names[walk.skipped[i].life_result], (int)walk.skipped[i].life, (int)walk.skipped[i].max,
               walk.skipped[i].state, (double)walk.skipped[i].distance);
  }
  int32_t life, max, *cur_ptr, *max_ptr;
  if (walk.link && actor_life(walk.link, &life, &max) == LIFE_OK && link_health(&cur_ptr, &max_ptr))
    log_printf("actors: Link's life %d/%d (link.c: %d/%d)", (int)life, (int)max, (int)*cur_ptr, (int)*max_ptr);
}

/* --- the enemies --------------------------------------------------------------------------------- */

int enemies_read(enemy_t *out, int max)
{
  if (!z64hd_game || !z64hd_game->actor_mgr_ptr)
    return 0;
  uintptr_t mgr = sys_ptr(MAIN(z64hd_game->actor_mgr_ptr));
  if (!readable(mgr + MGR_TREE_ROOT, 8))
    return 0;
  walk.out = out;
  walk.max = max < (int)(sizeof(walk.distance_sq) / sizeof(walk.distance_sq[0])) ? max :
             (int)(sizeof(walk.distance_sq) / sizeof(walk.distance_sq[0]));
  walk.n = walk.visits = walk.actors = walk.with_life = walk.enemies = walk.n_skipped = 0;
  walk.visit = use_actor;
  walk.link = link_actor();
  if (!link_drawn_pos(&walk.link_pos))
    return 0;
  walk_tree(*(uintptr_t *)(mgr + MGR_TREE_ROOT));
  report(out, walk.n);
  return walk.n;
}

/* --- actors by name ------------------------------------------------------------------------------ */

static struct {
  const char *name;
  uintptr_t *out;
  int max, n;
} named;

static bool same_name(const char *a, const char *b)
{
  for (; *a && *a == *b; a++, b++)
    ;
  return *a == *b;
}

static void find_named(uintptr_t actor)
{
  if (named.n >= named.max || !readable(actor, ACTOR_POS + 12))
    return;
  uintptr_t name = *(uintptr_t *)(actor + ACTOR_NAME);
  if (name && sys_mapped(name, 32, SYS_PERM_R) && same_name((const char *)name, named.name))
    named.out[named.n++] = actor;
}

int actors_named(const char *name, uintptr_t *out, int max)
{
  if (!z64hd_game || !z64hd_game->actor_mgr_ptr)
    return 0;
  uintptr_t mgr = sys_ptr(MAIN(z64hd_game->actor_mgr_ptr));
  if (!readable(mgr + MGR_TREE_ROOT, 8))
    return 0;
  named.name = name, named.out = out, named.max = max, named.n = 0;
  walk.visits = 0;
  walk.visit = find_named;
  walk_tree(*(uintptr_t *)(mgr + MGR_TREE_ROOT));
  return named.n;
}

/* --- the actors near Link (remote.h's "actors") -------------------------------------------------- */

static struct {
  actor_near_t *out;
  int n, max;
  float radius;
} near;

static void use_near(uintptr_t actor)
{
  if (!readable(actor, ACTOR_POS + 12) || actor == walk.link)
    return;
  uint32_t state = *(uint32_t *)(actor + ACTOR_STATE);
  uintptr_t name = *(uintptr_t *)(actor + ACTOR_NAME);
  if (state < STATE_RUNNING || state > STATE_RUNNING_LAST || !name || !sys_mapped(name, 8, SYS_PERM_R))
    return;
  float distance = distance_to_link(actor);
  if (!(distance <= near.radius))
    return;
  int at = near.n;
  if (near.n < near.max)
    near.n++;
  else if (near.out[near.max - 1].distance <= distance) /* full, and no nearer than the furthest kept */
    return;
  else
    at = near.max - 1;
  for (; at > 0 && near.out[at - 1].distance > distance; at--) /* kept nearest first */
    near.out[at] = near.out[at - 1];
  const float *pos = (const float *)(actor + ACTOR_POS);
  near.out[at] = (actor_near_t){ (const char *)name, distance, actor, { pos[0], pos[1], pos[2] }, state };
}

/* near a point: distance_to_link measures from walk.link_pos, set to the point */
static int near_from(const vec3f_t *centre, float radius, actor_near_t *out, int max)
{
  if (!z64hd_game || !z64hd_game->actor_mgr_ptr || max < 1)
    return 0;
  uintptr_t mgr = sys_ptr(MAIN(z64hd_game->actor_mgr_ptr));
  if (!readable(mgr + MGR_TREE_ROOT, 8))
    return 0;
  walk.visits = 0;
  walk.visit = use_near;
  walk.link = link_actor();
  vec3f_t link;
  if (!centre && !link_drawn_pos(&link))
    return 0;
  walk.link_pos = centre ? *centre : link;
  for (int i = 0; i < max; i++)
    out[i] = (actor_near_t){ 0 };
  near.out = out;
  near.n = 0;
  near.max = max;
  near.radius = radius;
  walk_tree(*(uintptr_t *)(mgr + MGR_TREE_ROOT));
  return near.n;
}

int actors_near(float radius, actor_near_t *out, int max) { return near_from(0, radius, out, max); }

int actors_near_point(const float point[3], float radius, actor_near_t *out, int max)
{
  vec3f_t centre = { point[0], point[1], point[2] };
  return near_from(&centre, radius, out, max);
}

/* --- every actor in the manager (debug > actors > browse) ------------------------------------------ */

static struct {
  actor_near_t *out;
  int n, max, more;
  bool link_known;
} every;

static void use_every(uintptr_t actor)
{
  if (!readable(actor, ACTOR_POS + 12) || actor == walk.link)
    return;
  uintptr_t name = *(uintptr_t *)(actor + ACTOR_NAME);
  if (!name || !sys_mapped(name, 8, SYS_PERM_R))
    return;
  if (every.n >= every.max) {
    every.more++;
    return;
  }
  const float *pos = (const float *)(actor + ACTOR_POS);
  every.out[every.n++] = (actor_near_t){ (const char *)name, every.link_known ? distance_to_link(actor) : 0, actor,
                                         { pos[0], pos[1], pos[2] }, *(uint32_t *)(actor + ACTOR_STATE) };
}

int actors_all(actor_near_t *out, int max, int *more)
{
  *more = 0;
  if (!z64hd_game || !z64hd_game->actor_mgr_ptr || max < 1)
    return 0;
  uintptr_t mgr = sys_ptr(MAIN(z64hd_game->actor_mgr_ptr));
  if (!readable(mgr + MGR_TREE_ROOT, 8))
    return 0;
  walk.visits = 0;
  walk.visit = use_every;
  walk.link = link_actor();
  every.link_known = link_drawn_pos(&walk.link_pos);
  every.out = out, every.n = 0, every.max = max, every.more = 0;
  walk_tree(*(uintptr_t *)(mgr + MGR_TREE_ROOT));
  *more = every.more;
  return every.n;
}

/* --- one actor, again (debug > actors) ----------------------------------------------------------- */

static struct {
  uintptr_t want;
  bool found;
} again;

static void find_again(uintptr_t actor)
{
  if (actor == again.want)
    again.found = true;
}

bool actor_read(uintptr_t actor, actor_read_t *out)
{
  *out = (actor_read_t){ 0 };
  if (!actor || !z64hd_game || !z64hd_game->actor_mgr_ptr)
    return false;
  uintptr_t mgr = sys_ptr(MAIN(z64hd_game->actor_mgr_ptr));
  if (!readable(mgr + MGR_TREE_ROOT, 8))
    return false;
  again.want = actor; /* still in the manager's tree: its memory is still the actor's */
  again.found = false;
  walk.visits = 0;
  walk.visit = find_again;
  walk_tree(*(uintptr_t *)(mgr + MGR_TREE_ROOT));
  if (!again.found || !readable(actor, ACTOR_POS + 12))
    return false;
  uintptr_t name = *(uintptr_t *)(actor + ACTOR_NAME);
  out->name = name && sys_mapped(name, 8, SYS_PERM_R) ? (const char *)name : "?";
  out->state = *(uint32_t *)(actor + ACTOR_STATE);
  const float *pos = (const float *)(actor + ACTOR_POS);
  out->pos[0] = pos[0], out->pos[1] = pos[1], out->pos[2] = pos[2];
  out->running = out->state >= STATE_RUNNING && out->state <= STATE_RUNNING_LAST;
  out->has_life = actor_life(actor, &out->life, &out->max_life) == LIFE_OK;
  if (link_drawn_pos(&walk.link_pos)) {
    out->distance = distance_to_link(actor);
    out->distance_known = true;
  }
  return true;
}

/* --- killing one (debug > actors > kill, FINDINGS 9.27) ------------------------------------------ */

#define COMPONENTS_DEATH 0x1C0
#define DEATH_ACTOR 0x18           /* the Death component's actor */
#define DEATH_FALL_INTO_ABYSS 8u   /* game__death DeathReason: what the void areas send */
#define DELETE_AS_AI 0u            /* the delete reason OneShotGenericDeleteThisActor gives */

typedef bool (*death_request_t)(uintptr_t *death, uint32_t reason, uint32_t sub);
typedef bool (*actor_delete_t)(uintptr_t actor, uint32_t reason);

/* deleted only if it has no Death component: an NPC or a thing, never Link, scenery, enemies' parts or the
 * game's own system actors */
static bool deletable(const char *name)
{
  return starts_with(name, "Npc_") || starts_with(name, "Obj_") || starts_with(name, "Item_");
}

actor_kill_t actor_kill(uintptr_t actor)
{
  actor_read_t a;
  if (!z64hd_game || !z64hd_game->death_request || !z64hd_game->actor_delete)
    return KILL_UNKNOWN;
  if (!actor_read(actor, &a)) /* found again in the manager: still this actor */
    return KILL_GONE;
  if (actor == link_actor())
    return KILL_NOT_ALLOWED;
  uintptr_t components = readable(actor + ACTOR_COMPONENTS, 8) ? *(uintptr_t *)(actor + ACTOR_COMPONENTS) : 0;
  uintptr_t death = readable(components + COMPONENTS_DEATH, 8) ? *(uintptr_t *)(components + COMPONENTS_DEATH) : 0;
  if (death && readable(death, DEATH_ACTOR + 8) && in_main(*(uintptr_t *)death) &&
      *(uintptr_t *)(death + DEATH_ACTOR) == actor) {
    bool ok = MAIN_FN(death_request_t, z64hd_game->death_request)(&death, DEATH_FALL_INTO_ABYSS, 0);
    log_printf("actors: death request (FallIntoAbyss) to %s %#lx: %s", a.name, (unsigned long)actor,
               ok ? "taken" : "refused");
    return ok ? KILL_DEATH : KILL_DEATH_REFUSED;
  }
  if (!deletable(a.name))
    return KILL_NOT_ALLOWED;
  bool ok = MAIN_FN(actor_delete_t, z64hd_game->actor_delete)(actor, DELETE_AS_AI);
  log_printf("actors: delete request to %s %#lx (no Death component): %s", a.name, (unsigned long)actor,
             ok ? "taken" : "refused");
  return ok ? KILL_DELETED : KILL_DELETE_REFUSED;
}

actor_kill_t actor_despawn(uintptr_t actor)
{
  actor_read_t a;
  if (!z64hd_game || !z64hd_game->actor_delete)
    return KILL_UNKNOWN;
  if (!actor_read(actor, &a))
    return KILL_GONE;
  if (actor == link_actor() || !(deletable(a.name) || starts_with(a.name, "Enemy_")))
    return KILL_NOT_ALLOWED;
  bool ok = MAIN_FN(actor_delete_t, z64hd_game->actor_delete)(actor, DELETE_AS_AI);
  log_printf("actors: despawn (delete request) to %s %#lx: %s", a.name, (unsigned long)actor, ok ? "taken" : "refused");
  return ok ? KILL_DELETED : KILL_DELETE_REFUSED;
}

/* --- making one (debug > actors > spawn, FINDINGS 9.29) ------------------------------------------- */

#define CREATE_POS 1u
#define CREATE_ROT 2u
#define CREATE_QUEUE 0u /* as the game's own air-wall spawner (1.1.1) */

typedef struct {             /* the create request's argument, 0xA0 bytes */
  float pos[3];              /* +0x00 */
  float rot[9];              /* +0x0C, 3x3 by rows, as the actor's at +0x328 */
  float scale[3];            /* +0x30, with flag 4 only */
  uint32_t pad3c;
  const char *instance_name; /* +0x40: never NULL */
  uint8_t opt[0x50];         /* +0x48: links, overrides, heap: none */
  uint8_t flags;             /* +0x98: CREATE_POS, CREATE_ROT */
  uint8_t pad[7];
} actor_create_arg_t;
_Static_assert(sizeof(actor_create_arg_t) == 0xA0, "the create argument is 0xA0 bytes");

typedef bool (*actor_create_t)(uintptr_t mgr, const char *const *name, const actor_create_arg_t *arg,
                               uintptr_t *handle, uint32_t queue, uintptr_t preloaded, uintptr_t tmpl, uintptr_t cb,
                               bool flag, uint32_t *result, uintptr_t *out_param);

bool actor_spawn(const char *name, const float pos[3], float yaw_degrees, uint32_t *result)
{
  *result = ~0u;
  if (!z64hd_game || !z64hd_game->actor_create || !z64hd_game->actor_create_mgr_ptr || !name || !*name)
    return false;
  uintptr_t mgr = sys_ptr(MAIN(z64hd_game->actor_create_mgr_ptr));
  if (!readable(mgr, 0x110))
    return false;
  float a = yaw_degrees * (3.14159265f / 180.0f), s = sinf(a), c = cosf(a);
  actor_create_arg_t arg;
  memset(&arg, 0, sizeof(arg));
  arg.pos[0] = pos[0], arg.pos[1] = pos[1], arg.pos[2] = pos[2];
  const float rot[9] = { c, 0, s, 0, 1, 0, -s, 0, c }; /* a turn about Y */
  memcpy(arg.rot, rot, sizeof(rot));
  arg.instance_name = "";
  arg.flags = CREATE_POS | CREATE_ROT;
  const char *the_name = name;
  bool ok = MAIN_FN(actor_create_t, z64hd_game->actor_create)(mgr, &the_name, &arg, 0, CREATE_QUEUE, 0, 0, 0, false,
                                                             result, 0);
  log_printf("actors: create request %s at %.2f %.2f %.2f: %s (result %u)", name, (double)pos[0], (double)pos[1],
             (double)pos[2], ok ? "queued" : "refused", (unsigned)*result);
  return ok;
}
