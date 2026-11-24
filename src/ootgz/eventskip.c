/* End the running event the way the game's own SeqExecuteEventForceSkipAndWaitFinish node does (FINDINGS 9.10).
 *
 * The event manager M = [[main+event_mgr_ptr]]; E = [M+0x68] is the running event (0: none). E holds its flows:
 * an array of pointers at E+flows, their count (s32) at E+flows-8, the current index (u32) at E+flows+0x60. A flow's
 * name is a char * at flow+0x8. The skip is a flow's player P = [flow+0x220], vtable slot 11 (main+0x3B3AF30),
 * which only takes it while P is playing (slot 8) and the flow is playing a timeline (flow+0x3B8 bit 25); it then
 * sets P+0x28 bit 5. The node skips the current flow only; this skips every flow playing a timeline. The node
 * repeats it every 180 frames while an event runs or M+0xB8 bit 14 is set, and finishes once neither holds.
 *
 * An event with no timeline (a flowchart: camera moves, text boxes) is ended the way the manager's own update
 * (1.0.0 main+0x3AEE85C) cancels one when M+0xB8 bit 2 or 3 asks it to: the current flow's flags get bit 11
 * (main+0x3ADD5B0) and E+0xF24 is set to 1 (end requested). */
#include "eventskip.h"

#include <stdint.h>

#include "binds.h"
#include "gfx.h"
#include "sys.h"
#include "z64hd.h"

#define MGR_EVENT 0x68          /* M+: the running event, 0 when none runs */
#define MGR_FLAGS 0xB8          /* M+: u32; bit 14 keeps the node waiting */
#define MGR_BUSY (1u << 14)
#define FLOW_COUNT (-8)         /* E+flows+: s32 count */
#define FLOW_INDEX 0x60         /* E+flows+: u32 current index */
#define FLOW_NAME 0x8           /* flow+: char * */
#define FLOW_PLAYER 0x220       /* flow+: the player whose slot 11 skips */
#define FLOW_FLAGS 0x3B8        /* flow+: u64 */
#define FLOW_TIMELINE (1ull << 25) /* playing a timeline: the skip applies */
#define FLOW_STOP (1ull << 11)     /* set by the manager's cancel */
#define EVENT_END_REQUESTED 0xF24  /* E+: u8, 1 = end requested */
#define PLAYER_REQUESTS 0x28    /* P+: u32; bit 5 = skip requested */
#define PLAYER_SKIP_REQUESTED (1u << 5)
#define PLAYER_STATE 0x2C       /* P+: u32; 3 = playing (the shared slot 8) */
#define PLAYER_SKIP_SLOT 11
#define MAX_FLOWS 64
#define NAME_MAX 64
#define REPEAT_FRAMES 180       /* the node's interval between skips */
#define GIVE_UP_FRAMES (REPEAT_FRAMES * 10)

typedef void (*skip_fn)(uintptr_t player);

typedef enum { SKIP_NONE, SKIP_NO_TIMELINE, SKIP_REFUSED, SKIP_TAKEN } skip_result_t;

static bool waiting;
static bool repeat_skip; /* while waiting: skip again every REPEAT_FRAMES (a timeline was skipped) */
static int frames_waited;

static uintptr_t next(uintptr_t p, intptr_t off)
{
  return p && sys_mapped(p + off, 8, SYS_PERM_R) ? sys_ptr(p + off) : 0;
}

bool eventskip_known(void)
{
  return z64hd_game && z64hd_game->event_mgr_ptr && z64hd_game->event_flows;
}

static uintptr_t manager(void)
{
  return eventskip_known() ? next(next(MAIN(z64hd_game->event_mgr_ptr), 0), 0) : 0;
}

bool eventskip_running(void)
{
  return next(manager(), MGR_EVENT) != 0;
}

static bool manager_busy(uintptr_t m)
{
  uint32_t flags;
  return m && sys_read32(m + MGR_FLAGS, &flags) && (flags & MGR_BUSY);
}

/* the flow's name, or "?" */
static void flow_name(uintptr_t flow, char *out)
{
  uintptr_t s = next(flow, FLOW_NAME);
  int n = 0;
  if (s && sys_mapped(s, NAME_MAX, SYS_PERM_R))
    for (; n < NAME_MAX - 1 && ((const volatile char *)s)[n]; n++)
      out[n] = ((const volatile char *)s)[n];
  if (!n)
    out[n++] = '?';
  out[n] = 0;
}

bool eventskip_flow_name(char out[EVENTSKIP_NAME_MAX])
{
  _Static_assert(EVENTSKIP_NAME_MAX == NAME_MAX, "eventskip.h's name size");
  out[0] = 0;
  uintptr_t event = next(manager(), MGR_EVENT);
  if (!event)
    return false;
  uintptr_t flows_at = event + z64hd_game->event_flows;
  uintptr_t flows = next(event, (intptr_t)z64hd_game->event_flows);
  uint32_t count, current;
  if (!flows || !sys_read32(flows_at + FLOW_COUNT, &count) || !sys_read32(flows_at + FLOW_INDEX, &current))
    return false;
  if ((int32_t)count < 1 || count > MAX_FLOWS || current >= count || !sys_mapped(flows + current * 8, 8, SYS_PERM_R))
    return false;
  uintptr_t flow = sys_ptr(flows + current * 8);
  if (!flow || !next(flow, FLOW_NAME))
    return false;
  flow_name(flow, out);
  return true;
}

static skip_result_t skip_flow(uintptr_t flow, uint32_t i, uint32_t count, uint32_t current, bool log)
{
  uintptr_t player = next(flow, FLOW_PLAYER);
  uintptr_t vtable = next(player, 0);
  uintptr_t fn = next(vtable, PLAYER_SKIP_SLOT * 8);
  uint32_t lo, hi, state = 0, requests = 0;
  if (!fn || !sys_read32(flow + FLOW_FLAGS, &lo) || !sys_read32(flow + FLOW_FLAGS + 4, &hi) ||
      !sys_read32(player + PLAYER_STATE, &state) || !sys_read32(player + PLAYER_REQUESTS, &requests))
    return SKIP_NONE;
  uint64_t flags = (uint64_t)hi << 32 | lo;
  bool timeline = (flags & FLOW_TIMELINE) != 0;
  if (timeline) {
    ((skip_fn)fn)(player);
    sys_read32(player + PLAYER_REQUESTS, &requests);
  }
  if (log) {
    char name[NAME_MAX];
    flow_name(flow, name);
    log_printf("eventskip: flow %u/%u%s %s: flags %llx, player state %u, requests %x, player vtable main+%lx",
               (unsigned)i + 1, (unsigned)count, i == current ? " (current)" : "", name, (unsigned long long)flags,
               (unsigned)state, (unsigned)requests, (unsigned long)(vtable - z64hd_main));
  }
  if (!timeline)
    return SKIP_NO_TIMELINE;
  return (requests & PLAYER_SKIP_REQUESTED) ? SKIP_TAKEN : SKIP_REFUSED;
}

/* every flow of the running event: the best result among them */
static skip_result_t skip_all(bool log)
{
  uintptr_t event = next(manager(), MGR_EVENT);
  if (!event)
    return SKIP_NONE;
  uintptr_t flows_at = event + z64hd_game->event_flows;
  uintptr_t flows = next(event, (intptr_t)z64hd_game->event_flows);
  uint32_t count, current;
  if (!flows || !sys_read32(flows_at + FLOW_COUNT, &count) || !sys_read32(flows_at + FLOW_INDEX, &current))
    return SKIP_NONE;
  if ((int32_t)count < 1 || count > MAX_FLOWS || !sys_mapped(flows, count * 8, SYS_PERM_R))
    return SKIP_NONE;
  skip_result_t best = SKIP_NONE;
  for (uint32_t i = 0; i < count; i++) {
    uintptr_t flow = sys_ptr(flows + i * 8);
    if (!flow || !sys_mapped(flow, FLOW_FLAGS + 8, SYS_PERM_R))
      continue;
    skip_result_t r = skip_flow(flow, i, count, current, log);
    if (r > best)
      best = r;
  }
  return best;
}

/* the manager's cancel of the running event: bit 11 on its current flow, E+0xF24 = 1 */
static bool end_event(void)
{
  uintptr_t event = next(manager(), MGR_EVENT);
  if (!event || !sys_mapped(event + EVENT_END_REQUESTED, 1, SYS_PERM_R | SYS_PERM_W))
    return false;
  uintptr_t flows_at = event + z64hd_game->event_flows;
  uintptr_t flows = next(event, (intptr_t)z64hd_game->event_flows);
  uint32_t count, current;
  if (!flows || !sys_read32(flows_at + FLOW_COUNT, &count) || !sys_read32(flows_at + FLOW_INDEX, &current))
    return false;
  if ((int32_t)count < 1 || current >= count || !sys_mapped(flows + current * 8, 8, SYS_PERM_R))
    return false;
  uintptr_t flow = sys_ptr(flows + current * 8);
  if (!flow || !sys_mapped(flow + FLOW_FLAGS, 8, SYS_PERM_R | SYS_PERM_W))
    return false;
  *(volatile uint64_t *)(flow + FLOW_FLAGS) |= FLOW_STOP;
  *(volatile uint8_t *)(event + EVENT_END_REQUESTED) = 1;
  return true;
}

bool eventskip_start(void)
{
  if (!eventskip_known()) {
    gfx_notice("end cutscene: not known for this game version");
    return false;
  }
  if (!eventskip_running()) {
    gfx_notice("no cutscene is playing");
    return false;
  }
  skip_result_t result = skip_all(true);
  frames_waited = 0;
  repeat_skip = result == SKIP_TAKEN;
  if (result == SKIP_TAKEN) {
    gfx_logf("cutscene skipped");
  } else if (result == SKIP_NO_TIMELINE) {
    if (!end_event()) {
      gfx_logf("event not found");
      return false;
    }
    gfx_logf("cutscene ended");
  } else {
    gfx_logf(result == SKIP_REFUSED ? "the game refused the skip" : "event not found");
    return false;
  }
  waiting = true;
  return true;
}

bool eventskip_end_now(void)
{
  if (!eventskip_known() || !eventskip_running())
    return false;
  return end_event() || skip_all(false) == SKIP_TAKEN;
}

void eventskip_frame(void)
{
  if (cmd_pressed(CMD_END_CUTSCENE)) /* unbound unless given buttons (settings > rebind commands) */
    eventskip_start();
  if (!waiting)
    return;
  uintptr_t m = manager();
  if (!m || (!eventskip_running() && !manager_busy(m))) {
    waiting = false;
    log_printf("eventskip: the event was gone after %d frames", frames_waited);
    return;
  }
  if (++frames_waited >= GIVE_UP_FRAMES) {
    waiting = false;
    gfx_logf("cutscene still playing, stopped skipping");
    return;
  }
  if (repeat_skip && frames_waited % REPEAT_FRAMES == 0)
    skip_all(false);
}
