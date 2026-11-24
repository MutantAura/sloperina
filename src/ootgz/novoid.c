/* Void immunity (FINDINGS §9.8 / §9.25).
 *
 * The IPS patch gates three sites on LOCK_BIT_NO_VOID in lock_slot:
 *   1. check_abyss (1.0.0: 0x818640, 1.1.1: 0x82C510): skips all void checks (voxel ground map,
 *      abyss floor material, void/crusher/whirlpool/mountain message flags, quicksand, deep lava),
 *      clears PlayerComponent+0x28 bits 2..6 and zeros PlayerComponent+0x191C (AbyssType).
 *   2. PlayerComponent::onMessage's 0x613B0000/1 branch (1.0.0: 0x2E2BB90, 1.1.1: 0x2E80890):
 *      ignores incoming void-plane / crusher / whirlpool messages before they set +0x28 or +0x1C34.
 *   3. SelectorHorseRoot's cause==8 check (1.0.0: 0x2AD102C, 1.1.1: 0x2B242EC): ignores FallIntoAbyss
 *      messages while riding Epona. */
#include "novoid.h"

#include <stdint.h>

#include "link.h"
#include "lock.h"
#include "sys.h"
#include "z64hd.h"

#define ACTOR_PARTS        0x270
#define PARTS_PLAYER       0x358
#define PLAYER_MSG_FLAGS   0x28
#define PLAYER_ABYSS_TYPE  0x191C
#define ABYSS_MSG_BITS     0x7Cu       /* bits 2..6: FallIntoAbyss / crushers / whirlpools / mountain */
#define ABYSS_EVENT_BIT    0x80000000u /* bit 31: abyss message received during an event */

static bool enabled;

static void clear_pending_void(void)
{
  uintptr_t actor = link_actor();
  if (!actor || !sys_mapped(actor + ACTOR_PARTS, 8, SYS_PERM_R))
    return;
  uintptr_t parts = sys_ptr(actor + ACTOR_PARTS);
  if (!parts || !sys_mapped(parts + PARTS_PLAYER, 8, SYS_PERM_R))
    return;
  uintptr_t player = sys_ptr(parts + PARTS_PLAYER);
  if (!player)
    return;
  if (sys_mapped(player + PLAYER_MSG_FLAGS, 4, SYS_PERM_R | SYS_PERM_W)) {
    volatile uint32_t *flags = (volatile uint32_t *)(player + PLAYER_MSG_FLAGS);
    *flags &= ~(ABYSS_MSG_BITS | ABYSS_EVENT_BIT);
  }
  if (sys_mapped(player + PLAYER_ABYSS_TYPE, 4, SYS_PERM_R | SYS_PERM_W))
    *(volatile uint32_t *)(player + PLAYER_ABYSS_TYPE) = 0;
}

bool novoid_available(void)
{
  return z64hd_game && z64hd_game->lock_slot != 0 && z64hd_game->void_check_abyss != 0;
}

bool novoid_enabled(void)
{
  return enabled;
}

void novoid_set(bool on)
{
  if (!novoid_available()) {
    enabled = false;
    return;
  }
  enabled = on;
  lock_flag_set(LOCK_BIT_NO_VOID, on);
  if (on)
    clear_pending_void();
}
