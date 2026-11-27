/* Remote "frametrace": one frame of the game's NVN command stream, from one present to the next, to the log: each
 * render target change (its colour and depth textures' sizes and formats), how many draws and dispatches follow it,
 * the end of each command buffer's recording and each queue submit. For finding where the game's 3D picture ends and
 * its HUD starts (hitboxes under the HUD). Debugging only. */
#include "frametrace.h"

#include <stdint.h>

#include <stdio.h>
#include <string.h>

#include "sdfs.h"
#include "sys.h"

typedef void (*set_rt_t)(void *cmd, int n, const nvn_texture_t *const *colors, const void *const *views,
                         const nvn_texture_t *depth, const void *depth_view);
typedef void (*draw_t)(void *cmd, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
typedef nvn_handle_t (*end_t)(void *cmd);
typedef void (*submit_t)(nvn_queue_t *q, int n, const nvn_handle_t *handles);
typedef int (*tex_int_t)(const nvn_texture_t *t);

enum { E_RT, E_DRAW, E_END, E_SUBMIT, E_CALL };
#define MAX_EVENTS 4096
static struct {
  uint8_t type;
  uint8_t n_colors;
  uintptr_t cmd;
  uint64_t a;          /* E_RT colour 0, E_END handle, E_SUBMIT first handle */
  uint32_t w, h, fmt;  /* colour 0's */
  uint32_t dw, dh, dfmt;
  uint32_t draws;      /* E_RT: draws after it on this command buffer */
  uint32_t n;          /* E_SUBMIT: handles */
  uint64_t handles[6]; /* E_SUBMIT: the first few */
} ev[MAX_EVENTS];
static volatile int n_ev;
static volatile int state; /* 0 off, 1 armed (starts at the next present), 2 recording, 3 to be written */
static uintptr_t presented_texture;

static set_rt_t real_set_rt;
static draw_t real_draw[8];
static const char *const draw_names[8] = {
  "nvnCommandBufferDrawArrays", "nvnCommandBufferDrawElements", "nvnCommandBufferDrawElementsBaseVertex",
  "nvnCommandBufferDrawArraysInstanced", "nvnCommandBufferDrawElementsInstanced", "nvnCommandBufferDrawArraysIndirect",
  "nvnCommandBufferDrawElementsIndirect", "nvnCommandBufferDispatchCompute",
};
static end_t real_end;
static submit_t real_submit;
static tex_int_t tex_w, tex_h, tex_fmt;

static int add(void)
{
  int i = __atomic_fetch_add(&n_ev, 1, __ATOMIC_RELAXED);
  return i < MAX_EVENTS ? i : -1;
}

typedef void (*call_t)(void *cmd, int n, const nvn_handle_t *handles);
static call_t real_call[2];
static const char *const call_names[2] = { "nvnCommandBufferCallCommands", "nvnCommandBufferCopyCommands" };
static void log_call(int k, void *cmd, int n, const nvn_handle_t *handles)
{
  if (state != 2)
    return;
  int i = add();
  if (i < 0)
    return;
  ev[i].type = E_CALL, ev[i].cmd = (uintptr_t)cmd, ev[i].n = (uint32_t)n, ev[i].n_colors = (uint8_t)k;
  for (int j = 0; j < 6; j++)
    ev[i].handles[j] = j < n ? handles[j] : 0;
}
static void hook_call0(void *cmd, int n, const nvn_handle_t *h) { log_call(0, cmd, n, h); real_call[0](cmd, n, h); }
static void hook_call1(void *cmd, int n, const nvn_handle_t *h) { log_call(1, cmd, n, h); real_call[1](cmd, n, h); }
static void hook_set_rt(void *cmd, int n, const nvn_texture_t *const *colors, const void *const *views,
                        const nvn_texture_t *depth, const void *depth_view)
{
  if (state == 2) {
    int i = add();
    if (i >= 0) {
      ev[i].type = E_RT, ev[i].cmd = (uintptr_t)cmd, ev[i].n_colors = (uint8_t)n, ev[i].draws = 0;
      const nvn_texture_t *c = n > 0 && colors ? colors[0] : 0;
      ev[i].a = (uintptr_t)c;
      ev[i].w = c && tex_w ? (uint32_t)tex_w(c) : 0, ev[i].h = c && tex_h ? (uint32_t)tex_h(c) : 0;
      ev[i].fmt = c && tex_fmt ? (uint32_t)tex_fmt(c) : 0;
      ev[i].dw = depth && tex_w ? (uint32_t)tex_w(depth) : 0, ev[i].dh = depth && tex_h ? (uint32_t)tex_h(depth) : 0;
      ev[i].dfmt = depth && tex_fmt ? (uint32_t)tex_fmt(depth) : 0;
    }
  }
  real_set_rt(cmd, n, colors, views, depth, depth_view);
}

/* a draw: counted on the latest render target event of the same command buffer */
static void count_draw(void *cmd)
{
  if (state != 2)
    return;
  int top = n_ev < MAX_EVENTS ? n_ev : MAX_EVENTS;
  for (int i = top - 1; i >= 0 && i >= top - 64; i--)
    if (ev[i].cmd == (uintptr_t)cmd && ev[i].type == E_RT) {
      __atomic_fetch_add(&ev[i].draws, 1, __ATOMIC_RELAXED);
      return;
    }
  int i = add(); /* none seen yet on it this frame */
  if (i >= 0)
    ev[i].type = E_DRAW, ev[i].cmd = (uintptr_t)cmd;
}

#define DRAW_HOOK(k) \
  static void hook_draw##k(void *cmd, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) \
  { count_draw(cmd); real_draw[k](cmd, a, b, c, d, e, f); }
DRAW_HOOK(0) DRAW_HOOK(1) DRAW_HOOK(2) DRAW_HOOK(3) DRAW_HOOK(4) DRAW_HOOK(5) DRAW_HOOK(6) DRAW_HOOK(7)
static void *const draw_hooks[8] = { hook_draw0, hook_draw1, hook_draw2, hook_draw3, hook_draw4, hook_draw5,
                                     hook_draw6, hook_draw7 };

static nvn_handle_t hook_end(void *cmd)
{
  nvn_handle_t h = real_end(cmd);
  if (state == 2) {
    int i = add();
    if (i >= 0)
      ev[i].type = E_END, ev[i].cmd = (uintptr_t)cmd, ev[i].a = h;
  }
  return h;
}

static void hook_submit(nvn_queue_t *q, int n, const nvn_handle_t *handles)
{
  if (state == 2) {
    int i = add();
    if (i >= 0)
      ev[i].type = E_SUBMIT, ev[i].cmd = (uintptr_t)q, ev[i].n = (uint32_t)n, ev[i].a = n > 0 ? handles[0] : 0;
    for (int k = 0; i >= 0 && k < 6; k++)
      ev[i].handles[k] = k < n ? handles[k] : 0;
  }
  real_submit(q, n, handles);
}

static bool same(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

void *frametrace_wrap(const char *name, void *proc)
{
  if (!proc)
    return 0;
  if (same(name, "nvnCommandBufferSetRenderTargets")) {
    real_set_rt = (set_rt_t)proc;
    return (void *)hook_set_rt;
  }
  for (int k = 0; k < 8; k++)
    if (same(name, draw_names[k])) {
      real_draw[k] = (draw_t)proc;
      return draw_hooks[k];
    }
  (void)hook_call0, (void)hook_call1; /* CallCommands / CopyCommands: not wrapped (the game crashed with them) */
  if (same(name, "nvnCommandBufferEndRecording")) {
    real_end = (end_t)proc;
    return (void *)hook_end;
  }
  if (same(name, "nvnQueueSubmitCommands")) {
    real_submit = (submit_t)proc;
    return (void *)hook_submit;
  }
  return 0;
}

void frametrace_arm(void)
{
  if (state == 0)
    state = 1;
}

void frametrace_present(getproc_t getproc, nvn_device_t *dev, const nvn_texture_t *presented)
{
  if (!tex_w && getproc && dev) {
    tex_w = (tex_int_t)getproc(dev, "nvnTextureGetWidth");
    tex_h = (tex_int_t)getproc(dev, "nvnTextureGetHeight");
    tex_fmt = (tex_int_t)getproc(dev, "nvnTextureGetFormat");
  }
  if (state == 1) {
    n_ev = 0;
    state = 2;
    return;
  }
  if (state != 2)
    return;
  presented_texture = (uintptr_t)presented;
  __atomic_store_n(&state, 3, __ATOMIC_RELEASE); /* written on the game thread (frametrace_write) */
}

void frametrace_write(void)
{
  const nvn_texture_t *presented = (const nvn_texture_t *)presented_texture;
  if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 3)
    return;
  state = 0;
  int n = n_ev < MAX_EVENTS ? n_ev : MAX_EVENTS;
  /* to sloperinaGZ/dump/frametrace.txt: the log takes only so much at once */
  static const char path[] = SD_DIR "/dump/frametrace.txt";
  static char line[512];
  fs_file_t f;
  fs_create_dir(SD_DIR "/dump");
  fs_delete_file(path);
  if (fs_create_file(path, 0) != 0 || fs_open(&f, path, FS_WRITE | FS_ALLOW_APPEND) != 0) {
    log_printf("frametrace: couldn't write %s", path);
    return;
  }
  int64_t at = 0;
  fs_write_option_t opt = { 0 };
  int len = snprintf(line, sizeof(line), "%d events, presented %#lx\n", n, (unsigned long)(uintptr_t)presented);
  fs_write(f, at, line, (size_t)len, &opt), at += len;
  int draws_run = 0;
  uintptr_t run_cmd = 0;
  for (int i = 0; i < n; i++) {
    len = 0;
    if (ev[i].type == E_DRAW) { /* runs of draws on one command buffer: one line */
      if (ev[i].cmd == run_cmd) {
        draws_run++;
        continue;
      }
      if (draws_run)
        len = snprintf(line, sizeof(line), "  ... %d draws on %#lx\n", draws_run, (unsigned long)run_cmd);
      if (len > 0)
        fs_write(f, at, line, (size_t)len, &opt), at += len;
      run_cmd = ev[i].cmd, draws_run = 1;
      continue;
    }
    if (draws_run) {
      len = snprintf(line, sizeof(line), "  ... %d draws on %#lx\n", draws_run, (unsigned long)run_cmd);
      fs_write(f, at, line, (size_t)len, &opt), at += len;
      draws_run = 0, run_cmd = 0;
    }
    if (ev[i].type == E_RT)
      len = snprintf(line, sizeof(line), "%4d cmd %#lx rt %#lx%s %ux%u fmt %#x depth %ux%u fmt %#x: %u draws\n", i,
                     (unsigned long)ev[i].cmd, (unsigned long)ev[i].a,
                     ev[i].a == (uintptr_t)presented ? " PRESENTED" : "", ev[i].w, ev[i].h, ev[i].fmt, ev[i].dw,
                     ev[i].dh, ev[i].dfmt, ev[i].draws);
    else if (ev[i].type == E_END)
      len = snprintf(line, sizeof(line), "%4d cmd %#lx end -> %#llx\n", i, (unsigned long)ev[i].cmd,
                     (unsigned long long)ev[i].a);
    else if (ev[i].type == E_SUBMIT)
      len = snprintf(line, sizeof(line), "%4d SUBMIT queue %#lx n %u: %#llx %#llx %#llx %#llx %#llx %#llx\n", i,
                     (unsigned long)ev[i].cmd, ev[i].n, (unsigned long long)ev[i].handles[0],
                     (unsigned long long)ev[i].handles[1], (unsigned long long)ev[i].handles[2],
                     (unsigned long long)ev[i].handles[3], (unsigned long long)ev[i].handles[4],
                     (unsigned long long)ev[i].handles[5]);
    else if (ev[i].type == E_CALL)
      len = snprintf(line, sizeof(line), "%4d cmd %#lx %s %u: %#llx %#llx %#llx %#llx %#llx %#llx\n", i,
                     (unsigned long)ev[i].cmd, ev[i].n_colors ? "COPY" : "CALL", ev[i].n,
                     (unsigned long long)ev[i].handles[0], (unsigned long long)ev[i].handles[1],
                     (unsigned long long)ev[i].handles[2], (unsigned long long)ev[i].handles[3],
                     (unsigned long long)ev[i].handles[4], (unsigned long long)ev[i].handles[5]);
    if (len > 0)
      fs_write(f, at, line, (size_t)len, &opt), at += len;
  }
  fs_close(f);
  log_printf("frametrace: %d events to %s", n, path);
}
