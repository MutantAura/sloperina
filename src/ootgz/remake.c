/* Making Link again (beta): see remake.h, FINDINGS 9.34. */
#include "remake.h"

#include <stdint.h>

#include "age.h"
#include "gamedata.h"
#include "gfx.h"
#include "levitate.h"
#include "link.h"
#include "sys.h"
#include "warp.h"

static bool wanted;
static int step;             /* 0: none under way; 1: loading at the other age; 2: loading back */
static int back_age;         /* the age to come back to */
static bool lost;            /* Link's physics record was lost since the step's load began (the map is changing) */
static uint32_t found_frame; /* the frame it was found again; 0: not yet */
static int last_age = -1;
static char tunic[GD_STR_LEN], boots[GD_STR_LEN];

void remake_request(bool on)
{
  wanted = on;
}

bool remake_pending(void)
{
  return wanted || step;
}

void remake_reload_starting(const char *map, int age)
{
  if (!wanted || step || age_wanted() != age)
    return; /* not asked for, under way, or the reload changes his age anyway */
  int other = age == AGE_CHILD ? AGE_ADULT : AGE_CHILD;
  if (!age_map_has_version(map, other)) {
    gfx_notice(other == AGE_CHILD ? "can't make link again here (no child version): change age instead"
                                  : "can't make link again here (no adult version): change age instead");
    return;
  }
  back_age = age;
  snprintf(tunic, sizeof(tunic), "%s", gd_get_str(GD_STR_TUNIC) ? gd_get_str(GD_STR_TUNIC) : "");
  snprintf(boots, sizeof(boots), "%s", gd_get_str(GD_STR_BOOTS) ? gd_get_str(GD_STR_BOOTS) : "");
  step = 1;
  lost = false;
  found_frame = 0;
  age_want(other);
  log_printf("reload: making link again, through %s", other == AGE_CHILD ? "child" : "adult");
}

/* Each step's map has loaded once Link's record has been lost and found again and no event runs any more (the
 * arrival's: a warp asked for during it is refused and falls back to the next exit) */
void remake_frame(void)
{
  int age = age_current();
  if (!step) {
    if (wanted && age >= 0 && last_age >= 0 && age != last_age)
      wanted = false; /* the player's own age change made him again */
    last_age = age;
    return;
  }
  last_age = -1;
  if (warp_abandoned()) {
    step = 0; /* age_frame puts the age back */
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
    found_frame = frame_counter();
  if (frame_counter() - found_frame < 30)
    return;
  if (step == 1) {
    step = 2;
    lost = false;
    found_frame = 0;
    age_want(back_age);
    age_reload_map();
    return;
  }
  gd_set_str(GD_STR_TUNIC, tunic); /* the child's load took the adult's clothes off */
  gd_set_str(GD_STR_BOOTS, boots);
  step = 0;
  wanted = false;
  gfx_notice("link made again");
}
