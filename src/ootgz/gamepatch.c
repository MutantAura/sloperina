/* Changes to the game's own files, made at run time and switchable (beta): see gamepatch.h, FINDINGS 9.34. */
#include "gamepatch.h"

#include <string.h>

#include "gamefile.h"
#include "got.h"
#include "sys.h"

void *aligned_alloc(size_t alignment, size_t size); /* from main */
void free(void *p);                                 /* from main */

static gamepatch_t *patches[GAMEPATCH_MAX];
static int n_patches;

/* --- the changed file ---------------------------------------------------------------------------- */

uint8_t *gamepatch_stored_frame(const uint8_t *data, size_t size, size_t *size_out)
{
  enum { BLOCK = 128 * 1024 }; /* a block's most */
  size_t blocks = size ? (size + BLOCK - 1) / BLOCK : 1;
  size_t total = 4 + 1 + 8 + blocks * 3 + size;
  uint8_t *f = aligned_alloc(16, (total + 15) & ~(size_t)15);
  if (!f)
    return 0;
  static const uint8_t magic[4] = { 0x28, 0xB5, 0x2F, 0xFD };
  memcpy(f, magic, sizeof(magic));
  size_t at = sizeof(magic);
  f[at++] = 0xE0; /* frame header: single segment, an 8-byte content size, no dictionary, no checksum */
  for (int i = 0; i < 8; i++)
    f[at++] = (uint8_t)((uint64_t)size >> (8 * i));
  size_t done = 0;
  do {
    size_t n = size - done < BLOCK ? size - done : BLOCK;
    uint32_t header = (uint32_t)(n << 3) | (done + n == size ? 1u : 0u); /* block type 0: stored */
    f[at++] = (uint8_t)header;
    f[at++] = (uint8_t)(header >> 8);
    f[at++] = (uint8_t)(header >> 16);
    memcpy(f + at, data + done, n);
    at += n;
    done += n;
  } while (done < size);
  *size_out = at;
  return f;
}

static bool same(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* note that the game read `path`, for the patches on it */
static void mark_read(const char *path)
{
  for (int i = 0; i < n_patches; i++)
    if (!patches[i]->read && same(patches[i]->file, path))
      patches[i]->read = true;
}

static bool any_on_for(const char *path)
{
  for (int i = 0; i < n_patches; i++)
    if (patches[i]->on && same(patches[i]->file, path))
      return true;
  return false;
}

/* The file at `path` with the fixes of the patches on for it, as the frame to answer with; 0 if none is on, nothing
 * needed changing, or it can't be read */
static uint8_t *changed_file(const char *path, size_t *size_out)
{
  if (!any_on_for(path))
    return 0;
  size_t size = 0;
  uint8_t *data = gamefile_read_zs(path, &size);
  if (!data) {
    log_printf("game files: can't read %s", path);
    return 0;
  }
  int changes = 0;
  for (int i = 0; i < n_patches; i++) {
    gamepatch_t *p = patches[i];
    if (!p->on || !same(p->file, path))
      continue;
    size_t len = size;
    uint8_t *at = data;
    if (p->member)
      at = (uint8_t *)gamefile_sarc_member(data, size, p->member, &len);
    int n = at ? p->fix(p, at, len, true) : -1;
    if (n > 0)
      changes += n;
  }
  uint8_t *frame = changes ? gamepatch_stored_frame(data, size, size_out) : 0;
  free(data);
  log_printf("game files: %s read with %d change(s)%s", path, changes, changes && !frame ? " (out of memory)" : "");
  return frame;
}

/* --- the nn::fs wrappers --------------------------------------------------------------------------- */

typedef struct { void *handle; } file_handle_t; /* nn::fs::FileHandle */
typedef uint32_t result_t;                       /* nn::Result: 0 = success */

typedef result_t (*open_t)(file_handle_t *out, const char *path, int mode);
typedef void (*close_t)(file_handle_t file);
typedef result_t (*get_size_t)(int64_t *out, file_handle_t file);
typedef result_t (*read_t)(file_handle_t file, int64_t at, void *buf, size_t size);
typedef result_t (*read_n_t)(size_t *n, file_handle_t file, int64_t at, void *buf, size_t size);
typedef result_t (*read_opt_t)(size_t *n, file_handle_t file, int64_t at, void *buf, size_t size, const void *opt);

enum { OPEN, CLOSE, SIZE, READ, READ_N, READ_OPT, N_HOOKS };
static const char *const fs_names[N_HOOKS] = {
  "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
  "_ZN2nn2fs9CloseFileENS0_10FileHandleE",
  "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",
  "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",
  "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",
  "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE",
};
/* what each wrapper calls: another module's wrapper hooked earlier, or the sdk's function (got_hook) */
static void *fs_next[N_HOOKS];
static bool hooked;

/* The files open now with our version: the handle the game holds and the frame we answer with. Few at once; loader
 * threads open and read: a slot is claimed by its frame (compare-and-swap) and its handle published last. */
#define SLOTS 4
static struct {
  void *volatile handle;
  uint8_t *volatile frame;
  size_t size;
} slots[SLOTS];

static int slot_of(file_handle_t file)
{
  if (!file.handle)
    return -1;
  for (int i = 0; i < SLOTS; i++)
    if (slots[i].handle == file.handle)
      return i;
  return -1;
}

/* up to `size` bytes of slot i's frame from `at` into buf; how many */
static size_t serve(int i, int64_t at, void *buf, size_t size)
{
  if (at < 0 || (size_t)at >= slots[i].size)
    return 0;
  size_t n = slots[i].size - (size_t)at;
  if (n > size)
    n = size;
  memcpy(buf, slots[i].frame + at, n);
  return n;
}

static result_t wrap_open(file_handle_t *out, const char *path, int mode)
{
  result_t r = ((open_t)fs_next[OPEN])(out, path, mode);
  if (r != 0 || !path)
    return r;
  mark_read(path);
  if (!any_on_for(path))
    return r;
  size_t size = 0;
  uint8_t *frame = changed_file(path, &size);
  if (!frame)
    return r;
  for (int i = 0; i < SLOTS; i++) {
    uint8_t *empty = 0;
    if (!__atomic_compare_exchange_n(&slots[i].frame, &empty, frame, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      continue;
    slots[i].size = size;
    __atomic_store_n(&slots[i].handle, out->handle, __ATOMIC_RELEASE);
    return r;
  }
  free(frame); /* no free slot: the game reads the file as it is */
  return r;
}

static void wrap_close(file_handle_t file)
{
  int i = slot_of(file);
  if (i >= 0) {
    uint8_t *frame = slots[i].frame;
    __atomic_store_n(&slots[i].handle, (void *)0, __ATOMIC_RELEASE);
    memset(frame, 0, slots[i].size); /* no stale copy of the file for a search to find */
    slots[i].size = 0;
    __atomic_store_n(&slots[i].frame, (uint8_t *)0, __ATOMIC_RELEASE); /* the slot is free again */
    free(frame);
  }
  ((close_t)fs_next[CLOSE])(file);
}

static result_t wrap_size(int64_t *out, file_handle_t file)
{
  int i = slot_of(file);
  if (i < 0)
    return ((get_size_t)fs_next[SIZE])(out, file);
  *out = (int64_t)slots[i].size;
  return 0;
}

static result_t wrap_read(file_handle_t file, int64_t at, void *buf, size_t size)
{
  int i = slot_of(file);
  if (i < 0)
    return ((read_t)fs_next[READ])(file, at, buf, size);
  serve(i, at, buf, size);
  return 0;
}

static result_t wrap_read_n(size_t *n, file_handle_t file, int64_t at, void *buf, size_t size)
{
  int i = slot_of(file);
  if (i < 0)
    return ((read_n_t)fs_next[READ_N])(n, file, at, buf, size);
  *n = serve(i, at, buf, size);
  return 0;
}

static result_t wrap_read_opt(size_t *n, file_handle_t file, int64_t at, void *buf, size_t size, const void *opt)
{
  int i = slot_of(file);
  if (i < 0)
    return ((read_opt_t)fs_next[READ_OPT])(n, file, at, buf, size, opt);
  *n = serve(i, at, buf, size);
  return 0;
}

static void *const fs_wraps[N_HOOKS] = { wrap_open, wrap_close, wrap_size, wrap_read, wrap_read_n, wrap_read_opt };

result_t gamepatch_sdk_open(file_handle_t *out, const char *path, int mode)
  __asm__("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");

const char *gamepatch_hook_game(uintptr_t main_base)
{
  /* the sdk's own functions, found by name in the module that defines OpenFile (imported here): the reads aren't
   * imported, and an import rtld couldn't find would stop the game loading */
  uintptr_t sdk = got_module_of((uintptr_t)gamepatch_sdk_open);
  if (!sdk)
    return "game files: the sdk module wasn't found";
  void *functions[N_HOOKS];
  for (int i = 0; i < N_HOOKS; i++)
    if (!(functions[i] = got_export(sdk, fs_names[i])))
      return "game files: an nn::fs function wasn't found";
  for (int i = 0; i < N_HOOKS; i++) {
    const char *error = got_hook(main_base, fs_names[i], fs_wraps[i], functions[i], &fs_next[i]);
    if (error)
      return error; /* those already wrapped only pass calls on while no patch is on */
  }
  hooked = true;
  return 0;
}

bool gamepatch_available(void)
{
  return hooked;
}

/* --- the copies the game keeps ------------------------------------------------------------------- */

#define SEARCH_BYTES_PER_FRAME (8u << 20) /* about a frame's worth here (all memory, 9.6 GiB, takes ~20 s) */

static bool searching;
static uintptr_t search_at, search_end, region_start, region_end;
static size_t search_max_region; /* 0: any */
static uint64_t search_read;
static int search_changes;

/* The window round `hit` the patch's fix is given, within [lo_limit, hi_limit) */
static void window(const gamepatch_t *p, uintptr_t hit, uintptr_t lo_limit, uintptr_t hi_limit, uintptr_t *lo,
                   uintptr_t *hi)
{
  *lo = hit - lo_limit > p->before ? hit - p->before : lo_limit;
  *hi = hi_limit - hit > p->after ? hit + p->after : hi_limit;
}

static void remember(gamepatch_t *p, uintptr_t hit)
{
  int free_at = -1;
  for (int k = 0; k < (int)(sizeof(p->known) / sizeof(*p->known)); k++) {
    if (p->known[k] == hit)
      return;
    if (!p->known[k] && free_at < 0)
      free_at = k;
  }
  p->known[free_at >= 0 ? free_at : 0] = hit;
}

/* the patch's fix on the copies found before; those no longer there are forgotten */
static int fix_known(gamepatch_t *p)
{
  int changes = 0;
  for (int k = 0; k < (int)(sizeof(p->known) / sizeof(*p->known)); k++) {
    uintptr_t hit = p->known[k];
    if (!hit)
      continue;
    sys_meminfo_t info;
    if (sys_query_memory(&info, hit) != 0 || (info.perm & 3) != 3) {
      p->known[k] = 0;
      continue;
    }
    uintptr_t lo, hi;
    window(p, hit, (uintptr_t)info.addr, (uintptr_t)(info.addr + info.size), &lo, &hi);
    int n = p->fix(p, (uint8_t *)lo, hi - lo, p->on);
    if (n < 0)
      p->known[k] = 0;
    else
      changes += n;
  }
  return changes;
}

static void start_search(void)
{
  uint64_t addr = 0, size = 0; /* the process's address-space region (svcGetInfo 12, 13): the packs are outside
                                * the heap region */
  if (!sys_get_info(12, 0, &addr) || !sys_get_info(13, 0, &size) || !size)
    return;
  search_at = (uintptr_t)addr;
  search_end = (uintptr_t)(addr + size);
  region_start = region_end = search_at;
  search_max_region = 0;
  for (int i = 0; i < n_patches; i++) { /* the largest any patch wants; a patch without a limit lifts it */
    const gamepatch_t *p = patches[i];
    if (!p->anchor)
      continue;
    if (!p->max_region) {
      search_max_region = 0;
      break;
    }
    if (p->max_region > search_max_region)
      search_max_region = p->max_region;
  }
  search_read = 0;
  search_changes = 0;
  searching = true;
}

/* the patches with an anchor equal to w get their fix run round `hit` (within the region) */
static void found(uint32_t w, uintptr_t hit)
{
  for (int i = 0; i < n_patches; i++) {
    gamepatch_t *p = patches[i];
    if (!p->anchor || p->anchor != w)
      continue;
    uintptr_t lo, hi;
    window(p, hit, region_start, region_end, &lo, &hi);
    int n = p->fix(p, (uint8_t *)lo, hi - lo, p->on);
    if (n >= 0) {
      remember(p, hit);
      search_changes += n;
    }
  }
}

static void search_step(void)
{
  size_t budget = SEARCH_BYTES_PER_FRAME;
  while (budget && search_at < search_end) {
    if (search_at >= region_end) { /* the next readable and writable region */
      sys_meminfo_t info;
      if (sys_query_memory(&info, search_at) != 0 || info.size == 0) {
        search_at = search_end;
        break;
      }
      region_start = (uintptr_t)info.addr;
      region_end = (uintptr_t)(info.addr + info.size);
      if (region_end <= search_at || (info.perm & 3) != 3 || (search_max_region && info.size > search_max_region)) {
        search_at = region_end > search_at ? region_end : search_end;
        continue;
      }
    }
    uintptr_t stop = region_end < search_end ? region_end : search_end;
    if (stop - search_at > budget)
      stop = search_at + budget;
    budget -= stop - search_at;
    search_read += stop - search_at;
    /* anchors are 4-byte aligned: 8 bytes at a time, both halves (regions are page-aligned) */
    for (uintptr_t p = (search_at + 7u) & ~(uintptr_t)7u; p + 8 <= stop; p += 8) {
      uint64_t w = *(const volatile uint64_t *)p;
      for (int i = 0; i < n_patches; i++) {
        uint32_t a = patches[i]->anchor;
        if (a && (uint32_t)w == a)
          found(a, p);
        if (a && (uint32_t)(w >> 32) == a)
          found(a, p + 4);
      }
    }
    search_at = stop;
  }
  if (search_at >= search_end) {
    searching = false;
    log_printf("game files: kept copies searched (%lu MiB), %d change(s)", (unsigned long)(search_read >> 20),
               search_changes);
  }
}

bool gamepatch_add(gamepatch_t *patch)
{
  if (n_patches >= GAMEPATCH_MAX)
    return false;
  patches[n_patches++] = patch;
  return true;
}

void gamepatch_set(gamepatch_t *patch, bool on)
{
  if (patch->on == on)
    return;
  patch->on = on;
  if (!patch->anchor || !patch->read)
    return;
  int changes = fix_known(patch);
  if (changes)
    log_printf("game files: %s: %d change(s) in the copies found before", patch->file, changes);
  start_search(); /* from the start again if one was under way: copies made since are found too */
}

bool gamepatch_searching(void)
{
  return searching;
}

void gamepatch_frame(void)
{
  if (searching)
    search_step();
}
