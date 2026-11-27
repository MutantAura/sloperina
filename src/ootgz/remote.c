/* Commands from a file (remote.h): the script's lines run on the game's thread from the frame hook, through the
 * same code the controller reaches: binds_fire for the commands, the menu items' own callbacks for the rest. */
#include "remote.h"

#include <stdarg.h>
#include <stdint.h>

#include "actornames.h"
#include "binds.h"
#include "enemyhp.h"
#include "gamedata.h"
#include "input.h"
#include "pausemgr.h"
#include "link.h"
#include "menu.h"
#include "levitate.h"
#include "menus.h"
#include "frametrace.h"
#include "nvn.h"
#include "physdump.h"
#include "position.h"
#include "pouch.h"
#include "sdfs.h"
#include "settings.h"
#include "guardview.h"
#include "hitview.h"
#include "interactview.h"
#include "sys.h"
#include "title.h"
#include "warp.h"
#include "watches.h"
#include "z64hd.h"

#define REMOTE_DIR SD_DIR "/remote"
#define CMD_FILE REMOTE_DIR "/cmd.txt"
#define OUT_FILE REMOTE_DIR "/out.txt"

#define SCRIPT_MAX 65536  /* a script's text */
#define LINE_MAX_LEN 512  /* one line of it */
#define REPEAT_DEPTH 4    /* repeats inside repeats */
#define OUT_MAX 8192      /* results waiting to be written */
#define POLL_CALLS 6      /* look for cmd.txt every this many calls (10 times a second) */
#define LINES_PER_CALL 64 /* at most this many lines a frame: a long script doesn't hold the game up */
#define STICK_MAX 32767

bool remote_enabled;

static char script[SCRIPT_MAX];
static size_t script_len, cursor;
static int line_no;
static bool running;
static char script_id[24] = "-";

/* the repeats the script is inside: where each one's lines start, and the rounds it still has */
static struct {
  size_t cursor;
  int line_no, left;
} repeats[REPEAT_DEPTH];
static int repeat_depth;

/* what the line being run waits for */
static int wait_left;               /* wait: frames still to go */
static bool pause_wanted;           /* pause: for frame advance to take */
static bool pause_waiting;          /* ... and until it holds the game */
static int steps_left;              /* step: frames still to let through */
static bool stepping;               /* ... and until the game is held again after them */
static bool run_wanted, run_waiting; /* run: the same two */
static bool title_waiting;          /* title: until the game has started the event or it failed */

static int trace_left;               /* trace: frames it still reports */

static uint16_t fire_pending[CMD_COUNT]; /* fire while the game is held: given on the next frame (1 + frames held) */
static int input_frames;                 /* frames the script's input still lasts (0: until changed) */

/* --- text ------------------------------------------------------------------------------------ */

static char lowercase(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static char *skip_spaces(char *p)
{
  while (is_space(*p))
    p++;
  return p;
}

static void trim_end(char *s)
{
  size_t n = strlen(s);
  while (n && is_space(s[n - 1]))
    s[--n] = 0;
}

/* The next word of *p (ended in place), or "" at the end; *p moves past it. */
static char *take_word(char **p)
{
  char *start = skip_spaces(*p);
  char *end = start;
  while (*end && !is_space(*end))
    end++;
  if (*end)
    *end++ = 0;
  *p = end;
  return start;
}

static bool same(const char *a, const char *b)
{
  while (*a && lowercase(*a) == lowercase(*b)) {
    a++;
    b++;
  }
  return !*a && !*b;
}

/* "12", "-3", "0x1f" */
static bool parse_int(const char *s, int32_t *out)
{
  bool negative = *s == '-';
  if (negative || *s == '+')
    s++;
  int64_t value = 0;
  int digits = 0;
  if (s[0] == '0' && lowercase(s[1]) == 'x') {
    for (s += 2; *s; s++, digits++) {
      char c = lowercase(*s);
      int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (digit < 0 || value > UINT32_MAX)
        return false;
      value = value * 16 + digit;
    }
  } else {
    for (; *s; s++, digits++) {
      if (*s < '0' || *s > '9' || value > UINT32_MAX)
        return false;
      value = value * 10 + (*s - '0');
    }
  }
  if (!digits || value > UINT32_MAX)
    return false;
  *out = (int32_t)(negative ? -value : value);
  return true;
}

/* A name as a script gives it: lower case, single spaces, without the "(... {bind} ...)" some labels end in
 * ("save position ({savepos})" is "save position"), and only what prints. */
static void plain_name(const char *in, char *out, size_t cap)
{
  size_t o = 0;
  bool space = false;
  for (size_t i = 0; in && in[i]; i++) {
    char c = in[i];
    if (c == '(') {
      size_t end = i;
      bool has_bind = false;
      while (in[end] && in[end] != ')') {
        if (in[end] == '{')
          has_bind = true;
        end++;
      }
      if (has_bind && in[end]) {
        i = end;
        continue;
      }
    }
    if (is_space(c) || c == '\n') {
      space = o > 0;
      continue;
    }
    if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) /* the menu's own glyphs */
      continue;
    if (space && o + 1 < cap)
      out[o++] = ' ';
    space = false;
    if (o + 1 < cap)
      out[o++] = lowercase(c);
  }
  out[o] = 0;
}

/* --- results --------------------------------------------------------------------------------- */

static char out[OUT_MAX];
static size_t out_len;

static void out_flush(void)
{
  if (!out_len)
    return;
  fs_file_t file;
  if (fs_open(&file, OUT_FILE, FS_WRITE | FS_ALLOW_APPEND) != 0) {
    fs_create_dir(REMOTE_DIR); /* fails harmlessly if it exists */
    fs_create_file(OUT_FILE, 0);
    if (fs_open(&file, OUT_FILE, FS_WRITE | FS_ALLOW_APPEND) != 0) {
      out_len = 0;
      return;
    }
  }
  int64_t size = 0;
  fs_size(&size, file);
  fs_write_option_t flush = { FS_WRITE_FLUSH };
  fs_write(file, size, out, out_len, &flush);
  fs_close(file);
  out_len = 0;
}

/* one line of out.txt: "<frame> <id> <text>" */
static void out_line(const char *text)
{
  char line[320];
  int n = snprintf(line, sizeof(line), "%u %s %s\n", (unsigned)frame_counter(), script_id, text);
  if (n < 0)
    return;
  if ((size_t)n >= sizeof(line)) {
    n = (int)sizeof(line) - 1;
    line[n - 1] = '\n';
  }
  if (out_len + (size_t)n > sizeof(out))
    out_flush();
  memcpy(out + out_len, line, (size_t)n);
  out_len += (size_t)n;
}

/* the result of the line being run: "<line number> ok|err <text>", to out.txt and the log */
static void result(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result(bool ok, const char *fmt, ...)
{
  char text[256];
  int n = snprintf(text, sizeof(text), "%d %s ", line_no, ok ? "ok" : "err");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text + n, sizeof(text) - (size_t)n, fmt, ap);
  va_end(ap);
  trim_end(text);
  out_line(text);
  log_printf("remote: %s %s", script_id, text);
}

/* --- menu items by name ---------------------------------------------------------------------- */

static const char *const kind_names[] = { "menu", "action", "check", "option", "number", "text", "back" };

static bool has_name(const menu_item_t *item) { return item->label && item->label[0] && item->kind != MI_BACK; }

/* After a part's name in a path: an optional "#n" (which of that name), then the "/" before the next part, or the
 * end. Gives n and how much was read; false if something else follows (the name was only the start of a longer one). */
static bool part_end(const char *after, int *nth, size_t *used)
{
  const char *p = after;
  *nth = 1;
  if (*p == '#') {
    int n = 0;
    for (p++; *p >= '0' && *p <= '9' && n < 10000; p++)
      n = n * 10 + (*p - '0');
    if (n < 1)
      return false;
    *nth = n;
  }
  while (*p == ' ')
    p++;
  if (*p == '/')
    p++;
  else if (*p)
    return false;
  while (*p == ' ')
    p++;
  *used = (size_t)(p - after);
  return true;
}

/* The item of `menu` that the start of `rest` names (a path from here on, as plain_name leaves it): "@7" is item
 * 7; otherwise the longest name that fits ("start/stop timer" before "start", as names may have a "/" in them),
 * "name#2" the second item of that name. Gives how much of `rest` it took, with the "/" after it. */
static menu_item_t *match_item(menu_t *menu, const char *rest, size_t *used)
{
  int nth = 1;
  size_t tail = 0;
  if (rest[0] == '@') {
    int index = 0;
    const char *p = rest + 1;
    if (*p < '0' || *p > '9')
      return 0;
    for (; *p >= '0' && *p <= '9' && index < 100000; p++)
      index = index * 10 + (*p - '0');
    if (index >= menu->n_items || !part_end(p, &nth, &tail) || nth != 1)
      return 0;
    *used = (size_t)(p - rest) + tail;
    return &menu->items[index];
  }
  char name[96];
  size_t best = 0;
  int best_nth = 1;
  for (int i = 0; i < menu->n_items; i++) {
    if (!has_name(&menu->items[i]))
      continue;
    plain_name(menu->items[i].label, name, sizeof(name));
    size_t len = strlen(name);
    bool fits = len > best;
    for (size_t k = 0; fits && k < len; k++)
      fits = rest[k] == name[k];
    if (fits && part_end(rest + len, &nth, &tail)) {
      best = len;
      best_nth = nth;
      *used = len + tail;
    }
  }
  for (int i = 0; best && i < menu->n_items; i++) {
    if (!has_name(&menu->items[i]))
      continue;
    plain_name(menu->items[i].label, name, sizeof(name));
    bool is_it = strlen(name) == best;
    for (size_t k = 0; is_it && k < best; k++)
      is_it = rest[k] == name[k];
    if (is_it && --best_nth == 0)
      return &menu->items[i];
  }
  return 0;
}

/* Follow a path from the top menu (or, starting "~", from the menu on screen). Gives the menu the last part is in and
 * its item (0 for the empty path: the top menu itself); returns what's wrong, or 0. */
static const char *resolve(const char *path, menu_t **menu_out, menu_item_t **item_out)
{
  static char problem[200];
  char plain[200];
  plain_name(path, plain, sizeof(plain));
  const char *rest = plain;
  menu_t *menu = &menu_root;
  menu_item_t *item = 0;
  if (*rest == '~') { /* the menu on screen: one opened from elsewhere (a guard's, from the actor browser) */
    if (!menu_current) {
      snprintf(problem, sizeof(problem), "no menu on screen for ~");
      return problem;
    }
    menu = menu_current;
    for (rest++; *rest == '/'; rest++)
      ;
  }
  while (*rest) {
    if (item) { /* the part before this one has to be a menu */
      if (item->kind != MI_SUBMENU || !item->submenu) {
        snprintf(problem, sizeof(problem), "\"%s\" isn't a menu", item->label ? item->label : "?");
        return problem;
      }
      menu = item->submenu;
    }
    size_t used = 0;
    item = match_item(menu, rest, &used);
    if (!item) {
      snprintf(problem, sizeof(problem), "no \"%.120s\" in %.40s", rest, menu->title ? menu->title : "the menu");
      return problem;
    }
    rest += used;
  }
  *menu_out = menu;
  *item_out = item;
  return 0;
}

static void choice_name(menu_item_t *item, int choice, char *buf, size_t cap)
{
  char raw[96] = "";
  if (choice >= 0 && choice < item->n_choices) {
    if (item->choices)
      snprintf(raw, sizeof(raw), "%s", item->choices[choice]);
    else if (item->choice_text)
      item->choice_text(item, choice, raw, sizeof(raw));
  }
  plain_name(raw, buf, cap);
}

/* an item's value as text: "on", "2 (medium)", "150", a read-only line's text; "" for an action or a menu */
static void item_value(menu_item_t *item, char *buf, size_t cap)
{
  char text[160] = "";
  buf[0] = 0;
  switch (item->kind) {
    case MI_CHECK:
      if (item->get_check)
        snprintf(buf, cap, "%s", item->get_check(item) ? "on" : "off");
      break;
    case MI_OPTION:
      if (item->get_option) {
        int choice = item->get_option(item);
        choice_name(item, choice, text, sizeof(text));
        snprintf(buf, cap, "%d (%s)", choice, text);
      }
      break;
    case MI_NUMBER:
      if (item->get_num) {
        int32_t number = item->get_num(item);
        if (number == NUM_UNKNOWN)
          snprintf(buf, cap, "unknown");
        else
          snprintf(buf, cap, "%d", (int)number);
      }
      break;
    default: /* a text line, or another item whose line is written as it's shown (a list's rows and arrows) */
      if (item->text) {
        item->text(item, text, sizeof(text));
        plain_name(text, buf, cap);
      }
      break;
  }
}

/* Run an item's callback as the menu would on L, with the menu closed. An item that opens something to type
 * or pick in (a keyboard, a list) needs the menu: what it opened is closed again. */
static const char *choose(menu_item_t *item)
{
  bool was_open = menu_current != 0;
  switch (item->kind) {
    case MI_ACTION:
    case MI_NUMBER:
      if (item->action)
        item->action(item);
      else if (item->press)
        item->press(item, 0); /* a list's scroll arrow, say */
      else
        return "nothing to do on it";
      break;
    case MI_CHECK:
      if (!item->get_check || !item->set_check)
        return "can't be changed";
      item->set_check(item, !item->get_check(item));
      break;
    case MI_OPTION: {
      if (!item->get_option || !item->set_option || item->n_choices < 1)
        return "can't be changed";
      int choice = item->get_option(item);
      item->set_option(item, choice < 0 ? 0 : (choice + 1) % item->n_choices);
      break;
    }
    case MI_SUBMENU:
      return "a menu: list it, or name an item in it";
    case MI_BACK: /* return: up a level in the menu on screen (closing it from a menu with none over it) */
      if (!menu_current)
        return "no menu on screen to go back in";
      menu_back();
      return 0;
    default:
      return "not something to choose";
  }
  if (!was_open && (menu_current || menu_editing)) {
    menu_close();
    return "it opened something to type or pick in: that needs the menu";
  }
  return 0;
}

static const char *assign(menu_item_t *item, char *value)
{
  int32_t number = 0;
  bool is_number = parse_int(value, &number);
  switch (item->kind) {
    case MI_CHECK: {
      bool on = same(value, "on") || same(value, "true") || same(value, "yes") || (is_number && number == 1);
      bool off = same(value, "off") || same(value, "false") || same(value, "no") || (is_number && number == 0);
      if (!item->set_check || (!on && !off))
        return "a checkbox: on or off";
      item->set_check(item, on);
      return 0;
    }
    case MI_OPTION: {
      if (!item->set_option)
        return "can't be changed";
      int choice = is_number ? number : -1;
      char wanted[96], name[96];
      plain_name(value, wanted, sizeof(wanted));
      for (int i = 0; choice < 0 && i < item->n_choices; i++) {
        choice_name(item, i, name, sizeof(name));
        if (same(name, wanted))
          choice = i;
      }
      if (choice < 0 || choice >= item->n_choices)
        return "not one of its choices (get shows the current one, by number and name)";
      item->set_option(item, choice);
      return 0;
    }
    case MI_NUMBER: {
      if (!item->set_num || !is_number)
        return "a number";
      int32_t max = item->num_max ? item->num_max(item) : INT32_MAX;
      if (number < item->num_min || (max != NUM_UNKNOWN && number > max))
        return "out of its range";
      item->set_num(item, number);
      return 0;
    }
    default:
      return "not something with a value: do chooses it";
  }
}

static void list_menu(menu_t *menu)
{
  char text[320], name[96], value[160];
  for (int i = 0; i < menu->n_items; i++) {
    menu_item_t *item = &menu->items[i];
    plain_name(item->label, name, sizeof(name));
    item_value(item, value, sizeof(value));
    snprintf(text, sizeof(text), "%d | @%d %s %s%s%s", line_no, i, kind_names[item->kind], name,
             value[0] ? " = " : "", value);
    out_line(text);
  }
  result(true, "%d items in %s", menu->n_items, menu->title ? menu->title : "the menu");
}

/* --- the commands ---------------------------------------------------------------------------- */

static int command_by_key(const char *key)
{
  for (int c = 0; c < CMD_COUNT; c++)
    if (same(key, command_keys[c]))
      return c;
  return -1;
}

static void input_off(void)
{
  input_remote_active = false;
  input_remote_buttons = 0;
  input_remote_sticks[0] = input_remote_sticks[1] = false;
  input_frames = 0;
}

/* "input <buttons> [ls <x> <y>] [rs <x> <y>] [for <frames>]" */
static const char *run_input(char *args)
{
  char *buttons = take_word(&args);
  if (same(buttons, "off")) {
    input_off();
    return 0;
  }
  uint32_t mask = 0, bind;
  int32_t number;
  if (same(buttons, "-") || same(buttons, "none"))
    mask = 0;
  else if (parse_int(buttons, &number))
    mask = (uint32_t)number;
  else if (bind_parse(buttons, &bind))
    mask = bind_mask(bind);
  else
    return "buttons: names with + between (A+ZL, at most four), a number (0x41) or -";
  bool sticks[2] = { false, false };
  int32_t axes[4] = { 0, 0, 0, 0 };
  int32_t frames = 1;
  for (char *word = take_word(&args); *word; word = take_word(&args)) {
    int stick = same(word, "ls") ? 0 : same(word, "rs") ? 1 : -1;
    if (stick >= 0) {
      for (int axis = 0; axis < 2; axis++)
        if (!parse_int(take_word(&args), &axes[2 * stick + axis]) || axes[2 * stick + axis] < -STICK_MAX ||
            axes[2 * stick + axis] > STICK_MAX)
          return "a stick: ls or rs, then x and y from -32767 to 32767";
      sticks[stick] = true;
    } else if (same(word, "for")) {
      if (!parse_int(take_word(&args), &frames) || frames < 0)
        return "for: a number of frames (0: until the next input)";
    } else {
      return "after the buttons: ls <x> <y>, rs <x> <y>, for <frames>";
    }
  }
  input_remote_buttons = mask & 0xFFFF; /* the buttons; the sticks' direction bits come from the sticks */
  memcpy(input_remote_sticks, sticks, sizeof(sticks));
  memcpy(input_remote_stick, axes, sizeof(axes));
  input_remote_active = true;
  input_frames = frames;
  return 0;
}

float hypotf(float x, float y); /* from sdk */

/* Link's own state, for status and trace: his velocity (the actor's, units a second), what he holds and what's
 * on ZR (the player object's: -1 nothing, 0 the Deku Stick, 3 the Boomerang...), whether ZL has a target (2 with
 * one, 3 with none), whether he's on the ground, the ids of his main and weapon actions (the game's action table:
 * 40 Wait, 1 Walk, 65 LockOnWait, 66 LockOnMove, 114 JumpAttack, 33 Fall, 32 Land; weapon 57 Drawn, 58 Sheathed,
 * 56 PutAway) and which way he faces (degrees, 0 = +z). The offsets are the running build's (z64hd.h); where one
 * isn't known (0) or can't be read, the value stays unknown: moving false, -1 for the rest. */
#define PLAYER_JS_HEIGHT 0x18E8 /* watches.c: js_height_address gives the player object + this */
#define OFFSET(field) (z64hd_game ? z64hd_game->field : 0)

typedef struct {
  bool moving; /* vx, vy, vz were read */
  float vx, vy, vz, facing;
  int32_t hand, zr, lock, ground, action, weapon;
} link_state_t;

/* the s32 at base + offset, or -1 where the offset isn't known or the word can't be read */
static int32_t read_s32(uintptr_t base, uint32_t offset)
{
  uint32_t word;
  return base && offset && sys_read32(base + offset, &word) ? (int32_t)word : -1;
}

/* the pointer at base + offset, or 0 */
static uintptr_t read_ptr(uintptr_t base, uint32_t offset)
{
  return base && offset ? sys_ptr(base + offset) : 0;
}

float atan2f(float y, float x); /* from sdk */

static void link_state(link_state_t *s)
{
  *s = (link_state_t){ false, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1 };
  double fx, fz;
  if (link_facing(&fx, &fz)) {
    s->facing = atan2f((float)fx, (float)fz) * (180.0f / 3.14159265f);
    if (s->facing < 0)
      s->facing += 360.0f;
  }
  uintptr_t link = link_actor();
  uint32_t velocity = OFFSET(link_velocity);
  if (link && velocity && sys_mapped(link + velocity, 12, SYS_PERM_R)) {
    const float *v = (const float *)(link + velocity);
    s->moving = true;
    s->vx = v[0];
    s->vy = v[1];
    s->vz = v[2];
  }
  uintptr_t js;
  if (!js_height_address(&js) || js < PLAYER_JS_HEIGHT)
    return;
  uintptr_t player = js - PLAYER_JS_HEIGHT;
  s->zr = read_s32(player, OFFSET(player_zr_item));
  s->hand = read_s32(player, OFFSET(player_held_kind));
  s->lock = read_s32(player, OFFSET(player_lock_state));
  s->ground = read_s32(player, OFFSET(player_on_ground));
  if (s->ground != -1)
    s->ground &= 1;
  uintptr_t actions = read_ptr(read_ptr(read_ptr(player, OFFSET(player_actions_1)), OFFSET(player_actions_2)),
                               OFFSET(player_actions_3));
  s->action = read_s32(read_ptr(actions, OFFSET(action_main_slot)), OFFSET(action_id));
  s->weapon = read_s32(read_ptr(actions, OFFSET(action_weapon_slot)), OFFSET(action_id));
}

/* "vy=6.125 hs=5.000", or "vy=- hs=-" where Link's velocity isn't known */
static void motion_text(const link_state_t *s, char *buf, size_t cap)
{
  if (s->moving)
    snprintf(buf, cap, "vy=%.3f hs=%.3f", (double)s->vy, (double)hypotf(s->vx, s->vz));
  else
    snprintf(buf, cap, "vy=- hs=-");
}

/* one frame of a trace: "<line> t y=... vy=... hs=... x=... z=... hand=... zr=... paused=..." */
static void trace_line(void)
{
  vec3f_t pos = { 0, 0, 0 };
  link_state_t s;
  link_drawn_pos(&pos);
  link_state(&s);
  char motion[48], text[240];
  motion_text(&s, motion, sizeof(motion));
  snprintf(text, sizeof(text), "%d t y=%.3f %s x=%.3f z=%.3f hand=%d zr=%d paused=%d lock=%d ground=%d act=%d wpn=%d",
           line_no, (double)pos.y, motion, (double)pos.x, (double)pos.z, (int)s.hand, (int)s.zr,
           (int)(gd_ready() && gd_game_paused(0, 0)), (int)s.lock, (int)s.ground, (int)s.action, (int)s.weapon);
  out_line(text);
}

/* "12.5", "-0.25", "110": a number with a decimal point */
static bool parse_decimal(const char *s, double *out)
{
  bool negative = *s == '-';
  if (negative || *s == '+')
    s++;
  double value = 0, scale = 1;
  int digits = 0;
  for (; *s >= '0' && *s <= '9'; s++, digits++)
    value = value * 10 + (*s - '0');
  if (*s == '.')
    for (s++; *s >= '0' && *s <= '9'; s++, digits++) {
      scale /= 10;
      value += (*s - '0') * scale;
    }
  if (*s || !digits)
    return false;
  *out = negative ? -value : value;
  return true;
}

/* "place <x> <y> <z> [facing]": Link to that spot of this map, as load position does it */
static void run_place(char *args)
{
  double v[4] = { 0, 0, 0, 0 };
  int n = 0;
  for (char *word = take_word(&args); *word; word = take_word(&args), n++)
    if (n >= 4 || !parse_decimal(word, &v[n])) {
      result(false, "place: x y z, then the way to face in degrees (0 = +z) if not as now");
      return;
    }
  if (n < 3) {
    result(false, "place: x y z, then the way to face in degrees (0 = +z) if not as now");
    return;
  }
  if (link_record_status() != REC_READY) {
    result(false, "place: Link not found");
    return;
  }
  if (n < 4) {
    link_state_t s;
    link_state(&s);
    v[3] = s.facing;
  }
  position_set(v[0], v[1], v[2], v[3]);
  position_load();
  result(true, "place %.3f %.3f %.3f facing %.1f", v[0], v[1], v[2], v[3]);
}

#define ACTORS_MAX 24

/* "spawn <actor> [metres]": the game makes that actor that far in front of Link (2 m unless said), facing him, as
 * debug > actors > quick spawn does, through the create request's handle (actor_spawn_handle, FINDINGS 9.29
 * and 9.34); the line ends when the actor made is running. Only the names the game has (actornames.c): a name the
 * game has no ActorInfo row for would take a template slot for good. */
#define SPAWN_AHEAD 2.0
#define SPAWN_WAIT 600 /* frames to wait for it (its pack may have to load) */

static struct {
  bool active;
  const char *name; /* actornames.c's */
  int frames;
  uintptr_t handle, actor; /* the request's status (the game writes it), then the actor made */
} spawning;

static const char *known_actor(const char *name)
{
  for (int i = 0; i < actor_names_count; i++)
    if (same(actor_names[i], name))
      return actor_names[i];
  return 0;
}

static void run_spawn(char *args)
{
  char *name = take_word(&args);
  char *metres = take_word(&args);
  double d = SPAWN_AHEAD;
  if (!*name || (*metres && !parse_decimal(metres, &d))) {
    result(false, "spawn: an actor's name (as debug > actors lists it), then metres in front of Link (%.0f unless "
           "said)", SPAWN_AHEAD);
    return;
  }
  const char *actor = known_actor(name);
  vec3f_t me;
  double fx = 0, fz = 1;
  if (!actor) {
    result(false, "spawn: %s isn't one of the %d actors the game has", name, actor_names_count);
    return;
  }
  if (!link_drawn_pos(&me)) {
    result(false, "spawn: Link not found");
    return;
  }
  link_facing(&fx, &fz);
  float at[3] = { (float)(me.x + fx * d), me.y, (float)(me.z + fz * d) };
  float yaw = atan2f((float)fx, (float)fz) * (180.0f / 3.14159265f) + 180.0f; /* facing Link */
  uint32_t code;
  if (!actor_spawn_handle(actor, at, yaw, &spawning.handle, &code)) {
    result(false, "spawn %s: the game refused it (%u)", actor, (unsigned)code);
    return;
  }
  spawning.active = true;
  spawning.name = actor;
  spawning.frames = 0;
  spawning.actor = 0;
}

/* while a spawn line waits: made (then running), failed, or too long */
static bool spawn_waiting(void)
{
  actor_read_t a;
  if (!spawning.actor) {
    spawn_poll_t now = actor_spawn_poll(&spawning.handle, &spawning.actor);
    if (now == SPAWN_FAILED) {
      spawning.active = false;
      result(false, "spawn %s: the game couldn't make it", spawning.name);
      return false;
    }
  }
  if (spawning.actor && actor_read(spawning.actor, &a) && a.running) {
    spawning.active = false;
    result(true, "spawn %s: made after %d frames", spawning.name, spawning.frames);
    return false;
  }
  if (spawning.actor && !actor_read(spawning.actor, &a)) {
    spawning.active = false;
    result(false, "spawn %s: made, but it went at once", spawning.name);
    return false;
  }
  if (spawning.frames >= SPAWN_WAIT) {
    actor_spawn_cancel(&spawning.handle);
    spawning.active = false;
    result(false, "spawn %s: not made after %d frames", spawning.name, spawning.frames);
    return false;
  }
  return true;
}

/* "findref <hex>": where Link's components hold that value (an actor's address, say): each component [[link+0x270]+k]
 * read 0x1000 bytes deep, and one more pointer down; for finding what the game keeps about Link's targets */
static uint64_t parse_hex(const char *s)
{
  uint64_t v = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    s += 2;
  for (; *s; s++) {
    int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
    if (d < 0)
      break;
    v = v * 16 + (uint64_t)d;
  }
  return v;
}

static void run_findref(char *args)
{
  uint64_t value = parse_hex(take_word(&args));
  uintptr_t link = link_actor();
  if (!value || !link || !sys_mapped(link + 0x270, 8, SYS_PERM_R)) {
    result(false, "findref: a hex value, with Link there");
    return;
  }
  uintptr_t components = *(uintptr_t *)(link + 0x270);
  int found = 0;
  /* an actor's address: also its components, its attention sender, and its id (+0x18, u32) */
  uint64_t values[4] = { value, 0, 0, 0 };
  const char *what[4] = { "actor", "components", "sender", "id" };
  if (sys_mapped(value + 0x270, 8, SYS_PERM_R) && !(value & 7)) {
    values[1] = *(uint64_t *)(value + 0x270);
    if (values[1] && sys_mapped(values[1] + 0x160, 8, SYS_PERM_R))
      values[2] = *(uint64_t *)(values[1] + 0x160);
    values[3] = *(uint32_t *)(value + 0x18);
  }
  for (int k = 0; k < 0x800 && found < 24; k += 8) {
    if (!sys_mapped(components + k, 8, SYS_PERM_R))
      break;
    uintptr_t c = *(uintptr_t *)(components + k);
    if (!c || (c & 7) || !sys_mapped(c, 0x2000, SYS_PERM_R))
      continue;
    for (int o = 0; o < 0x2000 && found < 24; o += 4) {
      for (int w = 0; w < 4; w++) {
        if (!values[w])
          continue;
        bool hit = w == 3 ? *(uint32_t *)(c + o) == (uint32_t)values[w] : !(o & 7) && *(uint64_t *)(c + o) == values[w];
        if (hit) {
          result(true, "findref: %s at [comp+%#x]+%#x", what[w], k, o);
          found++;
        }
      }
      uint64_t v = (o & 7) ? 0 : *(uint64_t *)(c + o);
      if (v && !(v & 7) && sys_mapped(v, 0x200, SYS_PERM_R))
        for (int p = 0; p < 0x200 && found < 24; p += 8)
          for (int w = 0; w < 3; w++)
            if (values[w] && *(uint64_t *)(v + p) == values[w]) {
              result(true, "findref: %s at [[comp+%#x]+%#x]+%#x", what[w], k, o, p);
              found++;
            }
    }
  }
  result(true, "findref: %d found", found);
}

/* "attention": Link, his attention object ([[link+0x270]+0x578]) and what its do-action slot (+0x1A70) holds, for a
 * debugger's watchpoint */
static void run_attention(void)
{
  uintptr_t link = link_actor(), comps = 0, attention = 0, record = 0, target = 0;
  if (link && sys_mapped(link + 0x270, 8, SYS_PERM_R))
    comps = *(uintptr_t *)(link + 0x270);
  if (comps && sys_mapped(comps + 0x578, 8, SYS_PERM_R))
    attention = *(uintptr_t *)(comps + 0x578);
  if (attention && sys_mapped(attention + 0x1A70, 8, SYS_PERM_R))
    record = *(uintptr_t *)(attention + 0x1A70);
  if (record && sys_mapped(record + 8, 8, SYS_PERM_R))
    target = *(uintptr_t *)(record + 8);
  result(true, "attention: link %#lx components %#lx attention %#lx slot %#lx record %#lx target %#lx main %#lx",
         (unsigned long)link, (unsigned long)comps, (unsigned long)attention, (unsigned long)(attention + 0x1A70),
         (unsigned long)record, (unsigned long)target, (unsigned long)z64hd_main);
}

/* "actor <name>": the addresses of the actors with that name (any state), for findref and a debugger */
static void run_actor(char *args)
{
  char *name = take_word(&args);
  uintptr_t found[8];
  int n = *name ? actors_named(name, found, 8) : 0;
  for (int i = 0; i < n; i++) { /* its instance id ([[actor+0x3D8]+0x88]) and the rails it walks (guardview.c) */
    uintptr_t pre = sys_ptr(found[i] + 0x3D8);
    uint64_t instance = pre && !(pre & 7) && sys_mapped(pre + 0x88, 8, SYS_PERM_R) ? *(uint64_t *)(pre + 0x88) : 0;
    result(true, "actor: %s %#lx instance %llu rails %d", name, (unsigned long)found[i], (unsigned long long)instance,
           guardview_rails_of(found[i]));
  }
  result(n > 0, "actor: %d named %s", n, name);
}

/* "peek <hex address> [bytes]": memory, as 8-byte words (up to 256 bytes), for finding where the game keeps things */
static void run_peek(char *args)
{
  char *word = take_word(&args);
  uint64_t addr = 0;
  int32_t size = 64;
  for (const char *c = word[0] == '0' && (word[1] == 'x' || word[1] == 'X') ? word + 2 : word; *c; c++) {
    int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10
          : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
    if (d < 0) {
      result(false, "peek: a hex address, then how many bytes (up to 256)");
      return;
    }
    addr = addr * 16 + (uint64_t)d;
  }
  char *count = take_word(&args);
  if (*count && (!parse_int(count, &size) || size < 8 || size > 256)) {
    result(false, "peek: a hex address, then how many bytes (up to 256)");
    return;
  }
  addr &= ~(uint64_t)7;
  if (!addr || !sys_mapped((uintptr_t)addr, (size_t)size, SYS_PERM_R)) {
    result(false, "peek: %#llx isn't readable", (unsigned long long)addr);
    return;
  }
  char text[160];
  for (int32_t o = 0; o < size; o += 16) {
    const uint64_t *q = (const uint64_t *)(uintptr_t)(addr + (uint64_t)o);
    if (o + 8 < size)
      snprintf(text, sizeof(text), "%d | +%#05x %016llx %016llx", line_no, (int)o, (unsigned long long)q[0],
               (unsigned long long)q[1]);
    else
      snprintf(text, sizeof(text), "%d | +%#05x %016llx", line_no, (int)o, (unsigned long long)q[0]);
    out_line(text);
  }
  result(true, "peek %#llx %d", (unsigned long long)addr, (int)size);
}

/* "doortimer <name>": the door close watch put on the first door of that name (as its actor option does, without
 * adding the watch), and what it reads now: run it again while the door is open to see the search find its node */
static void run_door_timer(char *args)
{
  char *name = take_word(&args);
  uintptr_t found[8];
  int n = *name ? actors_named(name, found, 8) : 0;
  if (!n) {
    result(false, "doortimer: none named %s", name);
    return;
  }
  interactview_door_watch(found[0]);
  char text[32];
  interactview_door_text(text, sizeof(text));
  result(true, "doortimer: %s %#lx: %s, open %d (turned %.1f deg)", name, (unsigned long)found[0], text,
         interactview_door_is_open(found[0]), (double)interactview_door_angle(found[0]));
}

static void run_actors(const char *args)
{
  int32_t radius = 6;
  if (*args && (!parse_int(args, &radius) || radius < 1)) {
    result(false, "actors: a distance in metres");
    return;
  }
  static actor_near_t near[ACTORS_MAX];
  int n = actors_near((float)radius, near, ACTORS_MAX);
  char text[160];
  for (int i = 0; i < n; i++) {
    snprintf(text, sizeof(text), "%d | %.2f %.80s", line_no, (double)near[i].distance, near[i].name);
    out_line(text);
  }
  result(true, "%d actors within %d m", n, (int)radius);
}

static void run_status(bool held)
{
  link_state_t s;
  link_state(&s);
  vec3f_t pos = { 0, 0, 0 };
  bool has_pos = link_drawn_pos(&pos);
  const char *map = gd_ready() ? gd_get_str(GD_STR_MAP) : 0;
  const char *warp = warp_state();
  char motion[48];
  motion_text(&s, motion, sizeof(motion));
  result(true, "frame=%u map=%s link=%s x=%.3f y=%.3f z=%.3f facing=%.1f %s hand=%d zr=%d lock=%d ground=%d act=%d "
               "wpn=%d held=%d menu=%d game_paused=%d warp=%s",
         (unsigned)frame_counter(), map && map[0] ? map : "?", has_pos ? "yes" : "no", (double)pos.x, (double)pos.y,
         (double)pos.z, (double)s.facing, motion, (int)s.hand, (int)s.zr, (int)s.lock, (int)s.ground,
         (int)s.action, (int)s.weapon, (int)held, menu_current != 0, (int)(gd_ready() && gd_game_paused(0, 0)),
         warp && warp[0] ? warp : "-");
}

/* Run one line. What it waits for, if anything, is left in the state above. */
static void run_line(char *line, bool held)
{
  char *args = line;
  char *verb = take_word(&args);
  args = skip_spaces(args);
  trim_end(args);
  int32_t number = 1;
  const char *problem = 0;
  menu_t *menu = 0;
  menu_item_t *item = 0;

  if (same(verb, "id")) {
    snprintf(script_id, sizeof(script_id), "%s", *args ? take_word(&args) : "-");
  } else if (same(verb, "repeat")) {
    if (!parse_int(args, &number) || number < 1)
      result(false, "repeat: how many times");
    else if (repeat_depth >= REPEAT_DEPTH)
      result(false, "repeat: more than %d inside one another", REPEAT_DEPTH);
    else {
      repeats[repeat_depth].cursor = cursor;
      repeats[repeat_depth].line_no = line_no;
      repeats[repeat_depth++].left = number;
    }
  } else if (same(verb, "end")) {
    if (!repeat_depth)
      result(false, "end: no repeat to end");
    else if (--repeats[repeat_depth - 1].left > 0) {
      cursor = repeats[repeat_depth - 1].cursor;
      line_no = repeats[repeat_depth - 1].line_no;
    } else {
      repeat_depth--;
    }
  } else if (same(verb, "echo")) {
    result(true, "%s", args);
  } else if (same(verb, "status")) {
    run_status(held);
  } else if (same(verb, "place")) {
    run_place(args);
  } else if (same(verb, "actor")) {
    run_actor(args);
  } else if (same(verb, "texdump")) {
    uint64_t texture = parse_hex(take_word(&args));
    int32_t bpp = 8;
    char *b = take_word(&args);
    if (*b)
      parse_int(b, &bpp);
    nvn_texdump((uintptr_t)texture, (int)bpp);
    result(texture != 0, "texdump %#llx", (unsigned long long)texture);
  } else if (same(verb, "hudtest")) { /* hudtest <hex target> [<hex cmd> <nth bind>] | off */
    char *what = take_word(&args);
    nvn_hud_test = same(what, "on");
    nvn_hud_test_target = 0;
    if (!same(what, "off") && !same(what, "on")) {
      nvn_hud_test_target = (uintptr_t)parse_hex(what);
      char *cmd = take_word(&args);
      int32_t k = 0;
      nvn_hud_test_cmd = *cmd ? (uintptr_t)parse_hex(cmd) : 0;
      parse_int(take_word(&args), &k);
      nvn_hud_test_bind = (int)k;
      nvn_hud_test = k > 0;
    }
    result(true, "hudtest %#lx %#lx %d", (unsigned long)nvn_hud_test_target, (unsigned long)nvn_hud_test_cmd,
           nvn_hud_test_bind);
  } else if (same(verb, "rtbinds")) {
    uintptr_t cmds[16];
    int binds[16];
    int n = nvn_hud_binds(cmds, binds, 16);
    for (int i = 0; i < n; i++)
      result(true, "rtbinds: cmd %#lx: %d binds", (unsigned long)cmds[i], binds[i]);
    result(true, "rtbinds: %d command buffers", n);
    static char text[3072];
    nvn_hud_bind_log(text, sizeof(text));
    for (char *line = text; *line;) {
      char *end = line;
      while (*end && *end != '\n')
        end++;
      char keep = *end;
      *end = 0;
      result(true, "rtbinds: %s", line);
      if (!keep)
        break;
      line = end + 1;
    }
  } else if (same(verb, "hitlist")) {
    static char text[24576];
    char *name = take_word(&args);
    if (*name)
      hitview_list_all(name, text, sizeof(text));
    else
      hitview_list(text, sizeof(text));
    int n = 0;
    for (char *line = text; *line; n++) {
      char *end = line;
      while (*end && *end != '\n')
        end++;
      char keep = *end;
      *end = 0;
      result(true, "hitlist: %s", line);
      if (!keep)
        break;
      line = end + 1;
    }
    result(true, "hitlist: %d", n);
  } else if (same(verb, "guardbits")) { /* guardbits <actor name>: debugging, an actor's BSA flags (FINDINGS 9.43) */
    uintptr_t found[8];
    int n = actors_named(take_word(&args), found, 8);
    for (int i = 0; i < n; i++) {
      uintptr_t comps = sys_ptr(found[i] + 0x270), bsa = comps ? sys_ptr(comps + 0x188) : 0;
      uint32_t flags = 0;
      if (bsa && sys_mapped(bsa + 0x24, 4, SYS_PERM_R))
        flags = *(const volatile uint32_t *)(bsa + 0x24);
      const volatile float *rot = (const volatile float *)(found[i] + 0x328);
      double yaw = atan2f(rot[2], rot[8]) * 57.29578f;
      /* the animation's hold events (QueryASGetHoldEventGuard, main+0x2984F80): each slot's, bit 0x10 Guard */
      uintptr_t as = comps ? sys_ptr(comps + 0x40) : 0, a1 = as ? sys_ptr(as + 0x18) : 0, ctl = a1 ? sys_ptr(a1 + 0x18) : 0;
      uint32_t n_slots = 0, holds = 0;
      if (ctl && sys_mapped(ctl + 0x18, 0x10, SYS_PERM_R))
        n_slots = *(const volatile uint32_t *)(ctl + 0x18);
      uintptr_t slots = ctl ? sys_ptr(ctl + 0x20) : 0;
      for (uint32_t k = 0; slots && k < n_slots && k < 8; k++) {
        uintptr_t e = sys_ptr(slots + k * 0x48), h = e ? sys_ptr(e + 0x58) : 0;
        if (h && sys_mapped(h + 0x40, 4, SYS_PERM_R))
          holds |= (*(const volatile uint32_t *)(h + 0x40) & 0x10 ? 1u : 0u) << k;
      }
      actor_read_t ar;
      bool got = actor_read(found[i], &ar);
      int life = got && ar.has_life ? (int)ar.life : -1;
      result(true, "guardbits %#lx: bsa %#lx flags %#x guard %d, as slots %u guard holds %#x, facing %.1f, life %d at %.2f %.2f",
             (unsigned long)found[i], (unsigned long)bsa, (unsigned)flags, (int)(flags >> 5 & 1), (unsigned)n_slots,
             (unsigned)holds, yaw < 0 ? yaw + 360 : yaw, life, got ? (double)ar.pos[0] : 0.0, got ? (double)ar.pos[2] : 0.0);
    }
    result(true, "guardbits: %d", n);
  } else if (same(verb, "equip")) { /* equip shield <actor>: debugging */
    char *what = take_word(&args), *item = take_word(&args);
    static char kept[64];
    snprintf(kept, sizeof(kept), "%s", item);
    if (!same(what, "shield") || !*item || !pouch_equip_shield_now(kept))
      result(false, "equip: shield <actor name> (Weapon_Shield_Deku, _Hylian, _Mirror), Link's gear found");
    else
      result(true, "equip shield %s", kept);
  } else if (same(verb, "pausetype")) { /* pausetype [<name or hex id> <+n or -n>]: the game's pause types */
    char *name = take_word(&args), *by = take_word(&args);
    int32_t delta = 0;
    if (*name && same(by, "masks")) { /* pausetype <name> masks <hex> <hex> <hex>: what it stops (debugging) */
      uint32_t m[3], was[3];
      for (int k = 0; k < 3; k++)
        m[k] = pausemgr_id(take_word(&args));
      if (!pausemgr_set_masks(pausemgr_id(name), m, was)) {
        result(false, "pausetype: no such type");
        return;
      }
      result(true, "pausetype %s masks %08x %08x %08x (were %08x %08x %08x)", name, (unsigned)m[0], (unsigned)m[1],
             (unsigned)m[2], (unsigned)was[0], (unsigned)was[1], (unsigned)was[2]);
    } else if (*name) {
      uint32_t id = pausemgr_id(name);
      if (!*by || !parse_int(by[0] == '+' ? by + 1 : by, &delta) || !pausemgr_add(id, delta)) {
        result(false, "pausetype: a type's name or hex id, then +n or -n (and the manager found)");
        return;
      }
      result(true, "pausetype %s %#x: count %d", name, (unsigned)id, pausemgr_count(id));
    } else {
      static char text[2048];
      pausemgr_list(text, sizeof(text));
      for (char *line = text; *line;) {
        char *end = line;
        while (*end && *end != '\n')
          end++;
        char keep = *end;
        *end = 0;
        result(true, "pausetype: %s", line);
        if (!keep)
          break;
        line = end + 1;
      }
    }
  } else if (same(verb, "button")) { /* button <0..3> [<item>]: an item button's choice, or put an item on it */
    int32_t slot = -1;
    parse_int(take_word(&args), &slot);
    char *item = take_word(&args);
    uint32_t was = 0;
    bool ok = pouch_button((int)slot, item, &was);
    result(ok, "button %d: was %#x%s%s", (int)slot, (unsigned)was, *item ? ", now " : "", item);
  } else if (same(verb, "rtlist")) {
    uintptr_t tex[48];
    uint32_t sets[48];
    int n = frametrace_targets(tex, sets, 48);
    for (int i = 0; i < n; i++) {
      int w = 0, h = 0, f = 0;
      nvn_texture_info(tex[i], &w, &h, &f);
      result(true, "rtlist: %#lx %dx%d format %#x, bound %u times", (unsigned long)tex[i], w, h, f, (unsigned)sets[i]);
    }
    result(true, "rtlist: %d targets", n);
  } else if (same(verb, "frametrace")) {
    frametrace_arm();
    result(true, "frametrace: the next frame goes to the log");
  } else if (same(verb, "physdump")) {
    char *name = take_word(&args);
    int records = 0;
    int64_t bytes = 0;
    bool ok = physdump_write(*name ? name : "phys", &records, &bytes);
    result(ok, "physdump: %d records, %lld bytes", records, (long long)bytes);
  } else if (same(verb, "peek")) {
    run_peek(args);
  } else if (same(verb, "doortimer")) {
    run_door_timer(args);
  } else if (same(verb, "attention")) {
    run_attention();
  } else if (same(verb, "findref")) {
    run_findref(args);
  } else if (same(verb, "actors")) {
    run_actors(args);
  } else if (same(verb, "spawn")) {
    run_spawn(args);
  } else if (same(verb, "trace")) {
    if (same(args, "off"))
      number = 0;
    else if (!parse_int(args, &number) || number < 1) {
      result(false, "trace: a number of frames, or off");
      return;
    }
    trace_left = number;
    result(true, "trace %d", (int)number);
  } else if (same(verb, "wait")) {
    if (!parse_int(args, &number) || number < 0)
      result(false, "wait: a number of frames");
    else if (held && number > 0)
      result(false, "wait: the game is held, no frames pass (step lets them through)");
    else {
      wait_left = number;
      result(true, "wait %d", (int)number);
    }
  } else if (same(verb, "fire")) {
    int cmd = command_by_key(take_word(&args));
    char *frames = take_word(&args);
    number = 0;
    if (cmd < 0)
      result(false, "fire: not a command (settings.ini's bind_<key> names: savepos, loadpos, ragdoll...)");
    else if (*frames && (!parse_int(frames, &number) || number < 0))
      result(false, "fire: a number of frames to hold it");
    else {
      if (held)
        fire_pending[cmd] = (uint16_t)(1 + (number > 60000 ? 60000 : number)); /* on the next frame let through */
      else
        binds_fire((command_t)cmd, number);
      result(true, "fire %s", command_keys[cmd]);
    }
  } else if (same(verb, "do")) {
    problem = resolve(args, &menu, &item);
    if (!problem && !item)
      problem = "which item?";
    if (!problem)
      problem = choose(item);
    if (problem)
      result(false, "do: %s", problem);
    else {
      char value[160];
      item_value(item, value, sizeof(value));
      result(true, "%s%s%s", kind_names[item->kind], value[0] ? " = " : "", value);
    }
  } else if (same(verb, "set")) {
    char *equals = args;
    while (*equals && *equals != '=')
      equals++;
    if (!*equals) {
      result(false, "set: <menu path> = <value>");
      return;
    }
    *equals = 0;
    char *value = skip_spaces(equals + 1);
    trim_end(args);
    problem = resolve(args, &menu, &item);
    if (!problem && !item)
      problem = "which item?";
    if (!problem)
      problem = assign(item, value);
    if (problem)
      result(false, "set: %s", problem);
    else {
      char now[160];
      item_value(item, now, sizeof(now));
      result(true, "%s = %s", kind_names[item->kind], now);
    }
  } else if (same(verb, "get")) {
    problem = resolve(args, &menu, &item);
    if (!problem && !item)
      problem = "which item?";
    if (problem)
      result(false, "get: %s", problem);
    else {
      char value[160];
      item_value(item, value, sizeof(value));
      result(true, "%s = %s", kind_names[item->kind], value);
    }
  } else if (same(verb, "list")) {
    problem = resolve(args, &menu, &item);
    if (!problem && item && (item->kind != MI_SUBMENU || !item->submenu))
      problem = "not a menu";
    if (problem)
      result(false, "list: %s", problem);
    else
      list_menu(item ? item->submenu : menu);
  } else if (same(verb, "open")) {
    /* the menu on screen at that menu, or at that item of its menu (for a look at it) */
    problem = resolve(args, &menu, &item);
    if (problem)
      result(false, "open: %s", problem);
    else {
      menu_t *target = item && item->kind == MI_SUBMENU && item->submenu ? item->submenu : menu;
      if (!menu_current)
        menu_open(&menu_root);
      menu_goto(target);
      if (item && target == menu)
        target->selected = (int)(item - menu->items);
      result(true, "open %s", target->title);
    }
  } else if (same(verb, "pause")) {
    if (held)
      result(true, "paused");
    else {
      pause_wanted = true;
      pause_waiting = true; /* its result once the game is held */
    }
  } else if (same(verb, "step")) {
    if (*args && (!parse_int(args, &number) || number < 1))
      result(false, "step: a number of frames");
    else {
      if (!held)
        pause_wanted = true;
      steps_left = number;
      stepping = true; /* its result once they've gone through and the game is held again */
    }
  } else if (same(verb, "run")) {
    if (!held)
      result(true, "running");
    else {
      run_wanted = true;
      run_waiting = true;
    }
  } else if (same(verb, "title")) {
    if (*args)
      result(false, "title: nothing goes after it");
    else if ((problem = title_start())) /* held: frame advance lets the game go (title_going) */
      result(false, "title: %s", problem);
    else
      title_waiting = true; /* its result once Link is gone, or not */
  } else if (same(verb, "input")) {
    problem = run_input(args);
    if (problem)
      result(false, "input: %s", problem);
    else
      result(true, "input 0x%x for %d", (unsigned)input_remote_buttons, input_frames);
  } else {
    result(false, "not a command: %s", verb);
  }
}

/* --- the script ------------------------------------------------------------------------------ */

/* The script's next line with something on it (ended in place), or 0 at its end. */
static char *next_line(void)
{
  static char line_buf[LINE_MAX_LEN]; /* a copy: the script's text stays as it is, for a repeat to run again */
  while (cursor < script_len) {
    size_t end = cursor, len = 0;
    for (; end < script_len && script[end] != '\n'; end++)
      if (len < sizeof(line_buf) - 1)
        line_buf[len++] = script[end];
    line_buf[len] = 0;
    cursor = end < script_len ? end + 1 : end;
    line_no++;
    char *line = skip_spaces(line_buf);
    trim_end(line);
    if (*line && *line != '#')
      return line;
  }
  return 0;
}

/* Take cmd.txt if there is one: read it, delete it (so the next can be written), start running it. */
static void poll(void)
{
  static int calls;
  static bool stuck; /* cmd.txt couldn't be deleted: said once, not run again and again */
  if (++calls < POLL_CALLS)
    return;
  calls = 0;
  fs_file_t file;
  if (fs_open(&file, CMD_FILE, FS_READ) != 0) {
    stuck = false;
    return;
  }
  int64_t size = 0;
  size_t got = 0;
  fs_size(&size, file);
  bool fits = size >= 0 && size < SCRIPT_MAX;
  if (fits && size && fs_read(&got, file, 0, script, (size_t)size) != 0)
    got = 0;
  fs_close(file);
  if (stuck)
    return;
  line_no = 0;
  snprintf(script_id, sizeof(script_id), "-");
  if (fs_delete_file(CMD_FILE) != 0) {
    stuck = true;
    result(false, "cmd.txt can't be deleted: not run");
    return;
  }
  if (!fits) {
    result(false, "cmd.txt is over %d bytes: not run", SCRIPT_MAX - 1);
    return;
  }
  script_len = got;
  script[got] = 0;
  cursor = 0;
  repeat_depth = 0;
  running = true;
}

/* Whether the line last run is still waiting; says so (its result) when the wait is over. */
static bool waiting(bool held)
{
  if (wait_left > 0) {
    if (!held || stepping)
      return true;
    /* the game was held while it waited (frame advance, or leaving a camera tool under it): no frames pass now,
     * so it ends here rather than never */
    result(false, "wait: the game was held with %d frames to go (step lets them through)", (int)wait_left);
    wait_left = 0;
  }
  if (spawning.active && spawn_waiting())
    return true;
  if (pause_waiting) {
    if (!held)
      return true;
    pause_waiting = false;
    result(true, "paused");
  }
  if (stepping) {
    if (steps_left > 0 || !held)
      return true;
    stepping = false;
    result(true, "stepped");
  }
  if (run_waiting) {
    if (held)
      return true;
    run_waiting = false;
    result(true, "running");
  }
  if (title_waiting) {
    const char *text = "";
    title_status_t now = title_status(&text);
    if (now == TITLE_PENDING)
      return true;
    title_waiting = false;
    result(now == TITLE_DONE, "title: %s", text);
  }
  return false;
}

void remote_frame(bool held)
{
  frametrace_write(); /* the debugging captures the render thread made: written here, on the game's thread */
  nvn_texdump_write();
  if (!remote_enabled || !settings_sd()) {
    /* remote control off: what a script holds or has queued goes (its input would otherwise stay on the
     * controller, "for 0" for good, as nothing here counts it down) */
    if (input_remote_active)
      input_off();
    for (int c = 0; c < CMD_COUNT; c++)
      fire_pending[c] = 0;
    return;
  }
  if (!held) { /* a frame of the game */
    for (int c = 0; c < CMD_COUNT; c++)
      if (fire_pending[c]) {
        binds_fire((command_t)c, fire_pending[c] - 1);
        fire_pending[c] = 0;
      }
    if (input_frames > 0 && --input_frames == 0)
      input_off();
    if (wait_left > 0)
      wait_left--;
    if (spawning.active) /* frames since the spawn line */
      spawning.frames++;
    if (trace_left > 0) { /* this frame's, before any line of it runs */
      trace_line();
      trace_left--;
    }
  }
  if (!running)
    poll();
  size_t traced = out_len; /* more than the trace's lines after this: the sender is waiting to read it */
  for (int lines = 0; running && lines < LINES_PER_CALL && !waiting(held); lines++) {
    char *line = next_line();
    if (!line) {
      running = false;
      out_line("end");
      break;
    }
    run_line(line, held);
  }
  /* a trace alone is written a buffer at a time, not a file write a frame; its last frames at once */
  if (out_len != traced || trace_left <= 0 || out_len > OUT_MAX - 512)
    out_flush();
}

bool remote_take_pause(void)
{
  bool wanted = pause_wanted;
  pause_wanted = false;
  return wanted;
}

bool remote_take_step(void)
{
  if (steps_left <= 0)
    return false;
  steps_left--;
  return true;
}

bool remote_take_run(void)
{
  bool wanted = run_wanted;
  run_wanted = false;
  return wanted;
}
