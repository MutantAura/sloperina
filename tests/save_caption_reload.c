/* Exercise the actual caption reload controller with modeled game/UI callbacks.
 * No save files, renderer or game process are involved. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Avoid a Windows CRT assertion dialog in unattended host runs. */
#undef assert
#define assert(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

#define GAME_FPS 60
#define SYS_PERM_R 1u
#define SYS_PERM_W 2u
#define MAIN(a) (a)
#define MAIN_FN(type, a) ((type)(a))
enum { MENU_SCREEN_LOAD = 1, MENU_SCREEN_SAVE, MENU_SCREEN_LOADING, MENU_SCREEN_GAME_OVER };
static struct {
  uintptr_t caption_mgr_ptr, caption_empty, caption_request, caption_update, caption_empty_entry;
  uintptr_t vt_load_list, load_list_hide_rows, vt_save_load_confirm;
} build, *z64hd_game = &build;
static uint32_t menu_screen;
static int busy;
static bool input_hold_game;
static uint64_t manager_words[0x98 / 8], entry_words[12][0x5A0 / 8], screen_words[0x2C0 / 8];
static uint8_t *manager, *entries[12];
static uint64_t fake_list;
static bool list_open, confirm_open, reader_available, empty_clears_mq;
static int empty_calls, entry_calls, request_calls, hide_calls, refresh_calls, notices;

static bool sys_mapped(uintptr_t a, size_t n, uint32_t perm)
{
  (void)perm;
  if (n > UINTPTR_MAX - a) return false;
  const struct { uintptr_t a; size_t n; } areas[] = {
    {(uintptr_t)&manager, sizeof(manager)}, {(uintptr_t)manager_words, sizeof(manager_words)},
    {(uintptr_t)entry_words, sizeof(entry_words)}, {(uintptr_t)entries, sizeof(entries)},
    {(uintptr_t)screen_words, sizeof(screen_words)}
  };
  for (unsigned i = 0; i < sizeof(areas)/sizeof(areas[0]); i++)
    if (a >= areas[i].a && a + n <= areas[i].a + areas[i].n) return true;
  return false;
}
static uintptr_t sys_ptr(uintptr_t a) { uintptr_t v = 0; if (sys_mapped(a, 8, SYS_PERM_R)) memcpy(&v, (void *)a, 8); return v; }
static uintptr_t ui_find_screen(uintptr_t vt)
{
  if (vt == build.vt_load_list) return list_open ? (uintptr_t)screen_words : 0;
  return vt == build.vt_save_load_confirm && confirm_open ? (uintptr_t)screen_words : 0;
}
static bool ui_refresh_now(uintptr_t vt)
{ assert(vt == build.vt_load_list && manager[0x8C] == 4); refresh_calls++; return true; }
static void log_printf(const char *fmt, ...) { (void)fmt; }
static void gfx_notice(const char *s) { assert(s && s[0]); notices++; }
static const char *T(const char *s) { return s; }

#include "save_caption_reload.generated.h"

static void put32(void *p, size_t off, int32_t v) { memcpy((uint8_t *)p + off, &v, 4); }
static void native_empty(void *p)
{
  assert(p == manager); empty_calls++;
  manager[CAP_STATE] = 0;
  if (empty_clears_mq) manager[CAP_MQ_MODE] = 0; /* native 1.0.0 behavior */
  for (int i = 0; i < 12; i++) entries[i][0] = 0;
}
static void native_empty_entry(void *p, int index, bool mq)
{
  assert(p == entries[index + (mq ? 6 : 0)]);
  ((uint8_t *)p)[0] = 0; entry_calls++;
}
static void native_request(void *p)
{
  assert(p == manager); request_calls++;
  if (reader_available) manager[CAP_REQUEST] |= 1;
}
static void native_hide(void *p) { assert(p == &fake_list); hide_calls++; }

static void reset(bool base, bool mq)
{
  memset(manager_words, 0, sizeof(manager_words)); memset(entry_words, 0, sizeof(entry_words));
  memset(screen_words, 0, sizeof(screen_words));
  manager = (uint8_t *)manager_words;
  for (int i = 0; i < 12; i++) {
    entries[i] = (uint8_t *)entry_words[i]; entries[i][0] = CAP_ENTRY_DONE;
    put32(entries[i], CAP_ENTRY_SLOT, i % 6);
  }
  for (int l = 0; l < 2; l++) {
    put32(manager, CAP_LIST_N(l), 6);
    uintptr_t p = (uintptr_t)&entries[l*6]; memcpy(manager + CAP_LIST(l), &p, 8);
  }
  manager[CAP_STATE] = CAP_LOADED; manager[CAP_MQ_MODE] = mq;
  uintptr_t list = (uintptr_t)&fake_list;
  memcpy((uint8_t *)screen_words + LOAD_LIST_LIST, &list, 8);
  build.caption_mgr_ptr = (uintptr_t)&manager; build.caption_empty = (uintptr_t)native_empty;
  build.caption_request = (uintptr_t)native_request; build.caption_update = 0;
  build.caption_empty_entry = (uintptr_t)native_empty_entry;
  build.vt_load_list = 1; build.vt_save_load_confirm = 2; build.load_list_hide_rows = (uintptr_t)native_hide;
  reload_step = RELOAD_IDLE; reload_frames = hidden_frames = 0; in_place = false;
  changed_known = false; changed_slots = 0; menu_screen = 0; busy = 0; input_hold_game = false;
  list_open = confirm_open = false; reader_available = true; empty_clears_mq = base;
  empty_calls = entry_calls = request_calls = hide_calls = refresh_calls = notices = 0;
}

static void finish_reading(void)
{
  for (int i = 0; i < 12; i++) entries[i][0] = CAP_ENTRY_DONE;
  manager[CAP_STATE] = CAP_LOADED; manager[CAP_REQUEST] = 0;
  reload_frame(); assert(reload_step == RELOAD_IDLE && !input_hold_game && notices == 1);
}

static void test_build(bool base, bool mq)
{
  reset(base, mq); reload_captions(); reload_frame();
  assert(empty_calls == 1 && request_calls == 1 && reload_step == RELOAD_READING);
  assert(manager[CAP_MQ_MODE] == mq && *(int32_t *)(manager + CAP_LATEST) == -1);
  finish_reading(); assert(!refresh_calls);

  reset(base, mq); menu_screen = MENU_SCREEN_LOAD; list_open = true;
  reload_captions(); reload_frame();
  assert(hide_calls == 1 && empty_calls == 0 && input_hold_game);
  for (int i = 1; i < HIDE_FRAMES; i++) {
    reload_frame(); assert(empty_calls == 0); /* old pictures must remain alive */
  }
  reload_frame(); assert(empty_calls == 1 && reload_step == RELOAD_READING);
  assert(manager[CAP_MQ_MODE] == mq); finish_reading(); assert(refresh_calls == 1);

  reset(base, mq); changed_known = true; changed_slots = (1u << 1) | (1u << 8);
  reload_captions(); reload_frame();
  assert(empty_calls == 0 && entry_calls == 2 && reload_step == RELOAD_READING);
  assert(entries[1][0] == 0 && entries[8][0] == 0 && entries[0][0] == CAP_ENTRY_DONE);
  assert(manager[CAP_MQ_MODE] == mq); finish_reading();

  reset(base, mq); reload_captions(); busy = 1; reload_frame(); assert(!empty_calls);
  busy = 0; manager[CAP_STATE] = 3; reload_frame(); assert(!empty_calls);
  manager[CAP_STATE] = CAP_LOADED; menu_screen = MENU_SCREEN_LOAD;
  list_open = confirm_open = true; reload_frame(); assert(!empty_calls && !hide_calls);
  confirm_open = false; reload_frame(); assert(hide_calls == 1 && input_hold_game);

  reset(base, mq); reader_available = false; reload_captions(); reload_frame();
  assert(reload_step == RELOAD_ASKING && empty_calls == 1);
  reader_available = true; reload_frame(); assert(reload_step == RELOAD_READING && empty_calls == 1);
  finish_reading();

  reset(base, mq); changed_known = true; changed_slots = 0;
  reload_captions(); reload_frame(); assert(reload_step == RELOAD_IDLE && !empty_calls && !request_calls);

  reset(base, mq); reload_captions(); manager = 0; input_hold_game = true; reload_frame();
  assert(reload_step == RELOAD_IDLE && !input_hold_game && !empty_calls);
  reset(base, mq); reload_step = RELOAD_READING; reload_frames = RELOAD_GIVE_UP;
  input_hold_game = true; reload_frame(); assert(reload_step == RELOAD_IDLE && !input_hold_game);
}

int main(void)
{
  for (int base = 0; base < 2; base++) for (int mq = 0; mq < 2; mq++) test_build(base, mq);
  puts("PASS: actual save-caption reload controller; both native reset behaviors; normal/MQ lists; hide-before-free; partial/full reload; waits and failure cleanup");
  return 0;
}
