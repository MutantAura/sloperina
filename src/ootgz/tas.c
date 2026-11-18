/* The input macro: record and play back controller input, frame by frame (tas.h).
 *
 * A frame is the interval from one frame hook to the next. tas_frame, at the top of the hook, bumps a
 * frame generation; every GetNpadStates call the game makes until the next hook belongs to that frame,
 * whichever of the game's job threads makes it (FINDINGS: 1 read a frame on the title and in menus, 2 in
 * gameplay, each a fresh sample).
 *
 * Recording: the first read of a frame (per overload) copies what the game saw, after input.c's filtering,
 * into a latch; later reads of the same frame are given the latch, so the two gameplay reads match and the
 * recorded run is the one playback gives. The next hook stores the latch as the frame (or the previous
 * frame's input when the game read nothing, e.g. while loading), so frames stay one-to-one with hooks.
 * Playback: each hook sets the frame the wrappers give out next, to every read of that frame.
 *
 * Frame advance's hold blocks the game inside the hook, after tas_frame, and the game reads nothing while
 * held (measured), so a step is exactly one macro frame and a hold adds none.
 *
 * The buffer is static (bss), as nvn.c's picture: the game's heap can't spare large blocks. */
#include "tas.h"

#include "gfx.h"
#include "lang.h"
#include "link.h"
#include "menus.h"
#include "position.h"
#include "sys.h"

#define CONFIRM_SECONDS 3 /* start recording again within this to record over a macro */
#define LATCH_SPIN 20000  /* a read waiting for the first read's copy gives up after this many tries */

static tas_frame_t tas_buf[TAS_MAX_FRAMES]; /* 432 KB */
static uint32_t tas_len;                    /* frames recorded */
static uint32_t tas_cursor;                 /* next frame to record or play */
static tas_state_t tas_st;

/* where recording began (return to start, restart): Link, as position.c saves him, and his map */
typedef struct {
  bool valid;
  position_slot_t pos;
} tas_start_t;
static tas_start_t tas_start;

static bool rec_armed;       /* recording starts at the next hook (the game is being paused first) */
static bool rec_active;      /* frames are being recorded: the next hook stores this frame's input */
static tas_frame_t rec_prev; /* the last frame stored: stored again for a frame with no reads */
static uint64_t confirm_at;  /* when "start recording" last asked to record over the macro (0: not asked) */

/* Link being put back at the start (position_busy): the game gets no input meanwhile, then this happens */
typedef enum { AFTER_NOTHING, AFTER_RECORD, AFTER_PLAY } tas_after_t;
static bool waiting;
static tas_after_t wait_after;
static bool wait_repause; /* the game was paused: it runs while Link is moved, then pauses again */

bool tas_log_positions; /* macro > log position each frame: "tas: frame N x y z", for the replay test */

/* --- shared with the wrappers (any thread) ------------------------------------------------------ */

enum { MODE_OFF, MODE_RECORD, MODE_PLAY, MODE_BLANK };
static uint32_t mode;     /* what the wrappers do */
static uint32_t frame_gen = 1; /* bumped by each hook; 0 never names a frame */

/* recording: per overload, the first read of each frame, in two buffers by the generation's parity (the
 * hook stores the frame just ended while the next frame's reads may already fill the other) */
static uint32_t latch_claim[NPAD_STYLES]; /* the generation whose first read took the latch */
static uint32_t latch_ready[NPAD_STYLES]; /* ... and finished copying into it */
static tas_frame_t latch[NPAD_STYLES][2];
static bool latch_connected[NPAD_STYLES][2];

/* playback: the frame being played, double-buffered so a read never sees half of one */
static tas_frame_t play_slot[2];
static uint32_t play_index;

static void set_mode(uint32_t m) { __atomic_store_n(&mode, m, __ATOMIC_RELEASE); }

static tas_frame_t frame_of(const npad_state_t *s)
{
  return (tas_frame_t){ (uint32_t)s->buttons, (int16_t)s->lx, (int16_t)s->ly, (int16_t)s->rx, (int16_t)s->ry };
}

static void give(npad_state_t *s, const tas_frame_t *f)
{
  s->buttons = (s->buttons & ~(uint64_t)0xFFFFFFFFu) | f->buttons;
  s->lx = f->lx;
  s->ly = f->ly;
  s->rx = f->rx;
  s->ry = f->ry;
}

void tas_filter(int style, npad_state_t *states, int count)
{
  uint32_t m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
  if (m == MODE_OFF || count <= 0 || style < 0 || style >= NPAD_STYLES)
    return;
  if (m == MODE_BLANK) {
    for (int i = 0; i < count; i++) {
      states[i].buttons = 0;
      states[i].lx = states[i].ly = states[i].rx = states[i].ry = 0;
    }
    return;
  }
  if (m == MODE_PLAY) {
    /* last, after the menu's blanking and the hide mask: the game gets the macro, nothing stripped. Only
     * connected states: the game ignores the others, and a style that isn't there stays empty */
    tas_frame_t f = play_slot[__atomic_load_n(&play_index, __ATOMIC_ACQUIRE)];
    for (int i = 0; i < count; i++)
      if (states[i].attributes & NPAD_ATTR_CONNECTED)
        give(&states[i], &f);
    return;
  }
  /* MODE_RECORD */
  uint32_t gen = __atomic_load_n(&frame_gen, __ATOMIC_ACQUIRE);
  int parity = gen & 1;
  if (__atomic_load_n(&latch_ready[style], __ATOMIC_ACQUIRE) != gen) {
    uint32_t claimed = __atomic_load_n(&latch_claim[style], __ATOMIC_ACQUIRE);
    if (claimed != gen &&
        __atomic_compare_exchange_n(&latch_claim[style], &claimed, gen, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      /* the first read of this frame: what it saw is the frame */
      latch[style][parity] = frame_of(&states[0]);
      latch_connected[style][parity] = (states[0].attributes & NPAD_ATTR_CONNECTED) != 0;
      __atomic_store_n(&latch_ready[style], gen, __ATOMIC_RELEASE);
    } else {
      /* another read of this frame is copying it right now: wait for its copy */
      for (int spin = 0; spin < LATCH_SPIN && __atomic_load_n(&latch_ready[style], __ATOMIC_ACQUIRE) != gen; spin++)
        __asm__ volatile("yield");
      if (__atomic_load_n(&latch_ready[style], __ATOMIC_ACQUIRE) != gen)
        return; /* never came (the hook moved on): leave this read as it is */
    }
  }
  tas_frame_t f = latch[style][parity];
  for (int i = 0; i < count; i++)
    give(&states[i], &f);
}

/* --- the state machine (game thread) ------------------------------------------------------------ */

uint32_t tas_length(void) { return tas_len; }
uint32_t tas_position(void) { return tas_cursor; }
tas_state_t tas_state(void) { return tas_st; }
bool tas_waiting(void) { return waiting; }

static void log_position(uint32_t frame)
{
  if (!tas_log_positions)
    return;
  vec3f_t p;
  if (link_drawn_pos(&p))
    log_printf("tas: frame %u %.3f %.3f %.3f", (unsigned)frame, p.x, p.y, p.z);
  else
    log_printf("tas: frame %u -", (unsigned)frame);
}

static void stop_recording(void)
{
  tas_st = tas_len ? TAS_STOPPED : TAS_IDLE;
  tas_cursor = 0;
  rec_armed = rec_active = false;
  waiting = false;
  set_mode(MODE_OFF);
}

/* the frame that just ended, from its first read's latch (a connected controller's, FullKey first) */
static void commit(uint32_t gen)
{
  tas_frame_t f = rec_prev;
  bool found = false;
  for (int s = 0; s < NPAD_STYLES; s++) {
    if (__atomic_load_n(&latch_ready[s], __ATOMIC_ACQUIRE) != gen)
      continue;
    if (latch_connected[s][gen & 1] || !found) {
      f = latch[s][gen & 1];
      found = true;
      if (latch_connected[s][gen & 1])
        break;
    }
  }
  tas_buf[tas_len++] = f;
  tas_cursor = tas_len;
  rec_prev = f;
  if (tas_len >= TAS_MAX_FRAMES) {
    stop_recording();
    log_printf("tas: buffer full, %u frames", (unsigned)tas_len);
    gfx_notice("macro: full (10 minutes), recording stopped");
  }
}

/* frames from now on are recorded (the reads after this, in this frame, are the first) */
static void begin_recording(bool save_start)
{
  if (save_start) {
    tas_start.valid = position_save_to(&tas_start.pos);
    if (!tas_start.valid)
      gfx_notice("macro: start position not saved, return to start won't work");
  }
  rec_prev = (tas_frame_t){ 0 };
  rec_active = true;
  rec_armed = false;
  set_mode(MODE_RECORD);
  log_printf("tas: recording from here");
  log_position(tas_len);
}

/* the frame the reads after this get, until the next hook */
static void play_next(void)
{
  if (tas_cursor >= tas_len) {
    tas_st = TAS_STOPPED;
    tas_cursor = 0;
    set_mode(MODE_OFF);
    log_printf("tas: playback done, %u frames", (unsigned)tas_len);
    gfx_notice("macro: done");
    return;
  }
  log_position(tas_cursor);
  uint32_t next = __atomic_load_n(&play_index, __ATOMIC_RELAXED) ^ 1;
  play_slot[next] = tas_buf[tas_cursor++];
  __atomic_store_n(&play_index, next, __ATOMIC_RELEASE);
  set_mode(MODE_PLAY);
}

/* Link is being put back (position_load_from has started): no input until he's there, then `after` */
static void begin_wait(tas_after_t after)
{
  waiting = true;
  wait_after = after;
  wait_repause = fa_is_paused();
  if (wait_repause)
    fa_run_request(); /* the load settles over frames the game has to run */
  set_mode(MODE_BLANK);
}

static void wait_done(void)
{
  waiting = false;
  set_mode(MODE_OFF);
  if (!position_load_ok())
    gfx_notice("macro: Link wasn't put back at the start");
  tas_after_t after = wait_after;
  wait_after = AFTER_NOTHING;
  if (wait_repause)
    fa_pause_request();
  if (after == AFTER_RECORD) {
    if (wait_repause)
      rec_armed = true; /* as start recording: from the next frame let through */
    else
      begin_recording(false);
  } else if (after == AFTER_PLAY) {
    play_next(); /* if pausing, the frame the pause lets through is the macro's first */
  }
}

void tas_frame(void)
{
  uint32_t gen = __atomic_load_n(&frame_gen, __ATOMIC_RELAXED);
  if (tas_st == TAS_RECORDING && rec_active && !waiting) {
    commit(gen);
    if (tas_st == TAS_RECORDING)
      log_position(tas_len);
  }
  __atomic_store_n(&frame_gen, gen + 1 ? gen + 1 : 1, __ATOMIC_RELEASE);

  if (waiting) {
    if (position_busy())
      return; /* the game still gets nothing */
    wait_done();
    return;
  }
  if (tas_st == TAS_RECORDING && rec_armed)
    begin_recording(true);
  else if (tas_st == TAS_PLAYING)
    play_next();
}

void tas_record_start(void)
{
  if (tas_st == TAS_RECORDING)
    return;
  if (tas_len > 0) { /* ask once */
    uint64_t now = sys_tick();
    if (!confirm_at || now - confirm_at > CONFIRM_SECONDS * sys_tick_freq()) {
      confirm_at = now;
      gfx_logf("macro: start recording again to record over %u frames", (unsigned)tas_len);
      return;
    }
  }
  confirm_at = 0;
  waiting = false;
  tas_st = TAS_RECORDING;
  tas_len = tas_cursor = 0;
  tas_start.valid = false;
  gfx_notice("macro: recording");
  if (fa_is_paused()) {
    begin_recording(true); /* the next frame let through is the first */
  } else {
    /* pause first: this frame runs, the game holds at the next hook, recording from there */
    set_mode(MODE_OFF);
    rec_armed = true;
    rec_active = false;
    fa_pause_request();
  }
}

void tas_record_stop(void)
{
  if (tas_st != TAS_RECORDING)
    return;
  stop_recording();
  log_printf("tas: recording stopped, %u frames", (unsigned)tas_len);
  gfx_logf("macro: stopped, %u frames", (unsigned)tas_len);
}

/* the start state, back: false (with a notice) when it can't be */
static bool restore_start(void)
{
  if (!tas_start.valid) {
    gfx_notice("macro: no start position saved");
    return false;
  }
  if (!position_in_map(&tas_start.pos)) {
    gfx_notice("macro: the start is in another map");
    return false;
  }
  return position_load_from(&tas_start.pos);
}

void tas_record_restart(void)
{
  if (tas_st != TAS_RECORDING || waiting)
    return;
  if (rec_armed) { /* nothing recorded yet */
    gfx_notice("macro: back to the start");
    return;
  }
  if (!restore_start())
    return;
  tas_len = tas_cursor = 0;
  rec_active = false;
  log_printf("tas: back to the start, recording again");
  gfx_notice("macro: back to the start");
  begin_wait(AFTER_RECORD);
}

void tas_play(void)
{
  if (tas_st == TAS_PLAYING || tas_st == TAS_RECORDING) {
    if (tas_st == TAS_RECORDING)
      gfx_notice("macro: stop recording first");
    return;
  }
  if (!tas_len) {
    gfx_notice("macro: nothing recorded");
    return;
  }
  tas_st = TAS_PLAYING;
  log_printf("tas: playing from frame %u", (unsigned)tas_cursor);
  gfx_notice("macro: playing");
  if (waiting) /* Link is still being put back: play once he's there */
    wait_after = AFTER_PLAY;
  else if (position_busy()) {
    waiting = true;
    wait_after = AFTER_PLAY;
    wait_repause = false;
    set_mode(MODE_BLANK);
  } else
    play_next();
}

void tas_pause(void)
{
  if (tas_st != TAS_PLAYING)
    return;
  tas_st = TAS_STOPPED;
  if (waiting)
    wait_after = AFTER_NOTHING;
  else if (__atomic_load_n(&mode, __ATOMIC_RELAXED) == MODE_PLAY && tas_cursor > 0)
    tas_cursor--; /* the frame set at this hook hasn't been played: the game reads after the hook */
  if (!waiting)
    set_mode(MODE_OFF);
  log_printf("tas: paused at frame %u", (unsigned)tas_cursor);
  gfx_logf("macro: paused at frame %u", (unsigned)tas_cursor);
}

void tas_rewind(void)
{
  if (tas_st == TAS_RECORDING) {
    gfx_notice("macro: stop recording first");
    return;
  }
  if (!tas_len) {
    gfx_notice("macro: nothing recorded");
    return;
  }
  if (!restore_start())
    return;
  bool playing = tas_st == TAS_PLAYING;
  tas_cursor = 0;
  log_printf("tas: restart");
  gfx_notice("macro: back to the start");
  begin_wait(playing ? AFTER_PLAY : AFTER_NOTHING);
}

void tas_draw(void)
{
  char line[32];
  if (tas_st == TAS_RECORDING && waiting)
    snprintf(line, sizeof(line), "%s", T("REC  wait"));
  else if (tas_st == TAS_RECORDING)
    snprintf(line, sizeof(line), TF("REC %u"), (unsigned)tas_len);
  else if (tas_st == TAS_PLAYING && waiting)
    snprintf(line, sizeof(line), "%s", T("PLAY wait"));
  else if (tas_st == TAS_PLAYING) /* the cursor is past the frame being played: frames 1..length */
    snprintf(line, sizeof(line), TF("PLAY %u/%u"), (unsigned)tas_cursor, (unsigned)tas_len);
  else
    return;
  gfx_status(line);
}
