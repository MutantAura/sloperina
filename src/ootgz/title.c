/* Back to the title screen, whatever the game is doing (title.h, FINDINGS 9.26).
 *
 * The game has no "go to the title" call: its sequence graphs (romfs/Sequence) change state. From play, the Title
 * state of FormalSequence.root is reached only from PlusMenu (the pause screen's Return to Title, chosen by its
 * buttons) and from Event: when no event plays any more, Event runs AfterEvent, which reads the String16
 * Sequence_EventResult at once and, if it's "Title", runs EventToTitle (how Game Over's Quit leaves play). A game
 * over is itself an event (Main › BeforeGameOver › BeforeEvent › Event), so is a cutscene, a dialogue, a chest.
 *
 * So, every frame until Link is gone (EventToTitle unloads the map with him):
 *   - a map loading (or reserved by an exit or warp's event: ending that event now leaves the load half begun), the
 *     game saving or loading, a warp or memfile under way: wait for it to finish (a warp armed for the next exit is
 *     cancelled instead);
 *   - one of the game's menus up (pause screen, map, item wheel): press B for it until it has closed, as its own
 *     screens go back to Main, never to Event, on anything but their own choices;
 *   - an event running (any: a cutscene, a game over, ours): write "Title" while it runs, then end it with the event
 *     manager's own cancel (eventskip.h), again every so often while one still runs (a chain goes on to the next);
 *     during a game over, Sequence_ToTitleFromGameOver too, so EventToTitle releases the game over's pause as its
 *     Quit does;
 *   - none of these: ask for an event to carry the result: DefaultParentEvent (one 30-frame wait), or if the game
 *     hasn't started it soon, DefaultBootEvent, whose setting ignores what Link is doing (in the air, swimming...).
 * "Title" is written only while an event runs, so AfterEvent is the only reader; once Link is gone (or on giving
 * up) it's set back to "", what AfterEvent leaves it as. Nothing is saved: Sequence_RequestLastGameOverSave and
 * Sequence_ToTitleFromGameClear stay as they are (false outside the game's own flows). */
#include "title.h"

#include <stdint.h>

#include "binds.h"
#include "eventskip.h"
#include "freecam.h"
#include "gamedata.h"
#include "gfx.h"
#include "input.h"
#include "link.h"
#include "memfile.h"
#include "sys.h"
#include "warp.h"
#include "z64hd.h"

#define F_EVENT_RESULT     0x6000F875u /* Sequence_EventResult (String16): "Title" in AfterEvent */
#define F_IS_LOADING_BANC  0xEF2B60C2u /* Sequence_IsLoadingBanc */
#define F_IS_SAVE_LOADING  0xF5DEEF0Au /* System_IsSaveLoading */
#define F_FROM_GAME_OVER   0x53F4FC89u /* Sequence_ToTitleFromGameOver: EventToTitle's Game Over cleanup */

#define CARRIER_EVENT "DefaultParentEvent"     /* a 30-frame wait; waits until Link can join an event */
#define CARRIER_ANYWAY "DefaultBootEvent"      /* FlowchartWithIgnoreDissentPlay: starts whatever Link does */
#define RESULT_TITLE "Title"
#define WRITE_FRAMES 2                         /* "Title" written this long in an event before it's ended */
#define END_AGAIN_FRAMES 20                    /* an event still running this long after the end: end it again */
#define REQUEST_AGAIN_FRAMES 30                /* no event this long after asking: ask again (the other carrier) */
#define B_DOWN_FRAMES 3                        /* a menu up: B held this long, then let go as long again */
#define LINK_GONE_FRAMES 10                    /* no Link this long, nothing loading: on the title screen */
#define RESERVED_WAIT_FRAMES (10 * GAME_FPS)   /* a map change reserved but not loading this long: stale, go on */
#define LOG_EVERY_FRAMES GAME_FPS
#define GIVE_UP_FRAMES (60 * GAME_FPS)

typedef void (*set_string16_t)(void *manager, const char *const *value, uint32_t hash);

static bool going;
static title_status_t status;
static const char *status_text = "";
static uint32_t frames_going;
static uint32_t event_frames;   /* frames in a row an event has run */
static uint32_t idle_frames;    /* frames in a row with nothing running, since the last request */
static uint32_t requests;
static uint32_t menu_frames;
static uint32_t gone_frames;
static bool ended_one;          /* an event was ended with "Title" written */
static bool loaded;             /* a map load has been seen since title_start */
static uint32_t reserved_frames; /* frames a map change has been reserved without loading */
static bool set_game_over;      /* Sequence_ToTitleFromGameOver was set here: unset on giving up */

bool title_known(void)
{
  return z64hd_game && z64hd_game->gd_set_string16 && eventskip_known() && z64hd_game->event_req_submit;
}

static bool flag(uint32_t hash)
{
  bool on = false;
  gd_get_bool(hash, &on);
  return on;
}

static bool contains(const char *s, const char *part)
{
  for (; *s; s++) {
    const char *a = s, *b = part;
    while (*a && *a == *b)
      a++, b++;
    if (!*b)
      return true;
  }
  return false;
}

/* Sequence_EventResult = value, through the game's SetString16 (applied at the next GameData flush) */
static bool set_result(const char *value)
{
  void *manager = gd_manager();
  if (!manager)
    return false;
  const char *text = value;
  MAIN_FN(set_string16_t, z64hd_game->gd_set_string16)(manager, &text, F_EVENT_RESULT);
  return true;
}

static void stop(title_status_t how, const char *text)
{
  going = false;
  input_title_buttons = 0;
  set_result("");
  if (how == TITLE_FAILED && set_game_over)
    gd_set_bool(F_FROM_GAME_OVER, false);
  set_game_over = false;
  status = how;
  status_text = text;
  log_printf("title: %s (%u frames, %u requests)", text, (unsigned)frames_going, (unsigned)requests);
  if (how == TITLE_FAILED)
    gfx_notice(text);
}

const char *title_start(void)
{
  if (!title_known())
    return "return to title screen: not known for this game version";
  if (going)
    return "already on the way to the title screen";
  if (!gd_ready() || !gd_manager())
    return "can't return to title screen: game data not found";
  if (!link_actor() && !flag(F_IS_LOADING_BANC) && !warp_pending() && !memfile_busy())
    return "already at the title screen";
  going = true;
  frames_going = event_frames = menu_frames = gone_frames = requests = 0;
  idle_frames = REQUEST_AGAIN_FRAMES; /* ask at once */
  ended_one = loaded = false;
  reserved_frames = 0;
  if (freecam_on)
    freecam_set(false); /* the camera it hooks goes with the map */
  if (warp_pending() && !warp_requested())
    warp_cancel(); /* armed for the next exit: nothing will load until Link takes one */
  status = TITLE_PENDING;
  status_text = "on the way to the title screen";
  log_printf("title: going");
  return 0;
}

bool title_going(void)
{
  return going;
}

void title_frame(void)
{
  if (cmd_pressed(CMD_TITLE)) { /* unbound unless given buttons (settings > rebind commands) */
    const char *problem = title_start();
    if (problem)
      gfx_notice(problem);
  }
  if (!going)
    return;
  input_title_buttons = 0;
  if (++frames_going > GIVE_UP_FRAMES) {
    stop(TITLE_FAILED, "can't return to title screen: the game didn't leave play within a minute");
    return;
  }
  bool loading = flag(F_IS_LOADING_BANC) || flag(F_IS_SAVE_LOADING) || warp_pending() || memfile_busy();
  if (flag(F_IS_LOADING_BANC))
    loaded = true; /* the reserved map change has begun loading */
  /* A map change reserved (an exit, a warp: Sequence_ReservedNextBanc) and not loaded yet: its event mustn't be ended
   * now, or the load starts on the way out and never finishes (the game stays black with IsLoadingBanc set). It's
   * let finish first; a reservation left over (no load within 10 s) is ignored. */
  const char *next = gd_get_str(GD_STR_NEXT_MAP);
  if (!loading && !loaded && next && *next && reserved_frames < RESERVED_WAIT_FRAMES) {
    reserved_frames++;
    loading = true;
  }
  if (frames_going % LOG_EVERY_FRAMES == 0)
    log_printf("title: waiting: loading %d, next map %s, menu %d, paused %d, event %d, link %d", loading,
               next && *next ? next : "-", gd_game_menu_screen(), gd_game_paused(0, 0), eventskip_running(),
               link_actor() != 0);
  if (!link_actor() && !loading) {
    if (++gone_frames >= LINK_GONE_FRAMES)
      stop(TITLE_DONE, ended_one ? "at the title screen" : "Link is gone: at the title screen");
    return;
  }
  gone_frames = 0;
  if (loading) { /* a map, a save: let it finish, then go on */
    event_frames = 0;
    idle_frames = 0;
    return;
  }
  bool busy = false;
  if (gd_game_menu_screen() || gd_game_paused(0, 0) || gd_game_pausing()) {
    busy = true;
    menu_frames++;
    if (menu_frames % (2 * B_DOWN_FRAMES) < B_DOWN_FRAMES)
      input_title_buttons = BTN_B;
  } else {
    menu_frames = 0;
  }
  if (eventskip_running()) {
    busy = true;
    char name[EVENTSKIP_NAME_MAX];
    if (event_frames == 0 && eventskip_flow_name(name)) {
      log_printf("title: event %s running", name);
      if (contains(name, "GameOver"))
        set_game_over = gd_set_bool(F_FROM_GAME_OVER, true) || set_game_over;
    }
    set_result(RESULT_TITLE);
    if (event_frames >= WRITE_FRAMES && (event_frames - WRITE_FRAMES) % END_AGAIN_FRAMES == 0) {
      if (eventskip_end_now())
        ended_one = true;
      log_printf("title: ending the event (frame %u)", (unsigned)event_frames);
    }
    event_frames++;
  } else {
    event_frames = 0;
  }
  if (busy) {
    idle_frames = 0;
    return;
  }
  if (++idle_frames >= REQUEST_AGAIN_FRAMES) {
    idle_frames = 0;
    const char *carrier = requests % 2 == 0 ? CARRIER_EVENT : CARRIER_ANYWAY;
    bool taken = warp_request_event(carrier);
    requests++;
    log_printf("title: requested %s: %s", carrier, taken ? "taken" : "refused");
  }
}

title_status_t title_status(const char **text)
{
  if (text)
    *text = status_text;
  return status;
}
