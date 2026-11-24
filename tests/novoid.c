/* Host tests for novoid.c and lock.c: LOCK_BIT_NO_VOID toggling, preservation across axis lock
 * changes and map changes, clearing pending PlayerComponent void state, and unavailable builds. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gamedata.h"
#include "gfx.h"
#include "link.h"
#include "lock.h"
#include "novoid.h"
#include "sys.h"
#include "z64hd.h"

uintptr_t z64hd_main;
const struct z64hd_build *z64hd_game;

static struct z64hd_pair fake_pair = { .off = 0x100 };
static struct z64hd_build fake_build;
static uint8_t fake_main[0x1000];
static const char *current_map = "KokiriForest_Child";
static int notices;

const char *gd_get_str(uint32_t hash)
{
  (void)hash;
  return current_map;
}

void gfx_notice(const char *text)
{
  (void)text;
  notices++;
}

static uint8_t fake_actor[0x300];
static uint8_t fake_parts[0x400];
static uint8_t fake_player[0x2000];
static bool link_present;

uintptr_t link_actor(void) { return link_present ? (uintptr_t)fake_actor : 0; }

bool sys_mapped(uintptr_t addr, size_t size, uint32_t perm)
{
  (void)perm;
  if (!link_present)
    return false;
  uintptr_t a = (uintptr_t)fake_actor, p = (uintptr_t)fake_parts, pl = (uintptr_t)fake_player;
  if (addr >= a && addr + size <= a + sizeof(fake_actor))
    return true;
  if (addr >= p && addr + size <= p + sizeof(fake_parts))
    return true;
  if (addr >= pl && addr + size <= pl + sizeof(fake_player))
    return true;
  return false;
}

uintptr_t sys_ptr(uintptr_t addr)
{
  uintptr_t v = 0;
  memcpy(&v, (const void *)addr, sizeof(v));
  return v;
}

static uint64_t slot_val(void)
{
  return *(const volatile uint64_t *)(fake_main + fake_build.lock_slot);
}

int main(void)
{
  /* 1. Unavailable build */
  memset(&fake_build, 0, sizeof(fake_build));
  z64hd_main = (uintptr_t)fake_main;
  z64hd_game = &fake_build;
  assert(!novoid_available());
  novoid_set(true);
  assert(!novoid_enabled());

  /* 2. Available build: wire fake actor -> parts -> player */
  memset(fake_main, 0, sizeof(fake_main));
  fake_build.pair = &fake_pair;
  fake_build.lock_slot = 0x200;
  fake_build.void_check_abyss = 0x82C510;
  assert(novoid_available());
  assert(!novoid_enabled());
  assert(slot_val() == 0);

  uintptr_t parts_ptr = (uintptr_t)fake_parts;
  uintptr_t player_ptr = (uintptr_t)fake_player;
  memcpy(fake_actor + 0x270, &parts_ptr, sizeof(parts_ptr));
  memcpy(fake_parts + 0x358, &player_ptr, sizeof(player_ptr));
  link_present = true;

  /* Put pending abyss message bits (2..6), event-abyss bit (31), unrelated bits, and AbyssType=7 */
  uint32_t msg_flags = 0x8000007Cu | 0x12340083u;
  uint32_t abyss_type = 7;
  memcpy(fake_player + 0x28, &msg_flags, 4);
  memcpy(fake_player + 0x191C, &abyss_type, 4);

  /* Enable ignore voids */
  novoid_set(true);
  assert(novoid_enabled());
  assert(slot_val() == (1ull << LOCK_BIT_NO_VOID));

  /* Pending void bits cleared, unrelated bits preserved, AbyssType zeroed */
  memcpy(&msg_flags, fake_player + 0x28, 4);
  memcpy(&abyss_type, fake_player + 0x191C, 4);
  assert(msg_flags == 0x12340083u);
  assert(abyss_type == 0);

  /* 3. Axis lock toggling and map-change lock release must preserve LOCK_BIT_NO_VOID */
  lock_set(LOCK_X, true);
  assert(slot_val() == ((1ull << LOCK_BIT_X) | (1ull << LOCK_BIT_COLLISION) | (1ull << LOCK_BIT_NO_VOID)));
  lock_hold_set(true);
  assert(slot_val() == ((1ull << LOCK_BIT_X) | (1ull << LOCK_BIT_Y) | (1ull << LOCK_BIT_Z) |
                        (1ull << LOCK_BIT_COLLISION) | (1ull << LOCK_BIT_NO_VOID)));
  lock_hold_set(false);
  assert(slot_val() == ((1ull << LOCK_BIT_X) | (1ull << LOCK_BIT_COLLISION) | (1ull << LOCK_BIT_NO_VOID)));

  /* Map change releases axis locks but keeps ignore voids enabled */
  current_map = "HyruleField_Child";
  lock_frame();
  assert(!lock_is_set(LOCK_X));
  assert(novoid_enabled());
  assert(slot_val() == (1ull << LOCK_BIT_NO_VOID));

  /* 4. Turning ignore voids off clears LOCK_BIT_NO_VOID without disturbing axis locks */
  lock_set(LOCK_Y, true);
  novoid_set(false);
  assert(!novoid_enabled());
  assert(slot_val() == ((1ull << LOCK_BIT_Y) | (1ull << LOCK_BIT_COLLISION)));
  lock_set(LOCK_Y, false);
  assert(slot_val() == 0);

  puts("novoid host tests passed");
  return 0;
}
