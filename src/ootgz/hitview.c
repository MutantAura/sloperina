/* Hitboxes (FINDINGS 9.39): the game's own physics shapes, read live from its two Havok worlds each frame.
 *
 * phive keeps two hknpWorlds (cct.c's path: the physics manager's +0xC0, then +0xC8 + 8 w, then +0xE0): world 0 the
 * solid bodies ("entities"), world 1 the sensors. A world's bodies are 0xC0 bytes each, 42 to a block, the blocks'
 * pointers at [world+0x48] (capacity at +0x130). In a body: its transform (three columns of four floats, then the
 * position) at +0x00, its bounding box's top corner at +0x60 (FLT_MAX while it isn't in the world), its shape at
 * +0x78 and phive's own object for it (the "owner") at +0xB8. In the owner: [+0x108]+8 the actor it's for, +0x120 the
 * layers it hits (a bit per layer, PhiveConfig's LayerHitMask*), [+0x150]+8 its own layer (low five bits: the index in
 * PhiveConfig's LayerEntityCollection, or LayerSensorCollection for a sensor) and [+0x150]+0xC a number of its
 * actor's (some of Link's sensors have no actor at +0x108: that number tells them).
 *
 * A shape (hknpConvexShape) keeps its points at (+0x38 + [+0x38]), [+0x3C] of them, 12 bytes each, and its convex
 * radius at +0x68: one point is a sphere round it, two a capsule; more, a convex polytope, whose faces (4 bytes each:
 * first index u16, index count u8) are at (+0x48 + [+0x48]) and their point indices (u8) at (+0x50 + [+0x50]). Bigger
 * shapes (the scenery's meshes and compounds) have other layouts and aren't drawn.
 *
 * Drawn as gz's hit view draws the game's AT, AC and OC lists, only what can touch something now: a body switched on
 * (the owner's +0x70 bit 6 clear) and, for a sensor, with a contact collection (the owner's +0x158). Red: an attack
 * (a sensor on EnemyAttack, NPCAttack or AttackAll) whose hit mask has Link's layer (Player) in it. Blue: a target
 * sensor (Player, Enemy, NPC, Animal, TgtHitAll): where Link or an actor can be hit; purple instead while Link or an
 * actor that can be fought is invincible (gz has nothing then). White: a solid body that collides with Link's
 * (Character with Player in its mask: the characters' controllers, and such as a Wolfos's head), and Link's own
 * character controller. Nearest first within each kind, attacks first. */
#include "hitview.h"

#include <stdio.h>
#include <string.h>

#include "enemyhp.h"
#include "lang.h"
#include "link.h"
#include "sys.h"
#include "view.h"
#include "z64hd.h"

float sinf(float x), cosf(float x); /* from sdk */

static float root(float x) /* the sdk exports no sqrtf: the CPU's own instruction */
{
  float r;
  __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x > 0 ? x : 0));
  return r;
}

bool hitview_shown;
int hitview_style[HV_PARTS] = { HV_FILL, HV_OUTLINE, HV_FILL, HV_OUTLINE };
bool hitview_link_only;
bool hitview_xray;

/* actors whose hitboxes are shown whatever the menu says (the actor browser's options), forgotten when they go */
#define MAX_OWN 16
static uintptr_t own[MAX_OWN];
static int n_own;

bool hitview_own(uintptr_t actor)
{
  for (int i = 0; i < n_own; i++)
    if (own[i] == actor)
      return true;
  return false;
}

void hitview_toggle(uintptr_t actor)
{
  for (int i = 0; i < n_own; i++)
    if (own[i] == actor) {
      own[i] = own[--n_own];
      log_printf("hitboxes: %#lx's off", (unsigned long)actor);
      return;
    }
  if (n_own < MAX_OWN) {
    own[n_own++] = actor;
    log_printf("hitboxes: %#lx's on", (unsigned long)actor);
  }
}

static void forget_gone(void)
{
  for (int i = 0; i < n_own; i++) {
    actor_read_t a;
    if (!actor_read(own[i], &a) || !a.running)
      own[i--] = own[--n_own];
  }
}

#define RANGE 25.0f /* metres from Link or the camera: bodies further aren't drawn */
#define PI 3.14159265f

/* hknpWorld */
#define WORLD_BODY_BLOCKS 0x48
#define WORLD_BODY_CAPACITY 0x130
#define BODY_SIZE 0xC0u
#define BODIES_PER_BLOCK 42u
#define MAX_BODIES 20000u
/* hknpBody */
#define BODY_POS 0x30
#define BODY_AABB_MAX 0x60
#define BODY_SHAPE 0x78
#define BODY_OWNER 0xB8
/* phive's object for a body */
#define OWNER_ACTOR 0x108 /* [+0x108]+8 */
#define OWNER_HIT_MASK 0x120
#define OWNER_LAYER 0x150 /* [+0x150]+8, low five bits */
#define OWNER_STATE 0x70  /* bit 6 set: switched off */
#define STATE_OFF 0x40
#define OWNER_REQUEST 0x60 /* a change queued for the physics' next step: */
#define REQUEST_ON 0x84    /*   the state asked for (1 on) */
#define REQUEST_FLAGS 0x100
#define REQUEST_PENDING 0x4 /*   ... while this bit is set (phive's set-enabled reads it the same way) */
#define OWNER_CONTACTS 0x158 /* a sensor's contact collection (its controller unit's ContactCollectionName) */
#define LAYER_MASK 0x1F
#define LAYER_NUMBER 0xC  /* [+0x150]+0xC: a number of the actor's, the same on all its bodies (Link's 0x20) */
/* hknpConvexShape */
#define SHAPE_SIZE 0x08
#define SHAPE_POINTS 0x38
#define SHAPE_FACES 0x48
#define SHAPE_INDICES 0x50
#define SHAPE_RADIUS 0x68
#define SHAPE_MIN_SIZE 0x80
#define SHAPE_MAX_SIZE 0x2000
#define MAX_POINTS 64
#define MAX_FACES 40

/* PhiveConfig's layers */
enum { SENSOR_PLAYER = 2, SENSOR_ENEMY = 3, SENSOR_NPC = 4, SENSOR_ANIMAL = 5, SENSOR_TGT_HIT_ALL = 6,
       SENSOR_PLAYER_ATTACK = 7, SENSOR_ENEMY_ATTACK = 8, SENSOR_NPC_ATTACK = 9, SENSOR_ATTACK_ALL = 10 };
enum { ENTITY_PLAYER = 3, ENTITY_CHARACTER = 4 };

enum { K_ATTACK, K_INVINCIBLE, K_BUMP_LINK, K_BUMP, K_HURT, N_KINDS }; /* drawing order */
static const uint8_t kind_colour[N_KINDS] = { AW_HIT_ATTACK, AW_HIT_INVINCIBLE, AW_HIT_LINK, AW_HIT_BUMP, AW_HIT_HURT };
static const uint8_t kind_part[N_KINDS] = { HV_ATTACK, HV_INVINCIBLE, HV_BUMP, HV_BUMP, HV_HURT };

#define MAX_FOUND 384
typedef struct {
  uintptr_t body, shape;
  float distance;
  uint8_t kind;
} found_t;
static found_t found[MAX_FOUND];
static int n_found;
static int counts[N_KINDS];

static float lines[HITVIEW_MAX_LINES][2][3];
static uint8_t line_colours[HITVIEW_MAX_LINES];
static int n_lines;
static aw_face_t faces[HITVIEW_MAX_FACES];
static int n_faces;
static view_cam_t cam;

static bool heap_ptr(uintptr_t p)
{
  return p && !(p & 7) && p > 0x10000000ull && p < 0x8000000000ull;
}

static uintptr_t rd(uintptr_t p, size_t o)
{
  return heap_ptr(p) && sys_mapped(p + o, 8, SYS_PERM_R) ? *(const volatile uintptr_t *)(p + o) : 0;
}

static bool sane(float f)
{
  return f == f && f < 1e7f && f > -1e7f;
}

static bool any_style(void)
{
  for (int i = 0; i < HV_PARTS; i++)
    if (hitview_style[i] != HV_OFF)
      return true;
  return false;
}

bool hitview_active(void)
{
  return any_style() && (hitview_shown || n_own > 0);
}

/* --- whom a body is for ----------------------------------------------------------------------- */

#define MAX_ACTORS_SEEN 64
static struct { uintptr_t actor; bool invincible, fightable; } seen[MAX_ACTORS_SEEN];
static int n_seen;

/* this frame's answer for an actor: invincible now, and Link or an actor that can be fought */
static void combat_of(uintptr_t actor, uintptr_t link, bool *invincible, bool *fightable)
{
  for (int i = 0; i < n_seen; i++)
    if (seen[i].actor == actor) {
      *invincible = seen[i].invincible, *fightable = seen[i].fightable;
      return;
    }
  bool killable = false;
  actor_combat(actor, invincible, &killable);
  *fightable = actor == link || killable;
  if (n_seen < MAX_ACTORS_SEEN)
    seen[n_seen].actor = actor, seen[n_seen].invincible = *invincible, seen[n_seen].fightable = *fightable, n_seen++;
}

/* Link's own number in the layer objects ([[owner+0x150]+0xC]), learnt from his bodies that lead to him: some of his
 * sensors (an AttackAll one 1 m round him) don't */
static uint32_t link_number;
static int link_attacks, link_attacks_before; /* PlayerAttack shapes this frame and last: logged as they appear */

/* on, as the game reads it (main+0x15B800): a change queued for the physics' next step counts already */
static bool switched_on(uintptr_t owner)
{
  if (!sys_mapped(owner + OWNER_STATE, 8, SYS_PERM_R))
    return false;
  uintptr_t request = rd(owner, OWNER_REQUEST);
  if (request && sys_mapped(request + REQUEST_FLAGS, 8, SYS_PERM_R) &&
      (*(const volatile uint64_t *)(request + REQUEST_FLAGS) & REQUEST_PENDING))
    return *(const volatile uint32_t *)(request + REQUEST_ON) != 0;
  return !(*(const volatile uint64_t *)(owner + OWNER_STATE) & STATE_OFF);
}

/* debugging (the log): each frame, the state of the PlayerAttack sensors within 3 m of Link that changed: in the world,
 * on, a pending change, its mask */
#define WATCH_SWORD 0 /* 1: log them */
#define WATCH_MAX 16
static struct { uintptr_t body; uint32_t state; } watched[WATCH_MAX];
static uint32_t frame_no;
static void watch_link_attacks(uintptr_t body, const vec3f_t *link_pos)
{
  uintptr_t owner = *(const volatile uintptr_t *)(body + BODY_OWNER);
  uintptr_t layer_obj = rd(owner, OWNER_LAYER);
  if (!layer_obj || !sys_mapped(layer_obj + 8, 1, SYS_PERM_R) ||
      (*(const volatile uint8_t *)(layer_obj + 8) & LAYER_MASK) != 7 || !sys_mapped(owner + OWNER_HIT_MASK, 8, SYS_PERM_R))
    return;
  const volatile float *pos = (const volatile float *)(body + BODY_POS);
  float dx = pos[0] - link_pos->x, dy = pos[1] - link_pos->y, dz = pos[2] - link_pos->z;
  if (!(dx * dx + dy * dy + dz * dz < 9))
    return;
  uintptr_t request = rd(owner, OWNER_REQUEST);
  uint64_t flags = request && sys_mapped(request + REQUEST_FLAGS, 8, SYS_PERM_R) ? *(const volatile uint64_t *)(request + REQUEST_FLAGS) : 0;
  uint32_t asked = request && sys_mapped(request + REQUEST_ON, 4, SYS_PERM_R) ? *(const volatile uint32_t *)(request + REQUEST_ON) : 9;
  bool in_world = sane(((const volatile float *)(body + BODY_AABB_MAX))[0]);
  uint32_t state = (uint32_t)in_world | (uint32_t)!(*(const volatile uint64_t *)(owner + OWNER_STATE) & STATE_OFF) << 1 |
                   (uint32_t)(flags & 0xFFFF) << 4 | (asked & 0xF) << 20 |
                   (uint32_t)(*(const volatile uint64_t *)(owner + OWNER_HIT_MASK) != 0) << 24;
  int slot = -1;
  for (int i = 0; i < WATCH_MAX; i++)
    if (watched[i].body == body)
      slot = i;
  if (slot < 0)
    for (int i = 0; i < WATCH_MAX && slot < 0; i++)
      if (!watched[i].body)
        slot = i, watched[i].body = body, watched[i].state = ~0u;
  if (slot < 0 || watched[slot].state == state)
    return;
  watched[slot].state = state;
  log_printf("hitboxes: frame %u sword body %#lx: in world %d, on %d, request flags %#x asks %u, mask %d", frame_no,
             (unsigned long)body, in_world, (int)(state >> 1 & 1), (unsigned)(flags & 0xFFFF), (unsigned)asked,
             (int)(state >> 24 & 1));
}

static int classify(int world, uintptr_t owner, uintptr_t link)
{
  uintptr_t layer_obj = rd(owner, OWNER_LAYER);
  if (!layer_obj || !sys_mapped(layer_obj + 8, 8, SYS_PERM_R) || !sys_mapped(owner + OWNER_HIT_MASK, 8, SYS_PERM_R))
    return -1;
  int layer = *(const volatile uint8_t *)(layer_obj + 8) & LAYER_MASK;
  uint32_t number = *(const volatile uint32_t *)(layer_obj + LAYER_NUMBER);
  uint64_t mask = *(const volatile uint64_t *)(owner + OWNER_HIT_MASK);
  uintptr_t actor_link = rd(owner, OWNER_ACTOR);
  uintptr_t actor = actor_link ? rd(actor_link, 8) : 0;
  if (actor && actor == link)
    link_number = number;
  bool links = (actor && actor == link) || (link_number && number == link_number) ||
               (world == 1 && layer == SENSOR_PLAYER_ATTACK); /* his sword is an actor of its own */
  /* whose: everyone's (or only Link's) with the menu's show, and the actors ticked in the actor browser */
  bool wanted = (hitview_shown && (!hitview_link_only || links)) || (actor && hitview_own(actor)) ||
                (links && hitview_own(link));
  if (!wanted)
    return -1;
  /* switched off (the AI's OneShotSensorHitAtSensorOff and the like: phive's set-enabled, main+0x15B800): there, but
   * nothing touches it */
  if (!switched_on(owner))
    return -1;
  /* a sensor nothing is collected for (no contact collection: the wagon horse's AttackAll, one of Link's) can't hit
   * or be hit: as gz draws only what the game's collision check has this frame */
  if (world == 1 && !rd(owner, OWNER_CONTACTS))
    return -1;
  if (world == 1) {
    /* Link's own (his sword, a thrown nut...: PlayerAttack, put in the world or switched on only while it's out), as
     * gz's AT list has them */
    if (layer == SENSOR_PLAYER_ATTACK || (links && layer == SENSOR_ATTACK_ALL)) {
      if (hitview_style[HV_ATTACK] == HV_OFF || !mask)
        return -1;
      link_attacks++;
      return K_ATTACK;
    }
    /* anyone else's, while it can hurt Link */
    if (layer == SENSOR_ENEMY_ATTACK || layer == SENSOR_NPC_ATTACK || layer == SENSOR_ATTACK_ALL)
      return hitview_style[HV_ATTACK] != HV_OFF && (mask & (1ull << SENSOR_PLAYER)) && !links ? K_ATTACK : -1;
    if (layer < SENSOR_PLAYER || layer > SENSOR_TGT_HIT_ALL || (!actor && !links))
      return -1;
    if (hitview_style[HV_INVINCIBLE] == HV_OFF && hitview_style[HV_HURT] == HV_OFF)
      return -1;
    bool invincible = false, fightable = false;
    combat_of(links ? link : actor, link, &invincible, &fightable);
    if (fightable && invincible) /* gz has none then (the game doesn't register it): purple instead */
      return hitview_style[HV_INVINCIBLE] != HV_OFF ? K_INVINCIBLE : -1;
    return hitview_style[HV_HURT] != HV_OFF ? K_HURT : -1;
  }
  if (hitview_style[HV_BUMP] == HV_OFF)
    return -1;
  /* a character controller (the body a character stands and walks with, and pushes others with) */
  bool controller = Z64HD_VT_CCT_BODY && sys_ptr(owner) == MAIN(Z64HD_VT_CCT_BODY);
  if (links)
    return controller && layer == ENTITY_PLAYER ? K_BUMP_LINK : -1;
  if (layer == ENTITY_CHARACTER && (mask & (1ull << ENTITY_PLAYER)))
    return K_BUMP;
  return -1;
}

/* --- the walk --------------------------------------------------------------------------------- */

static uintptr_t world_of(int w)
{
  uintptr_t holder = sys_ptr(MAIN(z64hd_game->phive_mgr_ptr));
  uintptr_t mgr = holder ? sys_ptr(holder) : 0;
  uintptr_t pworld = rd(mgr, 0xC0);
  uintptr_t wrap = rd(pworld, 0xC8 + (size_t)w * 8);
  return rd(wrap, 0xE0);
}

static void consider(int world, uintptr_t body, const vec3f_t *link_pos, uintptr_t link)
{
  const volatile float *top = (const volatile float *)(body + BODY_AABB_MAX);
  if (!sane(top[0]) || !sane(top[1]) || !sane(top[2])) /* not in the world */
    return;
  uintptr_t owner = *(const volatile uintptr_t *)(body + BODY_OWNER);
  uintptr_t shape = *(const volatile uintptr_t *)(body + BODY_SHAPE);
  if (!heap_ptr(owner) || !heap_ptr(shape))
    return;
  const volatile float *pos = (const volatile float *)(body + BODY_POS);
  float p[3] = { pos[0], pos[1], pos[2] };
  if (!sane(p[0]) || !sane(p[1]) || !sane(p[2]))
    return;
  float dl[3] = { p[0] - link_pos->x, p[1] - link_pos->y, p[2] - link_pos->z };
  float dc[3] = { p[0] - cam.pos[0], p[1] - cam.pos[1], p[2] - cam.pos[2] };
  float to_link = root(dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2]);
  float to_cam = root(dc[0] * dc[0] + dc[1] * dc[1] + dc[2] * dc[2]);
  float distance = to_link < to_cam ? to_link : to_cam;
  if (distance > RANGE)
    return;
  int kind = classify(world, owner, link);
  if (kind < 0)
    return;
  counts[kind]++;
  int slot = n_found;
  if (n_found < MAX_FOUND) {
    n_found++;
  } else { /* full: in place of the furthest of a later kind, or of its own */
    slot = -1;
    for (int i = 0; i < n_found; i++)
      if (found[i].kind > kind || (found[i].kind == kind && found[i].distance > distance))
        if (slot < 0 || found[i].kind > found[slot].kind ||
            (found[i].kind == found[slot].kind && found[i].distance > found[slot].distance))
          slot = i;
    if (slot < 0)
      return;
  }
  found[slot] = (found_t){ body, shape, distance, (uint8_t)kind };
}

static void walk(const vec3f_t *link_pos, uintptr_t link)
{
  for (int w = 0; w < 2; w++) {
    uintptr_t world = world_of(w);
    uint32_t capacity = 0;
    if (!world || !sys_read32(world + WORLD_BODY_CAPACITY, &capacity) || capacity > MAX_BODIES)
      continue;
    uintptr_t blocks = rd(world, WORLD_BODY_BLOCKS);
    unsigned n_blocks = (capacity + BODIES_PER_BLOCK - 1) / BODIES_PER_BLOCK;
    if (!blocks || !sys_mapped(blocks, n_blocks * 8, SYS_PERM_R))
      continue;
    for (unsigned b = 0; b < n_blocks; b++) {
      uintptr_t block = *(const volatile uintptr_t *)(blocks + b * 8);
      if (!heap_ptr(block) || !sys_mapped(block, BODIES_PER_BLOCK * BODY_SIZE, SYS_PERM_R))
        continue;
      for (unsigned i = 0; i < BODIES_PER_BLOCK && b * BODIES_PER_BLOCK + i < capacity; i++) {
        if (WATCH_SWORD && w == 1 && heap_ptr(*(const volatile uintptr_t *)(block + i * BODY_SIZE + BODY_OWNER)))
          watch_link_attacks(block + i * BODY_SIZE, link_pos);
        consider(w, block + i * BODY_SIZE, link_pos, link);
      }
    }
  }
}

/* --- drawing ---------------------------------------------------------------------------------- */

static bool outline = true; /* the shape being drawn: outlined (else filled, no lines) */

static void line(const float a[3], const float b[3], uint8_t colour)
{
  if (!outline || n_lines >= HITVIEW_MAX_LINES)
    return;
  memcpy(lines[n_lines][0], a, sizeof(float) * 3);
  memcpy(lines[n_lines][1], b, sizeof(float) * 3);
  line_colours[n_lines++] = colour;
}

static void triangle(const float a[3], const float b[3], const float c[3], uint8_t colour)
{
  if (n_faces >= HITVIEW_MAX_FACES)
    return;
  aw_face_t *f = &faces[n_faces++];
  for (int k = 0; k < 3; k++)
    f->corner[k] = a[k], f->u[k] = b[k] - a[k], f->v[k] = c[k] - a[k];
  f->color = colour;
  f->scene = !hitview_xray; /* hidden by what's nearer (the scenery, and the actors), as gz's are */
  f->triangle = true;
}

static float dot(const float a[3], const float b[3])
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross(const float a[3], const float b[3], float out[3])
{
  out[0] = a[1] * b[2] - a[2] * b[1], out[1] = a[2] * b[0] - a[0] * b[2], out[2] = a[0] * b[1] - a[1] * b[0];
}

static bool unit(float v[3])
{
  float l = root(dot(v, v));
  if (!(l > 1e-6f))
    return false;
  v[0] /= l, v[1] /= l, v[2] /= l;
  return true;
}

static void at(const float c[3], const float u[3], const float v[3], float r, float angle, float out[3])
{
  float cu = cosf(angle) * r, sv = sinf(angle) * r;
  for (int k = 0; k < 3; k++)
    out[k] = c[k] + u[k] * cu + v[k] * sv;
}

#define SEGMENTS 10
/* an arc round c in the plane of unit u, v, from a0 to a1 (radians); filled as a fan from c */
static void arc(const float c[3], const float u[3], const float v[3], float r, float a0, float a1, int segments,
                uint8_t colour, bool outline, bool fill)
{
  float prev[3], next[3];
  at(c, u, v, r, a0, prev);
  for (int s = 1; s <= segments; s++) {
    at(c, u, v, r, a0 + (a1 - a0) * (float)s / (float)segments, next);
    if (outline)
      line(prev, next, colour);
    if (fill)
      triangle(c, prev, next, colour);
    memcpy(prev, next, sizeof(prev));
  }
}

/* two unit vectors square to unit d */
static void square_to(const float d[3], float p[3], float q[3])
{
  float any[3] = { 0, 1, 0 };
  if (d[1] > 0.9f || d[1] < -0.9f)
    any[0] = 1, any[1] = 0;
  cross(d, any, p);
  unit(p);
  cross(d, p, q);
}

static void sphere(const float c[3], float r, uint8_t colour, bool fill)
{
  float flat_u[3] = { 1, 0, 0 }, flat_v[3] = { 0, 0, 1 };
  arc(c, cam.right, cam.up, r, 0, 2 * PI, SEGMENTS, colour, true, fill); /* its outline as seen */
  arc(c, flat_u, flat_v, r, 0, 2 * PI, SEGMENTS, colour, true, false);   /* and round its middle */
}

static void capsule(const float a[3], const float b[3], float r, uint8_t colour, bool fill)
{
  float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
  float length = root(dot(d, d));
  if (!unit(d)) {
    sphere(a, r, colour, fill);
    return;
  }
  float p[3], q[3];
  square_to(d, p, q);
  arc(a, p, q, r, 0, 2 * PI, SEGMENTS, colour, true, false); /* a ring round each end */
  arc(b, p, q, r, 0, 2 * PI, SEGMENTS, colour, true, false);
  /* its sides and rounded ends as seen: s across it, square to the view */
  float mid[3] = { (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2 };
  float view[3] = { mid[0] - cam.pos[0], mid[1] - cam.pos[1], mid[2] - cam.pos[2] }, s[3];
  cross(d, view, s);
  if (!unit(s))
    memcpy(s, p, sizeof(s));
  float minus_d[3] = { -d[0], -d[1], -d[2] };
  for (int side = -1; side <= 1; side += 2) {
    float from[3], to[3];
    for (int k = 0; k < 3; k++)
      from[k] = a[k] + s[k] * r * (float)side, to[k] = b[k] + s[k] * r * (float)side;
    line(from, to, colour);
  }
  arc(b, s, d, r, 0, PI, SEGMENTS / 2, colour, true, fill);
  arc(a, s, minus_d, r, 0, PI, SEGMENTS / 2, colour, true, fill);
  if (fill && length > 0) { /* the band between, as two triangles */
    float a0[3], a1[3], b0[3], b1[3];
    for (int k = 0; k < 3; k++)
      a0[k] = a[k] - s[k] * r, a1[k] = a[k] + s[k] * r, b0[k] = b[k] - s[k] * r, b1[k] = b[k] + s[k] * r;
    triangle(a0, a1, b1, colour);
    triangle(a0, b1, b0, colour);
  }
}

/* the shape's local point i, in the world */
static void to_world(uintptr_t body, const float local[3], float out[3])
{
  const volatile float *m = (const volatile float *)body;
  for (int k = 0; k < 3; k++)
    out[k] = m[k] * local[0] + m[4 + k] * local[1] + m[8 + k] * local[2] + m[12 + k];
}

/* a (offset, count) pair of the shape's: where its array starts and how many, if it lies inside the shape */
static bool rel_array(uintptr_t shape, uint32_t size, size_t field, size_t item, uintptr_t *start, uint32_t *count)
{
  uint32_t offset = *(const volatile uint32_t *)(shape + field), n = *(const volatile uint32_t *)(shape + field + 4);
  if (!n || field + offset + (uint64_t)n * item > size)
    return false;
  *start = shape + field + offset;
  *count = n;
  return true;
}

static void polytope(uintptr_t body, uintptr_t shape, uint32_t size, const float (*local)[3], int n, float r,
                     uint8_t colour, bool fill)
{
  float world[MAX_POINTS][3], centre[3] = { 0, 0, 0 };
  for (int i = 0; i < n; i++)
    for (int k = 0; k < 3; k++)
      centre[k] += local[i][k] / (float)n;
  for (int i = 0; i < n; i++) { /* its points pushed out by the convex radius (roughly: away from its middle) */
    float out[3] = { local[i][0] - centre[0], local[i][1] - centre[1], local[i][2] - centre[2] }, grown[3];
    if (r > 0 && unit(out))
      for (int k = 0; k < 3; k++)
        grown[k] = local[i][k] + out[k] * r;
    else
      memcpy(grown, local[i], sizeof(grown));
    to_world(body, grown, world[i]);
  }
  uintptr_t face_at = 0, index_at = 0;
  uint32_t n_faces_here = 0, n_indices = 0;
  bool ok = rel_array(shape, size, SHAPE_FACES, 4, &face_at, &n_faces_here) &&
            rel_array(shape, size, SHAPE_INDICES, 1, &index_at, &n_indices) && n_faces_here <= MAX_FACES;
  for (uint32_t f = 0; ok && f < n_faces_here; f++) { /* every face's indices must be points of it */
    uint32_t first = *(const volatile uint16_t *)(face_at + f * 4), count = *(const volatile uint8_t *)(face_at + f * 4 + 2);
    if (count < 3 || first + count > n_indices)
      ok = false;
    for (uint32_t j = 0; ok && j < count; j++)
      if (*(const volatile uint8_t *)(index_at + first + j) >= n)
        ok = false;
  }
  if (!ok) { /* its points' box in its own axes */
    float lo[3], hi[3];
    memcpy(lo, local[0], sizeof(lo));
    memcpy(hi, local[0], sizeof(hi));
    for (int i = 1; i < n; i++)
      for (int k = 0; k < 3; k++)
        lo[k] = local[i][k] < lo[k] ? local[i][k] : lo[k], hi[k] = local[i][k] > hi[k] ? local[i][k] : hi[k];
    float c[8][3];
    for (int i = 0; i < 8; i++) {
      float l[3] = { (i & 1 ? hi : lo)[0] + (i & 1 ? r : -r), (i & 2 ? hi : lo)[1] + (i & 2 ? r : -r),
                     (i & 4 ? hi : lo)[2] + (i & 4 ? r : -r) };
      to_world(body, l, c[i]);
    }
    for (int i = 0; i < 8; i++)
      for (int bit = 1; bit < 8; bit <<= 1)
        if (!(i & bit))
          line(c[i], c[i | bit], colour);
    return;
  }
  for (uint32_t f = 0; f < n_faces_here; f++) {
    uint32_t first = *(const volatile uint16_t *)(face_at + f * 4), count = *(const volatile uint8_t *)(face_at + f * 4 + 2);
    const volatile uint8_t *idx = (const volatile uint8_t *)(index_at + first);
    for (uint32_t j = 0; j < count; j++) {
      int i0 = idx[j], i1 = idx[(j + 1) % count];
      if (i0 < i1) /* each edge once (the face on its other side has it the other way round) */
        line(world[i0], world[i1], colour);
      if (fill && j >= 1 && j + 1 < count)
        triangle(world[idx[0]], world[i0], world[i1], colour);
    }
  }
}

static void draw(const found_t *f)
{
  uintptr_t shape = f->shape;
  uint32_t size = 0;
  if (!sys_read32(shape + SHAPE_SIZE, &size) || size < SHAPE_MIN_SIZE || size > SHAPE_MAX_SIZE ||
      !sys_mapped(shape, size, SYS_PERM_R))
    return;
  uintptr_t points;
  uint32_t n;
  if (!rel_array(shape, size, SHAPE_POINTS, 12, &points, &n) || n > MAX_POINTS)
    return;
  float r = *(const volatile float *)(shape + SHAPE_RADIUS);
  if (!sane(r) || r < 0 || r > 20)
    return;
  float local[MAX_POINTS][3];
  for (uint32_t i = 0; i < n; i++)
    for (int k = 0; k < 3; k++) {
      local[i][k] = *(const volatile float *)(points + i * 12 + k * 4);
      if (!sane(local[i][k]) || local[i][k] > 50 || local[i][k] < -50)
        return;
    }
  uint8_t colour = kind_colour[f->kind];
  bool fill = hitview_style[kind_part[f->kind]] == HV_FILL;
  outline = !fill;
  if (n == 1) {
    float c[3];
    to_world(f->body, local[0], c);
    sphere(c, r, colour, fill);
  } else if (n == 2) {
    float a[3], b[3];
    to_world(f->body, local[0], a);
    to_world(f->body, local[1], b);
    capsule(a, b, r, colour, fill);
  } else {
    polytope(f->body, shape, size, (const float (*)[3])local, (int)n, r, colour, fill);
  }
}

/* every REPORT_FRAMES while it's on: how many of each, and the first few attacks and invincible shapes, to the log */
#define REPORT_FRAMES 300
static void report(void)
{
  static int wait;
  if (wait-- > 0)
    return;
  wait = REPORT_FRAMES;
  log_printf("hitboxes: %d attacks, %d bodies, %d Link, %d invincible, %d hurt; %d lines %d faces", counts[K_ATTACK],
             counts[K_BUMP], counts[K_BUMP_LINK], counts[K_INVINCIBLE], counts[K_HURT], n_lines, n_faces);
  for (int i = 0, shown = 0; i < n_found && shown < 3; i++) {
    if (found[i].kind != K_ATTACK && found[i].kind != K_INVINCIBLE)
      continue;
    shown++;
    uintptr_t owner = rd(found[i].body, BODY_OWNER), layer_obj = rd(owner, OWNER_LAYER);
    uintptr_t actor_link = rd(owner, OWNER_ACTOR), actor = actor_link ? rd(actor_link, 8) : 0;
    uintptr_t name = actor ? rd(actor, 0x20) : 0;
    uint64_t layer = layer_obj ? rd(layer_obj, 8) : 0;
    const volatile float *pos = (const volatile float *)(found[i].body + BODY_POS);
    uint32_t points = 0;
    sys_read32(found[i].shape + SHAPE_POINTS + 4, &points);
    float r = sys_mapped(found[i].shape + SHAPE_RADIUS, 4, SYS_PERM_R) ? *(const volatile float *)(found[i].shape + SHAPE_RADIUS) : -1;
    log_printf("hitboxes: %s %s layer %#llx mask %#llx at %.2f %.2f %.2f, %u points r %.2f, body %#lx owner %#lx",
               found[i].kind == K_ATTACK ? "attack" : "invincible",
               name && sys_mapped(name, 8, SYS_PERM_R) ? (const char *)name : "(no actor)", (unsigned long long)layer,
               (unsigned long long)(owner && sys_mapped(owner + OWNER_HIT_MASK, 8, SYS_PERM_R) ? *(const volatile uint64_t *)(owner + OWNER_HIT_MASK) : 0),
               (double)pos[0], (double)pos[1], (double)pos[2], points, (double)r, (unsigned long)found[i].body,
               (unsigned long)owner);
  }
}

static int by_kind_then_distance(const found_t *a, const found_t *b)
{
  if (a->kind != b->kind)
    return a->kind - b->kind;
  return a->distance < b->distance ? -1 : a->distance > b->distance;
}

void hitview_frame(void)
{
  n_lines = n_faces = n_found = n_seen = 0;
  frame_no++;
  link_attacks_before = link_attacks;
  link_attacks = 0;
  memset(counts, 0, sizeof(counts));
  vec3f_t link_pos;
  if (!hitview_active() || !z64hd_game || !z64hd_game->phive_mgr_ptr || !link_drawn_pos(&link_pos) ||
      !view_camera(&cam))
    return;
  static int forget_wait;
  if (n_own && --forget_wait <= 0) {
    forget_gone();
    forget_wait = 30;
  }
  walk(&link_pos, link_actor());
  for (int i = 1; i < n_found; i++) { /* attacks first, then nearest first: what the budget keeps */
    found_t f = found[i];
    int j = i - 1;
    for (; j >= 0 && by_kind_then_distance(&found[j], &f) > 0; j--)
      found[j + 1] = found[j];
    found[j + 1] = f;
  }
  for (int i = 0; i < n_found && n_lines < HITVIEW_MAX_LINES; i++)
    draw(&found[i]);
  if (WATCH_SWORD && link_attacks && !link_attacks_before)
    log_printf("hitboxes: Link's attack out (%d shapes)", link_attacks);
  report();
}

int hitview_lines(float ends[][2][3], uint8_t colours[], int max)
{
  int n = n_lines < max ? n_lines : max;
  memcpy(ends, lines, sizeof(lines[0]) * (size_t)n);
  memcpy(colours, line_colours, (size_t)n);
  return n;
}

int hitview_faces(aw_face_t out[], int max)
{
  int n = n_faces < max ? n_faces : max;
  memcpy(out, faces, sizeof(faces[0]) * (size_t)n);
  return n;
}

void hitview_text(char *buf, int len)
{
  if (!hitview_active()) {
    snprintf(buf, (size_t)len, "%s", T("off"));
    return;
  }
  snprintf(buf, (size_t)len, TF("%d attacks, %d bodies, %d invincible near"), counts[K_ATTACK],
           counts[K_BUMP] + counts[K_BUMP_LINK], counts[K_INVINCIBLE]);
}
