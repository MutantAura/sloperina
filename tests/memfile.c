/* Host unit tests for memfile.c (no game process). */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z64hd.h"

/* Intercept MAIN_FN calls from memfile.c so game function offsets dispatch to test doubles. */
static bool      fake_gd_req_save(void *mgr, void *buf, int file_idx);
static bool      fake_gd_req_load(void *mgr, const void *buf, int file_idx);
static void      fake_wm_set_time(uintptr_t wm, uint32_t seconds);
static void      fake_wm_on_save(uintptr_t mgr);
static void      fake_wm_on_load(uintptr_t mgr);
static uintptr_t fake_navi_get(uintptr_t scene);
static void      fake_navi_on_load(uintptr_t mgr);
static void      fake_camera_snap(uintptr_t component);
static void      fake_camera_arrive(uintptr_t component);

static struct z64hd_build test_build;
const struct z64hd_build *z64hd_game = &test_build;
uintptr_t z64hd_main;

#define OFF_GD_REQ_SAVE   0x1100u
#define OFF_GD_REQ_LOAD   0x1200u
#define OFF_WM_SET_TIME   0x1300u
#define OFF_WM_ON_SAVE    0x1400u
#define OFF_WM_ON_LOAD    0x1500u
#define OFF_NAVI_GET      0x1600u
#define OFF_NAVI_ON_LOAD  0x1700u
#define OFF_CAMERA_SNAP   0x1800u
#define OFF_CAMERA_ARRIVE 0x1900u

static uintptr_t function_at(uint32_t off)
{
  if (off == OFF_GD_REQ_SAVE) return (uintptr_t)fake_gd_req_save;
  if (off == OFF_GD_REQ_LOAD) return (uintptr_t)fake_gd_req_load;
  if (off == OFF_WM_SET_TIME) return (uintptr_t)fake_wm_set_time;
  if (off == OFF_WM_ON_SAVE) return (uintptr_t)fake_wm_on_save;
  if (off == OFF_WM_ON_LOAD) return (uintptr_t)fake_wm_on_load;
  if (off == OFF_NAVI_GET) return (uintptr_t)fake_navi_get;
  if (off == OFF_NAVI_ON_LOAD) return (uintptr_t)fake_navi_on_load;
  if (off == OFF_CAMERA_ARRIVE) return (uintptr_t)fake_camera_arrive;
  assert(off == OFF_CAMERA_SNAP);
  return (uintptr_t)fake_camera_snap;
}

#undef MAIN_FN
#define MAIN_FN(type, off) ((type)function_at(off))

#include "../src/ootgz/memfile.c"

/* --- simulated GameData, actors, world/navi/warp/camera state ---------------------------------- */

#define H_SESSION_INT   0x11112222u
#define H_SESSION_BOOL  0x33334444u
#define H_SESSION_STR   0x55556666u
#define H_SESSION_UINT  0x77778888u

const memfilefield_t memfilefields[] = {
  { H_SESSION_INT,  "Session.Int"  },
  { H_SESSION_BOOL, "Session.Bool" },
  { H_SESSION_STR,  "Session.Str"  },
  { H_SESSION_UINT, "Session.UInt" },
};
const int memfilefields_count = (int)(sizeof(memfilefields) / sizeof(memfilefields[0]));

/* Test memory region registry for sys_mapped / sys_ptr / sys_read32. */
typedef struct {
  uintptr_t base;
  size_t    size;
  uint32_t  perm;
} region_t;

#define MAX_REGIONS 128
static region_t regions[MAX_REGIONS];
static int      n_regions;

static void clear_regions(void) { n_regions = 0; }
static void add_region(const void *p, size_t size, uint32_t perm)
{
  assert(n_regions < MAX_REGIONS);
  regions[n_regions++] = (region_t){ (uintptr_t)p, size, perm };
}

bool sys_mapped(uintptr_t addr, size_t size, uint32_t perm)
{
  if (!addr || size > UINTPTR_MAX - addr)
    return false;
  for (int i = 0; i < n_regions; i++)
    if (addr >= regions[i].base && addr + size <= regions[i].base + regions[i].size &&
        (regions[i].perm & perm) == perm)
      return true;
  return false;
}

uintptr_t sys_ptr(uintptr_t addr)
{
  uintptr_t v = 0;
  if (sys_mapped(addr, sizeof(v), SYS_PERM_R))
    memcpy(&v, (const void *)addr, sizeof(v));
  return v;
}

bool sys_read32(uintptr_t addr, uint32_t *out)
{
  if (!sys_mapped(addr, sizeof(*out), SYS_PERM_R))
    return false;
  memcpy(out, (const void *)addr, sizeof(*out));
  return true;
}

static bool block_scan_query;
uint32_t sys_query_memory(sys_meminfo_t *info, uint64_t addr)
{
  if (block_scan_query || !info)
    return 1;
  for (int i = 0; i < n_regions; i++) {
    if (addr >= regions[i].base && addr < regions[i].base + regions[i].size) {
      memset(info, 0, sizeof(*info));
      info->addr = regions[i].base;
      info->size = regions[i].size;
      info->perm = regions[i].perm;
      return 0;
    }
  }
  return 1;
}

static char last_notice[128];
void gfx_notice(const char *s)
{
  snprintf(last_notice, sizeof(last_notice), "%s", s ? s : "");
}
void gfx_logf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(last_notice, sizeof(last_notice), fmt, ap);
  va_end(ap);
}
void log_printf(const char *fmt, ...) { (void)fmt; }
const char *T(const char *s) { return s; }
const char *TF(const char *s) { return s; }
const char *flagname(uint32_t hash) { (void)hash; return 0; }

/* Typed field backing storage */
static uint8_t  gd_mgr_mem[0xE00];
static uint8_t  gd_desc0_mem[GD_DESC_STRIDE];
static uint32_t gd_tbl_int[16], gd_tbl_bool[16], gd_tbl_float[8], gd_tbl_enum[8];
static uint32_t gd_tbl_str[16], gd_tbl_uint[8], gd_tbl_vec3[8];
static uint8_t  gd_obj_int[8][0x28];
static uint8_t  gd_obj_bool[16][0x18];
static uint8_t  gd_obj_float[2][0x28];
static uint8_t  gd_obj_enum[2][0x38];
static uint8_t  gd_obj_str[8][Z64HD_STR_STRIDE];
static char     gd_str_bufs[8][GD_STR_LEN];
static uint8_t  gd_obj_uint[4][GD_UINT_STRIDE];
static uint8_t  gd_obj_vec3[4][GD_VEC3_STRIDE];
static gd_type_t type_int, type_bool, type_float, type_enum, type_str;

/* Lookup helper over fixed open-addressed test tables */
static uint32_t *find_pair(const gd_type_t *t, uint32_t hash)
{
  for (uint32_t i = 0; i < t->table_size; i++)
    if (t->table[2 * i] == hash)
      return &t->table[2 * i];
  return 0;
}

static void add_gd_entry(gd_type_t *t, uint32_t stride, uint32_t idx, uint32_t hash, int32_t save_file)
{
  uint32_t s = hash % t->table_size;
  while (t->table[2 * s] != 0)
    s = (s + 1) % t->table_size;
  t->table[2 * s] = hash;
  t->table[2 * s + 1] = idx;
  uintptr_t obj_vt = (uintptr_t)gd_mgr_mem;
  memcpy(t->objects + (size_t)idx * stride, &obj_vt, sizeof(obj_vt));
  memcpy(t->objects + (size_t)idx * stride + GD_FIELD_SAVE_FILE, &save_file, sizeof(save_file));
}

void *gd_manager(void) { return gd_mgr_mem; }
bool gd_ready(void) { return true; }
gd_type_t *gd_type(gd_kind_t k)
{
  return k == GD_INT ? &type_int : k == GD_BOOL ? &type_bool : k == GD_FLOAT ? &type_float :
         k == GD_ENUM ? &type_enum : 0;
}
gd_type_t *gd_string_type(void) { return &type_str; }
void gd_layout(gd_kind_t k, uint32_t *stride, uint32_t *voff)
{
  static const uint32_t st[GD_NTYPES] = { 0x28, 0x18, 0x28, 0x38 };
  static const uint32_t vo[GD_NTYPES] = { 0x1C, 0x16, 0x1C, 0x1C };
  *stride = st[k];
  *voff = vo[k];
}
gd_type_t *gd_type_at(uint32_t offset)
{
  if (offset == GD_UINT_TYPE || offset == GD_VEC3_TYPE)
    return (gd_type_t *)(gd_mgr_mem + offset);
  return 0;
}
bool gd_type_index(const gd_type_t *t, uint32_t hash, uint32_t *idx)
{
  uint32_t *p = t ? find_pair(t, hash) : 0;
  if (!p || p[1] >= t->count)
    return false;
  *idx = p[1];
  return true;
}

bool gd_get_int(uint32_t hash, int32_t *out)
{
  uint32_t i;
  if (!gd_type_index(&type_int, hash, &i))
    return false;
  memcpy(out, gd_obj_int[i] + 0x1C, 4);
  return true;
}
void gd_set_int(uint32_t hash, int32_t v) { gd_poke_int(hash, v); }
bool gd_poke_int(uint32_t hash, int32_t v)
{
  uint32_t i;
  if (!gd_type_index(&type_int, hash, &i))
    return false;
  memcpy(gd_obj_int[i] + 0x1C, &v, 4);
  memcpy(gd_obj_int[i] + 0x20, &v, 4);
  return true;
}
bool gd_get_bool(uint32_t hash, bool *out)
{
  uint32_t i;
  if (!gd_type_index(&type_bool, hash, &i))
    return false;
  *out = gd_obj_bool[i][0x16] != 0;
  return true;
}
bool gd_set_bool(uint32_t hash, bool v)
{
  uint32_t i;
  if (!gd_type_index(&type_bool, hash, &i))
    return false;
  gd_obj_bool[i][0x16] = gd_obj_bool[i][0x17] = (uint8_t)(v ? 1 : 0);
  return true;
}
const char *gd_str_index(uint32_t i) { return i < type_str.count ? gd_str_bufs[i] : 0; }
const char *gd_get_str(uint32_t hash)
{
  uint32_t i;
  return gd_type_index(&type_str, hash, &i) ? gd_str_bufs[i] : 0;
}
bool gd_set_str(uint32_t hash, const char *s)
{
  uint32_t i;
  if (!gd_type_index(&type_str, hash, &i))
    return false;
  snprintf(gd_str_bufs[i], GD_STR_LEN, "%s", s ? s : "");
  return true;
}
bool gd_set_vec3(uint32_t hash, const float v[3])
{
  gd_type_t *t = gd_type_at(GD_VEC3_TYPE);
  uint32_t i;
  if (!t || !gd_type_index(t, hash, &i))
    return false;
  memcpy(gd_obj_vec3[i] + GD_VEC3_VALUE, v, 12);
  memcpy(gd_obj_vec3[i] + GD_VEC3_VALUE + 12, v, 12);
  return true;
}
static void read_vec3(uint32_t hash, float v[3])
{
  gd_type_t *t = gd_type_at(GD_VEC3_TYPE);
  uint32_t i;
  assert(t && gd_type_index(t, hash, &i));
  memcpy(v, gd_obj_vec3[i] + GD_VEC3_VALUE, 12);
}

/* Mock WorldMgr, NaviMgr, Camera, Link, Warp, Eventskip, Gear */
static struct {
  uintptr_t wm_holder_ptr, wm_ptr;
  uintptr_t navi_scene_holder_ptr, navi_scene_ptr;
  uintptr_t cam_holder_ptr, cam_ptr;
} main_slots;

static uint8_t wm_mem[0x600], clock_mem[0x80], clock_vt[0x40];
static uint8_t navi_scene_mem[0x500], navi_mem[0x40];
static uint32_t sim_time = 21600;
static int wm_save_calls, wm_load_calls, navi_load_calls, gear_calls;

static uint32_t fake_clock_get(uintptr_t c) { (void)c; return sim_time; }
static void fake_wm_set_time(uintptr_t wm, uint32_t sec) { (void)wm; sim_time = sec; }
static void fake_wm_on_save(uintptr_t mgr) { (void)mgr; wm_save_calls++; }
static void fake_wm_on_load(uintptr_t mgr) { (void)mgr; wm_load_calls++; }
static uintptr_t fake_navi_get(uintptr_t scene) { assert(scene == (uintptr_t)navi_scene_mem); return (uintptr_t)navi_mem; }
static void fake_navi_on_load(uintptr_t mgr) { assert(mgr == (uintptr_t)navi_mem); navi_load_calls++; }

static bool allow_req_save = true, allow_req_load = true;
static int  req_save_calls, req_load_calls;
static uint8_t *last_req_save_buf;
static const uint8_t *last_req_load_buf;

static bool fake_gd_req_save(void *mgr, void *buf, int file_idx)
{
  (void)mgr; (void)file_idx;
  req_save_calls++;
  if (!allow_req_save)
    return false;
  last_req_save_buf = (uint8_t *)buf;
  uintptr_t p = (uintptr_t)buf;
  memcpy(gd_desc0_mem + GD_DESC_SAVE_BUF, &p, sizeof(p));
  gd_desc0_mem[GD_DESC_FLAGS] = GD_FLAG_SAVE_REQ | GD_FLAG_SAVE_WORK;
  return true;
}

static bool fake_gd_req_load(void *mgr, const void *buf, int file_idx)
{
  (void)mgr; (void)file_idx;
  req_load_calls++;
  if (!allow_req_load)
    return false;
  last_req_load_buf = (const uint8_t *)buf;
  uintptr_t p = (uintptr_t)buf;
  memcpy(gd_desc0_mem + GD_DESC_LOAD_BUF, &p, sizeof(p));
  gd_desc0_mem[GD_DESC_FLAGS] = GD_FLAG_LOAD_REQ;
  gd_desc0_mem[GD_DESC_LOAD_OK] = 0;
  return true;
}

/* Mock Link state */
static uintptr_t sim_link_actor;
static int32_t   sim_hp, sim_max_hp;
static vec3f_t   sim_pos;
static double    sim_fx, sim_fz;
static bool      sim_teleport_ok = true;
static int       teleport_calls;

uintptr_t link_actor(void) { return sim_link_actor; }
bool link_drawn_pos(vec3f_t *out) { *out = sim_pos; return sim_link_actor != 0; }
bool link_facing(double *fx, double *fz) { *fx = sim_fx; *fz = sim_fz; return sim_link_actor != 0; }
bool link_health(int32_t **cur, int32_t **max)
{
  if (!sim_link_actor)
    return false;
  *cur = &sim_hp;
  *max = &sim_max_hp;
  return true;
}
bool cct_teleport(const vec3f_t *p, const float *rot)
{
  teleport_calls++;
  if (!sim_teleport_ok)
    return false;
  sim_pos = *p;
  if (rot) {
    sim_fx = rot[2];
    sim_fz = rot[0];
  }
  return true;
}
bool cct_clear_fall_distance(uintptr_t actor) { (void)actor; return true; }
bool gear_reapply_now(void) { gear_calls++; return true; }

/* Mock Warp & Eventskip */
static bool sim_warp_pending, sim_warp_requested, sim_warp_abandoned;
static bool sim_silent_warp_accept = true;
static int  warp_silent_calls, warp_cancel_calls;
static bool sim_ev_known = true, sim_ev_running;

bool warp_pending(void) { return sim_warp_pending; }
bool warp_requested(void) { return sim_warp_requested; }
bool warp_abandoned(void) { return sim_warp_abandoned; }
void warp_cancel(void) { warp_cancel_calls++; sim_warp_pending = false; }
bool age_warp_to_silent(const char *map, const char *start, bool have_pos)
{
  (void)map; (void)start; (void)have_pos;
  warp_silent_calls++;
  if (!sim_silent_warp_accept)
    return false;
  sim_warp_requested = true;
  sim_warp_pending = true;
  sim_warp_abandoned = false;
  return true;
}
bool eventskip_known(void) { return sim_ev_known; }
bool eventskip_running(void) { return sim_ev_running; }

/* Mock Player Camera + AI Node Chain (cam_region holds cam_comp + optional scan-only node) */
static uint8_t cam_holder_mem[0x60], cam_handle_entry[0x60];
static uint8_t cam_actor_mem[0x300], cam_comps_mem[0x400], cam_glob_mem[0x60];
static struct {
  uint8_t comp[0x1000];
  uint8_t scan_node[ACTION_BYTES];
} cam_region;
#define cam_comp_mem cam_region.comp
static uint8_t ai_comp_mem[0x100], ai_ctrl_set_mem[0x40];
static uint8_t ai_entries_arr[16], ai_entry0[0x30], ai_entry1[0x30];
static uint8_t ctrl_bad_mem[0x220], ctrl_ok_mem[0x220];
static uint8_t unit_slots_arr[0x40], unit_bad_mem[0x80], unit_ok_mem[0x80];
static uint8_t node_slots_arr[3 * 0x18];
static uint8_t node_bad_mem[ACTION_BYTES], node_ok_mem[ACTION_BYTES], node_ok2_mem[ACTION_BYTES];
static int cam_snap_calls, cam_arrive_calls;

uintptr_t camera(void) { return (uintptr_t)cam_glob_mem; }
static void fake_camera_snap(uintptr_t component)
{
  assert(component == (uintptr_t)cam_comp_mem);
  cam_snap_calls++;
}
static void fake_camera_arrive(uintptr_t component)
{
  assert(component == (uintptr_t)cam_comp_mem);
  cam_arrive_calls++;
}

static void init_camera_chain(uint32_t cam_state_off)
{
  block_scan_query = false;
  memset(cam_holder_mem, 0, sizeof(cam_holder_mem));
  memset(cam_handle_entry, 0, sizeof(cam_handle_entry));
  memset(cam_actor_mem, 0, sizeof(cam_actor_mem));
  memset(cam_comps_mem, 0, sizeof(cam_comps_mem));
  memset(&cam_region, 0, sizeof(cam_region));
  memset(cam_glob_mem, 0, sizeof(cam_glob_mem));
  memset(ai_comp_mem, 0, sizeof(ai_comp_mem));
  memset(ai_ctrl_set_mem, 0, sizeof(ai_ctrl_set_mem));
  memset(ai_entry0, 0, sizeof(ai_entry0));
  memset(ai_entry1, 0, sizeof(ai_entry1));
  memset(ctrl_bad_mem, 0, sizeof(ctrl_bad_mem));
  memset(ctrl_ok_mem, 0, sizeof(ctrl_ok_mem));
  memset(unit_slots_arr, 0, sizeof(unit_slots_arr));
  memset(unit_bad_mem, 0, sizeof(unit_bad_mem));
  memset(unit_ok_mem, 0, sizeof(unit_ok_mem));
  memset(node_slots_arr, 0, sizeof(node_slots_arr));
  memset(node_bad_mem, 0, sizeof(node_bad_mem));
  memset(node_ok_mem, 0, sizeof(node_ok_mem));
  memset(node_ok2_mem, 0, sizeof(node_ok2_mem));

  test_build.player_camera_component = 0x360;
  test_build.player_camera_state = cam_state_off;
  uintptr_t cam_vt = z64hd_main + 0x10000;
  uintptr_t p;

  main_slots.cam_ptr = (uintptr_t)cam_holder_mem;
  main_slots.cam_holder_ptr = (uintptr_t)&main_slots.cam_ptr;
  p = (uintptr_t)cam_handle_entry; memcpy(cam_holder_mem + 0x38 + 8, &p, 8);
  uint32_t seq = 7; memcpy(cam_holder_mem + 0x38 + 0x10, &seq, 4);
  memcpy(cam_handle_entry + 0x48, &seq, 4);
  p = (uintptr_t)cam_actor_mem; memcpy(cam_handle_entry + 0x40, &p, 8);

  p = (uintptr_t)cam_comps_mem; memcpy(cam_actor_mem + 0x270, &p, 8);
  p = (uintptr_t)cam_comp_mem; memcpy(cam_comps_mem + 0x360, &p, 8);
  memcpy(cam_comp_mem, &cam_vt, 8);
  p = (uintptr_t)cam_actor_mem; memcpy(cam_comp_mem + 0x38, &p, 8); /* the actor the camera follows */

  /* Verified AI node chain:
   * actor+0x270 -> +0x48 (AIComponent) -> +0x28 (AIControllerSet) -> entry1(+0x18) -> unit1 -> node_slots (+0x30) */
  p = (uintptr_t)ai_comp_mem; memcpy(cam_comps_mem + 0x48, &p, 8);
  p = (uintptr_t)ai_ctrl_set_mem; memcpy(ai_comp_mem + 0x28, &p, 8);
  uint32_t set_cnt = 2; memcpy(ai_ctrl_set_mem, &set_cnt, 4);
  p = (uintptr_t)ai_entries_arr; memcpy(ai_ctrl_set_mem + 0x08, &p, 8);
  p = (uintptr_t)ai_entry0; memcpy(ai_entries_arr, &p, 8);
  p = (uintptr_t)ai_entry1; memcpy(ai_entries_arr + 8, &p, 8);
  p = (uintptr_t)ctrl_bad_mem; memcpy(ai_entry0 + 0x18, &p, 8);
  p = (uintptr_t)ctrl_ok_mem; memcpy(ai_entry1 + 0x18, &p, 8);

  /* ctrl_bad has wrong actor back-pointer; ctrl_ok matches cam_actor_mem */
  p = (uintptr_t)ctrl_bad_mem; memcpy(ctrl_bad_mem + 0x1F0, &p, 8);
  p = (uintptr_t)cam_actor_mem; memcpy(ctrl_ok_mem + 0x1F0, &p, 8);
  uint32_t unit_cnt = 2; memcpy(ctrl_ok_mem + 0x18, &unit_cnt, 4);
  p = (uintptr_t)unit_slots_arr; memcpy(ctrl_ok_mem + 0x20, &p, 8);
  p = (uintptr_t)unit_bad_mem; memcpy(unit_slots_arr, &p, 8);
  p = (uintptr_t)unit_ok_mem; memcpy(unit_slots_arr + 0x18, &p, 8);

  /* unit_bad has wrong ctrl back-pointer; unit_ok matches ctrl_ok_mem */
  p = (uintptr_t)ctrl_bad_mem; memcpy(unit_bad_mem + 0x08, &p, 8);
  p = (uintptr_t)ctrl_ok_mem; memcpy(unit_ok_mem + 0x08, &p, 8);
  uint32_t node_cnt = 3; memcpy(unit_ok_mem + 0x28, &node_cnt, 4);
  p = (uintptr_t)node_slots_arr; memcpy(unit_ok_mem + 0x30, &p, 8);
  p = (uintptr_t)node_bad_mem; memcpy(node_slots_arr, &p, 8);
  p = (uintptr_t)node_ok_mem; memcpy(node_slots_arr + 0x18, &p, 8);
  p = (uintptr_t)node_ok2_mem; memcpy(node_slots_arr + 2 * 0x18, &p, 8);

  /* node_bad has wrong state ptr; node_ok & node_ok2 share vtable with distinct node indices (+0x1C) */
  uintptr_t bad_state = (uintptr_t)cam_comp_mem;
  uintptr_t ok_state = (uintptr_t)cam_comp_mem + cam_state_off;
  memcpy(node_bad_mem, &cam_vt, 8); memcpy(node_bad_mem + 0x20, &bad_state, 8);
  uintptr_t act_vt = z64hd_main + 0x20000;
  uint16_t idx1 = 10, idx2 = 25;
  memcpy(node_ok_mem, &act_vt, 8); memcpy(node_ok_mem + 0x1C, &idx1, 2); memcpy(node_ok_mem + 0x20, &ok_state, 8);
  memcpy(node_ok2_mem, &act_vt, 8); memcpy(node_ok2_mem + 0x1C, &idx2, 2); memcpy(node_ok2_mem + 0x20, &ok_state, 8);
}

static void register_default_regions(void)
{
  clear_regions();
  add_region(gd_mgr_mem, sizeof(gd_mgr_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(gd_desc0_mem, sizeof(gd_desc0_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(&main_slots, sizeof(main_slots), SYS_PERM_R | SYS_PERM_W);
  add_region(wm_mem, sizeof(wm_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(clock_mem, sizeof(clock_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(clock_vt, sizeof(clock_vt), SYS_PERM_R);
  add_region(navi_scene_mem, sizeof(navi_scene_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(navi_mem, sizeof(navi_mem), SYS_PERM_R | SYS_PERM_W);

  add_region(gd_tbl_int, sizeof(gd_tbl_int), SYS_PERM_R);
  add_region(gd_tbl_bool, sizeof(gd_tbl_bool), SYS_PERM_R);
  add_region(gd_tbl_float, sizeof(gd_tbl_float), SYS_PERM_R);
  add_region(gd_tbl_enum, sizeof(gd_tbl_enum), SYS_PERM_R);
  add_region(gd_tbl_str, sizeof(gd_tbl_str), SYS_PERM_R);
  add_region(gd_tbl_uint, sizeof(gd_tbl_uint), SYS_PERM_R);
  add_region(gd_tbl_vec3, sizeof(gd_tbl_vec3), SYS_PERM_R);

  add_region(gd_obj_int, sizeof(gd_obj_int), SYS_PERM_R | SYS_PERM_W);
  add_region(gd_obj_bool, sizeof(gd_obj_bool), SYS_PERM_R | SYS_PERM_W);
  add_region(gd_obj_float, sizeof(gd_obj_float), SYS_PERM_R | SYS_PERM_W);
  add_region(gd_obj_enum, sizeof(gd_obj_enum), SYS_PERM_R | SYS_PERM_W);
  add_region(gd_obj_str, sizeof(gd_obj_str), SYS_PERM_R | SYS_PERM_W);
  add_region(gd_str_bufs, sizeof(gd_str_bufs), SYS_PERM_R | SYS_PERM_W);
  add_region(gd_obj_uint, sizeof(gd_obj_uint), SYS_PERM_R | SYS_PERM_W);
  add_region(gd_obj_vec3, sizeof(gd_obj_vec3), SYS_PERM_R | SYS_PERM_W);

  add_region(cam_holder_mem, sizeof(cam_holder_mem), SYS_PERM_R);
  add_region(cam_handle_entry, sizeof(cam_handle_entry), SYS_PERM_R);
  add_region(cam_actor_mem, sizeof(cam_actor_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(cam_comps_mem, sizeof(cam_comps_mem), SYS_PERM_R);
  add_region(&cam_region, sizeof(cam_region), SYS_PERM_R | SYS_PERM_W);
  add_region(cam_glob_mem, sizeof(cam_glob_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(ai_comp_mem, sizeof(ai_comp_mem), SYS_PERM_R);
  add_region(ai_ctrl_set_mem, sizeof(ai_ctrl_set_mem), SYS_PERM_R);
  add_region(ai_entries_arr, sizeof(ai_entries_arr), SYS_PERM_R);
  add_region(ai_entry0, sizeof(ai_entry0), SYS_PERM_R);
  add_region(ai_entry1, sizeof(ai_entry1), SYS_PERM_R);
  add_region(ctrl_bad_mem, sizeof(ctrl_bad_mem), SYS_PERM_R);
  add_region(ctrl_ok_mem, sizeof(ctrl_ok_mem), SYS_PERM_R);
  add_region(unit_slots_arr, sizeof(unit_slots_arr), SYS_PERM_R);
  add_region(unit_bad_mem, sizeof(unit_bad_mem), SYS_PERM_R);
  add_region(unit_ok_mem, sizeof(unit_ok_mem), SYS_PERM_R);
  add_region(node_slots_arr, sizeof(node_slots_arr), SYS_PERM_R);
  add_region(node_bad_mem, sizeof(node_bad_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(node_ok_mem, sizeof(node_ok_mem), SYS_PERM_R | SYS_PERM_W);
  add_region(node_ok2_mem, sizeof(node_ok2_mem), SYS_PERM_R | SYS_PERM_W);
}

static void init_env(uint32_t cam_state_off)
{
  for (int i = 0; i < MEMFILE_SLOTS; i++)
    free_memfile(&memfiles[i]);
  free_memfile(&staging);
  free(inflight_save_buf);
  inflight_save_buf = 0;
  phase = L_IDLE;
  slot = active_slot = phase_frames = quiet_frames = camera_frames = 0;
  memfile_session = memfile_fast_reload = true;
  last_notice[0] = status[0] = 0;

  uintptr_t fn_lo = (uintptr_t)fake_clock_get;
  uintptr_t dat_lo = (uintptr_t)&main_slots;
  uintptr_t lo = fn_lo < dat_lo ? fn_lo : dat_lo;
  z64hd_main = lo - 0x4000u;
  assert(fn_lo >= z64hd_main && fn_lo - z64hd_main < MAIN_CODE_SIZE);

  test_build = (struct z64hd_build){
    .gd_request_save = OFF_GD_REQ_SAVE,
    .gd_request_load = OFF_GD_REQ_LOAD,
    .wm_ptr = (uint32_t)((uintptr_t)&main_slots.wm_holder_ptr - z64hd_main),
    .wm_set_time = OFF_WM_SET_TIME,
    .wm_on_save = OFF_WM_ON_SAVE,
    .wm_on_load = OFF_WM_ON_LOAD,
    .navi_scene_ptr = (uint32_t)((uintptr_t)&main_slots.navi_scene_holder_ptr - z64hd_main),
    .navi_get = OFF_NAVI_GET,
    .navi_on_load = OFF_NAVI_ON_LOAD,
    .player_camera_holder = (uint32_t)((uintptr_t)&main_slots.cam_holder_ptr - z64hd_main),
    .player_camera_snap = OFF_CAMERA_SNAP,
    .player_camera_arrive = OFF_CAMERA_ARRIVE,
  };

  /* WorldMgr + Clock + NaviMgr */
  memset(wm_mem, 0, sizeof(wm_mem));
  memset(clock_mem, 0, sizeof(clock_mem));
  memset(clock_vt, 0, sizeof(clock_vt));
  main_slots.wm_ptr = (uintptr_t)wm_mem;
  main_slots.wm_holder_ptr = (uintptr_t)&main_slots.wm_ptr;
  uintptr_t p = (uintptr_t)clock_mem; memcpy(wm_mem + WM_CLOCK, &p, 8);
  p = (uintptr_t)clock_vt; memcpy(clock_mem, &p, 8);
  p = (uintptr_t)fake_clock_get; memcpy(clock_vt + CLOCK_GET_TIME, &p, 8);
  main_slots.navi_scene_ptr = (uintptr_t)navi_scene_mem;
  main_slots.navi_scene_holder_ptr = (uintptr_t)&main_slots.navi_scene_ptr;

  /* GameDataMgr + desc0 + typed fields */
  memset(gd_mgr_mem, 0, sizeof(gd_mgr_mem));
  memset(gd_desc0_mem, 0, sizeof(gd_desc0_mem));
  uint32_t count1 = 1; memcpy(gd_mgr_mem + GD_MGR_FILE_COUNT, &count1, 4);
  p = (uintptr_t)gd_desc0_mem; memcpy(gd_mgr_mem + GD_MGR_FILES_PTR, &p, 8);
  uint32_t buf_cap = 1024; memcpy(gd_desc0_mem + GD_DESC_BUF_SIZE, &buf_cap, 4);

  memset(gd_tbl_int, 0, sizeof(gd_tbl_int));
  memset(gd_tbl_bool, 0, sizeof(gd_tbl_bool));
  memset(gd_tbl_float, 0, sizeof(gd_tbl_float));
  memset(gd_tbl_enum, 0, sizeof(gd_tbl_enum));
  memset(gd_tbl_str, 0, sizeof(gd_tbl_str));
  memset(gd_tbl_uint, 0, sizeof(gd_tbl_uint));
  memset(gd_tbl_vec3, 0, sizeof(gd_tbl_vec3));
  memset(gd_obj_int, 0, sizeof(gd_obj_int));
  memset(gd_obj_bool, 0, sizeof(gd_obj_bool));
  memset(gd_obj_float, 0, sizeof(gd_obj_float));
  memset(gd_obj_enum, 0, sizeof(gd_obj_enum));
  memset(gd_obj_str, 0, sizeof(gd_obj_str));
  memset(gd_str_bufs, 0, sizeof(gd_str_bufs));
  memset(gd_obj_uint, 0, sizeof(gd_obj_uint));
  memset(gd_obj_vec3, 0, sizeof(gd_obj_vec3));

  type_int   = (gd_type_t){ 1, 4, 0, (uint8_t *)gd_obj_int,   gd_tbl_int,   0, 8 };
  type_bool  = (gd_type_t){ 2, 8, 0, (uint8_t *)gd_obj_bool,  gd_tbl_bool,  0, 8 };
  type_float = (gd_type_t){ 3, 1, 0, (uint8_t *)gd_obj_float, gd_tbl_float, 0, 4 };
  type_enum  = (gd_type_t){ 4, 1, 0, (uint8_t *)gd_obj_enum,  gd_tbl_enum,  0, 4 };
  type_str   = (gd_type_t){ 5, 6, 0, (uint8_t *)gd_obj_str,   gd_tbl_str,   0, 8 };

  gd_type_t *type_uint = gd_type_at(GD_UINT_TYPE);
  gd_type_t *type_vec3 = gd_type_at(GD_VEC3_TYPE);
  *type_uint = (gd_type_t){ 6, 2, 0, (uint8_t *)gd_obj_uint, gd_tbl_uint, 0, 4 };
  *type_vec3 = (gd_type_t){ 7, 2, 0, (uint8_t *)gd_obj_vec3, gd_tbl_vec3, 0, 4 };

  add_gd_entry(&type_int, 0x28, 0, F_MAGIC, 0);
  add_gd_entry(&type_int, 0x28, 1, F_RESET_GIMMICK_OPT, -1);
  add_gd_entry(&type_int, 0x28, 2, H_SESSION_INT, -1);
  add_gd_entry(&type_int, 0x28, 3, 0x11111111u, 0); /* dummy saved int */

  add_gd_entry(&type_bool, 0x18, 0, F_IS_LOADING_BANC, -1);
  add_gd_entry(&type_bool, 0x18, 1, F_IS_CHILDHOOD, 0);
  add_gd_entry(&type_bool, 0x18, 2, F_REQ_RESET_GIMMICK, -1);
  add_gd_entry(&type_bool, 0x18, 3, F_ALREADY_RESET_LOAD, -1);
  add_gd_entry(&type_bool, 0x18, 4, F_SET_CHILD_MANUALLY, -1);
  add_gd_entry(&type_bool, 0x18, 5, F_OVERRIDE_TRAVERSE, -1);
  add_gd_entry(&type_bool, 0x18, 6, F_IS_GAME_OVER, -1);
  add_gd_entry(&type_bool, 0x18, 7, H_SESSION_BOOL, -1);

  for (uint32_t i = 0; i < 6; i++) {
    char *sptr = gd_str_bufs[i];
    uint32_t cap = GD_STR_LEN;
    memcpy(gd_obj_str[i] + Z64HD_STR_VALUE, &sptr, sizeof(sptr));
    memcpy(gd_obj_str[i] + Z64HD_STR_VALUE + sizeof(uintptr_t), &cap, sizeof(cap));
  }
  add_gd_entry(&type_str, Z64HD_STR_STRIDE, 0, GD_STR_MAP, 0);
  add_gd_entry(&type_str, Z64HD_STR_STRIDE, 1, GD_STR_SPAWN, 0);
  add_gd_entry(&type_str, Z64HD_STR_STRIDE, 2, GD_STR_EVENT, -1);
  add_gd_entry(&type_str, Z64HD_STR_STRIDE, 3, GD_STR_STARTER, -1);
  add_gd_entry(&type_str, Z64HD_STR_STRIDE, 4, GD_STR_NEXT_MAP, -1);
  add_gd_entry(&type_str, Z64HD_STR_STRIDE, 5, H_SESSION_STR, -1);

  add_gd_entry(type_uint, GD_UINT_STRIDE, 0, GD_UINT_KNOWN, 0);
  add_gd_entry(type_uint, GD_UINT_STRIDE, 1, H_SESSION_UINT, -1);
  add_gd_entry(type_vec3, GD_VEC3_STRIDE, 0, GD_VEC3_KNOWN, 0);
  add_gd_entry(type_vec3, GD_VEC3_STRIDE, 1, F_OVERRIDE_TRAVERSE_POS, -1);

  init_camera_chain(cam_state_off);
  register_default_regions();

  sim_link_actor = (uintptr_t)cam_actor_mem; /* non-zero dummy */
  sim_hp = 12; sim_max_hp = 20;
  sim_pos = (vec3f_t){ 1, 2, 3 };
  sim_fx = 0; sim_fz = 1;
  sim_time = 21600;
  sim_teleport_ok = true;
  sim_warp_pending = sim_warp_requested = sim_warp_abandoned = sim_ev_running = false;
  sim_silent_warp_accept = true;
  allow_req_save = allow_req_load = true;
  wm_save_calls = wm_load_calls = navi_load_calls = gear_calls = 0;
  req_save_calls = req_load_calls = teleport_calls = warp_silent_calls = warp_cancel_calls = cam_snap_calls = cam_arrive_calls = 0;
  gd_set_str(GD_STR_MAP, "HyruleField_Adult");
  gd_set_str(GD_STR_SPAWN, "StartPos_00");
  gd_poke_int(F_MAGIC, 48);
  gd_set_bool(F_IS_CHILDHOOD, false);
}

static void complete_mock_save(uint32_t magic, uint32_t written, const char *payload_tag)
{
  assert(last_req_save_buf);
  memcpy(last_req_save_buf, &magic, 4);
  if (payload_tag)
    snprintf((char *)last_req_save_buf + 8, 64, "%s", payload_tag);
  memcpy(gd_desc0_mem + GD_DESC_WRITTEN, &written, 4);
  gd_desc0_mem[GD_DESC_FLAGS] = 0;
  gd_mgr_mem[GD_MGR_SYNC_BUSY] = 0;
  int32_t zero = 0;
  memcpy(gd_mgr_mem + GD_MGR_ACTIVE_SAVES, &zero, 4);
}

static void complete_mock_load(bool ok)
{
  gd_desc0_mem[GD_DESC_FLAGS] = 0;
  gd_desc0_mem[GD_DESC_LOAD_OK] = (uint8_t)(ok ? 1 : 0);
}

/* --- Test 1: async save & staging isolation ---------------------------------------------------- */

static void test_async_save_and_staging(void)
{
  init_env(0x5B8);
  gd_poke_int(H_SESSION_INT, 77);
  gd_set_bool(H_SESSION_BOOL, true);
  gd_set_str(H_SESSION_STR, "sess_val_1");
  uint32_t u = 0xCAFEBABEu;
  memcpy(gd_obj_uint[1] + GD_UINT_VALUE, &u, 4);

  /* Busy first -> L_SAVING_REQ retries until accepted */
  allow_req_save = false;
  assert(memfile_save(0) && phase == L_SAVING_REQ && memfile_busy());
  memfile_frame();
  assert(phase == L_SAVING_REQ && req_save_calls == 2);
  allow_req_save = true;
  memfile_frame();
  assert(phase == L_SAVING_WAIT && wm_save_calls == 1);

  complete_mock_save(GD_SAVE_MAGIC, 128, "SLOT0_V1");
  memfile_frame();
  assert(phase == L_IDLE && memfiles[0].used && memfiles[0].save_size == 128);
  assert(strcmp((const char *)(memfiles[0].save_buf + 8), "SLOT0_V1") == 0);
  uint8_t *committed_v1_buf = memfiles[0].save_buf;

  /* Bad magic on overwrite must preserve slot 0 intact */
  assert(memfile_save(0) && phase == L_SAVING_WAIT);
  complete_mock_save(0xDEADBEEFu, 128, "BAD_MAGIC");
  memfile_frame();
  assert(phase == L_IDLE && memfiles[0].used && memfiles[0].save_buf == committed_v1_buf);
  assert(strcmp((const char *)(memfiles[0].save_buf + 8), "SLOT0_V1") == 0);
  assert(!staging.save_buf && !inflight_save_buf);

  /* Timeout while worker still owns buffer -> parked in inflight_save_buf, never freed early */
  assert(memfile_save(0) && phase == L_SAVING_WAIT);
  gd_desc0_mem[GD_DESC_FLAGS] = GD_FLAG_SAVE_WORK;
  for (int i = 0; i <= SAVE_WAIT_TIMEOUT + 1; i++)
    memfile_frame();
  assert(phase == L_IDLE && inflight_save_buf != 0);
  assert(memfiles[0].save_buf == committed_v1_buf);
  assert(!memfile_save(0)); /* blocked while inflight worker holds buffer */

  /* Worker finishes -> retired automatically on next frame */
  gd_desc0_mem[GD_DESC_FLAGS] = 0;
  memfile_frame();
  assert(inflight_save_buf == 0);
  assert(memfile_save(0));
  complete_mock_save(GD_SAVE_MAGIC, 96, "SLOT0_V2");
  memfile_frame();
  assert(phase == L_IDLE && memfiles[0].save_size == 96);

  puts("PASS: async save retry, staging preservation on failure, inflight worker buffer retirement");
}

/* --- Test 2: full reload touches zero live GameData before warp accepted/started --------------- */

static void test_full_reload_no_early_corruption(void)
{
  init_env(0x5B8);
  assert(memfile_save(0));
  complete_mock_save(GD_SAVE_MAGIC, 128, "SAVED");
  memfile_frame();
  assert(memfiles[0].used);

  /* Move to different map with distinct live values */
  gd_set_str(GD_STR_MAP, "Market_Child");
  gd_poke_int(H_SESSION_INT, 999);

  /* Case A: event request immediately refused */
  sim_silent_warp_accept = false;
  assert(!memfile_load(0) && phase == L_IDLE && req_load_calls == 0);
  int32_t val = 0;
  assert(gd_get_int(H_SESSION_INT, &val) && val == 999);
  bool fl = true;
  assert(gd_get_bool(F_ALREADY_RESET_LOAD, &fl) && !fl);
  assert(gd_get_bool(F_OVERRIDE_TRAVERSE, &fl) && !fl);
  assert(gd_get_bool(F_SET_CHILD_MANUALLY, &fl) && !fl);

  /* Case B: event request accepted but reservation times out */
  sim_silent_warp_accept = true;
  assert(memfile_load(0) && phase == L_WAIT_LOADING);
  sim_warp_requested = false;
  sim_warp_pending = false;
  sim_warp_abandoned = true;
  memfile_frame();
  assert(phase == L_IDLE && req_load_calls == 0 && warp_cancel_calls == 0);
  assert(gd_get_int(H_SESSION_INT, &val) && val == 999);
  assert(gd_get_bool(F_OVERRIDE_TRAVERSE, &fl) && !fl);

  /* Case C: reservation accepted but loading never starts within LOAD_START_TIMEOUT */
  assert(memfile_load(0) && phase == L_WAIT_LOADING);
  sim_warp_pending = false; /* reservation finished, still haven't entered loading screen */
  for (int i = 0; i <= LOAD_START_TIMEOUT + 1; i++)
    memfile_frame();
  assert(phase == L_IDLE && req_load_calls == 0);
  assert(gd_get_int(H_SESSION_INT, &val) && val == 999);
  assert(gd_get_bool(F_OVERRIDE_TRAVERSE, &fl) && !fl);

  puts("PASS: full reload refusal/timeout touches zero live GameData and clears overrides");
}

/* --- Test 3: full reload happy path ------------------------------------------------------------ */

static void test_full_reload_happy_path(void)
{
  init_env(0x5B8);
  gd_set_str(GD_STR_MAP, "DekuTree");
  gd_set_str(GD_STR_SPAWN, "StartPos_01");
  gd_set_bool(F_IS_CHILDHOOD, true);
  sim_pos = (vec3f_t){ 10.0f, 20.0f, 30.0f };
  sim_fx = 1.0; sim_fz = 0.0;
  sim_hp = 16; sim_max_hp = 20;
  gd_poke_int(F_MAGIC, 32);
  sim_time = 36000;
  gd_poke_int(H_SESSION_INT, 321);
  gd_set_str(H_SESSION_STR, "deku_talked");
  uint8_t *cam_st = cam_comp_mem + 0x5B8;
  float saved_eye[3] = { 12.0f, 22.0f, 35.0f };
  memcpy(cam_st + 0x450, saved_eye, 12);
  memcpy(cam_glob_mem + 0x38, saved_eye, 12);

  assert(memfile_save(0));
  complete_mock_save(GD_SAVE_MAGIC, 256, "DEKU_SAVE");
  memfile_frame();
  assert(memfiles[0].used && memfiles[0].child == 1);

  /* Change current game state so full reload is chosen */
  uintptr_t old_link = (uintptr_t)cam_actor_mem;
  sim_link_actor = old_link;
  gd_set_str(GD_STR_MAP, "HyruleField_Child");
  gd_set_str(GD_STR_EVENT, "OldBootEvent");
  gd_set_str(GD_STR_STARTER, "OldStarter");
  gd_poke_int(H_SESSION_INT, 0);
  gd_set_str(H_SESSION_STR, "");
  sim_hp = 4; gd_poke_int(F_MAGIC, 0);
  sim_pos = (vec3f_t){ 0, 0, 0 }; sim_fx = 0; sim_fz = 1;
  sim_time = 0;

  assert(memfile_load(0) && phase == L_WAIT_LOADING && warp_silent_calls == 1);
  /* Reservation completes */
  sim_warp_pending = false;
  memfile_frame();
  bool b = false;
  assert(gd_get_bool(F_ALREADY_RESET_LOAD, &b) && b);
  assert(gd_get_bool(F_SET_CHILD_MANUALLY, &b) && b);
  assert(gd_get_bool(F_OVERRIDE_TRAVERSE, &b) && b);
  float ov_pos[3] = { 0 };
  read_vec3(F_OVERRIDE_TRAVERSE_POS, ov_pos);
  assert(ov_pos[0] == 10.0f && ov_pos[1] == 20.0f && ov_pos[2] == 30.0f);
  assert(strcmp(gd_get_str(GD_STR_EVENT), "") == 0);
  assert(strcmp(gd_get_str(GD_STR_STARTER), "") == 0);
  assert(req_load_calls == 0); /* still wait for loading */

  /* Engine starts loading -> GD load submitted */
  gd_set_bool(F_IS_LOADING_BANC, true);
  sim_link_actor = 0;
  memfile_frame();
  assert(req_load_calls == 1 && phase == L_FULL_WAIT_GD);

  /* Still pending 1 frame */
  memfile_frame();
  assert(phase == L_FULL_WAIT_GD);

  /* Deserializer completes & restores saved map/scalars */
  gd_set_str(GD_STR_MAP, "DekuTree");
  gd_set_bool(F_IS_CHILDHOOD, true);
  gd_poke_int(F_MAGIC, 32);
  complete_mock_load(true);
  memfile_frame();
  assert(phase == L_WAIT_ARRIVAL);
  int32_t si = 0;
  assert(gd_get_int(H_SESSION_INT, &si) && si == 321);
  assert(strcmp(gd_get_str(H_SESSION_STR), "deku_talked") == 0);
  assert(wm_load_calls == 1 && navi_load_calls == 1 && sim_time == 36000);

  /* Map finishes loading with new actor */
  gd_set_bool(F_IS_LOADING_BANC, false);
  sim_link_actor = old_link + 8; /* note: must remain in mapped memory? memfile.c checks actor+0x270 in camera only, or wait_arrival checks link_actor() != old_actor */
  memfile_frame();
  assert(phase == L_WAIT_SETTLE);
  assert(gd_get_bool(F_OVERRIDE_TRAVERSE, &b) && !b);
  assert(gd_get_bool(F_SET_CHILD_MANUALLY, &b) && !b);
  assert(gd_get_bool(F_ALREADY_RESET_LOAD, &b) && !b);

  /* QUIET_FRAMES */
  for (int i = 0; i < QUIET_FRAMES; i++)
    memfile_frame();
  assert(phase == L_MOVING && teleport_calls == 1 && gear_calls == 1 && sim_hp == 16);

  /* Advance 2 frames in L_MOVING -> reaches L_CAMERA */
  memfile_frame();
  assert(phase == L_MOVING); /* frame 1 rejected */
  memfile_frame();
  assert(phase == L_CAMERA); /* frame 2 accepted */

  /* Camera phase */
  sim_link_actor = (uintptr_t)cam_actor_mem; /* back to mapped actor for camera chain */
  memcpy(cam_st + 0x450, saved_eye, 12);
  for (int i = 0; i < CAMERA_WRITE_FRAMES; i++)
    memfile_frame();
  /* written; then watched, since a fresh map can turn the camera back to its default */
  assert(phase == L_CAMERA && camera_guard == 1 && status[0] == 0 && cam_snap_calls == CAMERA_WRITE_FRAMES);
  assert(cam_arrive_calls == 0); /* a map's arrival did it already */
  memfile_frame();
  assert(phase == L_CAMERA && camera_guard == 2); /* still the saved angle: nothing to do */

  /* the map turns it 90 degrees: put back again at once, the writes run again */
  float turned_eye[3] = { saved_eye[2], saved_eye[1], -saved_eye[0] };
  memcpy(cam_glob_mem + 0x38, turned_eye, 12);
  memfile_frame();
  assert(phase == L_CAMERA && camera_guard == 0 && camera_reapplies == 1 && camera_frames == 1);
  assert(cam_snap_calls == CAMERA_WRITE_FRAMES + 1);
  memcpy(cam_glob_mem + 0x38, saved_eye, 12); /* the writes took */
  for (int i = 1; i < CAMERA_WRITE_FRAMES; i++)
    memfile_frame();
  assert(phase == L_CAMERA && camera_guard == 1 && cam_snap_calls == 2 * CAMERA_WRITE_FRAMES);
  for (int i = 0; i < CAMERA_GUARD; i++)
    memfile_frame();
  assert(phase == L_IDLE && camera_reapplies == 1 && cam_snap_calls == 2 * CAMERA_WRITE_FRAMES);

  puts("PASS: silent full reload happy path through GD, session, world/navi, link, and camera");
}

/* --- Test 4: fast same-map ResetGimmick path + childhood parity + fallback triggers ------------- */

static void test_fast_same_map_reset_gimmick(void)
{
  for (int child_flag = 0; child_flag <= 1; child_flag++) {
    init_env(0x5B8);
    gd_set_str(GD_STR_MAP, "KokiriForest");
    gd_set_bool(F_IS_CHILDHOOD, child_flag != 0);
    sim_pos = (vec3f_t){ 5.0f, 1.0f, -4.0f };
    sim_fx = 0.0; sim_fz = 1.0;
    sim_hp = 14; sim_max_hp = 20;
    gd_poke_int(F_MAGIC, 24);
    gd_poke_int(H_SESSION_INT, 555);
    sim_time = 43200;

    assert(memfile_save(0));
    complete_mock_save(GD_SAVE_MAGIC, 200, "FAST_SLOT");
    memfile_frame();
    assert(memfiles[0].used && memfiles[0].child == child_flag);

    /* Mutate live state on same map & same age */
    sim_pos = (vec3f_t){ 99.0f, 50.0f, 99.0f };
    sim_hp = 6;
    gd_poke_int(F_MAGIC, 4);
    gd_poke_int(H_SESSION_INT, 1);
    sim_time = 1000;

    assert(memfile_load(0) && phase == L_FAST_WAIT_GD && warp_silent_calls == 0);
    gd_poke_int(F_MAGIC, 24);
    complete_mock_load(true);
    memfile_frame();
    assert(phase == L_FAST_MOVE_PRE && wm_load_calls == 1 && navi_load_calls == 1);
    assert(gear_calls == 1 && sim_hp == 14 && sim_time == 43200);
    int32_t si = 0;
    assert(gd_get_int(H_SESSION_INT, &si) && si == 555);

    /* Wait 2 frames in L_FAST_MOVE_PRE -> triggers ResetGimmick */
    memfile_frame();
    assert(phase == L_FAST_MOVE_PRE);
    memfile_frame();
    assert(phase == L_FAST_WAIT_RESET);
    bool req = false;
    int32_t opt = -1;
    assert(gd_get_bool(F_REQ_RESET_GIMMICK, &req) && req);
    assert(gd_get_int(F_RESET_GIMMICK_OPT, &opt) && opt == 0);

    /* Simulate ResetGimmickImpl briefly nudging MP before clearing req */
    gd_poke_int(F_MAGIC, 0);
    memfile_frame();
    gd_set_bool(F_REQ_RESET_GIMMICK, false);
    memfile_frame();
    /* L_FAST_WAIT_RESET must re-apply vitals/gear and enter L_MOVING */
    int32_t mp = 0;
    assert(gd_get_int(F_MAGIC, &mp) && mp == 24);
    assert(gear_calls == 2 && phase == L_MOVING);

    memfile_frame();
    memfile_frame();
    assert(phase == L_CAMERA);
    /* in place: no handover to wait for, so the writes start at once (the drawn eye isn't set up here) */
    for (int i = 0; i < CAMERA_WRITE_FRAMES - 1; i++)
      memfile_frame();
    assert(phase == L_CAMERA);
    memfile_frame();
    assert(phase == L_IDLE && cam_arrive_calls == 1); /* in place: the target reset as on arrival, once */
    cam_arrive_calls = 0;

    /* Verify fallback to full reload for all 5 disqualifiers */
    for (int why = 0; why < 5; why++) {
      phase = L_IDLE;
      memfile_fast_reload = true;
      gd_set_str(GD_STR_MAP, "KokiriForest");
      gd_set_bool(F_IS_CHILDHOOD, child_flag != 0);
      gd_set_bool(F_IS_GAME_OVER, false);
      sim_ev_running = false;
      sim_warp_pending = sim_warp_requested = sim_warp_abandoned = false;
      sim_hp = 12;
      warp_silent_calls = 0;
      if (why == 0) gd_set_bool(F_IS_CHILDHOOD, child_flag == 0);
      if (why == 1) sim_ev_running = true;
      if (why == 2) memfile_fast_reload = false;
      if (why == 3) gd_set_bool(F_IS_GAME_OVER, true);
      if (why == 4) sim_hp = 0;
      assert(memfile_load(0) && phase == L_WAIT_LOADING && warp_silent_calls == 1);
    }
  }

  puts("PASS: fast same-map ResetGimmick (child & adult) and all 5 full-reload fallback guards");
}

/* --- Test 5: link_at frame delay, position tolerance, facing dot & fallback -------------------- */

static void test_link_at_tolerance_and_facing(void)
{
  init_env(0x5B8);
  memfile_t st = { .x = 10.0, .y = 20.0, .z = 30.0, .fx = 0.0, .fz = 1.0 };

  sim_pos = (vec3f_t){ 10.0f, 20.0f, 30.0f };
  sim_fx = 0.0; sim_fz = 1.0;
  assert(!link_at(&st, 0));
  assert(!link_at(&st, 1)); /* always rejects frame 1 */
  assert(link_at(&st, 2));

  /* Wrong yaw rejected before frame 12, accepted on frame >= 12 */
  sim_fx = 1.0; sim_fz = 0.0;
  assert(!link_at(&st, 2));
  assert(!link_at(&st, 11));
  assert(link_at(&st, 12));

  /* Near boundary within 0.25u & dot >= 0.98 */
  sim_pos = (vec3f_t){ 10.24f, 19.76f, 30.24f };
  sim_fx = 0.1; sim_fz = 0.994987; /* dot = 0.995 >= 0.98 */
  assert(link_at(&st, 2));
  sim_pos = (vec3f_t){ 10.26f, 20.0f, 30.0f };
  assert(!link_at(&st, 2));
  assert(!link_at(&st, 12));

  /* Missing saved facing ignores yaw check */
  st.fx = 0.0; st.fz = 0.0;
  sim_pos = (vec3f_t){ 10.0f, 20.0f, 30.0f };
  sim_fx = -1.0; sim_fz = 0.0;
  assert(link_at(&st, 2));

  puts("PASS: link_at frame gate, 0.25u tolerance, yaw dot check, and frame-12 fallback");
}

/* --- Test 6: direct camera AI action chain, fallback scan & non-0x5B8 state offset ------------ */

static void test_camera_ai_chain(void)
{
  const uint32_t state_off = 0x600;
  init_env(state_off);

  /* Populate distinct plain floats in both same-vtable nodes and state+CAMERA_TURN_OFF (0x600+0x7F0) */
  uint8_t *turn = cam_comp_mem + state_off + CAMERA_TURN_OFF;
  for (uint32_t o = 0; o < CAMERA_TURN_BYTES; o += 4) {
    float v = 10.0f + (float)(o / 4);
    memcpy(turn + o, &v, 4);
  }
  for (size_t r = 0; r < sizeof(action_ranges) / sizeof(*action_ranges); r++) {
    for (uint32_t o = action_ranges[r].begin; o < action_ranges[r].end; o += 4) {
      float v1 = 100.0f + (float)o;
      float v2 = 200.0f + (float)o;
      memcpy(node_ok_mem + o, &v1, 4);
      memcpy(node_ok2_mem + o, &v2, 4);
    }
  }

  memfile_t snap = { 0 };
  snap.have_camera = camera_take(snap.camera);
  snap.have_component = camera_take_component(&snap);
  camera_take_actions(&snap);
  assert(snap.have_camera && snap.have_component && snap.camera_actions == 2);
  assert(snap.action_node_idx[0] == 10 && snap.action_node_idx[1] == 25);

  /* Corrupt live component turn + both node floats to different plain floats, then restore */
  for (uint32_t o = 0; o < CAMERA_TURN_BYTES; o += 4) {
    float z = 999.0f;
    memcpy(turn + o, &z, 4);
  }
  for (size_t r = 0; r < sizeof(action_ranges) / sizeof(*action_ranges); r++) {
    for (uint32_t o = action_ranges[r].begin; o < action_ranges[r].end; o += 4) {
      float z = -555.0f;
      memcpy(node_ok_mem + o, &z, 4);
      memcpy(node_ok2_mem + o, &z, 4);
    }
  }

  camera_put_component(&snap);
  camera_put_actions(&snap, true);
  assert(camera_put(snap.camera) && cam_snap_calls == 1);

  for (uint32_t o = 0; o < CAMERA_TURN_BYTES; o += 4) {
    float v = 0;
    memcpy(&v, turn + o, 4);
    assert(v == 10.0f + (float)(o / 4));
  }
  for (size_t r = 0; r < sizeof(action_ranges) / sizeof(*action_ranges); r++) {
    for (uint32_t o = action_ranges[r].begin; o < action_ranges[r].end; o += 4) {
      float v1 = 0, v2 = 0;
      memcpy(&v1, node_ok_mem + o, 4);
      memcpy(&v2, node_ok2_mem + o, 4);
      assert(v1 == 100.0f + (float)o);
      assert(v2 == 200.0f + (float)o);
    }
  }

  /* Safety against corrupted back-pointers / unmapped nodes */
  uintptr_t out_actions[CAMERA_ACTIONS];
  uintptr_t wrong = 0x1234;
  memcpy(unit_ok_mem + 0x08, &wrong, 8);
  assert(camera_find_actions(out_actions) == 0);
  uintptr_t ok_ctrl = (uintptr_t)ctrl_ok_mem;
  memcpy(unit_ok_mem + 0x08, &ok_ctrl, 8);
  assert(camera_find_actions(out_actions) == 2);
  memcpy(ctrl_ok_mem + 0x1F0, &wrong, 8);
  assert(camera_find_actions(out_actions) == 0);

  /* Fallback bounded scan finds an action in cam_region when direct AIComponent pointer is missing */
  memset(cam_comps_mem + 0x48, 0, 8);
  uintptr_t act_vt = z64hd_main + 0x20000;
  uintptr_t ok_state = (uintptr_t)cam_comp_mem + state_off;
  uint16_t scan_idx = 42;
  memcpy(cam_region.scan_node, &act_vt, 8);
  memcpy(cam_region.scan_node + 0x1C, &scan_idx, 2);
  memcpy(cam_region.scan_node + 0x20, &ok_state, 8);
  for (size_t r = 0; r < sizeof(action_ranges) / sizeof(*action_ranges); r++) {
    for (uint32_t o = action_ranges[r].begin; o < action_ranges[r].end; o += 4) {
      float v = 300.0f + (float)o;
      memcpy(cam_region.scan_node + o, &v, 4);
    }
  }
  assert(camera_find_actions(out_actions) == 1 && out_actions[0] == (uintptr_t)cam_region.scan_node);
  memfile_t scan_snap = { 0 };
  camera_take_actions(&scan_snap);
  assert(scan_snap.camera_actions == 1 && scan_snap.action_node_idx[0] == 42);
  for (size_t r = 0; r < sizeof(action_ranges) / sizeof(*action_ranges); r++) {
    for (uint32_t o = action_ranges[r].begin; o < action_ranges[r].end; o += 4) {
      float z = -777.0f;
      memcpy(cam_region.scan_node + o, &z, 4);
    }
  }
  camera_put_actions(&scan_snap, true);
  for (size_t r = 0; r < sizeof(action_ranges) / sizeof(*action_ranges); r++) {
    for (uint32_t o = action_ranges[r].begin; o < action_ranges[r].end; o += 4) {
      float v = 0;
      memcpy(&v, cam_region.scan_node + o, 4);
      assert(v == 300.0f + (float)o);
    }
  }
  /* Scan rejects unavailable / read-only query cleanly */
  block_scan_query = true;
  assert(camera_find_actions(out_actions) == 0);
  block_scan_query = false;

  puts("PASS: camera AI node traversal, node-idx match, scan fallback, non-0x5B8 turn, and safety");
}

int main(void)
{
  test_async_save_and_staging();
  test_full_reload_no_early_corruption();
  test_full_reload_happy_path();
  test_fast_same_map_reset_gimmick();
  test_link_at_tolerance_and_facing();
  test_camera_ai_chain();
  return 0;
}
