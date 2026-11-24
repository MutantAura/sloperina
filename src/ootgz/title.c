/* Back to the title screen (title.h, FINDINGS 9.26).
 *
 * The game has no "go to the title" call: its sequence graphs (romfs/Sequence) change state. From play there are two
 * ways to the Title state of FormalSequence.root: the pause screen's System > Return to Title (PlusMenu ›
 * ConfirmationBackToTitle › PlusMenuToTitle, chosen by the screen's buttons), and the end of an event whose result
 * is "Title" (Event › AfterEvent reads the String16 Sequence_EventResult; "Title" runs EventToTitle). The second is
 * how Game Over's Quit leaves play: DmF_SY_GameOver's GameSystemActor.EventSetEventResult(EventResult=Title), an
 * AINB command that only writes Sequence_EventResult through OneShotSetNonStructGameDataString16.
 *
 * So: request DefaultParentEvent (a registered event whose flowchart is one 30-frame GameSystemActor.EventWait),
 * and once it runs, write "Title" with the game's own SetString16. When it ends, AfterEvent takes the game to the
 * title screen as it does after Game Over > Quit; Sequence_ToTitleFromGameOver / FromGameClear and
 * Sequence_RequestLastGameOverSave stay false, so EventToTitle saves nothing. The result is written only while our
 * event runs, so a refused or dropped request leaves nothing behind; once the event is gone, the result is set back
 * to "" (what AfterEvent leaves it as anyway). */
#include "title.h"

#include <stdint.h>

#include "eventskip.h"
#include "gamedata.h"
#include "gfx.h"
#include "link.h"
#include "memfile.h"
#include "sys.h"
#include "warp.h"
#include "z64hd.h"

#define F_EVENT_RESULT         0x6000F875u /* Sequence_EventResult (String16): "Title" in AfterEvent */
#define F_IS_LOADING_BANC      0xEF2B60C2u /* Sequence_IsLoadingBanc */
#define F_IS_SAVE_LOADING      0xF5DEEF0Au /* System_IsSaveLoading */
#define F_IS_GAME_OVER         0x2FD0BC6Cu /* IsGameOverTrig */
#define F_FROM_GAME_OVER       0x53F4FC89u /* Sequence_ToTitleFromGameOver: EventToTitle's Game Over cleanup */
#define F_FROM_GAME_CLEAR      0x04D31E64u /* Sequence_ToTitleFromGameClear: EventToTitle's game clear save */
#define F_LAST_GAME_OVER_SAVE  0x2C095360u /* Sequence_RequestLastGameOverSave: EventToTitle's Game Over save */

#define CARRIER_EVENT "DefaultParentEvent" /* also its flowchart's name, which the running flow carries */
#define RESULT_TITLE "Title"
#define START_TIMEOUT_FRAMES (10 * GAME_FPS) /* the request is queued until Link can join an event */
#define TIDY_FRAMES (1 * GAME_FPS)            /* no event this long after ours: AfterEvent has read the result */

typedef void (*set_string16_t)(void *manager, const char *const *value, uint32_t hash);

static enum { W_NONE, W_START, W_END } watch; /* what title_frame waits for */
static title_status_t status;
static const char *status_text = "";
static uint32_t deadline_frame;
static int quiet_frames; /* W_END: frames in a row with no event running */

bool title_known(void)
{
  return z64hd_game && z64hd_game->gd_set_string16 && eventskip_known() && z64hd_game->event_req_submit;
}

static bool same(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static bool flag(uint32_t hash)
{
  bool on = false;
  gd_get_bool(hash, &on);
  return on;
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

static void fail(const char *why)
{
  watch = W_NONE;
  status = TITLE_FAILED;
  status_text = why;
  gfx_notice(why);
}

const char *title_start(void)
{
  if (!title_known())
    return "return to title: not known for this game version";
  if (watch != W_NONE)
    return "already on the way to the title screen";
  if (!gd_ready() || !gd_manager())
    return "can't return to title: game data not found";
  if (!link_actor())
    return "can't return to title: not in the game";
  if (flag(F_IS_LOADING_BANC) || warp_pending() || memfile_busy())
    return "can't return to title while a map is loading";
  if (flag(F_IS_SAVE_LOADING))
    return "can't return to title while the game saves or loads";
  if (flag(F_IS_GAME_OVER) || flag(F_FROM_GAME_OVER) || flag(F_FROM_GAME_CLEAR) || flag(F_LAST_GAME_OVER_SAVE))
    return "can't return to title during a game over";
  if (eventskip_running())
    return "can't return to title during a cutscene or event";
  if (gd_game_pausing() || gd_game_paused(0, 0) || gd_game_menu_screen())
    return "can't return to title while the game is paused";
  if (!warp_request_event(CARRIER_EVENT))
    return "can't return to title: the game refused the event";
  watch = W_START;
  status = TITLE_PENDING;
  status_text = "waiting for the game to start the event";
  deadline_frame = frame_counter() + START_TIMEOUT_FRAMES;
  log_printf("title: requested %s", CARRIER_EVENT);
  return 0;
}

void title_frame(void)
{
  if (watch == W_START) {
    char name[EVENTSKIP_NAME_MAX];
    if (eventskip_running() && eventskip_flow_name(name)) {
      if (!same(name, CARRIER_EVENT)) {
        log_printf("title: %s started instead of %s", name, CARRIER_EVENT);
        fail("can't return to title: another event started first");
      } else if (!set_result(RESULT_TITLE)) {
        fail("can't return to title: game data not found");
      } else {
        watch = W_END;
        quiet_frames = 0;
        status = TITLE_DONE;
        status_text = "on the way to the title screen";
        log_printf("title: %s runs, %s set", CARRIER_EVENT, RESULT_TITLE);
      }
    } else if ((int32_t)(frame_counter() - deadline_frame) > 0) {
      fail("can't return to title: the game didn't start the event");
    }
  } else if (watch == W_END) {
    if (eventskip_running()) {
      quiet_frames = 0;
    } else if (++quiet_frames >= TIDY_FRAMES) {
      set_result(""); /* AfterEvent left it so if it read it; otherwise "Title" mustn't wait for another event */
      watch = W_NONE;
      log_printf("title: event over, result cleared");
    }
  }
}

title_status_t title_status(const char **text)
{
  if (text)
    *text = status_text;
  return status;
}
