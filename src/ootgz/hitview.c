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

#include "age.h"
#include "enemyhp.h"
#include "lang.h"
#include "link.h"
#include "sys.h"
#include "view.h"
#include "z64hd.h"

float sinf(float x), cosf(float x), atan2f(float y, float x); /* from sdk */

static float root(float x) /* the sdk exports no sqrtf: the CPU's own instruction */
{
  float r;
  __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x > 0 ? x : 0));
  return r;
}

bool hitview_shown;
int hitview_style[HV_PARTS] = { HV_FILL, HV_FILL, HV_FILL, HV_FILL, HV_FILL };
bool hitview_link_only;
bool hitview_guard = true;
bool hitview_xray;

/* actors whose hitboxes are shown whatever the menu says (the actor browser's options), each part on its own (a
 * bit per HV_ part), forgotten when they go */
#define MAX_OWN 16
static struct { uintptr_t actor; uint8_t parts; } own[MAX_OWN];
static int n_own;

static uint8_t own_parts(uintptr_t actor)
{
  for (int i = 0; actor && i < n_own; i++)
    if (own[i].actor == actor)
      return own[i].parts;
  return 0;
}

bool hitview_own(uintptr_t actor, int part)
{
  return part >= 0 && part < HV_PARTS && (own_parts(actor) >> part & 1);
}

void hitview_toggle(uintptr_t actor, int part)
{
  if (!actor || part < 0 || part >= HV_PARTS)
    return;
  int i = 0;
  while (i < n_own && own[i].actor != actor)
    i++;
  if (i == n_own) {
    if (n_own == MAX_OWN)
      return;
    own[n_own++] = (typeof(own[0])){ actor, 0 };
  }
  own[i].parts ^= (uint8_t)(1u << part);
  log_printf("hitboxes: %#lx's part %d %s", (unsigned long)actor, part, own[i].parts >> part & 1 ? "on" : "off");
  if (!own[i].parts)
    own[i] = own[--n_own];
}

static void forget_gone(void)
{
  for (int i = 0; i < n_own; i++) {
    actor_read_t a;
    if (!actor_read(own[i].actor, &a) || !a.running)
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
#define MAX_FACES 255 /* a shape with more is drawn as its box (the boomerang's attack has 32 points) */

/* PhiveConfig's layers */
enum { SENSOR_OBJECT = 0, SENSOR_SMALL_OBJECT = 1, SENSOR_PLAYER = 2, SENSOR_ENEMY = 3, SENSOR_NPC = 4, SENSOR_ANIMAL = 5, SENSOR_TGT_HIT_ALL = 6,
       SENSOR_PLAYER_ATTACK = 7, SENSOR_ENEMY_ATTACK = 8, SENSOR_CHEMICAL_ELEMENT = 12, SENSOR_NPC_ATTACK = 9, SENSOR_ATTACK_ALL = 10 };
enum { ENTITY_OBJECT = 0, ENTITY_SMALL_OBJECT = 1, ENTITY_PLAYER = 3, ENTITY_CHARACTER = 4 };

enum { K_ATTACK, K_INVINCIBLE, K_BUMP_LINK, K_BUMP, K_HURT, K_PICKUP, N_KINDS }; /* drawing order */
static const uint8_t kind_colour[N_KINDS] = { AW_HIT_ATTACK, AW_HIT_INVINCIBLE, AW_HIT_LINK, AW_HIT_BUMP, AW_HIT_HURT,
                                              AW_HIT_PICKUP };
static const uint8_t kind_part[N_KINDS] = { HV_ATTACK, HV_INVINCIBLE, HV_BUMP, HV_BUMP, HV_HURT, HV_PICKUP };

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
  return (hitview_shown && any_style()) || n_own > 0;
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
static uint32_t frame_no;
#define WATCH_NEW 0   /* debugging: 1 logs sensors as they come into the world near Link */
#define WATCH_ATTACKS 0 /* debugging: 1 logs each attack shape as it first counts */
static struct { uintptr_t body; uint32_t seen; } fresh[64];
static void watch_new(uintptr_t body, const vec3f_t *link_pos)
{
  const volatile float *top = (const volatile float *)(body + BODY_AABB_MAX);
  if (!sane(top[0]))
    return;
  const volatile float *pos = (const volatile float *)(body + BODY_POS);
  float dx = pos[0] - link_pos->x, dy = pos[1] - link_pos->y, dz = pos[2] - link_pos->z;
  if (!(dx * dx + dy * dy + dz * dz < 400))
    return;
  int slot = -1, oldest = 0;
  for (int i = 0; i < 64; i++) {
    if (fresh[i].body == body) {
      fresh[i].seen = frame_no;
      return;
    }
    if (fresh[i].seen < fresh[oldest].seen)
      oldest = i;
  }
  slot = oldest;
  bool was_known = fresh[slot].body && frame_no - fresh[slot].seen < 2;
  fresh[slot].body = body, fresh[slot].seen = frame_no;
  if (was_known || frame_no < 60)
    return;
  uintptr_t owner = *(const volatile uintptr_t *)(body + BODY_OWNER), shape = *(const volatile uintptr_t *)(body + BODY_SHAPE);
  uintptr_t layer_obj = rd(owner, OWNER_LAYER), actor_link = rd(owner, OWNER_ACTOR), actor = actor_link ? rd(actor_link, 8) : 0;
  uintptr_t name = actor ? rd(actor, 0x20) : 0;
  uint32_t points = 0;
  sys_read32(shape + SHAPE_POINTS + 4, &points);
  float r = heap_ptr(shape) && sys_mapped(shape + SHAPE_RADIUS, 4, SYS_PERM_R) ? *(const volatile float *)(shape + SHAPE_RADIUS) : -1;
  log_printf("hitboxes: new sensor %#lx owner %#lx (vt %#lx) layer %#llx mask %#llx contacts %#lx on %d, %u points r %.2f, "
             "%s at %.2f %.2f %.2f", (unsigned long)body, (unsigned long)owner,
             (unsigned long)(rd(owner, 0) - z64hd_main), (unsigned long long)(layer_obj ? rd(layer_obj, 8) : 0),
             (unsigned long long)rd(owner, OWNER_HIT_MASK), (unsigned long)rd(owner, OWNER_CONTACTS), switched_on(owner),
             points, (double)r, name && sys_mapped(name, 8, SYS_PERM_R) ? (const char *)name : "(no actor)",
             (double)pos[0], (double)pos[1], (double)pos[2]);
}
#define WATCH_MAX 48
static struct { uintptr_t body; uint32_t state; } watched[WATCH_MAX];
static void watch_link_attacks(uintptr_t body, const vec3f_t *link_pos)
{
  uintptr_t owner = *(const volatile uintptr_t *)(body + BODY_OWNER);
  uintptr_t layer_obj = rd(owner, OWNER_LAYER);
  if (!layer_obj || !sys_mapped(layer_obj + 8, 1, SYS_PERM_R) ||
      (*(const volatile uint8_t *)(layer_obj + 8) & LAYER_MASK) != 7 || !sys_mapped(owner + OWNER_HIT_MASK, 8, SYS_PERM_R))
    return;
  const volatile float *pos = (const volatile float *)(body + BODY_POS);
  float dx = pos[0] - link_pos->x, dy = pos[1] - link_pos->y, dz = pos[2] - link_pos->z;
  if (!(dx * dx + dy * dy + dz * dz < 625))
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
  uintptr_t al = rd(owner, OWNER_ACTOR), ac = al ? rd(al, 8) : 0, nm = ac ? rd(ac, 0x20) : 0;
  log_printf("hitboxes: frame %u attack body %#lx %s: in world %d, on %d, request flags %#x asks %u, mask %d", frame_no,
             (unsigned long)body, nm && sys_mapped(nm, 8, SYS_PERM_R) ? (const char *)nm : "-", in_world,
             (int)(state >> 1 & 1), (unsigned)(flags & 0xFFFF), (unsigned)asked, (int)(state >> 24 & 1));
}

/* The actors whose chemistry has an explosion (an ExplodeBlast element, ElementType 3011306755: Item_Bomb's
 * BombFruit_ExplodeBlast, Size 4.3), from every pack's Chemical/ElementParam (FINDINGS 9.39). A blast isn't an attack
 * sensor: it's a sphere on the ChemicalElement layer, the element's Size round, belonging to the bomb (its actor),
 * there for a moment. Other elements (water, a torch's fire, light) are left out. */
static const char *const explosive[] = {
  "Enemy_Amos", "Enemy_Amos_AlwaysRespawn", "Enemy_BabyDodongo", "Enemy_Beamos", "Enemy_Beamos_For_High_Place",
  "Enemy_Dodongo", "Enemy_DungeonBoss_Dodongo_SpawnBabyDodongo", "Enemy_MiddleBoss_FlareDancer", "Item_Bomb",
  "Item_Bombchu", "Npc_Hylia_ImperialSoldier_Bomb", "Obj_BombFlower_A_01", "Obj_BombFlower_A_04",
  "Obj_BombFlower_Fruit_For_Break_Obj_DeathMtTrail_BrokenRock_A_01",
};

static bool is_explosive(uintptr_t actor)
{
  uintptr_t name = actor ? rd(actor, 0x20) : 0;
  if (!name || !sys_mapped(name, 8, SYS_PERM_R))
    return false;
  const char *n = (const char *)name;
  for (unsigned i = 0; i < sizeof(explosive) / sizeof(*explosive); i++) {
    const char *e = explosive[i];
    int k = 0;
    while (e[k] && n[k] == e[k])
      k++;
    if (!e[k] && !n[k])
      return true;
  }
  return false;
}

/* a blast's sphere is the element's Size round (4.3 m a bomb's, 4.5 a Dodongo's, 15 the default); the lit fuse's fire
 * on the same bomb is a far smaller element */
#define BLAST_MIN_RADIUS 2.0f

/* an actor that's out: its state 4 or 5 (not yet appeared, or put away, is 6: a bombchu that's gone off is kept as
 * one, its body still in the world where it fell; 7 on, dying or gone). No actor (scenery): yes */
#define ACTOR_STATE 0x2C
static bool running(uintptr_t actor)
{
  uint32_t state = 0;
  return !actor || (sys_read32(actor + ACTOR_STATE, &state) && (state == 4 || state == 5));
}

/* Collectables (FINDINGS 9.44): what Link picks up by touching it (a rupee's, a heart's... sensor on the sensor
 * Object or SmallObject layer, the drop's own shape, that his body's sensors touch), and what he catches in a bottle
 * (bugs, fish, butterflies: their sensors). Told by name: pots and other breakables have Object sensors too. */
static const char *const pickup_prefixes[] = {
  "Obj_Rupee_", "Obj_RupeeToken", "Obj_Heart", "Obj_MagicJar", "Obj_ArrowBundle", "Obj_DekuNut", "Obj_DekuSeed",
  "Obj_DekuStick", "Obj_ScrubBusinessSeed", "Obj_Fairy", "Animal_Insect", "Animal_Kokiri_Insect", "Animal_Fish",
  "Animal_Butterfly", "Insect_",
};
static const char *const pickup_names[] = { "Obj_Bomb", "Obj_Bombchu" }; /* (not Obj_BombFlower...) */

static bool starts(const char *s, const char *prefix)
{
  while (*prefix && *s == *prefix)
    s++, prefix++;
  return !*prefix;
}

static bool is_pickup(uintptr_t actor)
{
  uintptr_t name = actor ? rd(actor, 0x20) : 0;
  if (!name || !sys_mapped(name, 32, SYS_PERM_R))
    return false;
  const char *s = (const char *)name;
  for (size_t i = 0; i < sizeof(pickup_prefixes) / sizeof(pickup_prefixes[0]); i++)
    if (starts(s, pickup_prefixes[i]))
      return true;
  for (size_t i = 0; i < sizeof(pickup_names) / sizeof(pickup_names[0]); i++)
    if (starts(s, pickup_names[i]) && !s[__builtin_strlen(pickup_names[i])])
      return true;
  return false;
}

static bool part_on[HV_PARTS]; /* classify: the parts drawn for the body it's on */

static int classify(int world, uintptr_t owner, uintptr_t link, uintptr_t shape)
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
  /* whose: everyone's (or only Link's) with the menu's show, in its styles; and the parts ticked for an actor in the
   * actor browser (Link's for his sword and his other bodies), whatever the styles say */
  bool shown = hitview_shown && (!hitview_link_only || links);
  uint8_t ticked = own_parts(actor) | (links ? own_parts(link) : 0);
  for (int part = 0; part < HV_PARTS; part++)
    part_on[part] = (shown && hitview_style[part] != HV_OFF) || (ticked >> part & 1);
  if (!part_on[HV_ATTACK] && !part_on[HV_BUMP] && !part_on[HV_INVINCIBLE] && !part_on[HV_HURT] && !part_on[HV_PICKUP])
    return -1;
  /* switched off (the AI's OneShotSensorHitAtSensorOff and the like: phive's set-enabled, main+0x15B800): there, but
   * nothing touches it */
  if (!switched_on(owner))
    return -1;
  /* a sensor nothing is collected for (no contact collection: the wagon horse's AttackAll, one of Link's) can't hit
   * or be hit: as gz draws only what the game's collision check has this frame */
  /* (not Link's own: a Deku Nut's flash and Din's Fire have none and do hit) */
  if (world == 1 && !rd(owner, OWNER_CONTACTS) && layer != SENSOR_PLAYER_ATTACK)
    return -1;
  /* a collectable's sensors: where Link takes it (its target layers too: a bug's, a fish's) */
  if (world == 1 && !links && (layer <= SENSOR_SMALL_OBJECT || (layer >= SENSOR_PLAYER && layer <= SENSOR_TGT_HIT_ALL)) &&
      is_pickup(actor))
    return part_on[HV_PICKUP] && running(actor) ? K_PICKUP : -1;
  if (world == 1) {
    /* Link's own (his sword, a thrown nut...: PlayerAttack, put in the world or switched on only while it's out), as
     * gz's AT list has them */
    if (layer == SENSOR_PLAYER_ATTACK) { /* (his AttackAll ones aren't: a caught boomerang's stays on in his hand) */
      if (!part_on[HV_ATTACK] || !mask || !running(actor)) /* a spent nut, a caught boomerang: put away */
        return -1;
      link_attacks++;
      return K_ATTACK;
    }
    /* an explosion: an explosive's chemical element (a bomb's blast hurts Link too, his own or not) */
    if (layer == SENSOR_CHEMICAL_ELEMENT) {
      float r = heap_ptr(shape) && sys_mapped(shape + SHAPE_RADIUS, 4, SYS_PERM_R) ?
                *(const volatile float *)(shape + SHAPE_RADIUS) : 0;
      return part_on[HV_ATTACK] && r >= BLAST_MIN_RADIUS && is_explosive(actor) ? K_ATTACK : -1;
    }
    /* anyone else's, while it can hurt Link */
    if (layer == SENSOR_ENEMY_ATTACK || layer == SENSOR_NPC_ATTACK || layer == SENSOR_ATTACK_ALL)
      return part_on[HV_ATTACK] && (mask & (1ull << SENSOR_PLAYER)) && !links && running(actor) ? K_ATTACK
                                                                                                               : -1;
    if (layer < SENSOR_PLAYER || layer > SENSOR_TGT_HIT_ALL || (!actor && !links) || !running(actor))
      return -1;
    if (!part_on[HV_INVINCIBLE] && !part_on[HV_HURT])
      return -1;
    bool invincible = false, fightable = false;
    combat_of(links ? link : actor, link, &invincible, &fightable);
    if (fightable && invincible) /* gz has none then (the game doesn't register it): purple instead */
      return part_on[HV_INVINCIBLE] ? K_INVINCIBLE : -1;
    return part_on[HV_HURT] ? K_HURT : -1;
  }
  if (!part_on[HV_BUMP] || !running(actor))
    return -1;
  /* a character controller (the body a character stands and walks with, and pushes others with) */
  bool controller = Z64HD_VT_CCT_BODY && sys_ptr(owner) == MAIN(Z64HD_VT_CCT_BODY);
  if (links)
    return controller && layer == ENTITY_PLAYER ? K_BUMP_LINK : -1;
  if (layer == ENTITY_CHARACTER && (mask & (1ull << ENTITY_PLAYER)))
    return K_BUMP;
  /* a thing Link runs into and pushes (a bomb, a pot, a crate): an actor's object body that collides with him */
  if ((layer == ENTITY_OBJECT || layer == ENTITY_SMALL_OBJECT) && actor && (mask & (1ull << ENTITY_PLAYER)))
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
  int kind = classify(world, owner, link, shape);
  if (WATCH_ATTACKS && kind == K_ATTACK) { /* debugging: each attack shape, the first frame it counts */
    static uintptr_t logged[32];
    bool known = false;
    for (int i = 0; i < 32; i++)
      known |= logged[i] == body;
    if (!known) {
      static int at;
      logged[at++ % 32] = body;
      uintptr_t lo = rd(owner, OWNER_LAYER), al = rd(owner, OWNER_ACTOR), ac = al ? rd(al, 8) : 0, nm = ac ? rd(ac, 0x20) : 0;
      float r = sys_mapped(shape + SHAPE_RADIUS, 4, SYS_PERM_R) ? *(const volatile float *)(shape + SHAPE_RADIUS) : -1;
      uint32_t pts = 0;
      sys_read32(shape + SHAPE_POINTS + 4, &pts);
      log_printf("hitboxes: attack %#lx layer %#llx %s, %u points r %.2f, at %.2f %.2f %.2f", (unsigned long)body,
                 (unsigned long long)(lo ? rd(lo, 8) : 0), nm && sys_mapped(nm, 8, SYS_PERM_R) ? (const char *)nm : "-",
                 pts, (double)r, (double)p[0], (double)p[1], (double)p[2]);
    }
  }
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
        if (WATCH_NEW && w == 1 && heap_ptr(*(const volatile uintptr_t *)(block + i * BODY_SIZE + BODY_OWNER)))
          watch_new(block + i * BODY_SIZE, link_pos);
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

/* a triangle of a filled shape, if its side faces the camera (each surface drawn once, as gz's: the same opacity
 * all over); `inside`: a point inside the shape, to tell its outer side */
static bool camera_inside; /* the camera is inside the shape being filled: its inner side is drawn instead */

static void face_out(const float a[3], const float b[3], const float c[3], const float inside[3], uint8_t colour)
{
  float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] }, n[3];
  cross(u, v, n);
  if (!(dot(n, n) > 1e-12f))
    return;
  float mid[3] = { (a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3 };
  float out[3] = { mid[0] - inside[0], mid[1] - inside[1], mid[2] - inside[2] };
  if (dot(n, out) < 0)
    n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
  float view[3] = { mid[0] - cam.pos[0], mid[1] - cam.pos[1], mid[2] - cam.pos[2] };
  if ((dot(n, view) < 0) != camera_inside)
    triangle(a, b, c, colour);
}

/* a capsule (a sphere when a is b) as a mesh: its two half spheres and the tube between, filled */
#define MESH_SEGS 12
#define MESH_LATS 4 /* rings in each half sphere, its pole apart */
static void capsule_mesh(const float a[3], const float b[3], float r, uint8_t colour)
{
  float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
  if (!unit(d))
    d[0] = 0, d[1] = 1, d[2] = 0;
  float p[3], q[3];
  square_to(d, p, q);
  /* rings from a's pole to b's: a's half sphere (latitude -90 to 0 deg about a), then b's (0 to 90 about b) */
  float ring[2 * MESH_LATS + 2][MESH_SEGS][3];
  int n_rings = 0;
  for (int half = 0; half < 2; half++)
    for (int l = 0; l <= MESH_LATS; l++) {
      float phi = half ? (PI / 2) * (float)l / MESH_LATS : -(PI / 2) * (float)(MESH_LATS - l) / MESH_LATS;
      const float *c = half ? b : a;
      float cr = cosf(phi) * r, sr = sinf(phi) * r;
      for (int k = 0; k < MESH_SEGS; k++) {
        float t = 2 * PI * (float)k / MESH_SEGS, ct = cosf(t), st = sinf(t);
        for (int j = 0; j < 3; j++)
          ring[n_rings][k][j] = c[j] + (p[j] * ct + q[j] * st) * cr + d[j] * sr;
      }
      n_rings++;
    }
  float inside[3] = { (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2 };
  { /* the camera inside it (a bomb's blast round Link): its inner side, as gz shows a sphere from within */
    float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, ac[3] = { cam.pos[0] - a[0], cam.pos[1] - a[1], cam.pos[2] - a[2] };
    float len2 = dot(ab, ab), t = len2 > 0 ? dot(ac, ab) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float off[3] = { ac[0] - ab[0] * t, ac[1] - ab[1] * t, ac[2] - ab[2] * t };
    camera_inside = dot(off, off) < r * r;
  }
  for (int i = 0; i + 1 < n_rings; i++)
    for (int k = 0; k < MESH_SEGS; k++) {
      int k1 = (k + 1) % MESH_SEGS;
      /* the half sphere's own middle is the better "inside" for its faces (a long capsule's tube ends) */
      const float *in = i < MESH_LATS ? a : i > MESH_LATS ? b : inside;
      face_out(ring[i][k], ring[i][k1], ring[i + 1][k1], in, colour);
      face_out(ring[i][k], ring[i + 1][k1], ring[i + 1][k], in, colour);
    }
  camera_inside = false;
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
  float middle[3];
  to_world(body, centre, middle);
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
    if (fill) /* filled, its six sides (lines aren't drawn then) */
      for (int axis = 1; axis < 8; axis <<= 1) {
        int u = axis == 1 ? 2 : 1, v = axis == 4 ? 2 : 4;
        for (int side = 0; side < 2; side++) {
          int o = side ? axis : 0;
          face_out(c[o], c[o | u], c[o | u | v], middle, colour);
          face_out(c[o], c[o | u | v], c[o | v], middle, colour);
        }
      }
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
        face_out(world[idx[0]], world[i0], world[i1], middle, colour);
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
  int style = hitview_style[kind_part[f->kind]];
  bool fill = style == HV_FILL || style == HV_OFF; /* (off: drawn for an actor's own switch, filled) */
  outline = !fill;
  if (n == 1) {
    float c[3];
    to_world(f->body, local[0], c);
    if (fill)
      capsule_mesh(c, c, r, colour);
    else
      sphere(c, r, colour, false);
  } else if (n == 2) {
    float a[3], b[3];
    to_world(f->body, local[0], a);
    to_world(f->body, local[1], b);
    if (fill)
      capsule_mesh(a, b, r, colour);
    else
      capsule(a, b, r, colour, false);
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

/* Where a raised shield blocks from (FINDINGS 9.42, 9.43), while its holder guards: whether it does is its
 * animation's Guard hold event (what QueryASGetHoldEventGuard reads, main+0x2984F80): [[actor+0x270]+0x40] the AS
 * component, +0x18, +0x18 its slots (count +0x18, array +0x20, 0x48 bytes each), each slot's [+0x58] hold events,
 * bit 0x10 of their +0x40 Guard. On for Link while he guards (any guard: standing, crouching, its reaction), and
 * for a Stalfos while its shield is up (not otherwise: it took hits then).
 * The guarded directions: within GuardableAngleMaxDeg / 2 of the guard's direction (DamageParameters: Link's 120,
 * every enemy's 100). Link's shield turns up to 50 towards the attacker every way (MoveShieldLimitAngle), so his
 * reach 110 from his front (measured level: blocked to 110, through from 115); an enemy turns its whole body to
 * face what it guards against (a Stalfos was square to Link again within ten frames), 50 from its front. Drawn as
 * a bubble at the shield's height with the unguarded cone cut out behind and its rim ringed; its radius is only
 * for seeing it (the check has none). Above and below aren't measured. */
#define GUARD_LINK_DEG 110.0f
#define GUARD_ENEMY_DEG 50.0f
#define GUARD_RADIUS 1.3f
#define GUARD_HEIGHT_ADULT 0.95f
#define GUARD_HEIGHT_CHILD 0.6f
#define GUARD_HEIGHT_ENEMY 1.0f
#define GUARD_RINGS 5   /* bands from the front to the rim */
#define GUARD_AROUND 16 /* segments round */
#define GUARD_HOLD 0x10
#define AS_SLOT_SIZE 0x48
#define AS_MAX_SLOTS 16

static bool guard_held(uintptr_t actor)
{
  uintptr_t comps = rd(actor, 0x270), as = comps ? rd(comps, 0x40) : 0, a1 = as ? rd(as, 0x18) : 0;
  uintptr_t ctl = a1 ? rd(a1, 0x18) : 0;
  if (!ctl || !sys_mapped(ctl + 0x18, 0x10, SYS_PERM_R))
    return false;
  uint32_t n = *(const volatile uint32_t *)(ctl + 0x18);
  uintptr_t slots = rd(ctl, 0x20);
  if (!slots || n > AS_MAX_SLOTS || !sys_mapped(slots, n * AS_SLOT_SIZE, SYS_PERM_R))
    return false;
  for (uint32_t k = 0; k < n; k++) {
    uintptr_t e = *(const volatile uintptr_t *)(slots + k * AS_SLOT_SIZE), h = heap_ptr(e) ? rd(e, 0x58) : 0;
    if (h && sys_mapped(h + 0x40, 4, SYS_PERM_R) && (*(const volatile uint32_t *)(h + 0x40) & GUARD_HOLD))
      return true;
  }
  return false;
}

/* the guarded directions round c: within deg of the level direction (fx, fz) */
static void guard_shape(const float c[3], float fx, float fz, float deg)
{
  float f[3] = { fx, 0, fz }, u[3] = { fz, 0, -fx }, v[3] = { 0, 1, 0 };
  float inside[3] = { c[0] + f[0] * GUARD_RADIUS * 0.4f, c[1], c[2] + f[2] * GUARD_RADIUS * 0.4f };
  float dc[3] = { cam.pos[0] - inside[0], cam.pos[1] - inside[1], cam.pos[2] - inside[2] };
  camera_inside = dot(dc, dc) < GUARD_RADIUS * GUARD_RADIUS * 0.5f; /* (roughly: drawn from inside then) */
  float pts[GUARD_RINGS + 1][GUARD_AROUND][3];
  for (int r = 0; r <= GUARD_RINGS; r++) {
    float t = deg * PI / 180.0f * (float)r / GUARD_RINGS, ct = cosf(t), st = sinf(t);
    for (int k = 0; k < GUARD_AROUND; k++) {
      float p = 2 * PI * (float)k / GUARD_AROUND, cp = cosf(p), sp = sinf(p);
      for (int i = 0; i < 3; i++)
        pts[r][k][i] = c[i] + GUARD_RADIUS * (f[i] * ct + st * (u[i] * cp + v[i] * sp));
    }
  }
  outline = true;
  for (int k = 0; k < GUARD_AROUND; k++) {
    int k1 = (k + 1) % GUARD_AROUND;
    for (int r = 0; r < GUARD_RINGS; r++) { /* the sphere's part, front to rim */
      face_out(pts[r][k], pts[r + 1][k], pts[r + 1][k1], inside, AW_HIT_GUARD);
      if (r > 0)
        face_out(pts[r][k], pts[r + 1][k1], pts[r][k1], inside, AW_HIT_GUARD);
    }
    face_out(c, pts[GUARD_RINGS][k], pts[GUARD_RINGS][k1], inside, AW_HIT_GUARD); /* the cone cut out (or the rest) */
    line(pts[GUARD_RINGS][k], pts[GUARD_RINGS][k1], AW_HIT_GUARD);                /* its rim */
  }
  camera_inside = false;
}

#define GUARD_MAX_ACTORS 64
static void guards(const vec3f_t *link_pos, uintptr_t link)
{
  double dfx, dfz;
  if (link && guard_held(link) && link_facing(&dfx, &dfz)) {
    float c[3] = { link_pos->x, link_pos->y + (age_current() == 0 ? GUARD_HEIGHT_CHILD : GUARD_HEIGHT_ADULT),
                   link_pos->z };
    guard_shape(c, (float)dfx, (float)dfz, GUARD_LINK_DEG);
  }
  if (hitview_link_only)
    return;
  static actor_near_t near[GUARD_MAX_ACTORS];
  int n = actors_near(RANGE, near, GUARD_MAX_ACTORS);
  for (int i = 0; i < n; i++) {
    uintptr_t a = near[i].actor;
    if (a == link || (near[i].state != 4 && near[i].state != 5) || !guard_held(a) ||
        !sys_mapped(a + 0x328, 36, SYS_PERM_R))
      continue;
    const volatile float *rot = (const volatile float *)(a + 0x328); /* by rows: its front is the third column */
    float fx = rot[2], fz = rot[8], len = root(fx * fx + fz * fz);
    if (!(len > 1e-3f))
      continue;
    float c[3] = { near[i].pos[0], near[i].pos[1] + GUARD_HEIGHT_ENEMY, near[i].pos[2] };
    guard_shape(c, fx / len, fz / len, GUARD_ENEMY_DEG);
  }
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
  if (hitview_shown && hitview_guard)
    guards(&link_pos, link_actor());
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

/* debugging (remote hitlist): the shapes found last frame, one a line */
static bool has_text(const char *s, const char *part)
{
  for (; *s; s++) {
    int k = 0;
    while (part[k] && s[k] == part[k])
      k++;
    if (!part[k])
      return true;
  }
  return false;
}

/* debugging (remote "hitlist <name>"): every body of the actors whose name has `name` in it, drawn or not */
int hitview_list_all(const char *name, char *out, int len)
{
  int at = 0;
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
      for (unsigned i = 0; i < BODIES_PER_BLOCK && b * BODIES_PER_BLOCK + i < capacity && at < len - 300; i++) {
        uintptr_t body = block + i * BODY_SIZE, owner = *(const volatile uintptr_t *)(body + BODY_OWNER);
        if (!heap_ptr(owner))
          continue;
        uintptr_t al = rd(owner, OWNER_ACTOR), ac = al ? rd(al, 8) : 0, nm = ac ? rd(ac, 0x20) : 0;
        if (!nm || !sys_mapped(nm, 8, SYS_PERM_R) || !has_text((const char *)nm, name))
          continue;
        uintptr_t lo = rd(owner, OWNER_LAYER), shape = *(const volatile uintptr_t *)(body + BODY_SHAPE);
        uint32_t pts = 0;
        if (heap_ptr(shape))
          sys_read32(shape + SHAPE_POINTS + 4, &pts);
        float r = heap_ptr(shape) && sys_mapped(shape + SHAPE_RADIUS, 4, SYS_PERM_R) ?
                  *(const volatile float *)(shape + SHAPE_RADIUS) : -1;
        const volatile float *p = (const volatile float *)(body + BODY_POS);
        at += snprintf(out + at, (size_t)(len - at),
                       "w%d %s layer %#llx mask %#llx contacts %d on %d world %d, %u points r %.2f at %.2f %.2f %.2f\n", w,
                       (const char *)nm, (unsigned long long)(lo ? rd(lo, 8) : 0),
                       (unsigned long long)rd(owner, OWNER_HIT_MASK), rd(owner, OWNER_CONTACTS) != 0,
                       switched_on(owner), sane(((const volatile float *)(body + BODY_AABB_MAX))[0]), (unsigned)pts,
                       (double)r, (double)p[0], (double)p[1], (double)p[2]);
      }
    }
  }
  return at;
}

int hitview_list(char *out, int len)
{
  static const char *const kinds[N_KINDS] = { "attack", "invincible", "link", "bump", "hurt", "pickup" };
  int at = 0;
  for (int i = 0; i < n_found && at < len - 200; i++) {
    uintptr_t owner = rd(found[i].body, BODY_OWNER), lo = rd(owner, OWNER_LAYER), al = rd(owner, OWNER_ACTOR);
    uintptr_t ac = al ? rd(al, 8) : 0, nm = ac ? rd(ac, 0x20) : 0;
    uint32_t state = 0, flags = 0;
    if (ac)
      sys_read32(ac + 0x2C, &state);
    sys_read32(found[i].body + 0x50, &flags);
    const volatile float *p = (const volatile float *)(found[i].body + BODY_POS);
    at += snprintf(out + at, (size_t)(len - at), "%s %s state %u layer %#llx flags %#x owner %#lx %.1f m at %.2f %.2f %.2f\n",
                   kinds[found[i].kind], nm && sys_mapped(nm, 8, SYS_PERM_R) ? (const char *)nm : "-", (unsigned)state,
                   (unsigned long long)(lo ? rd(lo, 8) : 0), (unsigned)flags, (unsigned long)owner,
                   (double)found[i].distance, (double)p[0], (double)p[1], (double)p[2]);
  }
  return at;
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
