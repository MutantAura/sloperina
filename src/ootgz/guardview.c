/* Guard vision (FINDINGS 9.36): where the security guards see Link, on the ground round each, coloured by what each
 * is doing, and the paths they patrol.
 *
 * A guard is any actor whose parameter table has a SecurityGuardParam of its own (the Gerudo fortress's guards,
 * Hyrule Castle's soldiers, the courtyard's child soldiers among them), so nothing here goes by name. While a guard
 * is on duty (the game's IsSecurityMode) its AI decides it has found Link with one of two sights, chosen by its
 * AlertMode.UseCustomLOS:
 *   - the custom line of sight: from the Head bone, Link's distance across the ground under a range and the angle
 *     between the head's front (its +Y, flattened) and Link under AngleInFront either way, Link's height inside a band
 *     round the head's. The range, angle and band come from CustomLOSAreaParam: Light, Dark or PlayerRidingHorse
 *     (whichever applies now), each with an Alert and a Find set; the game's own functions give them for the guard
 *     as it is this frame. The alert set's further band is drawn fainter.
 *   - otherwise the engine's sight, switched to the receiver's "SecurityGuard" custom sight param while on duty:
 *     7.5 m across, 7.5 m up or down, DiscoverAngle 90. Whether the engine takes that angle either way of the front
 *     or as the whole cone isn't known: it's drawn either way (the larger), so nothing that would be seen is drawn
 *     as safe. Not read from the game: these are the values in the guards' files.
 * and either way Link is found when he's closer than AlertMode.CloseToPlayer's threshold (light, dark; on a horse
 * not told apart here), any direction: the ring.
 *
 * What hides Link: the game casts rays from the head to him, on the collision layer (26) the module's ground cast
 * uses. Here rays go out across each cone from the head to the height of Link's middle over the guard's feet, a few
 * guards a frame, and each sight stops where its ray hits: walls, hedges, crates. Link counts as seen only short of
 * that.
 *
 * The colour is the guard's state, its BSA brain verb ([[actor+0x270]+0x188]+0x28): green searching (on duty,
 * patrolling or looking round), blue alerted (Alerted, SuspiciousIndividualNotice, VeryCloseNotice), red caught
 * (SuspiciousIndividualDiscovered) or seeing Link now; grey off duty.
 *
 * The paths: the map's Guide rails (data_rails, tools/rails.py) that a guard is on or near, with a ring where guards
 * stop (WaitFrame) and a tick the way they face there. Any other actor the layout links to a rail (a townsperson, a
 * dog, a moving platform: data_rail_owners, by the instance id the game keeps on it) has its path too, by its own
 * switch in the actor browser. Not drawn: hearing. */
#include "guardview.h"

#include <string.h>

#include "data.h"
#include "enemyhp.h" /* actors_all */
#include "gamedata.h"
#include "lang.h"
#include "placer.h" /* the actor browser: its spot and actor */
#include "link.h"
#include "sys.h"
#include "voidmap.h" /* voidmap_ground_cast */
#include "z64hd.h"

float sinf(float x), cosf(float x), atan2f(float y, float x); /* from sdk */

bool guardview_shown;
bool guardview_paths = true;

#define RANGE 60.0f          /* metres from Link: guards further aren't looked at */
#define MAX_GUARDS 16
#define MAX_ACTORS 2048      /* actors_all's buffer */
#define SCAN_FRAMES 20       /* the actors are looked through for guards this often */
#define LOG_FRAMES 600       /* a guard's numbers go to the log when it's found and this often after */
#define GROUND_LIFT 0.05f    /* the fills sit this far over the guard's feet */
#define SLICES 24            /* a sight's fill: triangles across it */
#define RING_SLICES 20
#define RAYS 25              /* the rays across a guard's widest sight, for what hides Link */
#define RAY_BUDGET 75        /* rays cast a frame (a guard's at a time): the rest wait for the next frames */
#define TARGET_UP 0.8f       /* the rays aim this far over the guard's feet: Link's middle, about */
#define PATH_NEAR 4.0f       /* a rail is drawn when a guard is this near it (across) */
#define PATH_LIFT 0.1f
#define STOP_RING 0.4f       /* the ring where guards stop on a rail, and the tick the way they face */
#define STOP_TICK 1.2f
#define DEG (3.14159265f / 180.0f)

/* the standard sight's "SecurityGuard" custom param (the guards' PerceptionReceiverParam) */
#define SIGHT_RANGE 7.5f
#define SIGHT_BELOW -7.5f
#define SIGHT_ABOVE 7.5f
#define SIGHT_ANGLE 90.0f /* DiscoverAngle: drawn as this either way of the front (see above) */

/* the actor and its parameters (FINDINGS 9.5, 9.36) */
#define ACTOR_NAME 0x20
#define ACTOR_COMPONENTS 0x270
#define ACTOR_POS 0x31C
#define ACTOR_ROT 0x328            /* 3x3, rows */
/* the actor's game parameter table [[actor+0x270]+0xA8], as the param getter searches it: entries [+0x38], count
 * [+0x30], each {u32 hash, ..., +8 holder}, sorted by hash; the holder's +0x150 is the parameter object. The getter
 * gives every actor without one a shared default, so a guard is one whose table has it. */
#define COMPONENTS_PARAM_TABLE 0xA8
#define TABLE_COUNT 0x30
#define TABLE_ENTRIES 0x38
#define ENTRY_HOLDER 0x08
#define HOLDER_PARAM 0x150
#define HASH_SECURITY_GUARD_PARAM 0x6160B32Au
#define COMPONENTS_MODEL 0x08      /* the model component: its vtable's slot 0xF8 gives the model */
#define MODEL_GETTER_SLOT (0xF8 / 8)
#define MODEL_PARTS 0x28           /* the model's parts: a part's skeleton is **parts[i] */
#define MODEL_PART_COUNT 0x20      /* i32 */
#define SKELETON_MATRIX_SLOT (0x78 / 8) /* (skeleton, mtx34 *out, int bone): the bone's world matrix */
#define ALERT_CLOSE 0x58           /* AlertMode: the CloseToPlayer object */
#define ALERT_USE_CUSTOM_LOS 0xFD  /* AlertMode: u8 */
#define CLOSE_LIGHT 0x3C           /* CloseToPlayer: floats */
#define CLOSE_DARK 0x38
#define COMPONENTS_BSA 0x188       /* the BSA component: its brain verb, u32 at +0x28 (QueryBSAGetBrainVerb) */
#define BSA_BRAIN_VERB 0x28
/* brain verbs (the enum's names in main) */
#define VERB_ALERTED 5
#define VERB_DISCOVERED 51         /* SuspiciousIindividualDiscovered (sic) */
#define VERB_NOTICE 54             /* SuspiciousIindividualNotice */
#define VERB_VERY_CLOSE 59         /* VeryCloseNotice */

typedef uintptr_t (*guard_param_t)(uintptr_t actor);
typedef uintptr_t (*alert_mode_t)(uintptr_t param);
typedef bool (*actor_bool_t)(uintptr_t actor);
typedef float (*los_value_t)(uintptr_t actor, const bool *alert);
/* returned through x8 (more than 16 bytes), as the game's code calls it: the part (u16) and the bone (u16) */
typedef struct { uint16_t part, bone; uint32_t unused[7]; } bone_ref_t;
typedef bone_ref_t (*bone_find_t)(uintptr_t model, const char *const *name);
typedef uintptr_t (*model_get_t)(uintptr_t component);
typedef void (*bone_matrix_t)(uintptr_t skeleton, float *mtx34, int bone);

typedef struct {
  float range, angle, below, above; /* metres across, degrees either way, metres under / over the head */
} sight_t;

enum { STATUS_OFF, STATUS_SEARCH, STATUS_ALERTED, STATUS_CAUGHT };

typedef struct {
  uintptr_t actor;
  const char *name;
  bool on_duty, custom, dark, head_known;
  float feet[3];   /* the actor's position */
  float head[3];   /* the eye: the Head bone, or a guess over the feet */
  float rows[9];   /* the head's rotation, rows (its columns are its axes) */
  float front[2];  /* (x, z), unit */
  sight_t find, alert; /* custom: both; standard: find only */
  float close;     /* CloseToPlayer's threshold now */
  uint32_t verb;   /* its brain verb */
  int status;      /* STATUS_* */
  bool sees_link;  /* Link is inside one of them now, and no ray stops short of him */
  int logged;      /* frames since its numbers went to the log */
  /* the rays: across +-ray_half (radians) of the front, how far each goes (across) before it hits something */
  float ray_half, reach[RAYS];
  bool rays_known;
} guard_t;

/* a guard's own switches (the actor browser's options), off until turned on */
#define MAX_CHOICES 16
static struct { uintptr_t actor; uint64_t instance; bool shown[2]; } choices[MAX_CHOICES];
static int n_choices;

static guard_t guards[MAX_GUARDS];
static int n_guards;
static uintptr_t found[MAX_GUARDS]; /* the last scan's guards */
static int n_found, scan_wait, ray_next;
static actor_near_t scan_buf[MAX_ACTORS];

static bool readable(uintptr_t addr, size_t size)
{
  return addr && !(addr & 3) && sys_mapped(addr, size, SYS_PERM_R);
}

#define MAIN_SIZE_MAX 0x5000000
static bool in_main(uintptr_t addr)
{
  return addr >= z64hd_main && addr < z64hd_main + MAIN_SIZE_MAX;
}

static bool known(void)
{
  const struct z64hd_build *g = z64hd_game;
  return g && g->guard_param && g->guard_alert_mode && g->guard_in_dark && g->guard_los_range && g->guard_los_below &&
         g->guard_los_above && g->guard_los_angle && g->guard_on_duty && g->bone_find;
}

/* whether the actor has a SecurityGuardParam of its own (not the getter's default) */
static bool is_guard(uintptr_t actor)
{
  uintptr_t components = readable(actor + ACTOR_COMPONENTS, 8) ? *(uintptr_t *)(actor + ACTOR_COMPONENTS) : 0;
  uintptr_t table = readable(components + COMPONENTS_PARAM_TABLE, 8) ? *(uintptr_t *)(components + COMPONENTS_PARAM_TABLE) : 0;
  if (!readable(table + TABLE_ENTRIES, 8))
    return false;
  int32_t count = *(int32_t *)(table + TABLE_COUNT);
  uintptr_t entries = *(uintptr_t *)(table + TABLE_ENTRIES);
  if (count <= 0 || count > 4096 || !readable(entries, (size_t)count * 8))
    return false;
  for (int i = 0; i < count; i++) {
    uintptr_t e = *(uintptr_t *)(entries + i * 8);
    if (!readable(e, ENTRY_HOLDER + 8) || *(uint32_t *)e != HASH_SECURITY_GUARD_PARAM)
      continue;
    uintptr_t holder = *(uintptr_t *)(e + ENTRY_HOLDER);
    return readable(holder + HOLDER_PARAM, 8) && *(uintptr_t *)(holder + HOLDER_PARAM);
  }
  return false;
}

/* --- the head ----------------------------------------------------------------------------------- */

/* the Head bone's world matrix (3 rows of 4), as QueryModelGetBoneMatrix34f gets it */
/* a bone's world matrix (3 rows of 4), as QueryModelGetBoneMatrix34f gets it; false if it hasn't one by that name */
bool guardview_bone_matrix(uintptr_t actor, const char *bone, float m[12])
{
  if (!z64hd_game || !z64hd_game->bone_find)
    return false;
  uintptr_t components = readable(actor + ACTOR_COMPONENTS, 8) ? *(uintptr_t *)(actor + ACTOR_COMPONENTS) : 0;
  uintptr_t mc = readable(components + COMPONENTS_MODEL, 8) ? *(uintptr_t *)(components + COMPONENTS_MODEL) : 0;
  if (!readable(mc, 8))
    return false;
  uintptr_t vt = *(uintptr_t *)mc;
  if (!in_main(vt) || !readable(vt + MODEL_GETTER_SLOT * 8, 8) || !in_main(*(uintptr_t *)(vt + MODEL_GETTER_SLOT * 8)))
    return false;
  uintptr_t model = ((model_get_t) * (uintptr_t *)(vt + MODEL_GETTER_SLOT * 8))(mc);
  if (!readable(model + MODEL_PARTS, 8))
    return false;
  const char *name = bone;
  bone_ref_t ref;
  memset(&ref, 0xFF, sizeof(ref));
  ref = MAIN_FN(bone_find_t, z64hd_game->bone_find)(model, &name);
  if (ref.part == 0xFFFF || ref.bone == 0xFFFF || (int32_t)ref.part >= *(int32_t *)(model + MODEL_PART_COUNT))
    return false;
  uintptr_t parts = *(uintptr_t *)(model + MODEL_PARTS);
  uintptr_t part = readable(parts + ref.part * 8, 8) ? *(uintptr_t *)(parts + ref.part * 8) : 0;
  uintptr_t skeleton = readable(part, 8) ? *(uintptr_t *)part : 0;
  uintptr_t svt = readable(skeleton, 8) ? *(uintptr_t *)skeleton : 0;
  if (!in_main(svt) || !readable(svt + SKELETON_MATRIX_SLOT * 8, 8) ||
      !in_main(*(uintptr_t *)(svt + SKELETON_MATRIX_SLOT * 8)))
    return false;
  float out[12] __attribute__((aligned(16)));
  memset(out, 0, sizeof(out));
  ((bone_matrix_t) * (uintptr_t *)(svt + SKELETON_MATRIX_SLOT * 8))(skeleton, out, (int16_t)ref.bone);
  float scale = out[1] * out[1] + out[5] * out[5] + out[9] * out[9];
  if (!(scale > 0.01f && scale < 100.0f)) /* not a rotation: give up on it */
    return false;
  memcpy(m, out, sizeof(out));
  return true;
}

static bool head_matrix(uintptr_t actor, float m[12]) { return guardview_bone_matrix(actor, "Head", m); }

/* --- one guard ----------------------------------------------------------------------------------- */

static float read_float(uintptr_t addr, float otherwise)
{
  return readable(addr, 4) ? *(const float *)addr : otherwise;
}


static void custom_sight(uintptr_t actor, bool alert, sight_t *s)
{
  los_value_t range = MAIN_FN(los_value_t, z64hd_game->guard_los_range);
  los_value_t below = MAIN_FN(los_value_t, z64hd_game->guard_los_below);
  los_value_t above = MAIN_FN(los_value_t, z64hd_game->guard_los_above);
  los_value_t angle = MAIN_FN(los_value_t, z64hd_game->guard_los_angle);
  s->range = range(actor, &alert);
  s->below = below(actor, &alert);
  s->above = above(actor, &alert);
  s->angle = angle(actor, &alert);
}

static uint32_t brain_verb(uintptr_t actor)
{
  uintptr_t components = readable(actor + ACTOR_COMPONENTS, 8) ? *(uintptr_t *)(actor + ACTOR_COMPONENTS) : 0;
  uintptr_t bsa = readable(components + COMPONENTS_BSA, 8) ? *(uintptr_t *)(components + COMPONENTS_BSA) : 0;
  return readable(bsa + BSA_BRAIN_VERB, 4) ? *(const uint32_t *)(bsa + BSA_BRAIN_VERB) : 0;
}

static bool read_guard(uintptr_t actor, const char *name, guard_t *g)
{
  uintptr_t param = MAIN_FN(guard_param_t, z64hd_game->guard_param)(actor);
  if (!param)
    return false;
  uintptr_t alert_mode = MAIN_FN(alert_mode_t, z64hd_game->guard_alert_mode)(param);
  memset(g, 0, sizeof(*g));
  g->actor = actor;
  g->name = name;
  const float *pos = (const float *)(actor + ACTOR_POS);
  for (int i = 0; i < 3; i++)
    g->feet[i] = pos[i];
  g->on_duty = MAIN_FN(actor_bool_t, z64hd_game->guard_on_duty)(actor);
  g->dark = MAIN_FN(actor_bool_t, z64hd_game->guard_in_dark)(actor);
  g->custom = readable(alert_mode, ALERT_USE_CUSTOM_LOS + 1) && *(const uint8_t *)(alert_mode + ALERT_USE_CUSTOM_LOS);
  uintptr_t close = readable(alert_mode + ALERT_CLOSE, 8) ? *(uintptr_t *)(alert_mode + ALERT_CLOSE) : 0;
  g->close = close ? read_float(close + (g->dark ? CLOSE_DARK : CLOSE_LIGHT), 0) : 0;
  if (!(g->close >= 0 && g->close < 50))
    g->close = 0;
  g->verb = brain_verb(actor);

  float m[12];
  g->head_known = head_matrix(actor, m);
  if (g->head_known) {
    g->head[0] = m[3], g->head[1] = m[7], g->head[2] = m[11];
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        g->rows[r * 3 + c] = m[r * 4 + c];
  } else { /* the body's turn and a head's height */
    const float *rot = (const float *)(actor + ACTOR_ROT);
    for (int i = 0; i < 9; i++)
      g->rows[i] = rot[i];
    g->head[0] = pos[0], g->head[1] = pos[1] + 1.6f, g->head[2] = pos[2];
  }
  /* the front: the head's +Y; the body's +Z where there's no head */
  float fx = g->head_known ? g->rows[1] : g->rows[2], fz = g->head_known ? g->rows[7] : g->rows[8];
  float len = __builtin_sqrtf(fx * fx + fz * fz);
  if (len < 1e-3f)
    fx = 0, fz = 1, len = 1;
  g->front[0] = fx / len, g->front[1] = fz / len;

  if (g->custom) {
    custom_sight(actor, false, &g->find);
    custom_sight(actor, true, &g->alert);
  } else {
    g->find = (sight_t){ SIGHT_RANGE, SIGHT_ANGLE, SIGHT_BELOW, SIGHT_ABOVE };
  }
  float widest = g->find.angle > g->alert.angle ? g->find.angle : g->alert.angle;
  g->ray_half = (widest > 180 ? 180 : widest) * DEG;
  return true;
}

/* --- the rays --------------------------------------------------------------------------------------- */

static float max_range(const guard_t *g)
{
  return g->find.range > g->alert.range ? g->find.range : g->alert.range;
}

/* the direction `a` radians round from the front (either way: the sights are even) */
static void turned(const float front[2], float a, float d[2])
{
  float c = cosf(a), s = sinf(a);
  d[0] = front[0] * c + front[1] * s, d[1] = -front[0] * s + front[1] * c;
}

static float ray_angle(const guard_t *g, int i)
{
  return -g->ray_half + 2 * g->ray_half * i / (RAYS - 1);
}

static void cast_rays(guard_t *g)
{
  float range = max_range(g);
  for (int i = 0; i < RAYS; i++) {
    float d[2], pos[3], n[3];
    turned(g->front, ray_angle(g, i), d);
    float to[3] = { g->head[0] + d[0] * range, g->feet[1] + TARGET_UP, g->head[2] + d[1] * range };
    g->reach[i] = range;
    if (voidmap_ground_cast(g->head, to, pos, n)) {
      float dx = pos[0] - g->head[0], dz = pos[2] - g->head[2];
      g->reach[i] = __builtin_sqrtf(dx * dx + dz * dz);
    }
  }
  g->rays_known = true;
}

/* how far across the sight goes `a` radians round from the front before something's in the way */
static float reach_at(const guard_t *g, float a)
{
  if (!g->rays_known || g->ray_half <= 0)
    return 1e9f;
  float t = (a + g->ray_half) / (2 * g->ray_half) * (RAYS - 1);
  if (t <= 0)
    return g->reach[0];
  if (t >= RAYS - 1)
    return g->reach[RAYS - 1];
  int i = (int)t;
  float f = t - i;
  return g->reach[i] + (g->reach[i + 1] - g->reach[i]) * f;
}

/* --- Link in a sight ---------------------------------------------------------------------------------- */

/* across, the angle off the front, the height band as the game works it out (the head's up (+X) and front (+Y)
 * tilting it: rows[3], rows[4]), and short of what the rays hit */
static bool in_sight(const guard_t *g, const sight_t *s, const vec3f_t *link)
{
  if (s->range <= 0 || s->angle <= 0)
    return false;
  float dx = link->x - g->head[0], dz = link->z - g->head[2];
  float across = __builtin_sqrtf(dx * dx + dz * dz);
  if (across >= s->range)
    return false;
  float off = 0;
  if (across > 1e-3f) {
    float c = (dx * g->front[0] + dz * g->front[1]) / across, x = dx * g->front[1] - dz * g->front[0];
    off = atan2f(x, c);
    if ((off < 0 ? -off : off) >= s->angle * DEG)
      return false;
  }
  if (across >= reach_at(g, -off)) /* turned() turns the other way round */
    return false;
  float up = g->head_known ? g->rows[3] : 1, tilt = g->head_known ? g->rows[4] : 0;
  float low = g->head[1] + up * s->below + tilt * across, high = g->head[1] + up * s->above + tilt * across;
  return link->y >= low && link->y <= high;
}

static bool too_close(const guard_t *g, const vec3f_t *link)
{
  float dx = link->x - g->head[0], dy = link->y - g->head[1], dz = link->z - g->head[2];
  return g->close > 0 && dx * dx + dy * dy + dz * dz < g->close * g->close;
}

static int status_of(const guard_t *g)
{
  if (!g->on_duty)
    return STATUS_OFF;
  if (g->verb == VERB_DISCOVERED || g->sees_link)
    return STATUS_CAUGHT;
  if (g->verb == VERB_ALERTED || g->verb == VERB_NOTICE || g->verb == VERB_VERY_CLOSE)
    return STATUS_ALERTED;
  return STATUS_SEARCH;
}

static const char *const status_names[] = { "off duty", "searching", "alerted", "caught" };

static void log_guard(const guard_t *g)
{
  log_printf("guard vision: %s %#lx %s (verb %u)%s%s head %s (%.2f %.2f %.2f) front (%.2f %.2f) close %.1f", g->name,
             (unsigned long)g->actor, status_names[g->status], (unsigned)g->verb,
             g->custom ? ", custom sight" : ", standard sight", g->dark ? ", dark" : "", g->head_known ? "bone" : "guessed",
             (double)g->head[0], (double)g->head[1], (double)g->head[2], (double)g->front[0], (double)g->front[1],
             (double)g->close);
  log_printf("guard vision:   find %.1f m %.0f deg %.1f..%.1f   alert %.1f m %.0f deg %.1f..%.1f   rays %s %.1f %.1f %.1f",
             (double)g->find.range, (double)g->find.angle, (double)g->find.below, (double)g->find.above,
             (double)g->alert.range, (double)g->alert.angle, (double)g->alert.below, (double)g->alert.above,
             g->rays_known ? "left/mid/right" : "not yet", (double)g->reach[0], (double)g->reach[RAYS / 2],
             (double)g->reach[RAYS - 1]);
}

/* --- who walks a rail ------------------------------------------------------------------------------- */

/* the layout's instance id the game keeps on an actor: [[actor+0x3D8]+0x88] (memfile.c, both builds), or 0 */
#define ACTOR_PREACTOR 0x3D8
#define PREACTOR_INSTANCE 0x88

static uint64_t instance_of(uintptr_t actor)
{
  uintptr_t pre = readable(actor + ACTOR_PREACTOR, 8) ? *(uintptr_t *)(actor + ACTOR_PREACTOR) : 0;
  if (!readable(pre + PREACTOR_INSTANCE, 8))
    return 0;
  uint64_t id = *(uint64_t *)(pre + PREACTOR_INSTANCE);
  return id == UINT64_MAX || id == 0xFFFFFFFFull ? 0 : id;
}

/* the first of data_rail_owners for this instance (they're sorted), or data_n_rail_owners with none */
static int first_owned(uint64_t instance)
{
  int lo = 0, hi = data_n_rail_owners;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (data_rail_owners[mid].instance < instance)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < data_n_rail_owners && data_rail_owners[lo].instance == instance && instance ? lo : data_n_rail_owners;
}

int guardview_rails_of(uintptr_t actor)
{
  uint64_t instance = actor ? instance_of(actor) : 0;
  int n = 0;
  for (int i = first_owned(instance); i < data_n_rail_owners && data_rail_owners[i].instance == instance; i++)
    n++;
  return n;
}

/* --- each guard's own choice ------------------------------------------------------------------------- */

static int choice_of(uintptr_t actor)
{
  for (int i = 0; i < n_choices; i++)
    if (choices[i].actor == actor)
      return i;
  return -1;
}

bool guardview_is_guard(uintptr_t actor)
{
  return known() && actor && is_guard(actor);
}

bool guardview_has_path(uintptr_t actor)
{
  return guardview_is_guard(actor) || (known() && guardview_rails_of(actor) > 0);
}

bool guardview_own(uintptr_t actor, int what)
{
  int c = choice_of(actor);
  return c >= 0 && choices[c].shown[what];
}

bool guardview_shows(uintptr_t actor, int what)
{
  return guardview_own(actor, what) || (guardview_shown && (what == GV_VISION || guardview_paths));
}

void guardview_toggle(uintptr_t actor, int what)
{
  bool now = !guardview_own(actor, what);
  int c = choice_of(actor);
  if (c < 0) {
    if (n_choices >= MAX_CHOICES) /* the oldest makes room */
      memmove(&choices[0], &choices[1], sizeof(choices[0]) * --n_choices);
    c = n_choices++;
    choices[c].actor = actor;
    choices[c].instance = instance_of(actor);
    choices[c].shown[GV_VISION] = choices[c].shown[GV_PATH] = false;
  }
  choices[c].shown[what] = now;
  log_printf("guard vision: %#lx %s %s (%d rails its own)", (unsigned long)actor, what == GV_VISION ? "vision" : "path",
             now ? "on" : "off", guardview_rails_of(actor));
}

bool guardview_active(void)
{
  if (guardview_shown)
    return true;
  for (int i = 0; i < n_choices; i++)
    if (choices[i].shown[GV_VISION] || choices[i].shown[GV_PATH])
      return true;
  return false;
}

/* the guards with a choice of their own that are gone (a map change, killed) are forgotten */
static void forget_gone(void)
{
  int kept = 0;
  for (int i = 0; i < n_choices; i++) {
    actor_read_t a;
    if (actor_read(choices[i].actor, &a) && a.running)
      choices[kept++] = choices[i];
  }
  n_choices = kept;
}

/* --- each frame ---------------------------------------------------------------------------------- */

static int starts_with(const char *s, const char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return 0;
  return 1;
}

/* the actors near Link (or the actor browser's spot) that are guards: only NPCs are looked at */
static void scan(const vec3f_t *link)
{
  int more = 0, n = actors_all(scan_buf, MAX_ACTORS, &more);
  float spot[3];
  bool browsing = placer_spot(spot);
  uintptr_t selected = placer_selected();
  n_found = 0;
  if (selected && is_guard(selected)) /* the one the browser is on, first */
    found[n_found++] = selected;
  for (int i = 0; i < n && n_found < MAX_GUARDS; i++) {
    const actor_near_t *a = &scan_buf[i];
    if (a->state < 4 || a->state > 5 || !starts_with(a->name, "Npc_"))
      continue;
    float dx = a->pos[0] - link->x, dy = a->pos[1] - link->y, dz = a->pos[2] - link->z;
    float sx = a->pos[0] - spot[0], sy = a->pos[1] - spot[1], sz = a->pos[2] - spot[2];
    if (dx * dx + dy * dy + dz * dz > RANGE * RANGE && (!browsing || sx * sx + sy * sy + sz * sz > RANGE * RANGE))
      continue;
    if (a->actor == selected || !is_guard(a->actor))
      continue;
    found[n_found++] = a->actor;
  }
  forget_gone();
  for (int i = 0; i < n_choices && n_found < MAX_GUARDS; i++) { /* one chosen in the browser, wherever it is */
    bool listed = false;
    for (int k = 0; k < n_found; k++)
      listed |= found[k] == choices[i].actor;
    if (!listed && is_guard(choices[i].actor)) /* not one walking a path of its own */
      found[n_found++] = choices[i].actor;
  }
}

void guardview_frame(void)
{
  vec3f_t link;
  if (!guardview_active() || !known() || !link_drawn_pos(&link)) {
    n_guards = 0;
    n_found = 0;
    scan_wait = 0;
    return;
  }
  static uintptr_t selected_before;
  uintptr_t selected = placer_selected();
  if (--scan_wait <= 0 || selected != selected_before) { /* the browser on another actor: at once */
    scan(&link);
    scan_wait = SCAN_FRAMES;
    selected_before = selected;
  }
  static guard_t was[MAX_GUARDS];
  int n_was = n_guards;
  memcpy(was, guards, sizeof(guard_t) * n_was);
  n_guards = 0;
  for (int i = 0; i < n_found; i++) {
    actor_read_t a;
    if (!actor_read(found[i], &a) || !a.running) /* gone since the scan */
      continue;
    guard_t *g = &guards[n_guards];
    if (!read_guard(found[i], a.name, g))
      continue;
    const guard_t *before = 0;
    for (int k = 0; k < n_was; k++)
      if (was[k].actor == g->actor)
        before = &was[k];
    if (before && before->rays_known) { /* the rays cast before, until its turn comes again */
      memcpy(g->reach, before->reach, sizeof(g->reach));
      g->rays_known = true;
    }
    g->logged = before ? before->logged + 1 : 0;
    n_guards++;
  }
  /* the rays: a few guards a frame, in turn (a new one first) */
  int budget = RAY_BUDGET;
  for (int i = 0; i < n_guards && budget >= RAYS; i++)
    if (!guards[i].rays_known && guardview_shows(guards[i].actor, GV_VISION)) {
      cast_rays(&guards[i]);
      budget -= RAYS;
    }
  for (int tries = 0; tries < n_guards && budget >= RAYS; tries++) {
    ray_next = (ray_next + 1) % n_guards;
    if (!guardview_shows(guards[ray_next].actor, GV_VISION))
      continue;
    cast_rays(&guards[ray_next]);
    budget -= RAYS;
  }
  for (int i = 0; i < n_guards; i++) {
    guard_t *g = &guards[i];
    g->sees_link = g->on_duty && (in_sight(g, &g->find, &link) || (g->custom && in_sight(g, &g->alert, &link)) ||
                                  too_close(g, &link));
    int status = g->status = status_of(g);
    const guard_t *before = 0;
    for (int k = 0; k < n_was; k++)
      if (was[k].actor == g->actor)
        before = &was[k];
    if (!before || g->logged >= LOG_FRAMES) {
      log_guard(g);
      g->logged = 0;
    } else if (before->status != status) {
      log_printf("guard vision: %s %#lx %s -> %s (verb %u)", g->name, (unsigned long)g->actor,
                 status_names[before->status], status_names[status], (unsigned)g->verb);
    }
  }
}

/* --- drawing ------------------------------------------------------------------------------------- */

static const uint8_t near_colours[] = { AW_GUARD_OFF, AW_GUARD_SEARCH, AW_GUARD_ALERTED, AW_GUARD_CAUGHT };
static const uint8_t far_colours[] = { AW_GUARD_OFF_FAR, AW_GUARD_SEARCH_FAR, AW_GUARD_ALERTED_FAR, AW_GUARD_CAUGHT_FAR };

static struct {
  float (*ends)[2][3];
  uint8_t *colours;
  int n, max;
} out_lines;

static void line(const float a[3], const float b[3], uint8_t colour)
{
  if (out_lines.n >= out_lines.max)
    return;
  for (int i = 0; i < 3; i++) {
    out_lines.ends[out_lines.n][0][i] = a[i];
    out_lines.ends[out_lines.n][1][i] = b[i];
  }
  out_lines.colours[out_lines.n++] = colour;
}

static void apex_of(const guard_t *g, float apex[3])
{
  apex[0] = g->head[0], apex[1] = g->feet[1] + GROUND_LIFT, apex[2] = g->head[2];
}

/* the point r across from the apex, `a` radians round from the front */
static void at(const guard_t *g, float r, float a, float p[3])
{
  float d[2];
  apex_of(g, p);
  turned(g->front, a, d);
  p[0] += d[0] * r, p[2] += d[1] * r;
}

static float clampf(float v, float hi) { return v < hi ? v : hi; }

/* how far sight s goes at slice k of SLICES across its angle (short of the rays' hits), and that slice's angle */
static float sight_edge(const guard_t *g, const sight_t *s, int k, float *a)
{
  float half = (s->angle > 180 ? 180 : s->angle) * DEG;
  *a = -half + 2 * half * k / SLICES;
  return clampf(s->range, reach_at(g, *a));
}

/* the find sight's outline: its sides and its far edge */
static void sight_lines(const guard_t *g, const sight_t *s, uint8_t colour)
{
  if (s->range <= 0 || s->angle <= 0)
    return;
  float apex[3], prev[3], p[3], a;
  apex_of(g, apex);
  for (int k = 0; k <= SLICES; k++) {
    at(g, sight_edge(g, s, k, &a), a, p);
    if ((k == 0 || k == SLICES) && s->angle < 179.5f)
      line(apex, p, colour);
    if (k > 0)
      line(prev, p, colour);
    memcpy(prev, p, sizeof(p));
  }
}

/* the alert sight's further edge, and its sides from the find sight's edge out */
static void band_lines(const guard_t *g, uint8_t colour)
{
  float prev[3], p[3], inner[3], a;
  for (int k = 0; k <= SLICES; k++) {
    float r = sight_edge(g, &g->alert, k, &a);
    at(g, r, a, p);
    if ((k == 0 || k == SLICES) && g->alert.angle < 179.5f) {
      float in = a >= -g->find.angle * DEG && a <= g->find.angle * DEG ? clampf(g->find.range, reach_at(g, a)) : 0;
      at(g, in, a, inner);
      if (r > in)
        line(inner, p, colour);
    }
    if (k > 0)
      line(prev, p, colour);
    memcpy(prev, p, sizeof(p));
  }
}

static void ring_lines(const guard_t *g, uint8_t colour)
{
  float prev[3], p[3];
  for (int k = 0; k <= RING_SLICES; k++) {
    at(g, g->close, 2 * 3.14159265f * k / RING_SLICES, p);
    if (k > 0)
      line(prev, p, colour);
    memcpy(prev, p, sizeof(p));
  }
}

static bool same_text(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

/* the map's Guide rails a guard is on or near */
static const railmap_t *rail_map(void)
{
  static const railmap_t *map;
  static char name[64];
  const char *now = gd_get_str(GD_STR_MAP);
  if (!now || !now[0])
    return 0;
  if (map && same_text(name, now))
    return map;
  snprintf(name, sizeof(name), "%s", now);
  map = 0;
  for (int i = 0; i < data_n_railmaps; i++)
    if (same_text(data_railmaps[i].map, now))
      map = &data_railmaps[i];
  return map;
}

/* a guard's feet within PATH_NEAR across (and a few metres up or down) of the rail's run */
static bool rail_has_guard(const rail_t *rail)
{
  for (int i = 0; i < n_guards; i++) {
    if (!guardview_shows(guards[i].actor, GV_PATH))
      continue;
    const float *f = guards[i].feet;
    int segments = rail->closed ? rail->n : rail->n - 1;
    for (int s = 0; s < segments; s++) {
      const float *a = data_rail_points[rail->first + s].t, *b = data_rail_points[rail->first + (s + 1) % rail->n].t;
      float ux = b[0] - a[0], uz = b[2] - a[2], len2 = ux * ux + uz * uz;
      float t = len2 > 1e-6f ? ((f[0] - a[0]) * ux + (f[2] - a[2]) * uz) / len2 : 0;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      float dx = a[0] + ux * t - f[0], dz = a[2] + uz * t - f[2], dy = a[1] + (b[1] - a[1]) * t - f[1];
      if (dx * dx + dz * dz < PATH_NEAR * PATH_NEAR && dy > -3 && dy < 3)
        return true;
    }
  }
  return false;
}

/* an actor whose own path switch is on walks this rail (the layout links them) */
static bool rail_chosen(int r)
{
  for (int i = 0; i < n_choices; i++) {
    if (!choices[i].shown[GV_PATH] || !choices[i].instance)
      continue;
    for (int k = first_owned(choices[i].instance);
         k < data_n_rail_owners && data_rail_owners[k].instance == choices[i].instance; k++)
      if (data_rail_owners[k].rail == r)
        return true;
  }
  return false;
}

static void path_lines(void)
{
  const railmap_t *map = rail_map();
  if (!map)
    return;
  for (int r = map->first; r < map->first + map->n; r++) {
    const rail_t *rail = &data_rails[r];
    if (!rail_chosen(r) && !rail_has_guard(rail))
      continue;
    for (int i = 0; i < rail->n; i++) {
      const railpoint_t *p = &data_rail_points[rail->first + i];
      float a[3] = { p->t[0], p->t[1] + PATH_LIFT, p->t[2] };
      if (i + 1 < rail->n || rail->closed) {
        const railpoint_t *q = &data_rail_points[rail->first + (i + 1) % rail->n];
        float b[3] = { q->t[0], q->t[1] + PATH_LIFT, q->t[2] };
        line(a, b, AW_GUARD_PATH);
      }
      if (p->wait > 0) { /* where guards stop: a ring, and a tick the way they face */
        float prev[3], c[3];
        for (int k = 0; k <= 12; k++) {
          float ang = 2 * 3.14159265f * k / 12;
          c[0] = a[0] + sinf(ang) * STOP_RING, c[1] = a[1], c[2] = a[2] + cosf(ang) * STOP_RING;
          if (k > 0)
            line(prev, c, AW_GUARD_PATH);
          memcpy(prev, c, sizeof(c));
        }
        float tip[3] = { a[0] + sinf(p->yaw) * STOP_TICK, a[1], a[2] + cosf(p->yaw) * STOP_TICK };
        float from[3] = { a[0] + sinf(p->yaw) * STOP_RING, a[1], a[2] + cosf(p->yaw) * STOP_RING };
        line(from, tip, AW_GUARD_PATH);
      } else { /* a point passed through: a small cross */
        float x0[3] = { a[0] - 0.2f, a[1], a[2] }, x1[3] = { a[0] + 0.2f, a[1], a[2] };
        float z0[3] = { a[0], a[1], a[2] - 0.2f }, z1[3] = { a[0], a[1], a[2] + 0.2f };
        line(x0, x1, AW_GUARD_PATH);
        line(z0, z1, AW_GUARD_PATH);
      }
    }
  }
}

int guardview_lines(float ends[][2][3], uint8_t colours[], int max)
{
  out_lines.ends = ends, out_lines.colours = colours, out_lines.n = 0, out_lines.max = max;
  for (int i = 0; i < n_guards; i++) {
    const guard_t *g = &guards[i];
    if (!guardview_shows(g->actor, GV_VISION))
      continue;
    uint8_t colour = near_colours[g->status];
    if (g->custom && g->alert.range > 0)
      band_lines(g, colour);
    sight_lines(g, &g->find, colour);
    if (g->close > 0)
      ring_lines(g, colour);
  }
  path_lines();
  return out_lines.n;
}

static void triangle(aw_face_t faces[], int *n, int max, const float a[3], const float b[3], const float c[3],
                     uint8_t colour)
{
  if (*n >= max)
    return;
  aw_face_t *f = &faces[(*n)++];
  for (int j = 0; j < 3; j++) {
    f->corner[j] = a[j];
    f->u[j] = b[j] - a[j];
    f->v[j] = c[j] - a[j];
  }
  f->color = colour;
  f->scene = true;
  f->triangle = true;
}

int guardview_faces(aw_face_t faces[], int max)
{
  int n = 0;
  for (int i = 0; i < n_guards; i++) {
    const guard_t *g = &guards[i];
    if (!guardview_shows(g->actor, GV_VISION))
      continue;
    float apex[3], a, b, p[3], q[3];
    apex_of(g, apex);
    if (g->find.range > 0 && g->find.angle > 0) /* the find sight: a fan from the head */
      for (int k = 0; k < SLICES; k++) {
        at(g, sight_edge(g, &g->find, k, &a), a, p);
        at(g, sight_edge(g, &g->find, k + 1, &b), b, q);
        triangle(faces, &n, max, apex, p, q, near_colours[g->status]);
      }
    if (g->custom && g->alert.range > 0 && g->alert.angle > 0) /* the alert sight's band past it, fainter */
      for (int k = 0; k < SLICES; k++) {
        float ra = sight_edge(g, &g->alert, k, &a), rb = sight_edge(g, &g->alert, k + 1, &b);
        float fa = a >= -g->find.angle * DEG && a <= g->find.angle * DEG ? clampf(g->find.range, reach_at(g, a)) : 0;
        float fb = b >= -g->find.angle * DEG && b <= g->find.angle * DEG ? clampf(g->find.range, reach_at(g, b)) : 0;
        if (ra <= fa && rb <= fb)
          continue;
        float ia[3], ib[3];
        at(g, fa < ra ? fa : ra, a, ia);
        at(g, fb < rb ? fb : rb, b, ib);
        at(g, ra, a, p);
        at(g, rb, b, q);
        triangle(faces, &n, max, ia, p, q, far_colours[g->status]);
        triangle(faces, &n, max, ia, q, ib, far_colours[g->status]);
      }
    if (g->close > 0) /* the ring: closer than this, any way */
      for (int k = 0; k < RING_SLICES; k++) {
        at(g, g->close, 2 * 3.14159265f * k / RING_SLICES, p);
        at(g, g->close, 2 * 3.14159265f * (k + 1) / RING_SLICES, q);
        triangle(faces, &n, max, apex, p, q, near_colours[g->status]);
      }
  }
  return n;
}

void guardview_text(char *buf, int len)
{
  if (!known()) {
    snprintf(buf, len, TF("not known for this game version"));
    return;
  }
  if (!guardview_shown) {
    snprintf(buf, len, TF("off"));
    return;
  }
  int on = 0, alerted = 0, caught = 0;
  for (int i = 0; i < n_guards; i++) {
    on += guards[i].on_duty;
    alerted += guards[i].status == STATUS_ALERTED;
    caught += guards[i].status == STATUS_CAUGHT;
  }
  if (!n_guards)
    snprintf(buf, len, TF("no guards near"));
  else if (caught)
    snprintf(buf, len, TF("%d guards, %d on duty   CAUGHT by %d"), n_guards, on, caught);
  else if (alerted)
    snprintf(buf, len, TF("%d guards, %d on duty   %d alerted"), n_guards, on, alerted);
  else
    snprintf(buf, len, TF("%d guards, %d on duty"), n_guards, on);
}
