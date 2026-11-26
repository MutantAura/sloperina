/* Save / load position (gz).
 *
 * Saving reads Link's drawn position, his facing and the map. Moving him back goes through his
 * physics record (levitate.c), which resolves directly from his controller.
 *
 * A load waits until R and the d-pad of the shortcut are let go: holding R brings up the game's item
 * wheel, which freezes gameplay, and a frozen Link ignores a new position (every load made while they
 * were held failed). "loading position" shows at once, as "position saved" does (saving only reads, so
 * it doesn't wait), and "position loaded" when he's there. It's then checked: done once
 * Link is drawn there; otherwise written again a few times, and if it never takes, the record is put
 * back where Link is. While it settles, the game's stores
 * can't move X, Y or Z (lock.c), so the game can't pull him off the write. */
#include "position.h"

#include "gamedata.h"
#include "gfx.h"
#include "binds.h"
#include "input.h"
#include "levitate.h"
#include "link.h"
#include "lock.h"
#include "sys.h"

#define SETTLE 20       /* frames for a load to show: checked each frame, then given up */
#define MAX_WRITES 4    /* written again every RETRY_EVERY frames until Link is there */
#define RETRY_EVERY 3
#define NEAR 1.0        /* units per axis: Link counts as there */

static bool saved;
static double sx, sy, sz, sfx, sfz; /* where a load puts Link: drawn position; facing as a unit (x, z), 0 0: as he is */
static char smap[64];
/* the saved position (save / set): a load copies it into the above, which position_goto uses for its own */
static struct {
  double x, y, z, fx, fz;
  char map[64];
} slot;

static void keep_slot(void)
{
  slot.x = sx, slot.y = sy, slot.z = sz, slot.fx = sfx, slot.fz = sfz;
  snprintf(slot.map, sizeof(slot.map), "%s", smap);
}

static void use_slot(void)
{
  sx = slot.x, sy = slot.y, sz = slot.z, sfx = slot.fx, sfz = slot.fz;
  snprintf(smap, sizeof(smap), "%s", slot.map);
}

static bool pending;  /* load pressed: waiting for the buttons to be let go */
static bool quiet;    /* this load says nothing on screen (position_goto_quiet) */
static int settle;    /* frames left to see Link arrive */
static int writes;

static double absd(double v) { return v < 0 ? -v : v; }

static bool same_str(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static bool in_saved_map(void)
{
  const char *m = gd_get_str(GD_STR_MAP);
  return !m || !smap[0] || same_str(m, smap);
}

void position_save(void)
{
  vec3f_t p;
  if (!link_drawn_pos(&p)) {
    gfx_notice("save position: Link not found");
    return;
  }
  sx = p.x;
  sy = p.y;
  sz = p.z;
  sfx = sfz = 0;
  double fx, fz;
  if (link_facing(&fx, &fz)) {
    sfx = fx;
    sfz = fz;
  }
  const char *m = gd_get_str(GD_STR_MAP);
  snprintf(smap, sizeof(smap), "%s", m ? m : "");
  saved = true;
  keep_slot();
  log_printf("position saved: %.3f %.3f %.3f facing %.3f,%.3f in %s", sx, sy, sz, sfx, sfz, smap);
  gfx_notice("position saved");
}

float sinf(float x), cosf(float x); /* from sdk */

void position_set(double x, double y, double z, double facing_degrees)
{
  double a = facing_degrees * (3.14159265358979 / 180.0);
  sx = x;
  sy = y;
  sz = z;
  sfx = sinf((float)a);
  sfz = cosf((float)a);
  const char *m = gd_get_str(GD_STR_MAP);
  snprintf(smap, sizeof(smap), "%s", m ? m : "");
  saved = true;
  keep_slot();
  log_printf("position set: %.3f %.3f %.3f facing %.1f in %s", sx, sy, sz, facing_degrees, smap);
}

bool position_busy(void) { return pending || settle; }

bool position_goto_quiet(double x, double y, double z)
{
  if (pending || settle)
    return false;
  bool ok = position_goto(x, y, z);
  quiet = ok;
  return ok;
}

bool position_goto(double x, double y, double z)
{
  if (pending || settle) {
    gfx_notice("a position load is still under way");
    return false;
  }
  quiet = false;
  if (link_record_status() != REC_READY) {
    gfx_notice("go to: Link not found");
    return false;
  }
  sx = x;
  sy = y;
  sz = z;
  sfx = sfz = 0; /* he keeps facing as he does */
  const char *m = gd_get_str(GD_STR_MAP);
  snprintf(smap, sizeof(smap), "%s", m ? m : "");
  pending = true;
  log_printf("go to: %.3f %.3f %.3f in %s", sx, sy, sz, smap);
  return true;
}

void position_load(void)
{
  if (!saved) {
    gfx_notice("No position saved yet");
    return;
  }
  use_slot();
  if (!in_saved_map()) {
    gfx_notice("the saved position is in another map");
    return;
  }
  if (link_record_status() != REC_READY) {
    gfx_notice("load position: Link not found");
    return;
  }
  pending = true;
  gfx_notice("loading position"); /* now, while the wheel's up; "position loaded" once he's there */
}

static bool write_saved(void)
{
  writes++;
  return link_record_move(sx, sy, sz, 0, 0, SETTLE + 2);
}

static bool link_there(void)
{
  vec3f_t d;
  return link_drawn_pos(&d) && absd(d.x - sx) < NEAR && absd(d.y - sy) < NEAR && absd(d.z - sz) < NEAR;
}

float atan2f(float y, float x); /* from sdk */

/* Link is on the spot: turn him the rest of the way to the saved facing, in one step */
static void face_saved(void)
{
  double fx, fz;
  if ((sfx == 0 && sfz == 0) || !link_facing(&fx, &fz))
    return;
  double turn = atan2f((float)sfx, (float)sfz) - atan2f((float)fx, (float)fz);
  const double pi = 3.14159265358979;
  if (turn > pi)
    turn -= 2 * pi;
  else if (turn < -pi)
    turn += 2 * pi;
  link_record_turn(turn);
}

/* the load is over one way or another: the game's stores can move Link again */
static void settle_done(void)
{
  settle = 0;
  lock_hold_set(false);
}

/* each frame after a load's first write */
static void settle_step(void)
{
  if (!in_saved_map() || link_record_status() != REC_READY) {
    settle_done(); /* another map now (the record is gone): leave it alone */
    log_printf("position load abandoned (map changed)");
    return;
  }
  if (link_there()) {
    face_saved();
    log_printf("position loaded (%d write%s)", writes, writes == 1 ? "" : "s");
    if (!quiet)
      gfx_notice("position loaded");
    settle_done();
    return;
  }
  if (--settle == 0) {
    lock_hold_set(false);
    link_record_resync();
    log_printf("position load didn't take after %d writes", writes);
    if (!quiet)
      gfx_notice("load position didn't work, try again");
    return;
  }
  if (writes < MAX_WRITES && (SETTLE - settle) % RETRY_EVERY == 0)
    write_saved();
}

void position_frame(bool menu_open)
{
  if (!menu_open && cmd_pressed(CMD_SAVEPOS)) /* R + d-up unless changed (settings > rebind commands) */
    position_save();
  else if (!menu_open && cmd_pressed(CMD_LOADPOS)) /* R + d-down */
    position_load();

  if (pending && link_record_status() == REC_READY && !(input_held() & cmd_mask(CMD_LOADPOS))) {
    pending = false;
    writes = 0;
    lock_hold_set(true); /* the write holds: the game's stores can't pull him off it while it settles */
    if (write_saved()) {
      settle = SETTLE;
      log_printf("position load: writing %.3f %.3f %.3f", sx, sy, sz);
    } else {
      lock_hold_set(false);
    }
  } else if (settle) {
    settle_step();
  }
}
