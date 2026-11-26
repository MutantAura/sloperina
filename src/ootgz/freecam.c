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
 * The controls, with the menu closed, as the explorer's where they're alike (placer.c), shown in a bar across the
 * bottom with the game's button glyphs (gfx_guide): left stick moves along the view, right stick looks, d-up / d-down
 * rise and sink, ZR / ZL zoom (in and out along the view), X held is faster, A goes back to Link (behind him,
 * looking at him), R3 hides the controls bar (and shows it again), L3 hands the controller to Link and back (the game
 * gets it but for L3 and R3, A being Link's own; the camera stays where it is; the bar says which L3 goes to next), Y
 * follows Link (the camera keeps its angle and where it is from him, and goes with him; the controls move it from
 * him), B turns it off. The game sees no input meanwhile (but while Link has the controller), so Link stands where
 * he was, and the module's own commands are off but for the free camera's and the menu's (ootgz.c's
 * command_context). A script's input
 * (remote.h) steers it too.
 *
 * The actor placer (placer.c) steers it as well: freecam_steer_begin turns it on if it wasn't, and while it steers
 * the eye and look-at are the placer's and the sticks are left to it. freecam_steer_end gives it back: off again,
 * or flying on from the placer's view if it was on before. */
#include "freecam.h"

#include <stdio.h>
#include <string.h>

#include "binds.h"
#include "gfx.h"
#include "input.h"
#include "lang.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

float sinf(float x), cosf(float x), atan2f(float y, float x); /* from sdk */

#define CAM_EYE_F 0x38 /* the render camera: eye and look-at as floats (what the overlays read) */
#define CAM_AT_F 0x44
#define CAM_EYE_D 0x60 /* ... and as doubles (what its matrix is built from) */
#define CAM_AT_D 0x78
#define CAM_UP_F 0x50  /* ... and its up, as floats (the matrix is built from the doubles and this) */
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
#define BEHIND 4.0f       /* L3: the camera this far behind Link ... */
#define ABOVE 2.0f        /* ... and this far above his feet, looking at him from ... */
#define AT_HEIGHT 1.0f    /* ... this far up */

typedef void (*matrix_fn_t)(uintptr_t camera, float *out);

bool freecam_on;

static double eye[3], at[3];
static float yaw, pitch; /* yaw 0 looks along +z; pitch up positive */
static bool link_control; /* L3: the controller drives Link (the game gets it back but for L3), the camera stays put */
static bool follow;       /* Y: the camera keeps where it is from Link and its angle, and goes with him */
static double follow_off[3];
static bool steered;      /* the actor placer has the view: its eye and look-at, the sticks left to it */
static bool steer_was_on; /* the free camera was on before the placer took it */

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

/* the game's up, the first time it leans after the free camera goes on: what the view would have rolled by */
static bool lean_said;

static void note_lean(const float *up)
{
  if (!lean_said && up[0] * up[0] + up[2] * up[2] > 0.0001f) {
    lean_said = true;
    log_printf("freecam: the game's up leans (%.3f %.3f %.3f): set straight while on", (double)up[0], (double)up[1],
               (double)up[2]);
  }
}

static void matrix_hook(uintptr_t cam, float *out)
{
  if (freecam_on && cam == hooked) {
    double *e = (double *)(cam + CAM_EYE_D), *a = (double *)(cam + CAM_AT_D);
    float *ef = (float *)(cam + CAM_EYE_F), *af = (float *)(cam + CAM_AT_F), *up = (float *)(cam + CAM_UP_F);
    for (int i = 0; i < 3; i++) {
      e[i] = eye[i];
      a[i] = at[i];
      ef[i] = (float)eye[i];
      af[i] = (float)at[i];
    }
    /* the game's up is its own camera's, which may lean (it tilts with its own view): left as it was, the free
     * camera's picture rolls with it. Straight up, the horizon stays level. */
    note_lean(up);
    up[0] = 0, up[1] = 1, up[2] = 0;
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

/* on from the camera the game shows now; quiet: no notice (the placer gives its own) */
static bool start(bool quiet)
{
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
  lean_said = false;
  freecam_ui_hidden = false;
  link_control = false;
  follow = false;
  input_freecam_game = true;
  log_printf("freecam: on at %.2f %.2f %.2f", eye[0], eye[1], eye[2]);
  if (!quiet)
    gfx_notice("free camera on");
  return true;
}

static void stop(bool quiet)
{
  freecam_on = false;
  link_control = false;
  follow = false;
  steered = false;
  input_freecam_game = false;
  unhook();
  log_printf("freecam: off");
  if (!quiet)
    gfx_notice("free camera off");
}

bool freecam_set(bool on)
{
  if (on == freecam_on)
    return true;
  if (on)
    return start(false);
  stop(false); /* the placer, if it had the camera, sees it's gone and ends (placer.c) */
  return true;
}

/* --- the actor placer's view (placer.c) -------------------------------------------------------------- */

bool freecam_steer_begin(double eye_now[3], double at_now[3])
{
  if (!steered) {
    steer_was_on = freecam_on;
    if (!freecam_on && !start(true))
      return false;
    steered = true;
  }
  memcpy(eye_now, eye, sizeof(eye));
  memcpy(at_now, at, sizeof(at));
  return true;
}

bool freecam_steering(void) { return freecam_on && steered; }

void freecam_steer(const double eye_to[3], const double at_to[3])
{
  if (!freecam_steering())
    return;
  memcpy(eye, eye_to, sizeof(eye));
  memcpy(at, at_to, sizeof(at));
}

void freecam_steer_end(void)
{
  if (!steered)
    return;
  steered = false;
  if (!steer_was_on) {
    stop(true); /* the picture back on Link */
    return;
  }
  /* flying again from where the placer's view was */
  float dx = (float)(at[0] - eye[0]), dy = (float)(at[1] - eye[1]), dz = (float)(at[2] - eye[2]);
  yaw = atan2f(dx, dz);
  pitch = atan2f(dy, root(dx * dx + dz * dz));
  look_from_angles();
}

static float axis(int32_t v) { return v > DEADZONE || v < -DEADZONE ? (float)v / 32767.0f : 0; }

/* R3: the free camera's, the explorer's and the actor placer's own drawing hidden (the controls bar, the beam, the
 * side panel), for a clear picture; R3 again shows it. Shown again whenever one of them starts. */
bool freecam_ui_hidden;

/* L3: the controller drives the camera, or Link (link_control) */

static void set_link_control(bool on)
{
  link_control = on;
  input_freecam_game = freecam_on && !on;
  log_printf("freecam: the controller drives %s", on ? "Link" : "the camera");
}

/* A: behind Link as he faces, looking at him */
static void back_to_link(void)
{
  vec3f_t link;
  double fx = 0, fz = 1;
  if (!link_drawn_pos(&link))
    return;
  link_facing(&fx, &fz);
  eye[0] = link.x - fx * BEHIND, eye[1] = link.y + ABOVE, eye[2] = link.z - fz * BEHIND;
  float dx = (float)(link.x - eye[0]), dy = (float)(link.y + AT_HEIGHT - eye[1]), dz = (float)(link.z - eye[2]);
  yaw = atan2f(dx, dz);
  pitch = atan2f(dy, root(dx * dx + dz * dz));
}

/* following: the camera put back where it is from Link (follow_off), before the controls move it */
static void follow_link(void)
{
  vec3f_t link;
  if (!follow || !link_drawn_pos(&link))
    return;
  eye[0] = link.x + follow_off[0], eye[1] = link.y + follow_off[1], eye[2] = link.z + follow_off[2];
  look_from_angles();
}

/* ... and where it is from him now (the controls may have moved it), kept for the next frame */
static void keep_follow_off(void)
{
  vec3f_t link;
  if (follow && link_drawn_pos(&link))
    follow_off[0] = eye[0] - link.x, follow_off[1] = eye[1] - link.y, follow_off[2] = eye[2] - link.z;
}

void freecam_frame(bool menu_open)
{
  if (!menu_open && cmd_pressed(CMD_FREECAM)) /* unbound unless given buttons (settings > rebind commands) */
    freecam_set(!freecam_on);
  if (!freecam_on || !hook()) /* no camera now (a map change): hooked again once there is */
    return;
  static uint32_t held_before = ~0u; /* a press is a button that wasn't held (all, after the menu or turning on) */
  if (menu_open || steered) {
    held_before = ~0u;
    return;
  }
  int32_t lx, ly, rx, ry;
  input_sticks(&lx, &ly, &rx, &ry);
  if (input_remote_active && input_remote_sticks[0])
    lx = input_remote_stick[0], ly = input_remote_stick[1];
  if (input_remote_active && input_remote_sticks[1])
    rx = input_remote_stick[2], ry = input_remote_stick[3];
  uint32_t held = input_held() | (input_remote_active ? input_remote_buttons : 0);
  uint32_t pressed = held & ~held_before;
  held_before = held;
  if (pressed & BTN_LSTICK) /* the camera, or Link (L3: A is Link's own) */
    set_link_control(!link_control);
  follow_link();
  if (link_control) { /* the game has the rest but L3 and R3: the camera stays as it is (or goes with him, following) */
    input_hide_from_game |= BTN_LSTICK | BTN_RSTICK;
    if (pressed & BTN_RSTICK)
      freecam_ui_hidden = !freecam_ui_hidden;
    if (!freecam_ui_hidden) /* L3: what it does next */
      gfx_guide(T("L3 control camera  R3 hide controls")); /* following or not: Y is Link's here, so it isn't said */
    return;
  }
  if (pressed & BTN_Y) { /* follow Link from here, or stay */
    follow = !follow;
    if (follow) {
      vec3f_t link;
      follow = link_drawn_pos(&link);
      keep_follow_off();
    }
    log_printf("freecam: follow Link %s", follow ? "on" : "off");
  }
  if (!freecam_ui_hidden) {
    char bar[GFX_HINT_LEN];
    snprintf(bar, sizeof(bar),
             TF("LS move  RS look  d-up/down height  ZL/ZR zoom  X faster  A back to Link\n" /* moving on top */
                "Y follow Link: %s  L3 control Link  R3 hide controls  B done"), /* L3: what it does next */
             follow ? T("on") : T("off"));
    gfx_guide(bar);
  }
  if (pressed & BTN_B) {
    held_before = ~0u;
    freecam_set(false);
    return;
  }
  if (pressed & BTN_A)
    back_to_link();
  if (pressed & BTN_RSTICK)
    freecam_ui_hidden = !freecam_ui_hidden;
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
  if (held & BTN_DUP)
    eye[1] += speed;
  if (held & BTN_DDOWN)
    eye[1] -= speed;
  float zoom = (held & BTN_ZR ? speed : 0) - (held & BTN_ZL ? speed : 0); /* along the view itself, up or down too */
  for (int i = 0; zoom && i < 3; i++)
    eye[i] += (at[i] - eye[i]) * zoom; /* at is a metre along the view */
  look_from_angles();
  keep_follow_off(); /* following: where the controls put it from him is where it stays */
}
