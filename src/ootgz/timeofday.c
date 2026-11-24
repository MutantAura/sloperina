/* scene > time of day (timeofday.h, FINDINGS 9.17).
 *
 * The clock is the WorldMgr's: wm = [[main+wm_ptr]], [wm+0x578] the clock object, whose vtable slot +0x30 returns the
 * seconds into the day; WorldMgr::setTime (wm_set_time, what the game's ExecuteWmSetTime calls) sets it, and the
 * lighting and the sky follow. Frozen, the clock is set back to the frozen time every few frames. */
#include "timeofday.h"

#include <stdio.h>

#include "gfx.h"
#include "sys.h"
#include "z64hd.h"

#define WM_CLOCK 0x578u
#define CLOCK_GET_TIME 0x30u
#define DAY_SECONDS 86400u
#define MAIN_CODE_SIZE 0x3C80000u
#define FREEZE_EVERY 10 /* frames between settings of a frozen clock */
#define DAY_FROM (6 * 3600u)   /* 06:00 to 18:00 is day (the game's own day and night) */
#define NIGHT_FROM (18 * 3600u)

typedef uint32_t (*clock_get_t)(uintptr_t clock);
typedef void (*wm_set_time_t)(uintptr_t wm, uint32_t seconds);

bool tod_frozen;
static uint32_t frozen_at;
static uint32_t frames;

static uintptr_t world_manager(void)
{
  if (!z64hd_game || !z64hd_game->wm_ptr || !z64hd_game->wm_set_time)
    return 0;
  uintptr_t holder = sys_ptr(MAIN(z64hd_game->wm_ptr));
  uintptr_t wm = holder ? sys_ptr(holder) : 0;
  return wm && sys_mapped(wm + WM_CLOCK, 8, SYS_PERM_R) ? wm : 0;
}

bool tod_get(uint32_t *seconds)
{
  uintptr_t wm = world_manager();
  uintptr_t clock = wm ? sys_ptr(wm + WM_CLOCK) : 0;
  uintptr_t vtable = clock && sys_mapped(clock, 8, SYS_PERM_R) ? sys_ptr(clock) : 0;
  uintptr_t fn = vtable && sys_mapped(vtable + CLOCK_GET_TIME, 8, SYS_PERM_R) ? sys_ptr(vtable + CLOCK_GET_TIME) : 0;
  if (!fn || fn < z64hd_main || fn - z64hd_main >= MAIN_CODE_SIZE)
    return false;
  int32_t t = (int32_t)((clock_get_t)fn)(clock) % (int32_t)DAY_SECONDS;
  *seconds = (uint32_t)(t < 0 ? t + (int32_t)DAY_SECONDS : t);
  return true;
}

bool tod_set(uint32_t seconds)
{
  uintptr_t wm = world_manager();
  if (!wm)
    return false;
  seconds %= DAY_SECONDS;
  MAIN_FN(wm_set_time_t, z64hd_game->wm_set_time)(wm, seconds);
  if (tod_frozen)
    frozen_at = seconds;
  return true;
}

void tod_frame(void)
{
  if (!tod_frozen || ++frames % FREEZE_EVERY)
    return;
  uint32_t now;
  if (tod_get(&now) && now != frozen_at)
    tod_set(frozen_at);
}

/* --- the menu ------------------------------------------------------------------------------------ */

static menu_t m_tod;

static void text_now(menu_item_t *it, char *buf, int len)
{
  uint32_t t;
  if (!tod_get(&t))
    snprintf(buf, len, "no clock now");
  else
    snprintf(buf, len, "now %02u:%02u  (%s)%s", (unsigned)(t / 3600), (unsigned)(t / 60 % 60),
             t >= DAY_FROM && t < NIGHT_FROM ? "day" : "night", tod_frozen ? "  frozen" : "");
}

/* hour (data 0) or minute (data 1) of the clock */
static int32_t get_part(menu_item_t *it)
{
  uint32_t t;
  if (!tod_get(&t))
    return NUM_UNKNOWN;
  return it->data == 0 ? (int32_t)(t / 3600) : (int32_t)(t / 60 % 60);
}

static void set_part(menu_item_t *it, int32_t v)
{
  uint32_t t;
  if (!tod_get(&t)) {
    gfx_notice("time of day: no clock now");
    return;
  }
  uint32_t hour = t / 3600, minute = t / 60 % 60;
  if (it->data == 0)
    hour = (uint32_t)v % 24;
  else
    minute = (uint32_t)v % 60;
  tod_set(hour * 3600 + minute * 60);
}

static int32_t max_part(menu_item_t *it) { return it->data == 0 ? 23 : 59; }

static void act_preset(menu_item_t *it)
{
  if (!tod_set((uint32_t)it->data))
    gfx_notice("time of day: no clock now");
}

static bool get_frozen(menu_item_t *it) { return tod_frozen; }
static void set_frozen(menu_item_t *it, bool on)
{
  uint32_t t;
  if (on && !tod_get(&t)) {
    gfx_notice("time of day: no clock now");
    return;
  }
  if (on)
    frozen_at = t;
  tod_frozen = on;
}

menu_t *tod_menu_build(void)
{
  menu_init(&m_tod, "time of day", 11);
  menu_add_text(&m_tod, text_now, 0);
  menu_item_t *item = menu_add_number(&m_tod, "hour", get_part, set_part, max_part, 0, 0);
  if (item)
    item->hint = "d-left/right: the clock's hour (0 to 23); with L held, by 10";
  item = menu_add_number(&m_tod, "minute", get_part, set_part, max_part, 0, 1);
  if (item)
    item->hint = "d-left/right: the clock's minute; with L held, by 10";
  item = menu_add_check(&m_tod, "freeze time", get_frozen, set_frozen, 0);
  if (item) {
    item->hint = "hold the clock at the time it shows (setting it moves the held time)";
    item->log_label = "time frozen:";
  }
  menu_add_heading(&m_tod, "set to");
  static const struct { const char *label; uint32_t at; } presets[] = {
    { "dawn  06:00", 6 * 3600 }, { "noon  12:00", 12 * 3600 }, { "dusk  18:00", 18 * 3600 }, { "midnight  00:00", 0 },
  };
  for (unsigned i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
    item = menu_add_action(&m_tod, presets[i].label, act_preset, (intptr_t)presets[i].at);
    if (item)
      item->hint = "L: set the clock to this time";
  }
  menu_end_group(&m_tod);
  menu_add_return(&m_tod);
  return &m_tod;
}
