/* The game's pause manager (pausemgr.h, FINDINGS 9.1). [main+pause_mgr_ptr] is the manager: +0x00 the number of
 * pause types (20), +0x08 their table, sorted by ID, 0x20 bytes each: +0x00 the name, +0x08 the ID (MurmurHash3 of
 * the name), +0x0C and +0x10 masks, +0x1C the request count (bit 31 a flag of its own, kept). The game's request
 * (main+0xafb980 on 1.1.1) adds 1 to the count atomically and notifies a listener whose function is an empty ret,
 * so adding here is the same. What the game's systems read is the manager's +0x30: three words, the OR of the
 * masks (+0x0C, +0x10, +0x14) of the types with a count, worked out once a frame before the module's frame runs, so
 * a count changed by the module alone would take a frame more. pausemgr_add works them out at once too. */
#include "pausemgr.h"

#include <stdio.h>

#include "gamedata.h"
#include "sys.h"
#include "z64hd.h"

#define N_TYPES 20
#define TYPE_SIZE 0x20
#define TYPE_ID 0x08
#define TYPE_COUNT 0x1C
#define COUNT_FLAG 0x80000000u
#define TYPE_MASKS 0x0C /* three words */
#define MGR_MASKS 0x30  /* the OR of the masks of the types with a count: what the game's systems read */

static uintptr_t manager;

/* the table, if the manager looks right: 20 types, sorted, GamePause among them */
static uintptr_t table(void)
{
  manager = 0;
  if (!z64hd_game || !z64hd_game->pause_mgr_ptr)
    return 0;
  uintptr_t mgr = sys_ptr(MAIN(z64hd_game->pause_mgr_ptr));
  if (!mgr || (mgr & 7) || !sys_mapped(mgr, 0x10, SYS_PERM_R))
    return 0;
  if (*(const volatile uint64_t *)mgr != N_TYPES)
    return 0;
  uintptr_t t = *(const volatile uintptr_t *)(mgr + 8);
  if (!t || (t & 7) || !sys_mapped(t, N_TYPES * TYPE_SIZE, SYS_PERM_R | SYS_PERM_W))
    return 0;
  bool game = false;
  uint32_t last = 0;
  for (int i = 0; i < N_TYPES; i++) {
    uint32_t id = *(const volatile uint32_t *)(t + (uintptr_t)i * TYPE_SIZE + TYPE_ID);
    if (i && id <= last)
      return 0;
    last = id;
    game |= id == PAUSE_GAME;
  }
  if (!game || !sys_mapped(mgr + MGR_MASKS, 12, SYS_PERM_R | SYS_PERM_W))
    return 0;
  manager = mgr;
  return t;
}

/* the manager's mask, from the counts as they are now */
static void work_out_masks(uintptr_t t)
{
  uint32_t m[3] = { 0, 0, 0 };
  for (int i = 0; i < N_TYPES; i++) {
    uintptr_t e = t + (uintptr_t)i * TYPE_SIZE;
    if (*(const volatile uint32_t *)(e + TYPE_COUNT) & ~COUNT_FLAG)
      for (int k = 0; k < 3; k++)
        m[k] |= *(const volatile uint32_t *)(e + TYPE_MASKS + k * 4);
  }
  for (int k = 0; k < 3; k++)
    *(volatile uint32_t *)(manager + MGR_MASKS + k * 4) = m[k];
}

static volatile uint32_t *count_of(uint32_t id)
{
  uintptr_t t = table();
  for (int i = 0; t && i < N_TYPES; i++)
    if (*(const volatile uint32_t *)(t + (uintptr_t)i * TYPE_SIZE + TYPE_ID) == id)
      return (volatile uint32_t *)(t + (uintptr_t)i * TYPE_SIZE + TYPE_COUNT);
  return 0;
}

uint32_t pausemgr_id(const char *s)
{
  if (!s || !*s)
    return 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    uint32_t v = 0;
    for (const char *c = s + 2; *c; c++) {
      int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10
            : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
      if (d < 0)
        return 0;
      v = v * 16 + (uint32_t)d;
    }
    return v;
  }
  return murmur3(s);
}

int pausemgr_count(uint32_t id)
{
  volatile uint32_t *c = count_of(id);
  return c ? (int)(*c & ~COUNT_FLAG) : -1;
}

bool pausemgr_add(uint32_t id, int delta)
{
  volatile uint32_t *c = count_of(id);
  if (!c)
    return false;
  for (;;) { /* (the game's own requests add atomically too) */
    uint32_t was = __atomic_load_n(c, __ATOMIC_ACQUIRE), n = was & ~COUNT_FLAG;
    int to = (int)n + delta;
    uint32_t now = (was & COUNT_FLAG) | (uint32_t)(to < 0 ? 0 : to);
    if (__atomic_compare_exchange_n(c, &was, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      break;
  }
  uintptr_t t = table();
  if (t)
    work_out_masks(t);
  return true;
}

bool pausemgr_set_masks(uint32_t id, const uint32_t masks[3], uint32_t was[3])
{
  volatile uint32_t *c = count_of(id);
  if (!c)
    return false;
  volatile uint32_t *m = (volatile uint32_t *)((uintptr_t)c - TYPE_COUNT + TYPE_MASKS);
  for (int k = 0; k < 3; k++) {
    if (was)
      was[k] = m[k];
    m[k] = masks[k];
  }
  work_out_masks(table());
  return true;
}

#define FREEZE_TYPE PAUSE_DEBUG
#define FREEZE_VIEW_BIT 0x2u /* the third word's: the camera and the view go on (FINDINGS 9.41) */

bool pausemgr_freeze(bool on)
{
  static bool frozen;
  static uint32_t own[3];
  if (on == frozen)
    return true;
  volatile uint32_t *c = count_of(FREEZE_TYPE);
  if (!c)
    return false;
  volatile uint32_t *m = (volatile uint32_t *)((uintptr_t)c - TYPE_COUNT + TYPE_MASKS);
  if (on) {
    for (int k = 0; k < 3; k++)
      own[k] = m[k];
    uint32_t with[3] = { own[0], own[1], own[2] | FREEZE_VIEW_BIT };
    pausemgr_set_masks(FREEZE_TYPE, with, 0);
    pausemgr_add(FREEZE_TYPE, 1);
  } else {
    pausemgr_add(FREEZE_TYPE, -1);
    pausemgr_set_masks(FREEZE_TYPE, own, 0);
  }
  frozen = on;
  return true;
}

int pausemgr_list(char *out, int len)
{
  uintptr_t t = table();
  if (!t)
    return snprintf(out, (size_t)len, "the pause manager isn't where this build's offset says\n");
  int at = 0;
  for (int i = 0; i < N_TYPES && at < len - 80; i++) {
    uintptr_t e = t + (uintptr_t)i * TYPE_SIZE, name = *(const volatile uintptr_t *)e;
    at += snprintf(out + at, (size_t)(len - at), "%s %#x masks %08x %08x %08x count %u\n",
                   name && sys_mapped(name, 8, SYS_PERM_R) ? (const char *)name : "?",
                   (unsigned)*(const volatile uint32_t *)(e + TYPE_ID),
                   (unsigned)*(const volatile uint32_t *)(e + TYPE_MASKS),
                   (unsigned)*(const volatile uint32_t *)(e + TYPE_MASKS + 4),
                   (unsigned)*(const volatile uint32_t *)(e + TYPE_MASKS + 8),
                   (unsigned)(*(const volatile uint32_t *)(e + TYPE_COUNT) & ~COUNT_FLAG));
  }
  return at;
}
