/* Making Link again (beta): see remake.h, FINDINGS 9.35. */
#include "remake.h"

#include <stdint.h>

#include "age.h"
#include "gamedata.h"
#include "gamepatch.h"
#include "gfx.h"
#include "levitate.h"
#include "link.h"
#include "position.h"
#include "sys.h"
#include "warp.h"

#define START_MAX 1800  /* frames to wait for the search and a running event before giving up */
#define SETTLE 30       /* frames a loaded map is left alone before the next step */
#define PUT_BACK_MAX 120 /* frames to get Link back where he stood */
#define PUT_BACK_RETRY 20
#define PUT_BACK_NEAR 0.3f /* metres (across) that count as there */

enum { IDLE, STARTING, THROUGH_OTHER, TO_TARGET, PUT_BACK };
static int step;
static int target;           /* the age to end at */
static uint32_t step_frame;  /* when the step began */
static bool lost;            /* Link's physics record was lost since the step's load began (the map is changing) */
static uint32_t found_frame; /* the frame it was found again; 0: not yet */
static bool restore_clothes; /* he ends at the age he started at: the child's load took the adult's clothes off */
static char tunic[GD_STR_LEN], boots[GD_STR_LEN];
static bool have_spot;
static vec3f_t spot;

bool remake_pending(void)
{
  return step != IDLE;
}

bool remake_start(int age)
{
  if (step != IDLE) {
    gfx_notice("link is being made again: wait for it");
    return false;
  }
  if (!link_actor() || age_current() < 0) {
    gfx_notice("can't make link again: no link here");
    return false;
  }
  target = age == AGE_CHILD ? AGE_CHILD : AGE_ADULT;
  restore_clothes = age_current() == target;
  snprintf(tunic, sizeof(tunic), "%s", gd_get_str(GD_STR_TUNIC) ? gd_get_str(GD_STR_TUNIC) : "");
  snprintf(boots, sizeof(boots), "%s", gd_get_str(GD_STR_BOOTS) ? gd_get_str(GD_STR_BOOTS) : "");
  have_spot = link_drawn_pos(&spot);
  step = STARTING;
  step_frame = frame_counter();
  return true;
}

static void load(int age, int next_step)
{
  if (!age_reload_map_as(age)) {
    step = IDLE; /* age_reload_map_as said why */
    return;
  }
  step = next_step;
  step_frame = frame_counter();
  lost = false;
  found_frame = 0;
}

static void done(void)
{
  step = IDLE;
  gfx_notice("link made again");
}

/* A step's map has loaded once Link's record has been lost and found again and no event runs any more (the
 * arrival's: a warp asked for during it is refused and falls back to the next exit) */
void remake_frame(void)
{
  if (step == IDLE)
    return;
  uint32_t now = frame_counter();
  if (step == STARTING) {
    int age = age_current();
    if (gamepatch_searching() || warp_pending() || warp_event_running() || link_record_status() != REC_READY ||
        age < 0) {
      if (now - step_frame > START_MAX) {
        step = IDLE;
        gfx_notice("link wasn't made again: the game stayed busy");
      }
      return;
    }
    if (age != target) {
      log_printf("remake: one load, to %s", target == AGE_CHILD ? "child" : "adult");
      load(target, TO_TARGET);
    } else {
      int other = target == AGE_CHILD ? AGE_ADULT : AGE_CHILD;
      log_printf("remake: two loads, through %s", other == AGE_CHILD ? "child" : "adult");
      load(other, THROUGH_OTHER);
    }
    return;
  }
  if (step == PUT_BACK) {
    vec3f_t at;
    if (link_drawn_pos(&at)) {
      float dx = at.x - spot.x, dz = at.z - spot.z;
      if (dx * dx + dz * dz <= PUT_BACK_NEAR * PUT_BACK_NEAR && !position_busy()) {
        done();
        return;
      }
    }
    if (now - step_frame >= PUT_BACK_MAX) {
      done(); /* he's made again all the same */
      return;
    }
    if ((now - step_frame) % PUT_BACK_RETRY == 0 && link_record_status() == REC_READY)
      position_goto_quiet(spot.x, spot.y, spot.z);
    return;
  }
  if (warp_abandoned()) {
    step = IDLE; /* age_frame puts the age back */
    gfx_notice("link wasn't made again: the map didn't reload");
    return;
  }
  if (warp_pending())
    return;
  if (link_record_status() != REC_READY) {
    lost = true;
    found_frame = 0;
    return;
  }
  if (!lost)
    return;
  if (warp_event_running()) {
    found_frame = 0;
    return;
  }
  if (!found_frame)
    found_frame = now;
  if (now - found_frame < SETTLE)
    return;
  if (step == THROUGH_OTHER) {
    load(target, TO_TARGET);
    return;
  }
  if (restore_clothes) {
    gd_set_str(GD_STR_TUNIC, tunic);
    gd_set_str(GD_STR_BOOTS, boots);
  }
  if (!have_spot) {
    done();
    return;
  }
  step = PUT_BACK;
  step_frame = now;
}
