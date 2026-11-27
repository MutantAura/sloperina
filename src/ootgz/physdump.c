/* Remote "physdump <name>": the Havok worlds' bodies and what they point to, raw, to sloperinaGZ/dump/<name>.phd, for
 * working out their layout offline (tools: a reader in the session's scratchpad). Records: u64 address, u32 size,
 * u32 tag, then the bytes. Debugging only. */
#include "physdump.h"

#include <stdio.h>
#include <string.h>

#include "enemyhp.h"
#include "link.h"
#include "sdfs.h"
#include "sys.h"
#include "z64hd.h"

#define DUMP_DIR SD_DIR "/dump"
#define BODY_SIZE 0xC0u
#define BODIES_PER_BLOCK 42u
#define MAX_BODIES 16384u
#define SEEN_SLOTS 16384u

enum { TAG_MGR = 1, TAG_WORLD, TAG_BLOCKS, TAG_BLOCK, TAG_L1, TAG_L2, TAG_ACTOR, TAG_COMPONENTS, TAG_LINK, TAG_MOTIONS,
       TAG_MOTION_BLOCK, TAG_L3 };

static fs_file_t file;
static int64_t at;
static bool failed;
static uintptr_t seen[SEEN_SLOTS];
static int records;

static bool heap_ptr(uintptr_t p)
{
  return p && !(p & 7) && p > 0x10000000ull && p < 0x8000000000ull;
}

static bool mark(uintptr_t p) /* false if seen before */
{
  uint64_t h = (p >> 3) * 0x9E3779B97F4A7C15ull;
  for (unsigned i = 0; i < SEEN_SLOTS; i++) {
    unsigned s = (unsigned)((h >> 40) + i) % SEEN_SLOTS;
    if (seen[s] == p)
      return false;
    if (!seen[s]) {
      seen[s] = p;
      return true;
    }
  }
  return true; /* full: write it again */
}

static void put(uintptr_t addr, uint32_t size, uint32_t tag)
{
  if (failed || !addr)
    return;
  while (size && !sys_mapped(addr, size, SYS_PERM_R))
    size = size > 0x40 ? size / 2 : 0;
  if (!size)
    return;
  struct { uint64_t addr; uint32_t size, tag; } head = { addr, size, tag };
  fs_write_option_t opt = { 0 };
  if (fs_write(file, at, &head, sizeof(head), &opt) != 0 || fs_write(file, at + (int64_t)sizeof(head),
      (const void *)addr, size, &opt) != 0) {
    failed = true;
    return;
  }
  at += (int64_t)sizeof(head) + size;
  records++;
}

/* the object at p and, one level down, what its words point to */
static void put_tree(uintptr_t p, uint32_t size, uint32_t tag, int depth, uint32_t child_size)
{
  if (!heap_ptr(p) || !mark(p))
    return;
  put(p, size, tag);
  if (depth <= 0 || !sys_mapped(p, size, SYS_PERM_R))
    return;
  for (uint32_t o = 0; o < size; o += 8) {
    uintptr_t q = *(const uintptr_t *)(p + o);
    if (heap_ptr(q))
      put_tree(q, child_size, tag + 1, depth - 1, child_size);
  }
}

static uintptr_t rd(uintptr_t p, size_t o)
{
  return p && sys_mapped(p + o, 8, SYS_PERM_R) ? *(const uintptr_t *)(p + o) : 0;
}

static void dump_world(uintptr_t world)
{
  if (!heap_ptr(world))
    return;
  put(world, 0x1000, TAG_WORLD);
  uint32_t cap = 0;
  if (!sys_read32(world + 0x130, &cap) || cap > MAX_BODIES)
    return;
  uintptr_t blocks = rd(world, 0x48);
  unsigned n_blocks = (cap + BODIES_PER_BLOCK - 1) / BODIES_PER_BLOCK;
  put(blocks, n_blocks * 8, TAG_BLOCKS);
  for (unsigned b = 0; b < n_blocks; b++) {
    uintptr_t block = rd(blocks, b * 8);
    if (!heap_ptr(block))
      continue;
    put(block, BODIES_PER_BLOCK * BODY_SIZE, TAG_BLOCK);
    for (unsigned i = 0; i < BODIES_PER_BLOCK; i++) {
      uintptr_t body = block + i * BODY_SIZE;
      uintptr_t shape = rd(body, 0x78);
      uint32_t shape_size = 0;
      if (heap_ptr(shape) && sys_read32(shape + 8, &shape_size) && shape_size >= 0x80 && shape_size <= 0x2000)
        put(shape, shape_size, TAG_L3);
      for (uint32_t o = 0; o < BODY_SIZE; o += 8) {
        uintptr_t q = rd(body, o);
        if (heap_ptr(q))
          put_tree(q, 0x180, TAG_L1, o == 0xB8 ? 2 : 1, 0x100);
      }
    }
  }
  uint32_t motion_cap = 0;
  if (sys_read32(world + 0x798, &motion_cap) && motion_cap < 65536) {
    uintptr_t mblocks = rd(world, 0x708);
    unsigned n = (motion_cap + 63) / 64;
    put(mblocks, n * 8, TAG_MOTIONS);
    for (unsigned b = 0; b < n; b++)
      put(rd(mblocks, b * 8), 64 * 0x80, TAG_MOTION_BLOCK);
  }
}

bool physdump_write(const char *name, int *out_records, int64_t *out_bytes)
{
  if (!z64hd_game || !z64hd_game->phive_mgr_ptr)
    return false;
  char path[96];
  fs_create_dir(DUMP_DIR);
  snprintf(path, sizeof(path), DUMP_DIR "/%s.phd", name);
  fs_delete_file(path);
  if (fs_create_file(path, 0) != 0 || fs_open(&file, path, FS_WRITE | FS_ALLOW_APPEND) != 0)
    return false;
  memset(seen, 0, sizeof(seen));
  at = 0;
  failed = false;
  records = 0;
  uintptr_t holder = sys_ptr(MAIN(z64hd_game->phive_mgr_ptr));
  uintptr_t mgr = holder ? sys_ptr(holder) : 0;
  put(mgr, 0x200, TAG_MGR);
  uintptr_t pworld = rd(mgr, 0xC0);
  put(pworld, 0x200, TAG_MGR);
  for (int w = 0; w < 2; w++) {
    uintptr_t wrap = rd(pworld, 0xC8 + (size_t)w * 8);
    put(wrap, 0x200, TAG_MGR);
    dump_world(rd(wrap, 0xE0));
  }
  /* the actors: each one's head and components, Link's marked */
  static actor_near_t all[1024];
  int more = 0;
  int n = actors_all(all, 1024, &more);
  uintptr_t link = link_actor();
  if (link) {
    put(link, 0x800, TAG_LINK);
    put_tree(rd(link, 0x270), 0x800, TAG_COMPONENTS, 1, 0x200);
  }
  for (int i = 0; i < n; i++) {
    put(all[i].actor, 0x800, TAG_ACTOR);
    put(rd(all[i].actor, 0x270), 0x800, TAG_COMPONENTS);
    if (all[i].name)
      put((uintptr_t)all[i].name, 64, TAG_ACTOR + 100);
  }
  /* main's address, for reading vtables */
  struct { uint64_t addr; uint32_t size, tag; uint64_t main; } m = { 0, 8, 999, z64hd_main };
  fs_write_option_t flush = { FS_WRITE_FLUSH };
  fs_write(file, at, &m, sizeof(m), &flush);
  at += sizeof(m);
  fs_close(file);
  *out_records = records;
  *out_bytes = at;
  return !failed;
}
