/* debug > free camera (freecam.h, FINDINGS 9.30).
 *
 * The render camera ([[main+camera_ptr]+0x68], a sead look-at camera with its eye and look-at as doubles at +0x60
 * and +0x78, and as floats at +0x38 and +0x44) builds its view matrix in a virtual call (vtable slots 4 and 11, the
 * same function) after the game's own camera logic has set eye and look-at. While the free camera is on, that one
 * object gets a copy of its vtable whose two slots lead here first: the free camera's eye and look-at are written,
 * then the game's own function builds the matrix. So what's drawn, culled to the view and overlaid (view.c reads the
 * same camera) is the free camera's; the player camera the game knows carries on following Link. Off, the object
 * gets its own vtable back.
 *
 * The world isn't streamed round it: the game loads terrain and places actors round Link (FINDINGS 9.30 has what was
 * tried). Far from him, the free camera shows the far terrain and no actors.
 *
 * The controls, with the menu closed: left stick moves along the view, right stick looks, ZR rises, ZL sinks, X
 * held is faster. The game sees no input meanwhile, so Link stands where he was. A script's input (remote.h) steers
 * it too. */
#include "freecam.h"

#include <string.h>

#include "binds.h"
#include "gfx.h"
#include "input.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

float sinf(float x), cosf(float x), atan2f(float y, float x); /* from sdk */

#define CAM_EYE_F 0x38 /* the render camera: eye and look-at as floats (what the overlays read) */
#define CAM_AT_F 0x44
#define CAM_EYE_D 0x60 /* ... and as doubles (what its matrix is built from) */
#define CAM_AT_D 0x78
#define CAM_SPAN 0x90
#define VT_MATRIX_A 4 /* the vtable slots of its matrix function (the same function twice) */
#define VT_MATRIX_B 11
#define VT_BEFORE 2   /* the offset to the top and the type info, before the first slot */
#define VT_COPY 40    /* slots copied: more than the camera's vtable has */

#define DEADZONE 4000
#define MOVE_SPEED 0.15f  /* metres a frame at full tilt */
#define FAST 6.0f         /* X held */
#define TURN_SPEED 0.045f /* radians a frame */
#define PITCH_MAX 1.5f

typedef void (*matrix_fn_t)(uintptr_t camera, float *out);

bool freecam_on;

static double eye[3], at[3];
static float yaw, pitch; /* yaw 0 looks along +z; pitch up positive */

static uintptr_t hooked;       /* the camera object that has our vtable */
static uintptr_t own_vt;       /* its own vtable */
static matrix_fn_t own_matrix; /* its own matrix function */
static uintptr_t fake_vt[VT_BEFORE + VT_COPY];

static bool readable(uintptr_t a, size_t n) { return a && !(a & 7) && sys_mapped(a, n, SYS_PERM_R); }
static bool in_main(uintptr_t a) { return a >= z64hd_main && a - z64hd_main < 0x5000000; }

static float root(float x) /* the sdk exports no sqrtf */
{
  float r;
  __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
  return r;
}

/* --- the picture ------------------------------------------------------------------------------------ */

static void matrix_hook(uintptr_t cam, float *out)
{
  if (freecam_on && cam == hooked) {
    double *e = (double *)(cam + CAM_EYE_D), *a = (double *)(cam + CAM_AT_D);
    float *ef = (float *)(cam + CAM_EYE_F), *af = (float *)(cam + CAM_AT_F);
    for (int i = 0; i < 3; i++) {
      e[i] = eye[i];
      a[i] = at[i];
      ef[i] = (float)eye[i];
      af[i] = (float)at[i];
    }
  }
  own_matrix(cam, out);
}

static void unhook(void)
{
  if (hooked && readable(hooked, 8) && sys_ptr(hooked) == (uintptr_t)&fake_vt[VT_BEFORE])
    *(volatile uintptr_t *)hooked = own_vt;
  hooked = 0;
}

/* the render camera given our vtable (again, if the game made a new one) */
static bool hook(void)
{
  uintptr_t cam = camera();
  if (!readable(cam, CAM_SPAN))
    return false;
  if (cam == hooked && sys_ptr(cam) == (uintptr_t)&fake_vt[VT_BEFORE])
    return true;
  unhook();
  uintptr_t vt = sys_ptr(cam);
  if (!in_main(vt) || !readable(vt - 8 * VT_BEFORE, 8 * (VT_BEFORE + VT_COPY)))
    return false;
  uintptr_t matrix = sys_ptr(vt + 8 * VT_MATRIX_A);
  if (!in_main(matrix) || sys_ptr(vt + 8 * VT_MATRIX_B) != matrix) { /* not the camera this was made for */
    static bool said;
    if (!said)
      log_printf("freecam: the render camera's vtable main+%#lx isn't the one expected",
                 (unsigned long)(vt - z64hd_main));
    said = true;
    return false;
  }
  memcpy(fake_vt, (const void *)(vt - 8 * VT_BEFORE), sizeof(fake_vt));
  fake_vt[VT_BEFORE + VT_MATRIX_A] = (uintptr_t)matrix_hook;
  fake_vt[VT_BEFORE + VT_MATRIX_B] = (uintptr_t)matrix_hook;
  own_vt = vt;
  own_matrix = (matrix_fn_t)matrix;
  hooked = cam;
  *(volatile uintptr_t *)cam = (uintptr_t)&fake_vt[VT_BEFORE];
  log_printf("freecam: render camera %#lx hooked (vtable main+%#lx)", (unsigned long)cam,
             (unsigned long)(vt - z64hd_main));
  return true;
}

/* --- on, off and flying ------------------------------------------------------------------------------ */

static void look_from_angles(void)
{
  float cp = cosf(pitch);
  at[0] = eye[0] + sinf(yaw) * cp;
  at[1] = eye[1] + sinf(pitch);
  at[2] = eye[2] + cosf(yaw) * cp;
}

bool freecam_set(bool on)
{
  if (on == freecam_on)
    return true;
  if (on) {
    uintptr_t cam = camera();
    if (!readable(cam, CAM_SPAN)) {
      gfx_notice("free camera: no camera found");
      return false;
    }
    const double *e = (const double *)(cam + CAM_EYE_D), *a = (const double *)(cam + CAM_AT_D);
    float dx = (float)(a[0] - e[0]), dy = (float)(a[1] - e[1]), dz = (float)(a[2] - e[2]);
    memcpy(eye, e, sizeof(eye));
    yaw = atan2f(dx, dz);
    pitch = atan2f(dy, root(dx * dx + dz * dz));
    look_from_angles();
    if (!hook()) {
      gfx_notice("free camera: not known for this camera");
      return false;
    }
    freecam_on = true;
    input_freecam_game = true;
    log_printf("freecam: on at %.2f %.2f %.2f", eye[0], eye[1], eye[2]);
    gfx_notice("free camera: left stick move, right stick look, ZR up, ZL down, X fast");
  } else {
    freecam_on = false;
    input_freecam_game = false;
    unhook();
    log_printf("freecam: off");
    gfx_notice("free camera off");
  }
  return true;
}

static float axis(int32_t v) { return v > DEADZONE || v < -DEADZONE ? (float)v / 32767.0f : 0; }

void freecam_frame(bool menu_open)
{
  if (!menu_open && cmd_pressed(CMD_FREECAM)) /* unbound unless given buttons (settings > rebind commands) */
    freecam_set(!freecam_on);
  if (!freecam_on || !hook()) /* no camera now (a map change): hooked again once there is */
    return;
  if (menu_open)
    return;
  int32_t lx, ly, rx, ry;
  input_sticks(&lx, &ly, &rx, &ry);
  if (input_remote_active && input_remote_sticks[0])
    lx = input_remote_stick[0], ly = input_remote_stick[1];
  if (input_remote_active && input_remote_sticks[1])
    rx = input_remote_stick[2], ry = input_remote_stick[3];
  uint32_t held = input_held() | (input_remote_active ? input_remote_buttons : 0);
  float speed = MOVE_SPEED * (held & BTN_X ? FAST : 1.0f);
  yaw -= axis(rx) * TURN_SPEED;
  pitch += axis(ry) * TURN_SPEED;
  if (pitch > PITCH_MAX)
    pitch = PITCH_MAX;
  if (pitch < -PITCH_MAX)
    pitch = -PITCH_MAX;
  float fwd = axis(ly) * speed, side = axis(lx) * speed;
  float sy = sinf(yaw), cy = cosf(yaw);
  eye[0] += sy * fwd - cy * side;
  eye[2] += cy * fwd + sy * side;
  if (held & BTN_ZR)
    eye[1] += speed;
  if (held & BTN_ZL)
    eye[1] -= speed;
  look_from_angles();
}
