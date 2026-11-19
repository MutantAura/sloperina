/* Save backups. Every save the game makes, autosave or from the save screen, ends in nn::fs::CommitSaveData on its
 * save mount. main's GOT slot for it is hooked (got.c); once the game's commit has succeeded, the whole mount is
 * copied to a new folder on the SD card. The copy runs inside the hook, so the game can't write the save again
 * until it's done, and on whatever thread the game saves from: nothing here logs or draws (savebackup_frame
 * reports on the game's thread).
 *
 * The directory functions and DeleteDirectoryRecursively are looked up in the sdk's exports rather than imported:
 * an import rtld couldn't find would stop the game loading.
 *
 * From the menu (the game's thread): archive now makes the same copy whenever asked; restore copies an archive back
 * into the save mount, committing after each file through the sdk's CommitSaveData (not main's hooked slot, so it
 * isn't backed up as a new save), after first archiving what the mount held. The game's own Load then reads it. */
#include "savebackup.h"

#include "gfx.h"
#include "input.h"
#include "pouch.h"
#include "gamedata.h"
#include "got.h"
#include "lang.h"
#include "sdfs.h"
#include "settings.h"
#include "sys.h"
#include "z64hd.h"

bool save_backup_enabled;
bool save_backup_location;
bool save_backup_hard_numbered = true;
int save_backup_kinds = SAVE_BACKUP_BOTH;

/* Which kind a commit is: the game's menu screen when it happened (the Enum 0x578D41E1, as gd_game_menu_screen;
 * FINDINGS §9.1). None or Loading: an autosave (a loading zone shows Loading while it saves; checked in Lichii).
 * Option: the options alone, not backed up. Any other (the save screen, game over's save prompt): a hard save. */
#define GD_MENU_SCREEN 0x578D41E1u
#define MENU_SCREEN_NONE 0xB6EEDE09u   /* murmur3("None") */
#define MENU_SCREEN_LOADING 0x9C031F08u /* murmur3("Loading") */
#define MENU_SCREEN_OPTION 0xA60BA395u /* murmur3("Option") */
/* the screens that show the saves' captions (or save): the Load list isn't read again under them */
#define MENU_SCREEN_LOAD 0x3B849E33u      /* murmur3("Load") */
#define MENU_SCREEN_SAVE 0x42814D01u      /* murmur3("Save") */
#define MENU_SCREEN_GAME_OVER 0xC936EA1Cu /* murmur3("GameOver"): its save prompt */
static uint32_t menu_screen = MENU_SCREEN_NONE; /* as last seen on the game's thread (savebackup_frame) */
enum { KIND_AUTO, KIND_HARD, KIND_OPTIONS, KIND_MANUAL, KIND_RESTORE };
static const char *const kind_names[] = { "autosave", "hard save", "options", "manual", "before restore" }; /* not menu text */

/* The save caption's location (String64 0xF74E0E8E, kept in caption.sav), e.g. "TimeTemple", and its English
 * name from the game's LocationMsg/Location.msbt (EUen) */
#define GD_STR_SAVE_LOCATION 0xF74E0E8Eu
static const struct { const char *key, *name; } locations[] = {
  { "BottomOfTheWell", "Bottom of the Well" }, { "DeathMountainCrater", "Death Mountain Crater" }, /* not menu text */
  { "DeathMountainTrail", "Death Mountain Trail" }, { "DesertColossus", "Evil Desert Colossus" }, /* not menu text */
  { "DodongoCavern", "Dodongo Cavern" }, { "DreamWorld", "Hall of Fallen Foes" }, { "FireTemple", "Fire Temple" }, /* not menu text */
  { "ForestTemple", "Forest Temple" }, { "GanonCastle", "Ganondorf's Castle" }, /* not menu text */
  { "GerudoFortress", "Gerudo Fortress" }, { "GerudoTrainingGround", "Gerudo Proving Grounds" }, /* not menu text */
  { "GerudoValley", "Gerudo Valley" }, { "GoronCity", "Goron City" }, { "Graveyard", "Graveyard" }, /* not menu text */
  { "HountedWasteland", "Mirage Desert" }, { "HyruleCastle", "Hyrule Castle" }, { "HyruleField", "Hyrule Field" }, /* not menu text */
  { "IceCavern", "Ice Cavern" }, { "IngoRanch", "Ingo Ranch" }, { "InsideGanonCastle", "Inside Ganondorf's Castle" }, /* not menu text */
  { "InsideJabuJabuBelly", "Lord Jabu-Jabu's Belly" }, { "InsideTheDekuTree", "Great Deku Tree" }, /* not menu text */
  { "KakarikoVillage", "Kakariko Village" }, { "KokiriForest", "Kokiri Forest" }, { "LakeHylia", "Lake Hylia" }, /* not menu text */
  { "LonLonRanch", "Lon Lon Ranch" }, { "LostWood", "Lost Woods" }, { "Market", "Hyrule Castle Town" }, /* not menu text */
  { "SacredForestMeadow", "Sacred Grove" }, { "ShadowTemple", "Shadow Temple" }, { "SpiritTemple", "Spirit Temple" }, /* not menu text */
  { "TimeTemple", "Temple of Time" }, { "WaterTemple", "Water Temple" }, { "ZoraDomain", "Zora Domain" }, /* not menu text */
  { "ZoraFountain", "Zora Fountain" }, { "ZoraRiver", "Zora River" }, /* not menu text */
};
#define N_LOCATIONS (sizeof(locations) / sizeof(*locations))
/* the location as last seen on the game's thread (savebackup_frame): the copy may run on another */
static char location_key[64];

#define BACKUP_DIR SD_DIR "/save-backups"
#define ARCHIVE_DIR SD_DIR "/save-archives" /* archive now's named folders: never trimmed */
#define MAX_DEPTH 4             /* save mount: slot_NN/ inside its root */
#define MAX_BACKUP_FOLDERS 512  /* listed when trimming to SAVE_BACKUPS_KEPT */
#define COPY_CHUNK (64 * 1024)
#define PATH_LEN 256
#define STAMP_LEN 96            /* "2026-11-15_21-30-05 - Inside Ganondorf's Castle", with room to spare */
#define RESTORE_SKIP "option.sav" /* the options: not part of a save, left as they are on a restore */

/* nn::fs's directory types (the SDK's layout) */
typedef struct { void *handle; } fs_dir_t;
typedef struct {
  char name[769];
  uint8_t pad0[3];
  int8_t type;                  /* 0 directory, 1 file */
  uint8_t pad1[3];
  int64_t size;
} fs_dir_entry_t;
enum { FS_ENTRY_DIRECTORY = 0, FS_ENTRY_FILE = 1, FS_OPEN_DIR_ALL = 3 };

typedef nn_result_t (*commit_fn)(const char *mount);
typedef nn_result_t (*open_dir_fn)(fs_dir_t *out, const char *path, int mode);
typedef nn_result_t (*read_dir_fn)(int64_t *count, fs_dir_entry_t *entries, fs_dir_t dir, int64_t max);
typedef void (*close_dir_fn)(fs_dir_t dir);
typedef nn_result_t (*delete_tree_fn)(const char *path);

static open_dir_fn open_dir;
static read_dir_fn read_dir;
static close_dir_fn close_dir;
static delete_tree_fn delete_tree;

/* main's two commit imports: what each wrapper calls (got_hook) */
static void *next_commit_save, *next_commit;
/* the sdk's own CommitSaveData, for the restore's commits (main's slot would back them up) */
static commit_fn sdk_commit_save;
/* the game's save mount, as its commits name it ("save" in 1.1.1, before any commit this session) */
static char save_mount[32] = "save"; /* not menu text */

/* the console's clock (nn::time, as flaglog.c) */
typedef struct { int64_t value; } nn_posix_time_t;
typedef struct { int16_t year; int8_t month, day, hour, minute, second; } nn_calendar_time_t;
bool nn_time_initialized(void) __asm__("_ZN2nn4time13IsInitializedEv");
uint32_t nn_time_now(nn_posix_time_t *out)
  __asm__("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
uint32_t nn_time_to_calendar(nn_calendar_time_t *out, void *info, const nn_posix_time_t *time)
  __asm__("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE");
#define CALENDAR_INFO_SIZE 64

/* the copy's working state (one copy at a time: `busy`) */
static int busy;
static _Alignas(8) uint8_t chunk[COPY_CHUNK];
static fs_dir_entry_t entries[MAX_DEPTH + 1];
static char folder_names[MAX_BACKUP_FOLDERS][STAMP_LEN];
static int files_copied;
static int64_t bytes_copied;
static char error[320];
/* while restoring: the mount to commit after each file (else 0), and the top-level file not copied */
static const char *commit_mount;
static const char *skip_name;

/* what the last backup did, for savebackup_frame and the menu */
static volatile int reports;          /* backups finished; savebackup_frame reports each new one */
static int reported;
static char last_mount[32], last_stamp[STAMP_LEN], last_error[320];
static int last_files;
static int64_t last_bytes;
static uint64_t last_ticks;
static const char *hook_error = "not hooked yet"; /* not menu text */

static bool ends_with_slash(const char *s)
{
  size_t n = strlen(s);
  return n && s[n - 1] == '/';
}

static void join(char *out, const char *dir, const char *name)
{
  snprintf(out, PATH_LEN, ends_with_slash(dir) ? "%s%s" : "%s/%s", dir, name);
}

static void fail(const char *what, const char *path, nn_result_t r)
{
  if (!error[0])
    snprintf(error, sizeof(error), "%s %s (0x%x)", what, path, (unsigned)r);
}

/* are the two files the same (both missing counts)? false if either can't be read */
static _Alignas(8) uint8_t compare_chunk[4096];
static bool same_file(const char *a, const char *b)
{
  fs_file_t fa, fb;
  bool open_a = fs_open(&fa, a, FS_READ) == 0, open_b = fs_open(&fb, b, FS_READ) == 0;
  if (!open_a || !open_b) {
    if (open_a)
      fs_close(fa);
    if (open_b)
      fs_close(fb);
    return !open_a && !open_b;
  }
  int64_t size_a = -1, size_b = -2;
  fs_size(&size_a, fa);
  fs_size(&size_b, fb);
  bool same = size_a == size_b;
  for (int64_t at = 0; same && at < size_a;) {
    size_t want = size_a - at < (int64_t)sizeof(compare_chunk) ? (size_t)(size_a - at) : sizeof(compare_chunk);
    size_t got_a = 0, got_b = 0;
    if (fs_read(&got_a, fa, at, chunk, want) != 0 || fs_read(&got_b, fb, at, compare_chunk, want) != 0 ||
        got_a != want || got_b != want) {
      same = false;
      break;
    }
    for (size_t i = 0; i < want && same; i++)
      same = chunk[i] == compare_chunk[i];
    at += (int64_t)want;
  }
  fs_close(fa);
  fs_close(fb);
  return same;
}

static int compare_names(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

static void copy_file(const char *from, const char *to)
{
  fs_file_t in, out;
  int64_t size = 0;
  nn_result_t r;
  if ((r = fs_open(&in, from, FS_READ)) != 0) {
    fail("couldn't open", from, r); /* not menu text */
    return;
  }
  fs_size(&size, in);
  fs_delete_file(to);
  if ((r = fs_create_file(to, size)) != 0 || (r = fs_open(&out, to, FS_WRITE)) != 0) {
    fail("couldn't make", to, r); /* not menu text */
    fs_close(in);
    return;
  }
  for (int64_t at = 0; at < size;) {
    size_t want = size - at < COPY_CHUNK ? (size_t)(size - at) : COPY_CHUNK, got = 0;
    if ((r = fs_read(&got, in, at, chunk, want)) != 0 || !got) {
      fail("couldn't read", from, r); /* not menu text */
      break;
    }
    fs_write_option_t flush = { at + (int64_t)got >= size ? FS_WRITE_FLUSH : 0 };
    if ((r = fs_write(out, at, chunk, got, &flush)) != 0) {
      fail("couldn't write", to, r); /* not menu text */
      break;
    }
    at += got;
    bytes_copied += got;
  }
  fs_close(out);
  fs_close(in);
  files_copied++;
  if (commit_mount && (r = sdk_commit_save(commit_mount)) != 0)
    fail("couldn't commit", to, r); /* not menu text */
}

static void copy_tree(const char *from, const char *to, int depth)
{
  fs_dir_t dir;
  nn_result_t r = open_dir(&dir, from, FS_OPEN_DIR_ALL);
  if (r != 0) {
    fail("couldn't list", from, r); /* not menu text */
    return;
  }
  fs_dir_entry_t *entry = &entries[depth];
  int64_t count;
  char child_from[PATH_LEN], child_to[PATH_LEN];
  while (read_dir(&count, entry, dir, 1) == 0 && count == 1) {
    if (depth == 0 && skip_name && !compare_names(entry->name, skip_name))
      continue;
    join(child_from, from, entry->name);
    join(child_to, to, entry->name);
    if (entry->type == FS_ENTRY_DIRECTORY) {
      if (depth >= MAX_DEPTH) {
        fail("too deep:", child_from, 0); /* not menu text */
        continue;
      }
      fs_create_dir(child_to);
      copy_tree(child_from, child_to, depth + 1);
    } else {
      copy_file(child_from, child_to);
    }
  }
  close_dir(dir);
}

/* Delete the oldest backups (their names sort by date) past SAVE_BACKUPS_KEPT. */
static void trim_backups(void)
{
  if (!delete_tree)
    return;
  fs_dir_t dir;
  if (open_dir(&dir, BACKUP_DIR, FS_OPEN_DIR_ALL) != 0)
    return;
  int n = 0;
  int64_t count;
  while (n < MAX_BACKUP_FOLDERS && read_dir(&count, &entries[0], dir, 1) == 0 && count == 1)
    if (entries[0].type == FS_ENTRY_DIRECTORY && strlen(entries[0].name) < STAMP_LEN)
      snprintf(folder_names[n++], STAMP_LEN, "%s", entries[0].name);
  close_dir(dir);
  for (; n > SAVE_BACKUPS_KEPT; n--) {
    int oldest = 0;
    for (int i = 1; i < n; i++)
      if (compare_names(folder_names[i], folder_names[oldest]) < 0)
        oldest = i;
    char path[PATH_LEN];
    join(path, BACKUP_DIR, folder_names[oldest]);
    delete_tree(path);
    memcpy(folder_names[oldest], folder_names[n - 1], STAMP_LEN);
  }
}

/* the save's location as the save screen names it (a location the table doesn't know: its key), or "" */
static const char *location_name(void)
{
  for (unsigned i = 0; i < N_LOCATIONS; i++)
    if (!compare_names(location_key, locations[i].key))
      return locations[i].name;
  return location_key;
}

/* append `text` to out (of `len`, holding n characters), with nothing a folder name can't hold; the new length */
static int append_clean(char *out, int len, int n, const char *text)
{
  for (; *text && n < len - 1; text++)
    out[n++] = (*text == '/' || *text == '\\' || *text == ':' || *text == '*' || *text == '?' || *text == '"' ||
                *text == '<' || *text == '>' || *text == '|') ? '_' : *text;
  out[n] = 0;
  return n;
}

/* the folder name's " - <location>", or nothing */
static void location_suffix(char *out, int len)
{
  out[0] = 0;
  if (!save_backup_location || !location_key[0])
    return;
  append_clean(out, len, snprintf(out, len, " - "), location_name());
}

/* the console's date and time, or false */
static bool now_calendar(nn_calendar_time_t *cal)
{
  nn_posix_time_t posix;
  static _Alignas(8) uint8_t info[CALENDAR_INFO_SIZE];
  return nn_time_initialized() && nn_time_now(&posix) == 0 && nn_time_to_calendar(cal, info, &posix) == 0;
}

/* commits not backed up, for savebackup_frame to log: their kind and the menu screen then */
static volatile int skips;
static int skips_reported, last_skip_kind;
static uint32_t last_skip_screen, last_screen;
static int last_kind;

/* Copy the mount to a new folder: `folder` in `dir` (made if need be), or (NULL) a dated one in BACKUP_DIR, then
 * (trim) delete the oldest backups past SAVE_BACKUPS_KEPT. The caller holds `busy`. False: it failed (the error is
 * in last_error). */
static void make_archive_dirs(const char *dir);
static bool make_backup(const char *mount, int kind, uint32_t screen, const char *dir_path, const char *folder,
                        bool trim)
{
  last_kind = kind;
  last_screen = screen;
  uint64_t start = sys_tick();
  char stamp[STAMP_LEN] = "undated"; /* not menu text */
  nn_calendar_time_t cal;
  if (folder) {
    snprintf(stamp, sizeof(stamp), "%s", folder);
  } else {
    if (now_calendar(&cal))
      snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d-%02d-%02d", cal.year, cal.month, cal.day, cal.hour,
               cal.minute, cal.second);
    char suffix[64];
    location_suffix(suffix, sizeof(suffix));
    size_t at = strlen(stamp);
    snprintf(stamp + at, sizeof(stamp) - at, "%s", suffix);
  }
  const char *dir = folder ? dir_path : BACKUP_DIR;
  char root[40], to[PATH_LEN];
  snprintf(root, sizeof(root), "%s:/", mount);
  fs_create_dir(SD_DIR);
  if (folder)
    make_archive_dirs(dir);
  else
    fs_create_dir(dir);
  join(to, dir, stamp);
  /* two saves in one second (an autosave, then the save screen's), or an archive of the same number and name in
   * the same minute: the later one replaces it */
  if (delete_tree)
    delete_tree(to);
  error[0] = 0;
  files_copied = 0;
  bytes_copied = 0;
  commit_mount = 0;
  skip_name = 0;
  nn_result_t r = fs_create_dir(to);
  if (r != 0)
    fail("couldn't make", to, r); /* not menu text */
  else
    copy_tree(root, to, 0);
  if (trim)
    trim_backups();

  snprintf(last_mount, sizeof(last_mount), "%s", mount);
  memcpy(last_stamp, stamp, sizeof(last_stamp));
  memcpy(last_error, error, sizeof(last_error));
  last_files = files_copied;
  last_bytes = bytes_copied;
  last_ticks = sys_tick() - start;
  __atomic_add_fetch(&reports, 1, __ATOMIC_RELEASE);
  return !error[0];
}

/* A hard save commits twice, about a quarter of a second apart (the same files): a backup of the same kind that
 * soon after the last replaces it, so each save is one folder (a numbered one keeps its number). */
#define SAME_SAVE_MS 3000
static char last_auto[PATH_LEN]; /* the last automatic backup's folder (its path), and when, of what kind, its # */
static uint64_t last_auto_tick;
static int last_auto_kind = -1, last_auto_number;
static bool last_numbered;       /* the last backup was a hard save's numbered archive (savebackup_frame says so) */

/* save management's, below: where archive now puts them, and the next number there (0: not looked for yet) */
static char archive_to[SAVE_FOLDER_LEN];
static int next_number;
static void archive_path(char *out, const char *rel);
static void archive_name(char *folder, int len, int number, const char *label);

static void back_up(const char *mount)
{
  if (mount && strlen(mount) < sizeof(save_mount) && compare_names(mount, save_mount))
    snprintf(save_mount, sizeof(save_mount), "%s", mount); /* in practice always "save": set once, if at all */
  if (!save_backup_enabled || !settings_sd() || !open_dir || !read_dir || !close_dir || !mount ||
      strlen(mount) >= sizeof(last_mount) - 1)
    return;
  uint32_t screen = __atomic_load_n(&menu_screen, __ATOMIC_ACQUIRE);
  int kind = screen == MENU_SCREEN_NONE || screen == MENU_SCREEN_LOADING ? KIND_AUTO
           : screen == MENU_SCREEN_OPTION                                  ? KIND_OPTIONS
                                                                           : KIND_HARD;
  bool wanted = kind == KIND_AUTO ? save_backup_kinds != SAVE_BACKUP_HARD
              : kind == KIND_HARD ? save_backup_kinds != SAVE_BACKUP_AUTO : false;
  if (!wanted) {
    last_skip_kind = kind;
    last_skip_screen = screen;
    __atomic_add_fetch(&skips, 1, __ATOMIC_RELEASE);
    return;
  }
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
    return; /* another thread's commit is being copied: that copy is as new */
  uint64_t now = sys_tick();
  bool same_save = kind == last_auto_kind && last_auto[0] &&
                   now - last_auto_tick < SAME_SAVE_MS * (sys_tick_freq() / 1000);
  /* a hard save: the next numbered archive where archive now puts them ("#008 - Goron City - <date>"), as soon as
   * that number's known (savebackup_frame looks for it); else, as an autosave, a dated one in save-backups/ */
  bool numbered = kind == KIND_HARD && save_backup_hard_numbered;
  int number = 0;
  char name[STAMP_LEN], dir[PATH_LEN], path[PATH_LEN];
  if (numbered) {
    number = same_save && last_auto_number ? last_auto_number : __atomic_load_n(&next_number, __ATOMIC_ACQUIRE);
    numbered = number > 0;
  }
  if (numbered) {
    archive_name(name, sizeof(name), number, location_name());
    archive_path(dir, archive_to);
  }
  bool made = numbered ? make_backup(mount, kind, screen, dir, name, false)
                        : make_backup(mount, kind, screen, 0, 0, true);
  if (made) {
    join(path, numbered ? dir : BACKUP_DIR, last_stamp);
    if (same_save && delete_tree && compare_names(last_auto, path)) /* the same save's first commit */
      delete_tree(last_auto);
    if (numbered && !(same_save && last_auto_number == number))
      __atomic_store_n(&next_number, number + 1, __ATOMIC_RELEASE);
    memcpy(last_auto, path, sizeof(last_auto));
    last_auto_tick = now;
    last_auto_kind = kind;
    last_auto_number = numbered ? number : 0;
    last_numbered = numbered;
  }
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

static nn_result_t commit_save_wrap(const char *mount)
{
  nn_result_t r = ((commit_fn)next_commit_save)(mount);
  if (r == 0)
    back_up(mount);
  return r;
}

static nn_result_t commit_wrap(const char *mount)
{
  nn_result_t r = ((commit_fn)next_commit)(mount);
  if (r == 0)
    back_up(mount);
  return r;
}

const char *savebackup_hook_game(uintptr_t main_base)
{
  uintptr_t sdk = got_module_of((uintptr_t)&fs_open); /* an sdk function this module imports */
  if (!sdk)
    return hook_error = "sdk module not found"; /* not menu text */
  open_dir = (open_dir_fn)got_export(sdk, "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
  read_dir = (read_dir_fn)got_export(sdk, "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl");
  close_dir = (close_dir_fn)got_export(sdk, "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
  delete_tree = (delete_tree_fn)got_export(sdk, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc");
  if (!open_dir || !read_dir || !close_dir)
    return hook_error = "nn::fs directory functions not found in the sdk"; /* not menu text */
  void *commit_save = got_export(sdk, "_ZN2nn2fs14CommitSaveDataEPKc");
  sdk_commit_save = (commit_fn)commit_save;
  void *commit = got_export(sdk, "_ZN2nn2fs6CommitEPKc");
  const char *e1 = commit_save ? got_hook(main_base, "_ZN2nn2fs14CommitSaveDataEPKc", commit_save_wrap, commit_save,
                                          &next_commit_save)
                               : "CommitSaveData not in the sdk"; /* not menu text */
  const char *e2 = commit ? got_hook(main_base, "_ZN2nn2fs6CommitEPKc", commit_wrap, commit, &next_commit)
                          : "Commit not in the sdk"; /* not menu text */
  if (e1 && e2)
    return hook_error = e1;
  hook_error = 0;
  return 0;
}

static void reload_frame(void); /* the game's Load list after a restore */

void savebackup_frame(void)
{
  uint32_t screen;
  if (gd_get_enum(GD_MENU_SCREEN, &screen)) /* also when off: archive now and restore name folders the same way */
    __atomic_store_n(&menu_screen, screen, __ATOMIC_RELEASE);
  reload_frame();
  /* the next number for a hard save's archive, found on this thread before a save needs it (a save may come on
   * another; looking needs the folder's list) */
  if (save_backup_enabled && save_backup_hard_numbered && !__atomic_load_n(&next_number, __ATOMIC_ACQUIRE))
    savebackup_next_number();
  int skipped = __atomic_load_n(&skips, __ATOMIC_ACQUIRE);
  if (skipped != skips_reported) {
    skips_reported = skipped;
    log_printf("save backup: skipped a commit (%s, menu screen %08x)", kind_names[last_skip_kind],
               (unsigned)last_skip_screen);
  }
  const char *key = gd_get_str(GD_STR_SAVE_LOCATION);
  if (key && !__atomic_load_n(&busy, __ATOMIC_ACQUIRE)) {
    char copy[sizeof(location_key)];
    int i = 0;
    for (; i < (int)sizeof(copy) - 1 && key[i]; i++)
      copy[i] = key[i];
    copy[i] = 0;
    memcpy(location_key, copy, sizeof(location_key));
  }
  int now = __atomic_load_n(&reports, __ATOMIC_ACQUIRE);
  if (now == reported || __atomic_load_n(&busy, __ATOMIC_ACQUIRE))
    return;
  reported = now;
  unsigned ms = (unsigned)(last_ticks * 1000 / sys_tick_freq());
  if (last_error[0]) {
    log_printf("save backup %s (%s, menu screen %08x) from %s: %d files, then %s", last_stamp, kind_names[last_kind],
               (unsigned)last_screen, last_mount, last_files, last_error);
    gfx_notice("save backup failed (see the log)");
  } else {
    log_printf("save backup %s (%s, menu screen %08x) from %s: %d files, %lld bytes, %u ms", last_stamp,
               kind_names[last_kind], (unsigned)last_screen, last_mount, last_files, (long long)last_bytes, ms);
    gfx_notice(last_kind == KIND_MANUAL || (last_kind == KIND_HARD && last_numbered) ? "save archived"
                                                                                      : "save backed up");
  }
}

void savebackup_status_text(char *buf, int len)
{
  if (hook_error)
    snprintf(buf, len, TF("unavailable: %s"), hook_error); /* hook_error: not menu text */
  else if (!settings_sd())
    snprintf(buf, len, "%s", T("no SD card access"));
  else if (!reported)
    snprintf(buf, len, "%s", T("no backup yet this session"));
  else if (last_error[0])
    snprintf(buf, len, TF("last: %s failed (see the log)"), last_stamp);
  else
    snprintf(buf, len, TF("last: %s (%s)"), last_stamp, T(kind_names[last_kind]));
}

/* --- from the menu: archive now, the archives' folders, restore ---------------------------------------
 * Named archives live in ARCHIVE_DIR and in folders made in it (any depth, SAVE_FOLDER_LEN for the path). A folder
 * whose name starts with '#' is an archive (archive now names them "#007 - ..."; the keyboard has no '#'); any
 * other is a folder of them. Paths here are relative to ARCHIVE_DIR, "" for it. */

/* the last list (savebackup_list_named or _automatic): its folder, then its entries, folders first */
static char list_path[PATH_LEN];
static char list_names[SAVE_ARCHIVES_LISTED][STAMP_LEN];
static int list_count, list_dirs;
static char browsing[SAVE_FOLDER_LEN];  /* the named archives' folder looked at */
static char archive_to[SAVE_FOLDER_LEN]; /* where archive now puts them */
static char scan_names[SAVE_ARCHIVES_LISTED][STAMP_LEN]; /* next number's look at archive_to */
static int next_number; /* archive now's next number; 0: not looked for yet */

/* why the menu can't archive or restore now, or NULL */
static const char *unavailable(void)
{
  if (hook_error)
    return "save backups are unavailable (see the log)";
  if (!settings_sd())
    return "no SD card access";
  if (!sdk_commit_save || !delete_tree)
    return "the game's save functions weren't found";
  return 0;
}

/* a named archive's number ("#12 - ..."), or 0 */
static int archive_number(const char *name)
{
  if (name[0] != '#')
    return 0;
  int n = 0;
  for (name++; *name >= '0' && *name <= '9' && n < 100000; name++)
    n = n * 10 + (*name - '0');
  return n;
}

/* List `dir`'s folders into names[], greatest name first (the newest backup; the highest-numbered archive, as
 * their numbers are written with leading zeros); past `max`, the least go. How many. */
static int list_dir(const char *dir_path, char names[][STAMP_LEN], int max)
{
  fs_dir_t dir;
  if (!open_dir || open_dir(&dir, dir_path, FS_OPEN_DIR_ALL) != 0)
    return 0;
  static fs_dir_entry_t entry; /* 800 bytes: not on the stack */
  int64_t count;
  int n = 0;
  while (read_dir(&count, &entry, dir, 1) == 0 && count == 1) {
    if (entry.type != FS_ENTRY_DIRECTORY || strlen(entry.name) >= STAMP_LEN)
      continue;
    int at = n;
    while (at > 0 && compare_names(entry.name, names[at - 1]) > 0)
      at--;
    if (at >= max)
      continue;
    for (int i = n < max ? n : max - 1; i > at; i--)
      memcpy(names[i], names[i - 1], STAMP_LEN);
    snprintf(names[at], STAMP_LEN, "%s", entry.name);
    if (n < max)
      n++;
  }
  close_dir(dir);
  return n;
}

/* ARCHIVE_DIR/<rel> */
static void archive_path(char *out, const char *rel)
{
  if (rel[0])
    snprintf(out, PATH_LEN, "%s/%s", ARCHIVE_DIR, rel);
  else
    snprintf(out, PATH_LEN, "%s", ARCHIVE_DIR);
}

/* the folders of a full path under ARCHIVE_DIR, made one by one (each fails harmlessly if it's there) */
static void make_archive_dirs(const char *dir)
{
  fs_create_dir(SD_DIR);
  fs_create_dir(ARCHIVE_DIR);
  char path[PATH_LEN];
  snprintf(path, sizeof(path), "%s", dir);
  for (char *p = path + strlen(ARCHIVE_DIR) + 1; p < path + strlen(path); p++) {
    if (*p == '/') {
      *p = 0;
      fs_create_dir(path);
      *p = '/';
    }
  }
  fs_create_dir(path);
}

/* is rel a folder path made here: '/'-separated names, none empty, "." or "..", nothing a name can't hold? */
static bool good_folder(const char *rel)
{
  int len = (int)strlen(rel);
  if (len >= SAVE_FOLDER_LEN)
    return false;
  for (const char *p = rel; *p;) {
    const char *end = p;
    while (*end && *end != '/')
      end++;
    if (end == p || (end - p == 1 && p[0] == '.') || (end - p == 2 && p[0] == '.' && p[1] == '.') || p[0] == '#')
      return false;
    for (const char *c = p; c < end; c++)
      if (*c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|')
        return false;
    p = *end ? end + 1 : end;
  }
  return true;
}

const char *savebackup_archive_folder(void) { return archive_to; }

void savebackup_set_archive_folder(const char *rel)
{
  snprintf(archive_to, sizeof(archive_to), "%s", rel && good_folder(rel) ? rel : "");
  next_number = 0; /* looked for again, in that folder */
}

/* the highest archive number in a folder (relative to ARCHIVE_DIR) + 1 */
static int number_after_highest(const char *rel)
{
  char dir[PATH_LEN];
  archive_path(dir, rel);
  int n = list_dir(dir, scan_names, SAVE_ARCHIVES_LISTED), highest = 0;
  for (int i = 0; i < n; i++)
    if (archive_number(scan_names[i]) > highest)
      highest = archive_number(scan_names[i]);
  return highest + 1;
}

static bool same_folder(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

int savebackup_next_number(void)
{
  if (!next_number && open_dir && settings_sd() && !__atomic_load_n(&busy, __ATOMIC_ACQUIRE))
    next_number = number_after_highest(archive_to);
  return next_number ? next_number : 1;
}

void savebackup_set_next_number(int number)
{
  next_number = number < 1 ? 1 : number > SAVE_ARCHIVE_NUMBER_MAX ? SAVE_ARCHIVE_NUMBER_MAX : number;
}

const char *savebackup_location_name(void)
{
  return location_name();
}

/* a name typed with the keyboard as a folder's: what it can't hold made '_', spaces and dots off the end (Windows
 * drops them), spaces off the start; into out (SAVE_ARCHIVE_NAME_MAX + 1); its start */
static const char *clean_name(const char *typed, char *out)
{
  int t = append_clean(out, SAVE_ARCHIVE_NAME_MAX + 1, 0, typed ? typed : "");
  while (t > 0 && (out[t - 1] == ' ' || out[t - 1] == '.'))
    out[--t] = 0;
  const char *start = out;
  while (*start == ' ')
    start++;
  return start;
}

/* a numbered archive's folder name: "#007 - <label> - DD-MM-YYYY HH-MM" (no label: "#007 - <date>") */
static void archive_name(char *folder, int len, int number, const char *label)
{
  int n = snprintf(folder, len, "#%03d", number);
  if (label && *label)
    n = append_clean(folder, len, append_clean(folder, len, n, " - "), label);
  nn_calendar_time_t cal;
  if (now_calendar(&cal) && n < len)
    snprintf(folder + n, len - n, " - %02d-%02d-%04d %02d-%02d", cal.day, cal.month, cal.year, cal.hour, cal.minute);
}

/* archive now into folder `rel`: where new archives go (its next number, then up one), or another (numbered from
 * its own highest) */
static void archive_into(const char *rel, const char *name)
{
  const char *why = unavailable();
  if (why) {
    gfx_notice(T(why));
    return;
  }
  /* "#007 - Temple of Time - 18-11-2026 21-30": the name, or (none typed) the location */
  bool usual = same_folder(rel, archive_to);
  int number = usual ? savebackup_next_number() : number_after_highest(rel);
  char folder[STAMP_LEN], typed[SAVE_ARCHIVE_NAME_MAX + 1], dir[PATH_LEN];
  const char *label = clean_name(name, typed);
  archive_name(folder, sizeof(folder), number, *label ? label : location_name());
  archive_path(dir, rel);
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) {
    gfx_notice(T("the game is saving: try again"));
    return;
  }
  if (make_backup(save_mount, KIND_MANUAL, __atomic_load_n(&menu_screen, __ATOMIC_ACQUIRE), dir, folder, false) &&
      usual)
    savebackup_set_next_number(number + 1);
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); /* savebackup_frame reports it */
}

void savebackup_archive_now(const char *name) { archive_into(archive_to, name); }

void savebackup_archive_here_now(const char *name) { archive_into(browsing, name); }

/* list a folder into the last list: its folders first, A to Z, then its archives, greatest name first */
static int list_folder(const char *path)
{
  snprintf(list_path, sizeof(list_path), "%s", path);
  list_count = list_dirs = 0;
  if (!open_dir || !settings_sd() || __atomic_load_n(&busy, __ATOMIC_ACQUIRE))
    return 0;
  int n = list_dir(path, scan_names, SAVE_ARCHIVES_LISTED);
  for (int i = n - 1; i >= 0; i--) /* folders: the other way round, A to Z */
    if (scan_names[i][0] != '#')
      memcpy(list_names[list_count++], scan_names[i], STAMP_LEN);
  list_dirs = list_count;
  for (int i = 0; i < n; i++)
    if (scan_names[i][0] == '#')
      memcpy(list_names[list_count++], scan_names[i], STAMP_LEN);
  return list_count;
}

int savebackup_list_named(void)
{
  char path[PATH_LEN];
  archive_path(path, browsing);
  return list_folder(path);
}

int savebackup_list_automatic(void)
{
  list_folder(BACKUP_DIR);
  list_dirs = 0; /* every folder there is a backup */
  return list_count;
}

int savebackup_list_folders(void) { return list_dirs; }

const char *savebackup_archive_name(int i)
{
  return i >= 0 && i < list_count ? list_names[i] : "";
}

const char *savebackup_browsing(void) { return browsing; }

bool savebackup_browse_into(int i)
{
  if (i < 0 || i >= list_dirs)
    return false;
  char next[SAVE_FOLDER_LEN];
  int n = snprintf(next, sizeof(next), browsing[0] ? "%s/%s" : "%s%s", browsing, list_names[i]);
  if (n >= (int)sizeof(next) || !good_folder(next)) {
    gfx_notice(T("that folder is too deep to open here"));
    return false;
  }
  memcpy(browsing, next, sizeof(browsing));
  return true;
}

void savebackup_browse_up(void)
{
  int n = (int)strlen(browsing);
  while (n > 0 && browsing[n - 1] != '/')
    n--;
  browsing[n > 0 ? n - 1 : 0] = 0;
}

void savebackup_browse_top(void) { browsing[0] = 0; }

void savebackup_archive_here(void)
{
  savebackup_set_archive_folder(browsing);
  gfx_logf(TF("new archives go to: save-archives/%s"), archive_to);
}

bool savebackup_make_folder(const char *typed)
{
  const char *why = unavailable();
  char name_buf[SAVE_ARCHIVE_NAME_MAX + 1], rel[SAVE_FOLDER_LEN + SAVE_ARCHIVE_NAME_MAX + 2], path[PATH_LEN];
  const char *name = clean_name(typed, name_buf);
  if (!why && !*name)
    why = "no name: no folder made";
  int n = snprintf(rel, sizeof(rel), browsing[0] ? "%s/%s" : "%s%s", browsing, name);
  if (!why && (n >= (int)sizeof(rel) || !good_folder(rel))) /* (good_folder: shorter than SAVE_FOLDER_LEN) */
    why = "that folder name can't be used here";
  if (why) {
    gfx_notice(T(why));
    return false;
  }
  archive_path(path, rel);
  make_archive_dirs(path);
  fs_dir_t probe;
  if (open_dir(&probe, path, FS_OPEN_DIR_ALL) != 0) {
    log_printf("save archive: couldn't make the folder %s", path);
    gfx_notice(T("folder not made (see the log)"));
    return false;
  }
  close_dir(probe);
  log_printf("save archive: made the folder %s", path);
  gfx_logf(TF("folder made: %s"), name);
  return true;
}

bool savebackup_delete_folder(void)
{
  const char *why = unavailable();
  if (!why && !browsing[0])
    why = "the top folder can't be deleted";
  if (why) {
    gfx_notice(T(why));
    return false;
  }
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) {
    gfx_notice(T("the game is saving: try again"));
    return false;
  }
  char path[PATH_LEN];
  archive_path(path, browsing);
  nn_result_t r = delete_tree(path);
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
  if (r != 0) {
    log_printf("save archive: couldn't delete the folder %s (0x%x)", path, (unsigned)r);
    gfx_notice(T("folder not deleted (see the log)"));
    return false;
  }
  log_printf("save archive: deleted the folder %s", path);
  /* new archives were going there, or into a folder in it: to the top instead */
  int n = 0;
  while (browsing[n] && archive_to[n] == browsing[n])
    n++;
  if (!browsing[n] && (archive_to[n] == 0 || archive_to[n] == '/'))
    savebackup_set_archive_folder("");
  savebackup_browse_up();
  gfx_notice(T("folder deleted"));
  return true;
}

/* --- the game's Load list after a restore (FINDINGS 9.11) ---------------------------------------------
 * The game reads the saves' captions (location, date, picture) into its CaptionMgr once, at start-up; its Load
 * list shows those, and its own load request does nothing once they're loaded. After a restore they're emptied
 * here and the game's request made, so its CaptionMgr update reads every slot's caption.sav again, a slot every
 * few frames. Only from the game's thread (savebackup_frame), with no save being copied, the captions settled (the
 * game's own reader takes one request at a time), and none of the game's screens up that show them: its Load and
 * Save lists draw the entries' pictures, which this frees. The rest of the pause menu doesn't.
 * The pause screen's Load list open (not under its "Load this save?"): its rows are hidden first (so nothing draws
 * the old pictures) with the game's input held (so it doesn't scroll one back), a few frames later the captions
 * are read again, and then the screen's own refresh (what opening it does) binds its rows to the new ones. */
#define CAP_SIZE 0x98
#define CAP_LIST_N(l) ((l) ? 0x48 : 0x08) /* s32: the entries of the normal slots (slot_00..05), then 06..11 */
#define CAP_LIST(l) ((l) ? 0x50 : 0x10)   /* Entry *[]: */
#define CAP_ENTRY_DONE 5                  /* an entry's state (+0x00): read, its picture made */
#define CAP_ENTRY_SLOT 0x04               /* s32: an entry's slot in its list (0..5) */
#define CAP_ENTRY_SIZE 0x5A0
#define CAP_LATEST 0x88                   /* s32: the latest save's slot; -1: the newest caption's */
#define CAP_STATE 0x8C                    /* u8: 0 empty, 1 reading, 2 sorting, 3 busy, 4 loaded */
#define CAP_REQUEST 0x8D                  /* u8: bit 0, a load is asked for */
#define CAP_EXISTS 0x8E                   /* u8: a save is in the normal list (set as its entries are read) */
#define CAP_LOADED 4
#define CAP_MQ_MODE 0x91              /* which six slots the menus show */
#define RELOAD_GIVE_UP (30 * GAME_FPS) /* from when the reading starts */
#define LOAD_LIST_LIST 0x2B0          /* the Load list screen's list (0: not built) */
#define HIDE_FRAMES 3                 /* rows hidden this long before their pictures are freed: the GPU's done */
/* While the captions are read again, the CaptionMgr's update could be run this many more times a frame than the
 * game runs it. Measured in Lichii with 4: no quicker (55 frames, against 57 with none; the 6 normal slots 6 frames
 * apart, the 6 Master Quest ones 3), as each step waits on something that moves once a frame (the save reader, the
 * picture): so none. */
#define EXTRA_UPDATES 0
#define MAX_TIMED_ENTRIES 12

enum { RELOAD_IDLE, RELOAD_WAITING, RELOAD_HIDING, RELOAD_ASKING, RELOAD_READING };
static int reload_step, reload_frames, hidden_frames;
static bool in_place; /* the open Load list's rows are hidden: refreshed at the end */
/* the slots whose caption the restore changed (bit n: slot_NN; 0..5 the normal list, 6..11 the Master Quest one),
 * when known: only those are read again (an entry already read is passed over at once, FINDINGS 9.11), about 6
 * frames each instead of about a second for all 12 */
#define CAPTION_SLOTS 12
static uint16_t changed_slots;
static bool changed_known;
/* how the reading went, for the log: the frame (from the request) each entry was done, and the state turned 2, 4 */
static int16_t entry_done_at[MAX_TIMED_ENTRIES], sorting_at, loaded_at;

static void time_reading(const uint8_t *m)
{
  int k = 0;
  for (int l = 0; l < 2; l++) {
    int32_t n = *(const volatile int32_t *)(m + CAP_LIST_N(l));
    uint8_t *const *entries = *(uint8_t *const *volatile *)(m + CAP_LIST(l));
    if (n < 0 || n > 6 || (n && !sys_mapped((uintptr_t)entries, (size_t)n * 8, SYS_PERM_R)))
      continue;
    for (int i = 0; i < n && k < MAX_TIMED_ENTRIES; i++, k++)
      if (entry_done_at[k] < 0 && entries[i] && sys_mapped((uintptr_t)entries[i], 4, SYS_PERM_R) &&
          entries[i][0] == CAP_ENTRY_DONE)
        entry_done_at[k] = (int16_t)reload_frames;
  }
  if (sorting_at < 0 && m[CAP_STATE] == 2)
    sorting_at = (int16_t)reload_frames;
  if (loaded_at < 0 && m[CAP_STATE] == CAP_LOADED)
    loaded_at = (int16_t)reload_frames;
}

static void log_timing(void)
{
  char line[160];
  int n = snprintf(line, sizeof(line), "save restore: timing (frames, %d extra updates a frame): slots done at",
                   EXTRA_UPDATES);
  for (int k = 0; k < MAX_TIMED_ENTRIES && n < (int)sizeof(line); k++)
    n += snprintf(line + n, sizeof(line) - n, " %d", entry_done_at[k]);
  log_printf("%s; sorting at %d, loaded at %d", line, sorting_at, loaded_at); /* not menu text */
}

/* the pause screen's Load list, open and refreshable in place (not under "Load this save?"), or 0 */
static uintptr_t open_load_list(void)
{
  if (!z64hd_game->vt_load_list || !z64hd_game->load_list_hide_rows || !z64hd_game->vt_save_load_confirm)
    return 0;
  uintptr_t screen = ui_find_screen(z64hd_game->vt_load_list);
  if (!screen || ui_find_screen(z64hd_game->vt_save_load_confirm))
    return 0;
  return sys_ptr(screen + LOAD_LIST_LIST) ? screen : 0;
}

static uint8_t *caption_mgr(void)
{
  if (!z64hd_game->caption_mgr_ptr || !sys_mapped(MAIN(z64hd_game->caption_mgr_ptr), 8, SYS_PERM_R))
    return 0;
  uint8_t *m = *(uint8_t *volatile *)MAIN(z64hd_game->caption_mgr_ptr);
  return m && sys_mapped((uintptr_t)m, CAP_SIZE, SYS_PERM_R | SYS_PERM_W) ? m : 0;
}

/* what the game's WaitCaptionMgrStable waits for: empty, or loaded with every entry done */
static bool captions_settled(const uint8_t *m)
{
  uint8_t state = m[CAP_STATE];
  if (state == 0)
    return true;
  if (state != CAP_LOADED)
    return false;
  for (int l = 0; l < 2; l++) {
    int32_t n = *(const volatile int32_t *)(m + CAP_LIST_N(l));
    uint8_t *const *entries = *(uint8_t *const *volatile *)(m + CAP_LIST(l));
    if (n < 0 || n > 16 || (n && !sys_mapped((uintptr_t)entries, (size_t)n * 8, SYS_PERM_R)))
      return false;
    for (int i = 0; i < n; i++)
      if (!entries[i] || !sys_mapped((uintptr_t)entries[i], 4, SYS_PERM_R) || entries[i][0] != CAP_ENTRY_DONE)
        return false;
  }
  return true;
}

/* one of the game's screens that show the captions (or save) is up */
static bool captions_shown(void)
{
  uint32_t screen = __atomic_load_n(&menu_screen, __ATOMIC_ACQUIRE);
  return screen == MENU_SCREEN_LOAD || screen == MENU_SCREEN_SAVE || screen == MENU_SCREEN_LOADING ||
         screen == MENU_SCREEN_GAME_OVER;
}

static void reload_captions(void)
{
  if (!z64hd_game->caption_mgr_ptr)
    return;
  reload_step = RELOAD_WAITING;
  reload_frames = 0;
}

static void reload_stop(const char *how)
{
  log_printf("save restore: the game's Load list %s (%d frames)", how, reload_frames);
  reload_step = RELOAD_IDLE;
  in_place = false;
  input_hold_game = false;
}

/* only the changed slots' entries emptied, and the CaptionMgr set reading (its state 1 reads what isn't done);
 * false (nothing changed) if that can't be done: the whole list then */
static bool start_partial(uint8_t *m)
{
  if (!changed_known || !z64hd_game->caption_empty_entry || m[CAP_STATE] != CAP_LOADED)
    return false;
  uint8_t *entry[CAPTION_SLOTS] = { 0 };
  for (int slot = 0; slot < CAPTION_SLOTS; slot++) {
    if (!(changed_slots & 1u << slot))
      continue;
    /* the list's entry for that slot: by the slot it holds (+0x04), as the lists are sorted once read */
    int list = slot >= CAPTION_SLOTS / 2, index = slot % (CAPTION_SLOTS / 2);
    int32_t n = *(const volatile int32_t *)(m + CAP_LIST_N(list));
    uint8_t *const *entries = *(uint8_t *const *volatile *)(m + CAP_LIST(list));
    if (n < 0 || n > CAPTION_SLOTS / 2 || (n && !sys_mapped((uintptr_t)entries, (size_t)n * 8, SYS_PERM_R)))
      return false;
    for (int i = 0; i < n && !entry[slot]; i++)
      if (entries[i] && sys_mapped((uintptr_t)entries[i], CAP_ENTRY_SIZE, SYS_PERM_R | SYS_PERM_W) &&
          *(const volatile int32_t *)(entries[i] + CAP_ENTRY_SLOT) == index)
        entry[slot] = entries[i];
    if (!entry[slot])
      return false; /* (checked for all before any is emptied) */
  }
  typedef void (*entry_fn)(void *, int, bool);
  int read = 0;
  for (int slot = 0; slot < CAPTION_SLOTS; slot++) {
    if (!entry[slot])
      continue;
    MAIN_FN(entry_fn, z64hd_game->caption_empty_entry)(entry[slot], slot % (CAPTION_SLOTS / 2),
                                                       slot >= CAPTION_SLOTS / 2);
    read++;
  }
  m[CAP_EXISTS] = 0;                          /* set again as the entries are gone through */
  *(volatile int32_t *)(m + CAP_LATEST) = -1; /* the newest restored save is the latest */
  m[CAP_REQUEST] |= 1;                        /* as a request leaves it (state 2 clears it) */
  m[CAP_STATE] = 1;                           /* reading */
  reload_step = RELOAD_READING;
  reload_frames = 0;
  for (int k = 0; k < MAX_TIMED_ENTRIES; k++)
    entry_done_at[k] = -1;
  sorting_at = loaded_at = -1;
  log_printf("save restore: the game's Load list: %d changed slot(s) being read again (mask %03x)", read,
             (unsigned)changed_slots); /* not menu text */
  return true;
}

/* the captions emptied, the game's request to read them next */
static void start_reading(uint8_t *m)
{
  if (start_partial(m))
    return;
  typedef void (*mgr_fn)(void *);
  /* The 1.0.0 empty routine also clears the list's Master Quest selector.
   * A caption refresh must preserve the current mode, as 1.1.1 already does. */
  uint8_t mq_mode = m[CAP_MQ_MODE];
  MAIN_FN(mgr_fn, z64hd_game->caption_empty)(m); /* every entry emptied: state 0, which the request needs */
  m[CAP_MQ_MODE] = mq_mode;
  *(volatile int32_t *)(m + CAP_LATEST) = -1;     /* the newest restored save is the latest */
  reload_step = RELOAD_ASKING;
  reload_frames = 0;
  for (int k = 0; k < MAX_TIMED_ENTRIES; k++)
    entry_done_at[k] = -1;
  sorting_at = loaded_at = -1;
}

static void reload_frame(void)
{
  if (reload_step == RELOAD_IDLE)
    return;
  reload_frames++;
  uint8_t *m = caption_mgr();
  if (!m) {
    reload_stop("wasn't reloaded: its CaptionMgr wasn't found"); /* not menu text */
    gfx_notice(T("restart the game to see the restored save in its Load screen"));
    return;
  }
  if (reload_frames > RELOAD_GIVE_UP) {
    log_printf("save restore: CaptionMgr state %u, request %u", (unsigned)m[CAP_STATE], (unsigned)m[CAP_REQUEST]);
    reload_stop("didn't reload in time"); /* not menu text */
    gfx_notice(T("restart the game to see the restored save in its Load screen"));
    return;
  }
  typedef void (*mgr_fn)(void *);
  if (reload_step == RELOAD_WAITING) {
    reload_frames = 0; /* waiting doesn't count towards giving up */
    if (__atomic_load_n(&busy, __ATOMIC_ACQUIRE) || !captions_settled(m))
      return;
    if (changed_known && !changed_slots && m[CAP_STATE] == CAP_LOADED) {
      reload_stop("needn't be read again: no slot's caption changed"); /* not menu text */
      return;
    }
    if (captions_shown()) {
      uintptr_t screen = __atomic_load_n(&menu_screen, __ATOMIC_ACQUIRE) == MENU_SCREEN_LOAD ? open_load_list() : 0;
      if (!screen)
        return; /* the Save list, a save, game over, "Load this save?": once it's gone */
      MAIN_FN(mgr_fn, z64hd_game->load_list_hide_rows)((void *)sys_ptr(screen + LOAD_LIST_LIST));
      input_hold_game = true;
      in_place = true;
      hidden_frames = 0;
      reload_step = RELOAD_HIDING;
      log_printf("save restore: the open Load list's rows hidden, to read it again"); /* not menu text */
      return;
    }
    start_reading(m);
  }
  if (reload_step == RELOAD_HIDING) {
    if (++hidden_frames < HIDE_FRAMES)
      return;
    start_reading(m);
  }
  if (reload_step == RELOAD_ASKING) { /* until the game's own request takes (its save reader free) */
    MAIN_FN(mgr_fn, z64hd_game->caption_request)(m);
    if (!(m[CAP_REQUEST] & 1))
      return;
    log_printf("save restore: the game's Load list is being read again"); /* not menu text */
    reload_step = RELOAD_READING;
    return;
  }
  /* the game's update has run once this frame: a few more, quicker */
  for (int k = 0; k < EXTRA_UPDATES && z64hd_game->caption_update && m[CAP_STATE] != CAP_LOADED; k++)
    MAIN_FN(mgr_fn, z64hd_game->caption_update)(m);
  time_reading(m);
  if (m[CAP_STATE] == CAP_LOADED && captions_settled(m)) {
    log_timing();
    if (in_place) { /* its rows back, bound to the new captions (if it's still open: else it binds them when it opens) */
      bool refreshed = open_load_list() && ui_refresh_now(z64hd_game->vt_load_list);
      log_printf("save restore: the open Load list %s", refreshed ? "refreshed" : "had closed"); /* not menu text */
    }
    reload_stop("is up to date"); /* not menu text */
    gfx_notice(T("the game's Load list now shows the restored save"));
  }
}

bool savebackup_restore(int i)
{
  const char *why = unavailable();
  if (!why && (i < list_dirs || i >= list_count))
    why = "that archive is gone";
  if (why) {
    gfx_notice(T(why));
    return false;
  }
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) {
    gfx_notice(T("the game is saving: try again"));
    return false;
  }
  char name[STAMP_LEN], from[PATH_LEN], to[40];
  snprintf(name, sizeof(name), "%s", savebackup_archive_name(i)); /* the safety archive below may change the list */
  join(from, list_path, name);
  snprintf(to, sizeof(to), "%s:/", save_mount);
  fs_dir_t probe;
  if (open_dir(&probe, from, FS_OPEN_DIR_ALL) != 0) {
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    gfx_notice(T("that archive is gone"));
    return false;
  }
  close_dir(probe);

  /* what the mount holds now, first: a restore can always be undone. Not trimmed here: the oldest archive may be
   * the one being restored (the next backup trims). Reported below rather than by savebackup_frame (this is the
   * game's thread too), so its notice doesn't replace the restore's. */
  bool archived = make_backup(save_mount, KIND_RESTORE, __atomic_load_n(&menu_screen, __ATOMIC_ACQUIRE), 0, 0, false);
  reported = __atomic_load_n(&reports, __ATOMIC_ACQUIRE);
  if (!archived) {
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    log_printf("save restore of %s: not done, the archive of the save before it failed: %s", name, last_error);
    gfx_notice(T("save not restored (see the log)"));
    return false;
  }
  /* which slots' captions change: the archive's against the copy just made of what the mount held */
  char before[PATH_LEN], a[PATH_LEN], b[PATH_LEN], file[32];
  join(before, BACKUP_DIR, last_stamp);
  changed_slots = 0;
  changed_known = true;
  for (int slot = 0; slot < CAPTION_SLOTS; slot++) {
    snprintf(file, sizeof(file), "slot_%02d/caption.sav", slot); /* not menu text */
    join(a, from, file);
    join(b, before, file);
    if (!same_file(a, b))
      changed_slots |= (uint16_t)(1u << slot);
  }
  uint64_t start = sys_tick();
  error[0] = 0;
  files_copied = 0;
  bytes_copied = 0;
  commit_mount = save_mount;
  skip_name = RESTORE_SKIP;
  copy_tree(from, to, 0);
  commit_mount = 0;
  skip_name = 0;
  unsigned ms = (unsigned)((sys_tick() - start) * 1000 / sys_tick_freq());
  bool ok = !error[0] && files_copied > 0;
  if (ok)
    log_printf("save restore: %s into %s: %d files, %lld bytes, %u ms (the save before it archived as %s)", name, to,
               files_copied, (long long)bytes_copied, ms, last_stamp);
  else
    log_printf("save restore: %s into %s: %d files, then %s (the save before it is archived as %s)", name, to,
               files_copied, error[0] ? error : "nothing to copy", last_stamp); /* not menu text */
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
  if (!ok)
    changed_known = false; /* partway: all of them read again */
  if (files_copied > 0)
    reload_captions(); /* what was written, even after a failure partway */
  if (!ok)
    gfx_notice(T("save restore failed (see the log)"));
  else if (z64hd_game->caption_mgr_ptr && captions_shown() &&
           !(__atomic_load_n(&menu_screen, __ATOMIC_ACQUIRE) == MENU_SCREEN_LOAD && z64hd_game->vt_load_list))
    gfx_notice(T("save restored: the game's Load list shows it once this screen has closed"));
  else if (z64hd_game->caption_mgr_ptr)
    gfx_notice(T("save restored: load it from the game's Load screen"));
  else /* no CaptionMgr for this version: the game reads the captions at start-up */
    gfx_notice(T("save restored: restart the game to see it in its Load screen"));
  return ok;
}

bool savebackup_delete(int i)
{
  const char *why = unavailable();
  if (!why && (i < list_dirs || i >= list_count))
    why = "that archive is gone";
  if (why) {
    gfx_notice(T(why));
    return false;
  }
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) {
    gfx_notice(T("the game is saving: try again"));
    return false;
  }
  char name[STAMP_LEN], path[PATH_LEN];
  snprintf(name, sizeof(name), "%s", savebackup_archive_name(i));
  join(path, list_path, name);
  nn_result_t r = delete_tree(path);
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
  if (r != 0) {
    log_printf("save archive: couldn't delete %s (0x%x)", path, (unsigned)r);
    gfx_notice(T("archive not deleted (see the log)"));
    return false;
  }
  log_printf("save archive: deleted %s", path);
  gfx_notice(T("archive deleted"));
  return true;
}
