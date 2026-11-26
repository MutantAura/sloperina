/* Host tests for game file patches (gamepatch.c) and Lank / Lunk's name swap (lank.c): the swap on made-up string
 * tables, and the frame of stored blocks decoded by the module's own zstd decoder. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamepatch.h"
#include "lank.h"
#include "menu.h"
#include "sys.h"
#include "zstd.h"

/* what gamepatch.c and zstd.c reach in the game process: nothing here */
void log_printf(const char *fmt, ...)
{
  (void)fmt;
}
bool sys_get_info(uint32_t type, uint64_t sub, uint64_t *out)
{
  (void)type, (void)sub, (void)out;
  return false;
}
uint32_t sys_query_memory(sys_meminfo_t *info, uint64_t addr)
{
  (void)info, (void)addr;
  return 1;
}
uintptr_t got_module_of(uintptr_t addr) { return addr ? 0 : 0; }
void *got_export(uintptr_t module, const char *name) { return (void)module, (void)name, NULL; }
const char *got_hook(uintptr_t module, const char *name, void *replacement, void *function, void **next)
{
  (void)module, (void)name, (void)replacement, (void)function, (void)next;
  return "no game here";
}
uint32_t sdk_open(void *out, const char *path, int mode) __asm__("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
uint32_t sdk_open(void *out, const char *path, int mode)
{
  (void)out, (void)path, (void)mode;
  return 1;
}
uint8_t *gamefile_read_zs(const char *path, size_t *size_out) { return (void)path, (void)size_out, NULL; }
/* what lank.c's menu and apply reach: not run here */
void remake_request(bool on) { (void)on; }
bool remake_pending(void) { return false; }
void gfx_notice(const char *s) { (void)s; }
uintptr_t link_actor(void) { return 0; }
const char *T(const char *s) { return s; }
const char *TF(const char *s) { return s; }
void menu_init(menu_t *m, const char *title, int capacity) { (void)m, (void)title, (void)capacity; }
menu_item_t *menu_add_option(menu_t *m, const char *label, int n, const char *const *choices,
                             int (*get)(menu_item_t *), void (*set)(menu_item_t *, int), intptr_t data)
{
  return (void)m, (void)label, (void)n, (void)choices, (void)get, (void)set, (void)data, NULL;
}
menu_item_t *menu_add_action(menu_t *m, const char *label, void (*fn)(menu_item_t *), intptr_t data)
{
  return (void)m, (void)label, (void)fn, (void)data, NULL;
}
menu_item_t *menu_add_return(menu_t *m) { return (void)m, NULL; }
menu_item_t *menu_add_text(menu_t *m, void (*text)(menu_item_t *, char *, int), intptr_t data)
{
  return (void)m, (void)text, (void)data, NULL;
}
menu_item_t *menu_add_submenu(menu_t *m, const char *label, menu_t *sub)
{
  return (void)m, (void)label, (void)sub, NULL;
}
const uint8_t *gamefile_sarc_member(const uint8_t *sarc, size_t size, const char *name, size_t *size_out)
{
  return (void)sarc, (void)size, (void)name, (void)size_out, NULL;
}

/* A BYML-like layout: the key table, a header and offsets, then the value strings */
static size_t table(uint8_t *out, bool with_key, const char *const *names, int n)
{
  size_t at = 0;
  static const char keys[] = "AnimationResources\0ModelProjectName\0";
  if (with_key) {
    memcpy(out, keys, sizeof(keys));
    at = sizeof(keys);
  } else {
    static const char other[] = "Player_Base\0Player_Child\0";
    memcpy(out, other, sizeof(other));
    at = sizeof(other);
  }
  memset(out + at, 0xC2, 28); /* header and offsets: no NUL before the first name's start but the last byte */
  at += 28;
  out[at - 1] = 0;
  for (int i = 0; i < n; i++) {
    size_t len = strlen(names[i]) + 1;
    memcpy(out + at, names[i], len);
    at += len;
  }
  return at;
}

static bool has(const uint8_t *data, size_t len, const char *s)
{
  size_t n = strlen(s) + 1;
  for (size_t i = 0; i + n <= len; i++)
    if (!memcmp(data + i, s, n))
      return true;
  return false;
}

static const char *const child_names[] = { "Player_Child_01_Animation", "Player_Child_02_Animation",
                                           "Player_Child_03_Animation", "Player_Child_04_Animation",
                                           "Player_Child_Cba_Animation", "Player_Child_Demo_Animation" };
static const char *const adult_names[] = { "Player_Adult_01_Animation",   "Player_Adult_02_Animation",
                                           "Player_Adult_02_H_Animation", "Player_Adult_03_Animation",
                                           "Player_Adult_04_Animation",   "Player_Adult_Cba_Animation",
                                           "Player_Adult_Demo_Animation", "Player_Dark_Animation" };

static void test_child_list(void)
{
  uint8_t buf[512];
  size_t len = table(buf, true, child_names, 6);
  assert(lank_swap_names(buf, len, LANK_ADULT, true) == -1); /* not the adult's list */
  assert(lank_swap_names(buf, len, LANK_CHILD, true) == 6);
  assert(has(buf, len, "Player_Adult_01_Animation") && has(buf, len, "Player_Adult_Demo_Animation"));
  assert(!has(buf, len, "Player_Child_Cba_Animation"));
  assert(lank_swap_names(buf, len, LANK_CHILD, true) == 0); /* already so */
  assert(lank_swap_names(buf, len, LANK_ADULT, true) == -1); /* swapped, still the child's list */
  assert(lank_swap_names(buf, len, LANK_CHILD, false) == 6);
  assert(has(buf, len, "Player_Child_01_Animation") && !has(buf, len, "Player_Adult_01_Animation"));
}

static void test_adult_list(void)
{
  uint8_t buf[512];
  size_t len = table(buf, true, adult_names, 8);
  assert(lank_swap_names(buf, len, LANK_CHILD, true) == -1);
  assert(lank_swap_names(buf, len, LANK_ADULT, true) == 6);
  assert(has(buf, len, "Player_Child_02_Animation") && has(buf, len, "Player_Child_Demo_Animation"));
  assert(has(buf, len, "Player_Adult_02_H_Animation") && has(buf, len, "Player_Dark_Animation")); /* no child's */
  assert(lank_swap_names(buf, len, LANK_ADULT, false) == 6);
  assert(has(buf, len, "Player_Adult_02_Animation") && !has(buf, len, "Player_Child_02_Animation"));
}

static void test_not_a_list(void)
{
  uint8_t buf[512];
  size_t len = table(buf, false, child_names, 6); /* the actor database's look: no key before the names */
  assert(lank_swap_names(buf, len, LANK_CHILD, true) == -1);
  len = table(buf, true, child_names, 5); /* five of the six */
  assert(lank_swap_names(buf, len, LANK_CHILD, true) == -1);
  memset(buf, 0, sizeof(buf));
  assert(lank_swap_names(buf, sizeof(buf), LANK_CHILD, true) == -1);
}

static void test_stored_frame(void)
{
  size_t sizes[] = { 0, 1, 128 * 1024, 128 * 1024 + 1, 1177699 };
  for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); s++) {
    size_t size = sizes[s];
    uint8_t *data = malloc(size + 1);
    for (size_t i = 0; i < size; i++)
      data[i] = (uint8_t)(i * 2654435761u >> 13);
    size_t frame_size = 0;
    uint8_t *frame = gamepatch_stored_frame(data, size, &frame_size);
    assert(frame);
    assert(zstd_frame_dict_id(frame, frame_size) == 0);
    assert(zstd_content_size(frame, frame_size) == size || size == 0);
    uint8_t *back = malloc(size + 16);
    const char *why = NULL;
    size_t n = zstd_decompress(back, size + 16, frame, frame_size, NULL, &why);
    assert(n == size);
    assert(!size || !memcmp(back, data, size));
    free(back);
    free(frame);
    free(data);
  }
}

int main(void)
{
  test_child_list();
  test_adult_list();
  test_not_a_list();
  test_stored_frame();
  puts("gamepatch: all tests passed");
  return 0;
}
