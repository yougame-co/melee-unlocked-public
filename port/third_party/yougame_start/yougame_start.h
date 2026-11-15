/*
 * yougame_start.h - the YouGame start hook. One call when your game starts; the game draws nothing.
 *
 *   #define YOUGAME_START_IMPLEMENTATION      (in exactly one .c or .cpp file)
 *   #include "yougame_start.h"
 *
 *   yg_start_options o = {0};
 *   o.game = "your-game-slug";               the game's address on YouGame: yougame.co/g/<slug>
 *   o.tray_from = "0.13.1407";               first shipped quiet-start version for the target OS
 *   yg_start(&o);                            then carry on starting the game, whatever it returns
 *
 * It never brings up YouGame's client window or a game page, and never brings a running YouGame
 * forward. What it does, in this order:
 *   1. YouGame is running for this user: nothing is shown. YG_START_RUNNING.
 *   U. Windows: YouGame is not running and has just quit to install an update (its marker file
 *      %APPDATA%\<profile>\updating.json, written by the app, is valid: see "The update marker"
 *      below), and the marker file is under 20 seconds old or its version is not installed: nothing is started
 *      and nothing is shown. YG_START_UPDATING, with the version it is installing in `update_to`
 *      (when a buffer is given; with `update_to_magic` it is also cleared at entry). The game shows its own
 *      "YouGame is updating" line and calls yg_start again about every 10 seconds, for at most
 *      5 minutes. Also while it waits for a
 *      YouGame it started (`open_wait_ms`): an old YouGame that finds an update quits to install it,
 *      and the hook returns YG_START_UPDATING the moment the marker appears, not "didn't open".
 *   2. YouGame is installed but not running.
 *      Windows, when `no_open` is 0, `tray_from` is set, the registered program is the signed
 *      YouGame program at its install location (%LOCALAPPDATA%\Programs\<folder>\<exe>, its version
 *      texts naming the YouGame app, signed by Fluo Inc. through Microsoft's identity verified CA),
 *      its file version is at least `tray_from`, and the game does not run as administrator: it
 *      starts that same file (checked again once started, before it runs) with the tray link
 *      yougame[-staging]://tray, so YouGame starts with no window, only its tray icon.
 *      macOS checks its bundle identity, version and Developer ID signature, then uses
 *      /usr/bin/open -g -j -a <bundle> --args --yougame-tray: hidden, in the background, menu bar
 *      icon only (Dock icon kept). The update-marker branch remains Windows-only.
 *      YG_START_OPENED. With `open_wait_ms` set and YouGame not up in that time, the small window
 *      below says "YouGame didn't open".
 *      Otherwise nothing is started and the small window says why: "YouGame needs an update" when
 *      the installed version is older than `tray_from` (or its file version cannot be read), else
 *      "YouGame isn't running" (also when its identity cannot be read). Linux always takes this
 *      path: a Linux desktop may have no tray.
 *   3. YouGame is not on this computer: the small window says "Play online with YouGame".
 * The small window is YouGame's own, with two buttons: "Get YouGame" opens YouGame's download page
 * in the browser (YG_START_OFFERED); "Not now" or closing it returns YG_START_DECLINED. It waits for
 * an answer. Every answer and every failure returns, so the game starts as usual.
 *
 * It only looks and starts; it does not talk to YouGame. Online calls are yougame.h's job (or the
 * game's own client): they find YouGame once it is running, with no restart of the game.
 *
 * Link flags for this header alone:
 *   Windows/MinGW: -ladvapi32 -lshell32 -luser32 -lgdi32 (MSVC: pragmas below name the same .lib).
 *     wintrust.dll, crypt32.dll, version.dll, ole32.dll, userenv.dll (and shell32.dll's
 *     SHGetKnownFolderPath) are loaded at run time from the system directory
 *     (LOAD_LIBRARY_SEARCH_SYSTEM32), never from the game's folder, unless the game itself already
 *     loaded a copy of that name.
 *   macOS: -framework CoreServices -framework CoreFoundation.
 *   Linux: -pthread. Its small window uses /usr/bin/zenity, else /usr/bin/kdialog; with neither
 *     there is no window and the result is YG_START_DECLINED.
 * It downloads nothing. The only programs it starts, after the player's answer or the checks above:
 * on Windows the signed YouGame program at its install location, with the tray link (never when
 * the game runs as administrator); on Mac codesign and open for the verified bundle; everywhere the system's opener for one fixed https address (the
 * download page); on Linux also zenity or kdialog (the small window). All by absolute path. On macOS and Linux a game
 * that ignores SIGCHLD or reaps every child itself makes those answers read as failures.
 *
 * Runs that must never show a window or start anything (tests, headless runs): set `quiet`, or the
 * environment variable YOUGAME_START_QUIET=1. Then it only looks (a pipe or socket, the registry or
 * the system's link handler, the update marker and the installed version) and reports what it
 * found; it starts no program on any system.
 *
 * The update marker (Windows): right before YouGame quits to install an update it writes one line of
 * UTF-8 JSON, {"v":1,"from":"0.13.1412","to":"0.13.1415","at":1763106830000} (`at` = epoch ms when it
 * quit), to updating.json in its profile folder (Electron's userData) under FOLDERID_RoamingAppData.
 * Production: "yougame-desktop" (the packaged app's folder) first, then "YouGame" (should a later
 * build add a productName); the first valid one wins (see yg__start_profiles). Staging: "YouGame
 * Staging (agent build)" only. The hook only reads it, never writes or deletes it, and only on a
 * fixed local drive: a Roaming folder redirected to a network share or a removable drive is never
 * opened, so it cannot stall the game. It takes the marker only when it is a regular
 * file under 1 KiB, `v` is 1, `at` is at most 5 minutes old and at most 60 s ahead of this
 * computer's clock, and `to` is digits and dots (at most 32 characters). When the installed YouGame
 * is already at `to` or newer (compared number by number), the marker is a leftover of a finished
 * update and is ignored once the marker file is at least 20 seconds old (installer post-steps may
 * still run). Anything else, malformed included, is no marker: today's behaviour. macOS and Linux have no marker yet, so YG_START_UPDATING never comes back there.
 *
 * MIT licence (see LICENSE beside this file).
 */
/* Strict C (-std=c99) hides glibc's POSIX declarations, which the implementation needs: include this
   header before libc headers in that one file. Only then, and only in strict mode, is the feature
   macro set, so a gnu/C++ build and every other file keep glibc's defaults untouched. */
#if defined(__linux__) && defined(YOUGAME_START_IMPLEMENTATION) && defined(__STRICT_ANSI__) && \
    !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef YOUGAME_START_H
#define YOUGAME_START_H

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum yg_start_result {
  YG_START_RUNNING = 0,     /* YouGame is running: nothing was shown */
  YG_START_OPENED = 1,      /* Windows/macOS: installed YouGame was started quietly */
  YG_START_OFFERED = 2,     /* the small window was shown and the player pressed Get YouGame;
                               the download page is open */
  YG_START_DECLINED = 3,    /* the small window was shown and the player pressed Not now (or closed
                               it), or no window could be shown */
  YG_START_UNAVAILABLE = 4, /* nothing was done: unsupported system, bad options, or an error */
  YG_START_INSTALLED = 5,   /* quiet runs only: installed but not running; nothing was started */
  YG_START_MISSING = 6,     /* quiet runs only: not installed, and nothing was shown */
  YG_START_UPDATING = 7     /* Windows: YouGame quit to install an update (its marker is valid):
                               nothing was started or shown. Show the game's own "YouGame is
                               updating" line and call yg_start again in about 10 s, for at most
                               5 minutes. The version it installs is in update_to, when given. */
} yg_start_result;

/* "YGU1": with it, update_to is also cleared at entry (empty on every result but YG_START_UPDATING). */
#define YG_START_UPDATE_TO_MAGIC 0x59475531u

typedef struct yg_start_options {
  const char *game;      /* required: the game's slug, a-z 0-9 and '-', at most 80 characters */
  int staging;           /* 1 = YouGame's staging app and site (test builds); 0 = YouGame */
  int quiet;             /* 1 = never show a window and never start anything; only report */
  unsigned open_wait_ms; /* Windows, after starting YouGame in the tray: wait up to this long for it
                            to be running (0 = return at once, the usual choice; at most 60000). If it
                            is not running by then, the small window says "YouGame didn't open". */
  int no_open;           /* 1 = never start an installed YouGame; show the small window instead */
  const char *tray_from; /* Windows: the first YouGame version that starts hidden in the tray, as
                            "0.13.1407". An installed YouGame older than this is never started (it
                            would show its whole window); the small window says it needs an update.
                            NULL (or not three dot-separated numbers) = never start it. */
  /* Last, so a caller that zeroes the struct (= {0} or memset) keeps working.
     Give a valid buffer and size, or leave both zero. On YG_START_UPDATING the version is written,
     magic or not; with update_to_magic = YG_START_UPDATE_TO_MAGIC it is also cleared at entry. */
  char *update_to;          /* optional: cleared at entry (with the magic); on YG_START_UPDATING, the version YouGame
                               is installing (at most 32 characters), cut to fit, always NUL-terminated */
  size_t update_to_cap;     /* buffer size including terminator (0 = not written) */
  unsigned update_to_magic; /* YG_START_UPDATE_TO_MAGIC: also clear update_to at entry */
} yg_start_options;

/* Call once, on the thread that starts the game, before or after the game's own window exists. */
yg_start_result yg_start(const yg_start_options *options);

#ifdef __cplusplus
}
#endif

#ifdef YOUGAME_START_IMPLEMENTATION

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every text the small window shows, on every system: {title, body}. Paragraphs split by "\n\n";
   Windows turns "\n" into "\r\n". Plain ASCII only. */
#define YG__START_TEXT_MISSING 0     /* not installed */
#define YG__START_TEXT_DIDNT_OPEN 1  /* started in the tray, but not running within open_wait_ms */
#define YG__START_TEXT_UPDATE 2      /* installed, older than tray_from (or its version unreadable) */
#define YG__START_TEXT_NOT_RUNNING 3 /* installed, not started by the hook */
static const char *const yg__start_texts[4][2] = {
  {"Play online with YouGame",
   "This game uses YouGame for online matches, friends and feedback. "
   "YouGame isn't on this computer yet.\n\n"
   "Get YouGame opens YouGame's download page. It installs the YouGame app; a smaller helper is coming."},
  {"YouGame didn't open",
   "This game uses YouGame for online matches, friends and feedback. "
   "YouGame is on this computer, but it didn't open just now.\n\n"
   "Open YouGame yourself, or Get YouGame opens its download page to install it again. "
   "Not now starts the game."},
  {"YouGame needs an update",
   "This game uses YouGame for online matches, friends and feedback. "
   "The YouGame on this computer is an older version.\n\n"
   "Get YouGame opens its download page to get the latest version. Not now starts the game."},
  {"YouGame isn't running",
   "This game uses YouGame for online matches, friends and feedback. "
   "YouGame is on this computer but isn't running.\n\n"
   "Open YouGame to play online, or Get YouGame opens its download page to get the latest version. "
   "Not now starts the game."}};
#define YG__START_GET "Get YouGame"
#define YG__START_LATER "Not now"
#define YG__START_WIDE_(s) L##s
#define YG__START_WIDE(s) YG__START_WIDE_(s)

/* What a tray start attempt came to (Windows and macOS; the test seam drives it everywhere). */
#define YG__START_TRAY_STARTED 0 /* the verified app was started with the tray link or Mac flag */
#define YG__START_TRAY_OLD 1     /* its version is older than tray_from, or could not be read */
#define YG__START_TRAY_REFUSED 2 /* no usable program, not the YouGame app, or not signed */
#define YG__START_TRAY_FAILED 3  /* verified, but starting it failed */

/* The update marker's `to`: at most 32 characters, plus the terminator. */
#define YG__START_TO_CAP 33

static int yg__start_slug_ok(const char *s) {
  size_t n = 0;
  if (!s || !*s) return 0;
  for (; s[n]; n++) {
    char c = s[n];
    if (n >= 80) return 0;
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
  }
  return 1;
}

/* "0.13.1407" -> {0, 13, 1407}: exactly three dot-separated numbers, each 0..65535 (a Windows file
   version's first three parts). Anything else is refused. */
static int yg__start_version_parse(const char *s, unsigned out[3]) {
  int part = 0, digits = 0;
  if (!s) return 0;
  out[0] = out[1] = out[2] = 0;
  for (; *s; s++) {
    if (*s >= '0' && *s <= '9') {
      out[part] = out[part] * 10 + (unsigned)(*s - '0');
      if (++digits > 5 || out[part] > 65535) return 0;
    } else if (*s == '.' && digits && part < 2) {
      part++;
      digits = 0;
    } else {
      return 0;
    }
  }
  return part == 2 && digits;
}

static int yg__start_version_at_least(const unsigned have[3], const unsigned want[3]) {
  if (have[0] != want[0]) return have[0] > want[0];
  if (have[1] != want[1]) return have[1] > want[1];
  return have[2] >= want[2];
}

#if defined(_WIN32) || defined(YOUGAME_START_TESTING)
/* The Windows tray start's pure text checks. They sit outside the _WIN32 block so the Mac test runs
   them; the checks that need a file or a process stay in the Windows block (and its test seam). */
#define YG__START_PATH_CAP 1024

/* The one program the Windows tray start may start, one entry per flavour: [0] production (yougame://),
   [1] staging (yougame-staging://). It must be <per-user install root>\<folder>\<file> (the installer
   is per-user only), and its signed version resource must say: ProductName = product, FileDescription
   = product, InternalName present and = product, CompanyName = company. OriginalFilename is not
   required (the real app's is empty); a non-empty one must be the file name. This is the only place
   these names live. The same certificate also signs the installer ("YouGame-<version>-win-x64.exe"),
   the uninstaller ("Uninstall <product>.exe": same ProductName, a long FileDescription, no
   InternalName) and resources\elevate.exe: each fails these checks.
   Sources:
   - STAGING: read from signed staging 0.13.1408 by the release lead (file "YouGame Staging.exe",
     ProductName = FileDescription = InternalName "YouGame Staging", OriginalFilename present and
     empty, CompanyName "YouGame", installed in %LOCALAPPDATA%\Programs\yougame-staging). It matches the
     config: desktop/build/staging.cjs:19 productName "YouGame Staging"; staging-installer.cjs:23
     Windows package name "yougame-staging" (a one-click per-user install folder is the package name:
     app-builder-lib targetUtil.js getWindowsInstallationDirName); rcedit writes InternalName = exe
     name without ".exe" and OriginalFilename = "" (app-builder-lib winPackager.js
     signAndEditResources).
   - PRODUCTION: from config, not yet read from a production binary: desktop/electron-builder.yml:6
     productName "YouGame", :83-84 oneClick true / perMachine false, desktop/package.json:2 name
     "yougame-desktop"; FileDescription, InternalName and CompanyName assumed as on staging. */
typedef struct yg__start_allowed {
  const wchar_t *file;    /* the exe's name, the path's last part (any letter case) */
  const wchar_t *product; /* ProductName = FileDescription = InternalName (exact) */
  const wchar_t *company; /* CompanyName (exact) */
  const wchar_t *folder;  /* its folder under the per-user install root (any letter case) */
} yg__start_allowed;
static const yg__start_allowed yg__start_allow[2] = {
  /* production: from config, not yet read from a production binary */
  {L"YouGame.exe", L"YouGame", L"YouGame", L"yougame-desktop"},
  /* staging: read from signed staging 0.13.1408 */
  {L"YouGame Staging.exe", L"YouGame Staging", L"YouGame", L"yougame-staging"}};

/* SIGNER: the leaf's subject CN and O are both "Fluo Inc.", and its issuer is Microsoft's identity
   verified code signing CA: issuer O "Microsoft Corporation", issuer CN starting with
   "Microsoft ID Verified CS " (the CA number changes). Read from signed staging 0.13.1408 (subject
   "CN=Fluo Inc., O=Fluo Inc., L=Orlando, S=Florida, C=US", issuer "CN=Microsoft ID Verified CS EOC CA
   03, O=Microsoft Corporation, C=US"). The leaf is re-issued every few days, so no thumbprint or
   serial pin. L, S and C are not pinned: desktop/WINDOWS_SIGNING.md says the certificate "still lists
   Orlando until it is renewed", so the locality is expected to change. */
#define YG__START_SIGNER_CN L"Fluo Inc."
#define YG__START_SIGNER_O L"Fluo Inc."
#define YG__START_ISSUER_O L"Microsoft Corporation"
#define YG__START_ISSUER_CN_PREFIX L"Microsoft ID Verified CS "

static wchar_t yg__start_wfold(wchar_t c) { return c >= L'A' && c <= L'Z' ? (wchar_t)(c + 32) : c; }

/* Whole strings equal, ASCII letters in any case (Windows paths). Anything else must match exactly,
   so a name that differs only in a non-ASCII letter's case is refused, never accepted. */
static int yg__start_wsame(const wchar_t *a, const wchar_t *b) {
  if (!a || !b) return 0;
  for (; yg__start_wfold(*a) == yg__start_wfold(*b); ++a, ++b)
    if (!*a) return 1;
  return 0;
}

/* The length of `prefix` when `s` starts with it (ASCII letters in any case), else 0. */
static size_t yg__start_wprefix(const wchar_t *s, const wchar_t *prefix) {
  size_t i;
  for (i = 0; prefix[i]; ++i)
    if (yg__start_wfold(s[i]) != yg__start_wfold(prefix[i])) return 0;
  return i;
}

/* Whole strings equal, exactly (version resource and certificate texts). NULL never equals. */
static int yg__start_wequal(const wchar_t *a, const wchar_t *b) {
  if (!a || !b) return 0;
  for (; *a == *b; ++a, ++b)
    if (!*a) return 1;
  return 0;
}

static const wchar_t *yg__start_basename(const wchar_t *path) {
  const wchar_t *last = path;
  for (; *path; ++path)
    if (*path == L'\\') last = path + 1;
  return last;
}

/* The program in a registered open command: the quoted first part, or the text up to the first space.
   Only a local drive path ("C:\...\X.exe") is taken: no relative path (it would be searched for), no
   UNC, \\?\ or \\.\ path, no quote, wildcard, forward slash, control character or second colon (an
   alternate data stream). Writes it to `out` (cap characters with the terminator); 1 when taken. */
static int yg__start_exe_from_command(const wchar_t *command, wchar_t *out, size_t cap) {
  const wchar_t *first = command, *end;
  wchar_t c;
  size_t n, i;
  while (*first == L' ' || *first == L'\t') ++first;
  if (*first == L'"') {
    ++first;
    for (end = first; *end && *end != L'"'; ++end) {}
    if (!*end) return 0;
  } else {
    for (end = first; *end && *end != L' ' && *end != L'\t'; ++end) {}
  }
  n = (size_t)(end - first);
  if (n < 8 || n >= cap) return 0;
  c = yg__start_wfold(first[0]);
  if (!(c >= L'a' && c <= L'z') || first[1] != L':' || first[2] != L'\\') return 0;
  for (i = 0; i < n; ++i)
    if (first[i] < 32 || first[i] == L'"' || first[i] == L'/' || first[i] == L'*' ||
        first[i] == L'?' || first[i] == L'<' || first[i] == L'>' || first[i] == L'|' ||
        (i > 1 && first[i] == L':')) return 0;
  if (first[n - 4] != L'.' || yg__start_wfold(first[n - 3]) != L'e' ||
      yg__start_wfold(first[n - 2]) != L'x' || yg__start_wfold(first[n - 1]) != L'e') return 0;
  for (i = 0; i < n; ++i) out[i] = first[i];
  out[n] = 0;
  return 1;
}

/* The held file's final path (GetFinalPathNameByHandleW, normalized, DOS volume name) is the path that
   was opened: so no folder on the way is a junction or link, and the drive letter is the volume's own
   (not a subst or a redefined letter). A network file (\\?\UNC\...) or a volume with no letter
   (\\?\Volume{...}) never matches a "C:\" path. */
static int yg__start_final_same(const wchar_t *final, const wchar_t *path) {
  size_t skip = yg__start_wprefix(final, L"\\\\?\\");
  if (skip && yg__start_wprefix(final + skip, L"UNC\\")) return 0;
  return yg__start_wsame(final + skip, path);
}

/* `path` is exactly <root>\<folder>\<file>: the program sits directly in its own install folder. */
static int yg__start_in_folder(const wchar_t *path, const wchar_t *root, const wchar_t *folder,
                               const wchar_t *file) {
  size_t r, k;
  if (!root || !folder || !*folder) return 0;
  for (r = 0; root[r]; ++r) {}
  while (r > 3 && root[r - 1] == L'\\') --r; /* "C:\Programs\" as "C:\Programs"; "C:\" stays */
  if (r < 3 || root[1] != L':' || root[2] != L'\\') return 0;
  for (k = 0; k < r; ++k)
    if (yg__start_wfold(path[k]) != yg__start_wfold(root[k])) return 0;
  path += r;
  if (root[r - 1] != L'\\' && *path++ != L'\\') return 0;
  k = yg__start_wprefix(path, folder);
  if (!k || path[k] != L'\\') return 0;
  return yg__start_wsame(path + k + 1, file);
}

/* The program is the YouGame app of this flavour, by its file name and its version resource texts
   (NULL = the text is missing). */
static int yg__start_identity_ok(int staging, const wchar_t *file, const wchar_t *product,
                                 const wchar_t *description, const wchar_t *internal,
                                 const wchar_t *company, const wchar_t *original) {
  const yg__start_allowed *a = &yg__start_allow[staging ? 1 : 0];
  return yg__start_wsame(file, a->file) && yg__start_wequal(product, a->product) &&
         yg__start_wequal(description, a->product) && yg__start_wequal(internal, a->product) &&
         yg__start_wequal(company, a->company) &&
         (!original || !*original || yg__start_wsame(original, a->file));
}

/* The signing certificate is Fluo Inc.'s, issued by Microsoft's identity verified CA (see SIGNER
   above). Each text is one attribute read on its own; NULL (unreadable) fails. */
static int yg__start_signer_ok(const wchar_t *cn, const wchar_t *org, const wchar_t *issuer_cn,
                               const wchar_t *issuer_org) {
  size_t k;
  if (!issuer_cn) return 0;
  k = 0;
  while (YG__START_ISSUER_CN_PREFIX[k] && issuer_cn[k] == YG__START_ISSUER_CN_PREFIX[k]) ++k;
  return !YG__START_ISSUER_CN_PREFIX[k] && issuer_cn[k] && yg__start_wequal(cn, YG__START_SIGNER_CN) &&
         yg__start_wequal(org, YG__START_SIGNER_O) && yg__start_wequal(issuer_org, YG__START_ISSUER_O);
}

/* Environment entries ("NAME=value") dropped from the started YouGame's environment (which is the
   user's own, never the game's: yg__start_env_block), should the user's or system's settings carry
   them: Electron's own switches (ELECTRON_RUN_AS_NODE would run the signed app as plain Node) and
   Node's preload and extra-CA settings. Names in any letter case, as
   Windows treats them; the hidden "=C:=..." entries are kept. */
static int yg__start_env_drop(const wchar_t *entry) {
  size_t n = 0;
  if (!entry || entry[0] == L'=') return 0;
  while (entry[n] && entry[n] != L'=') ++n;
  return yg__start_wprefix(entry, L"ELECTRON_") ||
         (n == 12 && yg__start_wprefix(entry, L"NODE_OPTIONS") == 12) ||
         (n == 19 && yg__start_wprefix(entry, L"NODE_EXTRA_CA_CERTS") == 19);
}

/* THE UPDATE MARKER. Right before YouGame quits to install an update it writes updating.json in its
   profile folder, app.getPath("userData"), under FOLDERID_RoamingAppData (the known folder Electron's
   app.getPath("appData") is). The app side is desktop PR 1777 (desktop/src/updating-marker.ts).
   [0] production: Electron names userData after app.name, which is package.json's productName, else
   its name. desktop/package.json has name "yougame-desktop" and no productName ("YouGame" is set only
   in electron-builder.yml, for the installer and program names), main.ts never calls setName or sets
   userData for production, and desktop/WINDOWS_HANDOFF.md and the agent-test proof look under
   %APPDATA%\yougame-desktop. So "yougame-desktop" is the folder, read first; "YouGame" is read too,
   since a later build that sets productName would move it there and games already shipped must keep
   working. The first valid marker wins. [1] staging: main.ts sets userData to "YouGame Staging (agent
   build)" (STAGING.userDataName), and the installed signed staging app's folder really is that. */
#define YG__START_PROFILES 2
static const wchar_t *const yg__start_profiles[2][YG__START_PROFILES] = {
  {L"yougame-desktop", L"YouGame"},
  {L"YouGame Staging (agent build)", NULL}};
#define YG__START_MARKER_MAX 1024        /* a marker is under 1 KiB */
#define YG__START_MARKER_AGE_MS 300000.0 /* `at` at most 5 minutes before this computer's clock */
#define YG__START_MARKER_SKEW_MS 60000.0 /* and at most 60 s after it */

/* <root>\<profile>\updating.json into out (cap characters with the terminator); 0 when root or
   profile is missing or empty, or the path does not fit. Trailing backslashes of root are dropped
   ("C:\" gives "C:\YouGame\..."). */
static int yg__start_marker_path(const wchar_t *root, const wchar_t *profile, wchar_t *out, size_t cap) {
  const wchar_t *tail = L"\\updating.json";
  size_t r, at, k;
  if (!root || !profile || !profile[0] || !cap) return 0;
  for (r = 0; root[r]; ++r) {}
  while (r && root[r - 1] == L'\\') --r;
  if (!r || r >= cap) return 0;
  for (at = 0; at < r; ++at) out[at] = root[at];
  if (at + 1 >= cap) return 0;
  out[at++] = L'\\';
  for (k = 0; profile[k]; ++k) {
    if (at + 1 >= cap) return 0;
    out[at++] = profile[k];
  }
  for (k = 0; tail[k]; ++k) {
    if (at + 1 >= cap) return 0;
    out[at++] = tail[k];
  }
  out[at] = 0;
  return 1;
}

/* `to` looks like a version: 1 to 32 characters, digits and single dots, a digit first and last. */
static int yg__start_marker_version_ok(const char *s, size_t n) {
  size_t i;
  if (n < 1 || n > YG__START_TO_CAP - 1) return 0;
  for (i = 0; i < n; ++i) {
    if (s[i] >= '0' && s[i] <= '9') continue;
    if (s[i] != '.' || i == 0 || i + 1 == n || s[i - 1] == '.') return 0;
  }
  return 1;
}

static size_t yg__start_json_space(const char *s, size_t n, size_t i) {
  while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
  return i;
}

/* A JSON string at s[*i] (its opening quote), moved past. 1: plain text (no escape), at most cap - 1
   bytes, copied into out (NUL-terminated) with its length in *len. 2: well formed but with an escape
   or longer than that: skipped, out empty and *len 0 (usable only as a field the hook ignores). 0: a
   control character, or unclosed within n. */
static int yg__start_json_string(const char *s, size_t n, size_t *i, char *out, size_t cap, size_t *len) {
  size_t k = 0;
  int plain = 1;
  if (*i >= n || s[*i] != '"') return 0;
  for (++*i; *i < n; ++*i) {
    unsigned char c = (unsigned char)s[*i];
    if (c == '"') {
      ++*i;
      if (!plain) k = 0;
      out[k] = 0;
      *len = k;
      return plain ? 1 : 2;
    }
    if (c < 0x20) return 0;
    if (c == '\\') {
      plain = 0;
      if (++*i >= n || (unsigned char)s[*i] < 0x20) return 0; /* the escaped byte, never a closing quote */
    } else if (plain && k + 1 < cap) {
      out[k++] = (char)c;
    } else {
      plain = 0;
    }
  }
  return 0;
}

/* A JSON whole number at s[*i]: 1 to 15 digits (so it is exact in a double; no `long long`, which
   C89 builds lack), no sign, no leading zero, no fraction or exponent. */
static int yg__start_json_whole(const char *s, size_t n, size_t *i, double *out) {
  size_t start = *i;
  double v = 0;
  while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
    if (*i - start >= 15) return 0;
    v = v * 10 + (double)(s[*i] - '0');
    ++*i;
  }
  if (*i == start || (s[start] == '0' && *i - start > 1)) return 0;
  if (*i < n && (s[*i] == '.' || s[*i] == 'e' || s[*i] == 'E')) return 0;
  *out = v;
  return 1;
}

/* true, false or null at s[*i]. */
static int yg__start_json_word(const char *s, size_t n, size_t *i) {
  static const char *const words[3] = {"true", "false", "null"};
  size_t w, k;
  for (w = 0; w < 3; ++w) {
    for (k = 0; words[w][k] && *i + k < n && s[*i + k] == words[w][k]; ++k) {}
    if (!words[w][k]) {
      *i += k;
      return 1;
    }
  }
  return 0;
}

/* The marker's body (n bytes, not NUL-terminated) is a valid marker at now_ms (epoch milliseconds):
   one JSON object with "v": 1, "to": a version (yg__start_marker_version_ok), "at": whole epoch
   milliseconds at most 5 minutes before now_ms and at most 60 s after it, and optionally "from": a
   string. Other keys (any string, escapes included) with a string, whole number, true, false or null
   value are skipped, so the app can add fields later. Strict and bounded: a UTF-8 byte order mark and
   whitespace between tokens are allowed; anything else (an escape in a known key or in "to", a nested
   object or array, a negative or fractional number, a known key twice, text after the object, n of
   1024 or more) is no marker. On 1, to holds the version. */
static int yg__start_marker_parse(const char *s, size_t n, double now_ms, char to[YG__START_TO_CAP]) {
  char key[32], value[64], version[YG__START_TO_CAP];
  size_t i = 0, len = 0;
  int have_v = 0, have_to = 0, have_at = 0, have_from = 0;
  double at = 0, num = 0;
  if (!s || n >= YG__START_MARKER_MAX) return 0;
  if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
    i = 3;
  i = yg__start_json_space(s, n, i);
  if (i >= n || s[i] != '{') return 0;
  i = yg__start_json_space(s, n, i + 1);
  for (;;) {
    int kind; /* 0 true/false/null, 1 plain string, 2 whole number, 3 other string */
    int r = yg__start_json_string(s, n, &i, key, sizeof key, &len);
    if (!r) return 0; /* r == 2: a key no field of the hook's has, so key is "" and matches none */
    i = yg__start_json_space(s, n, i);
    if (i >= n || s[i] != ':') return 0;
    i = yg__start_json_space(s, n, i + 1);
    if (i >= n) return 0;
    if (s[i] == '"') {
      r = yg__start_json_string(s, n, &i, value, sizeof value, &len);
      if (!r) return 0;
      kind = r == 1 ? 1 : 3;
    } else if (s[i] >= '0' && s[i] <= '9') {
      if (!yg__start_json_whole(s, n, &i, &num)) return 0;
      kind = 2;
    } else if (yg__start_json_word(s, n, &i)) {
      kind = 0;
    } else {
      return 0;
    }
    if (!strcmp(key, "v")) {
      if (have_v++ || kind != 2 || num != 1) return 0;
    } else if (!strcmp(key, "at")) {
      if (have_at++ || kind != 2) return 0;
      at = num;
    } else if (!strcmp(key, "to")) {
      if (have_to++ || kind != 1 || !yg__start_marker_version_ok(value, len)) return 0;
      memcpy(version, value, len + 1);
    } else if (!strcmp(key, "from")) {
      if (have_from++ || (kind != 1 && kind != 3)) return 0;
    }
    i = yg__start_json_space(s, n, i);
    if (i >= n) return 0;
    if (s[i] == '}') break;
    if (s[i] != ',') return 0;
    i = yg__start_json_space(s, n, i + 1);
  }
  if (yg__start_json_space(s, n, i + 1) != n) return 0;
  if (!have_v || !have_to || !have_at) return 0;
  if (at > now_ms + YG__START_MARKER_SKEW_MS || now_ms - at > YG__START_MARKER_AGE_MS) return 0;
  memcpy(to, version, strlen(version) + 1);
  return 1;
}

/* GetDriveTypeW's DRIVE_FIXED, and the file attributes the marker must not have (FILE_ATTRIBUTE_
   DIRECTORY, _DEVICE, _REPARSE_POINT), written out so the Mac test runs the same checks. */
#define YG__START_DRIVE_FIXED 3u
#define YG__START_ATTR_REFUSED (0x10UL | 0x40UL | 0x400UL)

/* "X:\..." with a drive letter: not a UNC path (\\server\share), not a \\?\ or \\.\ device path, not
   relative. */
static int yg__start_drive_path(const wchar_t *path) {
  return path && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
         path[1] == L':' && path[2] == L'\\';
}

/* The marker may be opened at all: its path is on a drive letter (yg__start_drive_path) whose type,
   GetDriveTypeW of "X:\", is a fixed local drive. A network drive (a Roaming folder redirected to a
   share, mapped or UNC), a removable, CD or RAM drive, or an unknown type is no marker, decided before
   anything is opened, so a slow or gone server cannot hold up the game (a folder on the way that is
   itself a link to a share is not detected). */
static int yg__start_marker_local(const wchar_t *path, unsigned drive_type) {
  return yg__start_drive_path(path) && drive_type == YG__START_DRIVE_FIXED;
}

/* The opened marker is a plain file the hook takes: on disk (GetFileType is FILE_TYPE_DISK), not a
   folder, device or reparse point (it is opened as itself, so a link is seen, never followed), and
   under 1 KiB (the size's high half 0, its low half below YG__START_MARKER_MAX). */
static int yg__start_marker_file_ok(int on_disk, unsigned long attrs, unsigned long size_high,
                                    unsigned long size_low) {
  return on_disk && !(attrs & YG__START_ATTR_REFUSED) && !size_high && size_low < YG__START_MARKER_MAX;
}

/* A FILETIME (100 ns steps since 1601-01-01 UTC, in its 32-bit halves) as epoch milliseconds in *ms;
   0 for a time before 1970. The Unix epoch is FILETIME 116444736000000000 (0x019DB1DE D53E8000);
   the halves are subtracted exactly (no 64-bit integer type, which C89 lacks), and only the
   difference goes into a double: within a microsecond for the next few hundred years. */
#define YG__START_EPOCH_HIGH 0x019DB1DEUL
#define YG__START_EPOCH_LOW 0xD53E8000UL
static int yg__start_filetime_ms(unsigned long high, unsigned long low, double *ms) {
  unsigned long dh, dl;
  high &= 0xFFFFFFFFUL;
  low &= 0xFFFFFFFFUL;
  if (high < YG__START_EPOCH_HIGH || (high == YG__START_EPOCH_HIGH && low < YG__START_EPOCH_LOW))
    return 0;
  dh = high - YG__START_EPOCH_HIGH;
  dl = (low - YG__START_EPOCH_LOW) & 0xFFFFFFFFUL;
  if (low < YG__START_EPOCH_LOW) --dh;
  *ms = ((double)dh * 4294967296.0 + (double)dl) / 10000.0;
  return 1;
}

/* The installed program's version (its file version's first three parts) is already the marker's
   `to` or newer, so the marker is a leftover of an update that has finished. Number by number from
   the left; parts `to` lacks count as 0, and so do have's parts past the third (a file version's fourth part,
   which the hook does not read). Leading zeros do not count; a part of more than 9 digits is larger
   than any have. `to` is digits and dots (yg__start_marker_version_ok). */
static int yg__start_version_reached(const unsigned have[3], const char *to) {
  size_t i = 0;
  unsigned part;
  for (part = 0;; ++part) {
    unsigned long v = 0, h = part < 3 ? have[part] : 0;
    int digits = 0;
    for (; to[i] >= '0' && to[i] <= '9'; ++i) {
      if (!v && to[i] == '0') continue;
      if (++digits > 9) return 0;
      v = v * 10 + (unsigned long)(to[i] - '0');
    }
    if (v != h) return h > v;
    if (to[i] != '.') return 1;
    ++i;
  }
}
#endif

#if defined(__APPLE__) || defined(YOUGAME_START_TESTING)
/* Pure Mac adapters are shared with the seam tests; no real helpers run in those tests. */
#include <limits.h>
#ifndef PATH_MAX /* MSVC builds of the seam tests */
#define PATH_MAX 1024
#endif
/* codesign --strict on a large bundle on a cold disk may approach this 8 s deadline.
   A timeout reads as FAILED; the game keeps running and can retry. */
#define YG__START_CODESIGN_MS 8000u
#define YG__START_OPEN_MS 5000u
static void yg__start_sleep(unsigned ms);
static unsigned long yg__start_ms(void);
static int yg__start_running(int staging);

/* LaunchServices may finish launching even after the open child was killed at its deadline; the
   app's socket can take a moment more, so look up to three times, 250 ms apart. */
static int yg__start_mac_opened(int waited, int staging) {
  int tries;
  if (waited == 1) return 1;
  if (waited != -1) return 0;
  for (tries = 0; tries < 3; ++tries) {
    if (tries) yg__start_sleep(250);
    if (yg__start_running(staging)) return 1;
  }
  return 0;
}

/* poll_child: 0 pending (including EINTR), 1 clean exit, -1 failed/reaped elsewhere.
   stop_child kills and reaps. Return -1 only for a deadline, 0 for another failure. */
static int yg__start_mac_wait(unsigned deadline, int (*poll_child)(void *),
                              void (*stop_child)(void *), void *child) {
  unsigned slept = 0;
  unsigned long start = yg__start_ms();
  for (;;) {
    unsigned long elapsed = (yg__start_ms() - start) & 0xFFFFFFFFUL;
    unsigned step;
    int result;
    if (elapsed >= deadline || slept >= deadline) {
      stop_child(child);
      return -1;
    }
    result = poll_child(child);
    if (result) return result > 0;
    elapsed = (yg__start_ms() - start) & 0xFFFFFFFFUL;
    if (elapsed >= deadline) continue;
    step = deadline - (unsigned)elapsed;
    if (step > deadline - slept) step = deadline - slept;
    if (step > 50) step = 50;
    yg__start_sleep(step);
    slept += step; /* also bounds the wait if the clock fails */
  }
}

static int yg__start_mac_requirement(int staging, char *out, size_t cap) {
  /* codesign -R reads its argument as requirement text only after a leading "=" (else a file path:
     "invalid requirement specification", so every installed app read as unsigned). */
  const char *base = "=anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists "
    "and certificate leaf[field.1.2.840.113635.100.6.1.13] exists "
    "and certificate leaf[subject.OU] = \"4894SAXKGG\" and identifier ";
  int n = snprintf(out, cap, "%s%s", base,
    staging ? "co.yougame.desktop.staging" : "co.yougame.desktop");
  return n >= 0 && (size_t)n < cap;
}

static void yg__start_mac_verify_argv(char *path, char *requirement, char *argv[8]) {
  argv[0] = (char *)"/usr/bin/codesign";
  argv[1] = (char *)"--verify";
  argv[2] = (char *)"--strict";
  argv[3] = (char *)"--all-architectures";
  argv[4] = (char *)"-R";
  argv[5] = requirement;
  argv[6] = path;
  argv[7] = NULL;
}

static void yg__start_mac_open_argv(char *path, char *argv[8]) {
  argv[0] = (char *)"/usr/bin/open";
  argv[1] = (char *)"-g";
  argv[2] = (char *)"-j";
  argv[3] = (char *)"-a";
  argv[4] = path;
  argv[5] = (char *)"--args";
  argv[6] = (char *)"--yougame-tray";
  argv[7] = NULL;
}

/* Keep ordinary launcher variables, but never app/test switches or Electron/Node overrides.
   LaunchServices does not pass open's caller environment to the app anyway; filter both helpers
   as well so neither helper inherits these controls. The returned pointer array is caller-owned. */
static char **yg__start_mac_environment(char *const source[]) {
  size_t n = 0, kept = 0, i;
  char **out;
  while (source[n]) ++n;
  if (n > (size_t)-1 / sizeof *out - 1) return NULL;
  out = (char **)malloc((n + 1) * sizeof *out);
  if (!out) return NULL;
  for (i = 0; i < n; ++i) {
    if (!strncmp(source[i], "YOUGAME_", 8) || !strncmp(source[i], "ELECTRON_", 9) ||
        !strncmp(source[i], "NODE_OPTIONS=", 13) || !strcmp(source[i], "NODE_OPTIONS")) continue;
    out[kept++] = source[i];
  }
  out[kept] = NULL;
  return out;
}

static int yg__start_mac_owner_ok(unsigned long owner, unsigned long user) {
  return owner == 0 || owner == user;
}

/* Audit every link, including links inside another link's target, reusing the same buffers.
   link_target checks ownership and refuses any lstat/readlink failure. At most 40 links. */
static int yg__start_mac_links_ok(const char *path,
                                 int (*link_target)(const char *, char *, size_t)) {
  char prefix[PATH_MAX], target[PATH_MAX], next[PATH_MAX];
  unsigned links = 0;
  size_t n = strlen(path);
  if (path[0] != '/' || n >= sizeof next) return 0;
  memcpy(next, path, n + 1);
  for (;;) {
    size_t i;
    n = strlen(next);
    memcpy(prefix, next, n + 1);
    for (i = 1; i <= n; ++i) {
      int link, written;
      char saved;
      size_t parent = i;
      if (next[i] && next[i] != '/') continue;
      saved = prefix[i];
      prefix[i] = 0;
      link = link_target(prefix, target, sizeof target);
      prefix[i] = saved;
      if (link < 0) return 0;
      if (!link) continue;
      if (++links > 40) return 0;
      while (parent && prefix[parent - 1] != '/') --parent;
      written = target[0] == '/' ? snprintf(next, sizeof next, "%s%s", target, prefix + i) :
        snprintf(next, sizeof next, "%.*s%s%s", (int)parent, prefix, target, prefix + i);
      if (written < 0 || (size_t)written >= sizeof next) return 0;
      break;
    }
    if (i > n) return 1;
  }
}

static int yg__start_mac_bundle_ok(unsigned long owner, unsigned long user,
                                  unsigned mode, int directory) {
  return directory && yg__start_mac_owner_ok(owner, user) && !(mode & 0022u);
}

/* Match desktop/src/os-handlers.ts's development exclusions. Resolved paths have no dot parts.
   Additionally restrict quiet starts to Applications folders; mounted/build copies must not run. */
static int yg__start_mac_candidate(const char *path, const char *home) {
  const char *p;
  char lower[4096];
  size_t n, i, h;
  int rank = 0;
  if (!path || path[0] != '/') return 0;
  n = strlen(path);
  if (n < 5 || n >= sizeof lower) return 0;
  for (i = 0; i <= n; ++i)
    lower[i] = path[i] >= 'A' && path[i] <= 'Z' ? (char)(path[i] + ('a' - 'A')) : path[i];
  if (strcmp(lower + n - 4, ".app")) return 0;
  if (!strncmp(path, "/Applications/", 14)) rank = 2;
  h = home ? strlen(home) : 0;
  while (h > 1 && home[h - 1] == '/') --h;
  if (!rank && h > 1 && home[0] == '/' && !strncmp(path, home, h) &&
      !strncmp(path + h, "/Applications/", 14)) rank = 1;
  if (!rank) return 0;
  if (strstr(lower, "/apptranslocation/") || strstr(lower, "/volumes/") ||
      strstr(lower, "worktree") || strstr(lower, "/.claude/") ||
      strstr(lower, "/release-staging/") || strstr(path, "/desktop/release/")) return 0;
  for (p = lower; (p = strstr(p, "/release/mac")) != NULL; ++p)
    if (p[12] == '/' || p[12] == '-') return 0;
  for (p = lower; *p; ++p) {
    const char *end;
    if (*p == '/' && (p[1] == '/' || (p[1] == '.' &&
        (p[2] == '/' || (p[2] == '.' && p[3] == '/'))))) return 0;
    /* os-handlers also excludes win[-arch]-unpacked and linux[-arch]-unpacked folders. */
    end = *p == '/' ? strchr(p + 1, '/') : NULL;
    if (end && end - p >= 13 && !strncmp(end - 9, "-unpacked", 9) &&
        (!strncmp(p + 1, "win-", 4) || !strncmp(p + 1, "linux-", 6))) return 0;
    /* The executable must be inside this bundle, not an outer .app's nested helper. */
    if (!strncmp(p, ".app/", 5)) return 0;
  }
  return rank;
}

static int yg__start_mac_prefer(const char *path, const char *home, int best) {
  int rank = yg__start_mac_candidate(path, home);
  return rank > best ? rank : 0;
}
#endif

#ifdef YOUGAME_START_TESTING
/* The test translation unit defines these; no OS calls, apps or windows in seam builds. */
typedef struct yg__start_app { int id; } yg__start_app;
static int yg__start_test_windows; /* Windows tray and update-marker path */
static int yg__start_test_macos; /* macOS tray path, without update markers; both 0 = Linux */
#define YG__START_TRAY_SYSTEM (yg__start_test_windows || yg__start_test_macos)
#define YG__START_UPDATE_SYSTEM yg__start_test_windows
static int yg__start_running(int staging);
static int yg__start_installed(int staging);
static int yg__start_app_find(int staging, yg__start_app *app);
static int yg__start_app_named(yg__start_app *app, int staging);
static int yg__start_app_version(yg__start_app *app, unsigned have[3]);
static int yg__start_app_signed(yg__start_app *app);
static int yg__start_app_launch(yg__start_app *app, int staging);
static void yg__start_app_close(yg__start_app *app);
static int yg__start_shell_open(const char *url);
static int yg__start_offer(int text);
static int yg__start_quiet_env(void);
static void yg__start_sleep(unsigned ms);
static unsigned long yg__start_ms(void);
static int yg__start_installed_version(int staging, unsigned have[3]);
typedef struct yg__start_mark { int looked; double age_ms; } yg__start_mark;
static int yg__start_marker(int staging, yg__start_mark *mark, char to[YG__START_TO_CAP]);
#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <wchar.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

#define YG__START_TRAY_SYSTEM 1
#define YG__START_UPDATE_SYSTEM 1
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
/* GetFinalPathNameByHandleW flags (FILE_NAME_NORMALIZED with VOLUME_NAME_DOS / VOLUME_NAME_NT),
   FileRemoteProtocolInfo and PROCESS_NAME_NATIVE, written out so a game that sets an old
   _WIN32_WINNT still builds; the functions themselves are looked up in kernel32 at run time. */
#define YG__START_FINAL_DOS 0x0
#define YG__START_FINAL_NT 0x2
#define YG__START_REMOTE_INFO 13
#define YG__START_IMAGE_NATIVE 0x1
#define YG__START_VER_NEUTRAL 0x2 /* FILE_VER_GET_NEUTRAL */

/* The installed program: its path, a handle that stops it being changed, replaced or deleted from
   the moment it is checked until it has been started, and what its version resource says. */
typedef struct yg__start_app {
  HANDLE file;
  wchar_t path[YG__START_PATH_CAP];
  int named;       /* its version resource and file name are the YouGame app's (yg__start_allow) */
  int have_ok;     /* its file version was read */
  unsigned have[3];
} yg__start_app;

static void yg__start_sleep(unsigned ms) { Sleep(ms); }

/* A monotonic millisecond clock for the tray wait: GetTickCount (kernel32, every Windows; time since
   boot, not the wall clock the user can set). It wraps every 49.7 days; the wait takes differences
   modulo 2^32, so a wrap inside a wait of at most 60 s is harmless. */
static unsigned long yg__start_ms(void) { return (unsigned long)GetTickCount(); }

static int yg__start_quiet_env(void) {
  wchar_t v[8];
  DWORD n = GetEnvironmentVariableW(L"YOUGAME_START_QUIET", v, 8);
  return n == 1 && v[0] == L'1';
}

/* The app's local channel for this Windows user: \\.\pipe\yougame[-staging]-sdk-v1-<user SID>.
   Only its presence is checked; nothing connects to it here. */
static int yg__start_running(int staging) {
  HANDLE tok = NULL;
  union { TOKEN_USER user; BYTE bytes[256]; } buf; /* aligned for TOKEN_USER */
  DWORD len = 0;
  LPWSTR sid = NULL;
  wchar_t name[400];
  int up = 0;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
  if (GetTokenInformation(tok, TokenUser, &buf, sizeof buf, &len) &&
      ConvertSidToStringSidW(buf.user.User.Sid, &sid) && sid) {
    if (wcslen(sid) < 200) {
      wcscpy(name, staging ? L"\\\\.\\pipe\\yougame-staging-sdk-v1-" : L"\\\\.\\pipe\\yougame-sdk-v1-");
      wcscat(name, sid);
      /* TRUE: a pipe instance is free. ERROR_SEM_TIMEOUT: the pipe exists and is busy. */
      if (WaitNamedPipeW(name, 1) || GetLastError() == ERROR_SEM_TIMEOUT) up = 1;
    }
    LocalFree(sid);
  }
  CloseHandle(tok);
  return up;
}

/* The link type's open command, as the app registers it ("<exe>" "%1"). HKCU wins over the merged
   HKCR view; HKCR is read only when HKCU has no key at all. */
static int yg__start_command(int staging, wchar_t *command, DWORD cap) {
  HKEY key;
  LONG rc;
  DWORD type = 0, size;
  wchar_t raw[2048];
  rc = RegOpenKeyExW(HKEY_CURRENT_USER, staging
    ? L"Software\\Classes\\yougame-staging\\shell\\open\\command"
    : L"Software\\Classes\\yougame\\shell\\open\\command", 0, KEY_QUERY_VALUE, &key);
  if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)
    rc = RegOpenKeyExW(HKEY_CLASSES_ROOT, staging
      ? L"yougame-staging\\shell\\open\\command" : L"yougame\\shell\\open\\command",
      0, KEY_QUERY_VALUE, &key);
  if (rc != ERROR_SUCCESS) return 0;
  size = sizeof raw;
  rc = RegQueryValueExW(key, NULL, NULL, &type, (BYTE *)raw, &size);
  RegCloseKey(key);
  /* A value that does not end in its terminator is refused, never patched up. */
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
      size < 2 * sizeof(wchar_t) || size % sizeof(wchar_t) || raw[size / sizeof(wchar_t) - 1])
    return 0;
  if (type == REG_EXPAND_SZ) {
    DWORD n = ExpandEnvironmentStringsW(raw, command, cap);
    return n && n <= cap;
  }
  if (size / sizeof(wchar_t) > cap) return 0;
  memcpy(command, raw, size);
  return 1;
}

/* Installed = the link type has a non-empty open command for this user (the app writes it). */
static int yg__start_installed(int staging) {
  wchar_t command[2048];
  return yg__start_command(staging, command, 2048) && command[0];
}

/* A game run as administrator would start YouGame with the same rights: never do that. Raised means
   a full token from UAC (TokenElevationTypeFull) or any elevated token (TokenIsElevated), which also
   covers UAC turned off and the built-in Administrator, whose full tokens have type Default. A
   question that cannot be answered counts as raised. */
static int yg__start_raised(void) {
  HANDLE tok = NULL;
  TOKEN_ELEVATION_TYPE type = TokenElevationTypeFull;
  TOKEN_ELEVATION elevation;
  DWORD len = 0;
  int raised = 1;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 1;
  elevation.TokenIsElevated = 1;
  if (GetTokenInformation(tok, TokenElevationType, &type, sizeof type, &len) &&
      GetTokenInformation(tok, TokenElevation, &elevation, sizeof elevation, &len))
    raised = type == TokenElevationTypeFull || elevation.TokenIsElevated != 0;
  CloseHandle(tok);
  return raised;
}

/* Only the system directory's copy, and its own imports from there too (LOAD_LIBRARY_SEARCH_SYSTEM32):
   never a DLL of that name beside the game. Windows without that flag (Windows 7 before KB2533623)
   refuses the load, so nothing is started there. */
static HMODULE yg__start_system_dll(const wchar_t *name) {
  return LoadLibraryExW(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

/* memcpy avoids ISO C's object/function pointer casts; FARPROC is a function pointer. */
#define YG__START_PROC(dst, dll, symbol) do { \
  FARPROC yg__proc = GetProcAddress(dll, symbol); memcpy(&(dst), &yg__proc, sizeof(dst)); \
} while (0)

typedef DWORD (WINAPI *yg__start_final_fn)(HANDLE, LPWSTR, DWORD, DWORD);

/* A file's final path (see yg__start_final_same) into out; 0 when it cannot be had or does not fit. */
static int yg__start_final_path(HANDLE file, DWORD flags, wchar_t *out, DWORD cap) {
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  yg__start_final_fn get_final = NULL;
  DWORD n;
  if (!k) return 0;
  YG__START_PROC(get_final, k, "GetFinalPathNameByHandleW");
  if (!get_final) return 0;
  n = get_final(file, out, cap, flags);
  return n && n < cap;
}

/* The file at `path` right now is the held file: same volume serial number and file index. */
static int yg__start_same_file(HANDLE held, const wchar_t *path) {
  BY_HANDLE_FILE_INFORMATION a, b;
  HANDLE f;
  int same;
  if (!GetFileInformationByHandle(held, &a)) return 0;
  f = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return 0;
  same = GetFileInformationByHandle(f, &b) && a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
         a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
  CloseHandle(f);
  return same;
}

/* The held file is where its path says, with no junction, link or remapped drive letter on the way
   (its final path is the path itself), and it is on a fixed local drive: not a network file, even one
   reached through a drive letter. */
static int yg__start_held_path_ok(yg__start_app *app) {
  typedef BOOL (WINAPI *info_fn)(HANDLE, int, LPVOID, DWORD);
  wchar_t final[YG__START_PATH_CAP + 16], root[4];
  union { ULONGLONG align; BYTE bytes[512]; } remote;
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  info_fn info = NULL;
  if (!k) return 0;
  YG__START_PROC(info, k, "GetFileInformationByHandleEx");
  if (!info ||
      !yg__start_final_path(app->file, YG__START_FINAL_DOS, final, YG__START_PATH_CAP + 16) ||
      !yg__start_final_same(final, app->path))
    return 0;
  /* Only a remote (redirector) file answers FileRemoteProtocolInfo. */
  if (info(app->file, YG__START_REMOTE_INFO, &remote, sizeof remote)) return 0;
  root[0] = app->path[0];
  root[1] = L':';
  root[2] = L'\\';
  root[3] = 0;
  return GetDriveTypeW(root) == DRIVE_FIXED;
}

/* The program sits directly in its flavour's install folder under the per-user install root
   (FOLDERID_UserProgramFiles, %LOCALAPPDATA%\Programs): the installer is per-user only. */
static int yg__start_install_folder_ok(const wchar_t *path, int staging) {
  typedef HRESULT (WINAPI *known_fn)(const GUID *, DWORD, HANDLE, PWSTR *);
  typedef void (WINAPI *free_fn)(LPVOID);
  static const GUID user_programs = /* FOLDERID_UserProgramFiles */
    {0x5cd7aee2, 0x2219, 0x4a67, {0xb8, 0x5d, 0x6c, 0x9c, 0xe1, 0x56, 0x60, 0xcb}};
  const yg__start_allowed *a = &yg__start_allow[staging ? 1 : 0];
  HMODULE shell = yg__start_system_dll(L"shell32.dll");
  HMODULE ole = yg__start_system_dll(L"ole32.dll");
  known_fn known = NULL;
  free_fn release = NULL;
  PWSTR root = NULL;
  int ok = 0;
  if (shell && ole) {
    YG__START_PROC(known, shell, "SHGetKnownFolderPath");
    YG__START_PROC(release, ole, "CoTaskMemFree");
  }
  if (known && release) {
    if (known(&user_programs, 0, NULL, &root) == S_OK && root)
      ok = yg__start_in_folder(path, root, a->folder, a->file);
    if (root) release(root);
  }
  if (ole) FreeLibrary(ole);
  if (shell) FreeLibrary(shell);
  return ok;
}

/* The update marker (see THE UPDATE MARKER), only read: never written or deleted. Its folder is
   FOLDERID_RoamingAppData from SHGetKnownFolderPath (loaded from the system directory, as above),
   never %APPDATA% from the game's environment, looked up once per yg_start call (mark), with the
   drive type of its root: only paths on a fixed local drive are kept (yg__start_marker_local), so a
   Roaming folder redirected to a network share is never opened (a folder on the way that is itself
   a link to a share is not detected: FILE_FLAG_OPEN_REPARSE_POINT covers the file alone). Each kept path, in yg__start_profiles' order, is read
   by yg__start_marker_read; the first valid marker wins. Any failure is no marker. */
typedef struct yg__start_mark {
  double age_ms; /* marker file age, for the installer post-step grace period */
  int looked, count; /* the folder was looked up; count paths kept in path */
  wchar_t path[YG__START_PROFILES][YG__START_PATH_CAP];
} yg__start_mark;

/* One marker file: opened as itself, not through a link, shared for reading, writing and deleting
   so the app is never held up; a plain file under 1 KiB (yg__start_marker_file_ok) whose body is
   valid against the system clock now (yg__start_filetime_ms, yg__start_marker_parse). */
static int yg__start_marker_read(const wchar_t *path, char to[YG__START_TO_CAP], double *age_ms) {
  char body[YG__START_MARKER_MAX];
  BY_HANDLE_FILE_INFORMATION info;
  FILETIME ft;
  DWORD got = 0;
  double now, written;
  HANDLE f;
  int ok = 0;
  f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  if (f == INVALID_HANDLE_VALUE) return 0;
  if (GetFileInformationByHandle(f, &info) &&
      yg__start_marker_file_ok(GetFileType(f) == FILE_TYPE_DISK, info.dwFileAttributes,
                               info.nFileSizeHigh, info.nFileSizeLow) &&
      ReadFile(f, body, (DWORD)sizeof body, &got, NULL) && got < YG__START_MARKER_MAX) {
    GetSystemTimeAsFileTime(&ft);
    if (yg__start_filetime_ms(ft.dwHighDateTime, ft.dwLowDateTime, &now) &&
        yg__start_filetime_ms(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime, &written)) {
      ok = yg__start_marker_parse(body, got, now, to);
      if (ok) *age_ms = now - written;
    }
  }
  CloseHandle(f);
  return ok;
}

static int yg__start_marker(int staging, yg__start_mark *mark, char to[YG__START_TO_CAP]) {
  typedef HRESULT (WINAPI *known_fn)(const GUID *, DWORD, HANDLE, PWSTR *);
  typedef void (WINAPI *free_fn)(LPVOID);
  static const GUID roaming = /* FOLDERID_RoamingAppData */
    {0x3eb685db, 0x65f9, 0x4cf6, {0xa0, 0x3a, 0xe3, 0xef, 0x65, 0x72, 0x9f, 0x3d}};
  int i;
  if (!mark->looked) {
    HMODULE shell = yg__start_system_dll(L"shell32.dll");
    HMODULE ole = yg__start_system_dll(L"ole32.dll");
    known_fn known = NULL;
    free_fn release = NULL;
    PWSTR root = NULL;
    mark->looked = 1;
    mark->count = 0;
    if (shell && ole) {
      YG__START_PROC(known, shell, "SHGetKnownFolderPath");
      YG__START_PROC(release, ole, "CoTaskMemFree");
    }
    if (known && release) {
      if (known(&roaming, 0, NULL, &root) == S_OK && root && yg__start_drive_path(root)) {
        wchar_t drive[4];
        unsigned type;
        drive[0] = root[0];
        drive[1] = L':';
        drive[2] = L'\\';
        drive[3] = 0;
        type = (unsigned)GetDriveTypeW(drive);
        for (i = 0; i < YG__START_PROFILES; ++i) {
          wchar_t *out = mark->path[mark->count];
          if (yg__start_marker_path(root, yg__start_profiles[staging ? 1 : 0][i], out, YG__START_PATH_CAP) &&
              yg__start_marker_local(out, type))
            ++mark->count;
        }
      }
      if (root) release(root);
    }
    if (ole) FreeLibrary(ole);
    if (shell) FreeLibrary(shell);
  }
  for (i = 0; i < mark->count; ++i)
    if (yg__start_marker_read(mark->path[i], to, &mark->age_ms)) return 1;
  return 0;
}

/* One text of the version resource, \StringFileInfo\<lang><codepage>\<name>, copied into out (at most
   cap - 1 characters): 1. Missing: 0 (out empty). Too long to be ours: -1. */
typedef BOOL (WINAPI *yg__start_query_fn)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
static int yg__start_version_text(yg__start_query_fn query, const void *block, const WORD *lang,
                                  const wchar_t *name, wchar_t *out, size_t cap) {
  static const wchar_t hex[] = L"0123456789abcdef";
  wchar_t key[64];
  const wchar_t *p = L"\\StringFileInfo\\", *value = NULL;
  UINT len = 0;
  size_t at = 0, i;
  DWORD code = ((DWORD)lang[0] << 16) | lang[1];
  while (*p) key[at++] = *p++;
  for (i = 0; i < 8; ++i) key[at++] = hex[(code >> (28 - 4 * i)) & 15];
  key[at++] = L'\\';
  for (i = 0; name[i] && at + 1 < sizeof key / sizeof *key; ++i) key[at++] = name[i];
  key[at] = 0;
  out[0] = 0;
  if (!query(block, key, (LPVOID *)&value, &len) || !value) return 0;
  for (i = 0; i < len && value[i]; ++i) {
    if (i + 1 >= cap) return -1;
    out[i] = value[i];
  }
  out[i] = 0;
  return 1;
}

/* Reads the program's version resource once: its file version (first three parts; electron-builder
   writes 0.13.1407 as 0,13,1407,0) and whether it is the YouGame app of this flavour
   (yg__start_identity_ok). Version.dll has no call that reads from a handle, so the read goes by the
   held file's own final path (GetFinalPathNameByHandleW, its "\\?\" prefix removed: a local drive
   path, already checked to be the registered one), not the registered text; yg__start_app_find checks
   right after that the path still leads to the held file, and the start itself is bound to the held
   file (yg__start_image_is_held). FILE_VER_GET_NEUTRAL reads the texts from the program itself, never
   from a .mui file beside it, so they are inside the signed image and a changed text fails the
   signature check. Without the Ex calls (before Vista) nothing is read, so nothing is started. */
static void yg__start_app_read(yg__start_app *app, int staging) {
  typedef DWORD (WINAPI *size_fn)(DWORD, LPCWSTR, LPDWORD);
  typedef BOOL (WINAPI *info_fn)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
  HMODULE dll = yg__start_system_dll(L"version.dll");
  size_fn get_size = NULL;
  info_fn get_info = NULL;
  yg__start_query_fn query = NULL;
  DWORD handle = 0, size = 0;
  void *buf = NULL;
  VS_FIXEDFILEINFO *fixed = NULL;
  WORD *lang = NULL;
  UINT len = 0;
  wchar_t product[128], description[128], internal[128], company[128], original[128];
  wchar_t final[YG__START_PATH_CAP + 16];
  const wchar_t *at;
  if (!dll) return;
  if (!yg__start_final_path(app->file, YG__START_FINAL_DOS, final, YG__START_PATH_CAP + 16) ||
      !yg__start_final_same(final, app->path)) {
    FreeLibrary(dll);
    return;
  }
  at = final + yg__start_wprefix(final, L"\\\\?\\");
  YG__START_PROC(get_size, dll, "GetFileVersionInfoSizeExW");
  YG__START_PROC(get_info, dll, "GetFileVersionInfoExW");
  YG__START_PROC(query, dll, "VerQueryValueW");
  if (get_size && get_info && query) size = get_size(YG__START_VER_NEUTRAL, at, &handle);
  if (size && size <= (1u << 20)) buf = HeapAlloc(GetProcessHeap(), 0, size);
  if (buf && get_info(YG__START_VER_NEUTRAL, at, 0, size, buf)) {
    if (query(buf, L"\\", (LPVOID *)&fixed, &len) && fixed && len >= sizeof *fixed &&
        fixed->dwSignature == 0xFEEF04BDu) {
      app->have[0] = HIWORD(fixed->dwFileVersionMS);
      app->have[1] = LOWORD(fixed->dwFileVersionMS);
      app->have[2] = HIWORD(fixed->dwFileVersionLS);
      app->have_ok = 1;
    }
    /* The texts of the first language the resource lists. A missing text reads as NULL. */
    len = 0;
    if (query(buf, L"\\VarFileInfo\\Translation", (LPVOID *)&lang, &len) && lang && len >= 4) {
      int p = yg__start_version_text(query, buf, lang, L"ProductName", product, 128);
      int d = yg__start_version_text(query, buf, lang, L"FileDescription", description, 128);
      int i = yg__start_version_text(query, buf, lang, L"InternalName", internal, 128);
      int c = yg__start_version_text(query, buf, lang, L"CompanyName", company, 128);
      int o = yg__start_version_text(query, buf, lang, L"OriginalFilename", original, 128);
      app->named = p >= 0 && d >= 0 && i >= 0 && c >= 0 && o >= 0 &&
                   yg__start_identity_ok(staging, yg__start_basename(app->path), p ? product : NULL,
                                         d ? description : NULL, i ? internal : NULL,
                                         c ? company : NULL, o ? original : NULL);
    }
  }
  if (buf) HeapFree(GetProcessHeap(), 0, buf);
  FreeLibrary(dll);
}

/* The program in the open command (yg__start_exe_from_command), opened with read sharing only so
   nothing can write, replace or delete it while it is checked and started; not a link or junction
   itself, and none on the way to it; on a fixed local drive; directly in its flavour's install folder;
   then its version resource is read, and the path must still lead to this same file. Refused outright
   when the game runs raised. */
static int yg__start_app_find(int staging, yg__start_app *app) {
  wchar_t command[2048];
  BY_HANDLE_FILE_INFORMATION info;
  app->file = INVALID_HANDLE_VALUE;
  app->path[0] = 0;
  app->named = app->have_ok = 0;
  if (yg__start_raised() || !yg__start_command(staging, command, 2048) ||
      !yg__start_exe_from_command(command, app->path, YG__START_PATH_CAP))
    return 0;
  app->file = CreateFileW(app->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  if (app->file == INVALID_HANDLE_VALUE) return 0;
  if (GetFileType(app->file) != FILE_TYPE_DISK || !GetFileInformationByHandle(app->file, &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      !yg__start_held_path_ok(app) || !yg__start_install_folder_ok(app->path, staging)) {
    CloseHandle(app->file);
    app->file = INVALID_HANDLE_VALUE;
    return 0;
  }
  yg__start_app_read(app, staging);
  if (!yg__start_held_path_ok(app) || !yg__start_same_file(app->file, app->path)) {
    CloseHandle(app->file);
    app->file = INVALID_HANDLE_VALUE;
    return 0;
  }
  return 1;
}

static void yg__start_app_close(yg__start_app *app) {
  if (app->file != INVALID_HANDLE_VALUE) CloseHandle(app->file);
  app->file = INVALID_HANDLE_VALUE;
}

/* The installed program's file version (first three parts), for the leftover-marker check only
   (yg__start_update_done). A valid marker usually means an installer is replacing the program right
   now, so the hook holds no handle of its own on it (yg__start_app_find's read-only sharing would
   block the installer's writes and deletes): version.dll reads it by path, for the moment of the
   read. The path is the open command's program (yg__start_exe_from_command: absolute, no UNC or
   device path) and its drive must be a fixed local one, checked before the read. Nothing else is
   checked: this only decides to skip "updating", and the tray start that follows checks it all. */
static int yg__start_installed_version(int staging, unsigned have[3]) {
  typedef DWORD (WINAPI *size_fn)(DWORD, LPCWSTR, LPDWORD);
  typedef BOOL (WINAPI *info_fn)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
  wchar_t command[2048], path[YG__START_PATH_CAP], drive[4];
  HMODULE dll;
  size_fn get_size = NULL;
  info_fn get_info = NULL;
  yg__start_query_fn query = NULL;
  DWORD handle = 0, size = 0;
  void *buf = NULL;
  VS_FIXEDFILEINFO *fixed = NULL;
  UINT len = 0;
  int ok = 0;
  if (!yg__start_command(staging, command, 2048) ||
      !yg__start_exe_from_command(command, path, YG__START_PATH_CAP) || !yg__start_drive_path(path))
    return 0;
  drive[0] = path[0];
  drive[1] = L':';
  drive[2] = L'\\';
  drive[3] = 0;
  if (!yg__start_marker_local(path, (unsigned)GetDriveTypeW(drive))) return 0;
  dll = yg__start_system_dll(L"version.dll");
  if (!dll) return 0;
  YG__START_PROC(get_size, dll, "GetFileVersionInfoSizeExW");
  YG__START_PROC(get_info, dll, "GetFileVersionInfoExW");
  YG__START_PROC(query, dll, "VerQueryValueW");
  if (get_size && get_info && query) size = get_size(YG__START_VER_NEUTRAL, path, &handle);
  if (size && size <= (1u << 20)) buf = HeapAlloc(GetProcessHeap(), 0, size);
  if (buf && get_info(YG__START_VER_NEUTRAL, path, 0, size, buf) &&
      query(buf, L"\\", (LPVOID *)&fixed, &len) && fixed && len >= sizeof *fixed &&
      fixed->dwSignature == 0xFEEF04BDu) {
    have[0] = HIWORD(fixed->dwFileVersionMS);
    have[1] = LOWORD(fixed->dwFileVersionMS);
    have[2] = HIWORD(fixed->dwFileVersionLS);
    ok = 1;
  }
  if (buf) HeapFree(GetProcessHeap(), 0, buf);
  FreeLibrary(dll);
  return ok;
}

static int yg__start_app_named(yg__start_app *app, int staging) {
  (void)staging; /* decided by yg__start_app_read for the flavour find was given */
  return app->named;
}

static int yg__start_app_version(yg__start_app *app, unsigned have[3]) {
  if (!app->have_ok) return 0;
  have[0] = app->have[0];
  have[1] = app->have[1];
  have[2] = app->have[2];
  return 1;
}

/* One name attribute (oid) of the certificate's subject, or of its issuer, into out. 0 when it cannot
   be read, does not fit, or has a NUL inside it. */
typedef DWORD (WINAPI *yg__start_name_fn)(PCCERT_CONTEXT, DWORD, DWORD, void *, LPWSTR, DWORD);
static int yg__start_cert_attr(yg__start_name_fn name, PCCERT_CONTEXT cert, int issuer, const char *oid,
                               wchar_t *out, DWORD cap) {
  DWORD n;
  memset(out, 0, cap * sizeof *out);
  n = name(cert, CERT_NAME_ATTR_TYPE, issuer ? CERT_NAME_ISSUER_FLAG : 0, (void *)oid, out, cap);
  return n > 1 && n < cap && wcslen(out) == n - 1;
}

/* The program's own Authenticode signature (WinVerifyTrust on the held file, no UI, no network, no
   revocation check) is valid, and its signing certificate passes yg__start_signer_ok: subject CN and
   O "Fluo Inc.", issuer O "Microsoft Corporation", issuer CN "Microsoft ID Verified CS ...". Any step
   that fails or cannot run, an issuer that cannot be read included, means not signed. */
static int yg__start_app_signed(yg__start_app *app) {
  typedef LONG (WINAPI *verify_fn)(HWND, GUID *, LPVOID);
  typedef CRYPT_PROVIDER_DATA *(WINAPI *data_fn)(HANDLE);
  typedef CRYPT_PROVIDER_SGNR *(WINAPI *signer_fn)(CRYPT_PROVIDER_DATA *, DWORD, BOOL, DWORD);
  /* crypt32 first, so wintrust's import of it is already the system copy. */
  HMODULE crypt = yg__start_system_dll(L"crypt32.dll");
  HMODULE trust = yg__start_system_dll(L"wintrust.dll");
  verify_fn verify = NULL;
  data_fn data = NULL;
  signer_fn signer = NULL;
  yg__start_name_fn name = NULL;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_FILE_INFO file;
  WINTRUST_DATA check;
  int ok = 0;
  if (trust && crypt) {
    YG__START_PROC(verify, trust, "WinVerifyTrust");
    YG__START_PROC(data, trust, "WTHelperProvDataFromStateData");
    YG__START_PROC(signer, trust, "WTHelperGetProvSignerFromChain");
    YG__START_PROC(name, crypt, "CertGetNameStringW");
  }
  if (verify && data && signer && name) {
    memset(&file, 0, sizeof file);
    file.cbStruct = sizeof file;
    file.pcwszFilePath = app->path;
    file.hFile = app->file; /* the held file; the start is bound to it (yg__start_image_is_held) */
    memset(&check, 0, sizeof check);
    check.cbStruct = sizeof check;
    check.dwUIChoice = WTD_UI_NONE;
    check.fdwRevocationChecks = WTD_REVOKE_NONE;
    check.dwUnionChoice = WTD_CHOICE_FILE;
    check.pFile = &file;
    check.dwStateAction = WTD_STATEACTION_VERIFY;
    /* Cache only: no network retrieval, so a game start never waits on one. */
    check.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL;
    if (verify((HWND)INVALID_HANDLE_VALUE, &action, &check) == ERROR_SUCCESS) {
      CRYPT_PROVIDER_DATA *provider = data(check.hWVTStateData);
      CRYPT_PROVIDER_SGNR *sig = provider ? signer(provider, 0, FALSE, 0) : NULL;
      /* pasCertChain[0] is the leaf that signed the file (not a countersigner). */
      PCCERT_CONTEXT leaf =
        sig && sig->csCertChain && sig->pasCertChain ? sig->pasCertChain[0].pCert : NULL;
      wchar_t cn[128], org[128], issuer_cn[128], issuer_org[128];
      if (leaf) {
        int c = yg__start_cert_attr(name, leaf, 0, szOID_COMMON_NAME, cn, 128);
        int o = yg__start_cert_attr(name, leaf, 0, szOID_ORGANIZATION_NAME, org, 128);
        int ic = yg__start_cert_attr(name, leaf, 1, szOID_COMMON_NAME, issuer_cn, 128);
        int io = yg__start_cert_attr(name, leaf, 1, szOID_ORGANIZATION_NAME, issuer_org, 128);
        ok = yg__start_signer_ok(c ? cn : NULL, o ? org : NULL, ic ? issuer_cn : NULL,
                                 io ? issuer_org : NULL);
      }
    }
    /* Always release the verification state, whatever the verdict. */
    check.dwStateAction = WTD_STATEACTION_CLOSE;
    verify((HWND)INVALID_HANDLE_VALUE, &action, &check);
  }
  if (trust) FreeLibrary(trust);
  if (crypt) FreeLibrary(crypt);
  return ok;
}

/* The new (still suspended) process runs the held file: the kernel's own name for its image equals
   the held file's final NT path (no drive letter or junction involved), and the image's path opened
   afresh is the held file by volume serial number and file index. */
static int yg__start_image_is_held(yg__start_app *app, HANDLE process) {
  typedef BOOL (WINAPI *image_fn)(HANDLE, DWORD, LPWSTR, PDWORD);
  wchar_t image[YG__START_PATH_CAP + 64], held[YG__START_PATH_CAP + 64];
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  image_fn query = NULL;
  DWORD n = YG__START_PATH_CAP + 64;
  if (!k) return 0;
  YG__START_PROC(query, k, "QueryFullProcessImageNameW");
  if (!query || !query(process, YG__START_IMAGE_NATIVE, image, &n) || !n ||
      n >= YG__START_PATH_CAP + 64 ||
      !yg__start_final_path(app->file, YG__START_FINAL_NT, held, YG__START_PATH_CAP + 64) ||
      !yg__start_wsame(image, held))
    return 0;
  n = YG__START_PATH_CAP + 64;
  if (!query(process, 0, image, &n) || !n || n >= YG__START_PATH_CAP + 64) return 0;
  return yg__start_same_file(app->file, image);
}

/* The started YouGame's environment: the one Explorer would give this user (CreateEnvironmentBlock
   on this process's token with bInherit FALSE: the system and user variables from the registry,
   nothing of the game's own), without the entries yg__start_env_drop names. A Unicode block, as
   CREATE_UNICODE_ENVIRONMENT expects; HeapFree it. NULL when it cannot be made, and then nothing is
   started: there is no hand-built fallback, because userenv.dll and CreateEnvironmentBlock ship with
   every Windows since 2000, so a normal PC never lands there. */
static wchar_t *yg__start_env_block(void) {
  typedef BOOL (WINAPI *create_fn)(LPVOID *, HANDLE, BOOL);
  typedef BOOL (WINAPI *destroy_fn)(LPVOID);
  HMODULE dll = yg__start_system_dll(L"userenv.dll");
  create_fn create = NULL;
  destroy_fn destroy = NULL;
  HANDLE tok = NULL;
  LPVOID clean = NULL;
  wchar_t *out = NULL;
  const wchar_t *env, *e;
  size_t total = 0, at = 0, len;
  if (!dll) return NULL;
  YG__START_PROC(create, dll, "CreateEnvironmentBlock");
  YG__START_PROC(destroy, dll, "DestroyEnvironmentBlock");
  /* The documented rights for CreateEnvironmentBlock's token. */
  if (create && destroy &&
      OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, &tok)) {
    if (!create(&clean, tok, FALSE)) clean = NULL;
    CloseHandle(tok);
  }
  if (clean) {
    env = (const wchar_t *)clean;
    for (e = env; *e; e += wcslen(e) + 1) total += wcslen(e) + 1;
    out = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (total + 2) * sizeof(wchar_t));
    if (out) {
      for (e = env; *e; e += len + 1) {
        len = wcslen(e);
        if (yg__start_env_drop(e)) continue;
        memcpy(out + at, e, len * sizeof(wchar_t));
        at += len;
        out[at++] = 0;
      }
      out[at++] = 0;
      if (at == 1) out[at++] = 0; /* an empty block is two NULs */
    }
    destroy(clean);
  }
  FreeLibrary(dll);
  return out;
}

/* Starts the checked program by its full path (no search), with one argument: the tray link. That is
   what the link type's own command does; the app then starts with no window. It starts suspended and
   runs only once yg__start_image_is_held confirms it is the held file; otherwise it is ended (and if
   ending it fails, it stays suspended and never runs) and counts as not started. The game's handles
   and environment are not inherited (yg__start_env_block), and it runs in the program's own folder.
   It leaves the game's job when the job allows that, so a launcher that ends the game's job does not
   end YouGame. */

static int yg__start_app_launch(yg__start_app *app, int staging) {
  const wchar_t *link = staging ? L"yougame-staging://tray" : L"yougame://tray";
  wchar_t line[YG__START_PATH_CAP + 40], dir[YG__START_PATH_CAP];
  const DWORD flags =
    DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED |
    CREATE_UNICODE_ENVIRONMENT;
  wchar_t *env;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  size_t n = wcslen(app->path), cut = n;
  int attempt, ok = 0;
  if (n + wcslen(link) + 4 >= sizeof line / sizeof *line) return 0;
  while (cut && app->path[cut - 1] != L'\\') --cut;
  if (cut < 3) return 0;
  if (cut > 3) --cut; /* "C:\Games\YouGame" without its last backslash; a root keeps "C:\" */
  memcpy(dir, app->path, cut * sizeof(wchar_t));
  dir[cut] = 0;
  env = yg__start_env_block();
  if (!env) return 0;
  for (attempt = 0; attempt < 2 && !ok; ++attempt) {
    /* Built afresh each time: CreateProcessW may write to the command line. */
    line[0] = L'"';
    memcpy(line + 1, app->path, n * sizeof(wchar_t));
    line[n + 1] = L'"';
    line[n + 2] = L' ';
    wcscpy(line + n + 3, link);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    ok = CreateProcessW(app->path, line, NULL, NULL, FALSE,
                        attempt == 0 ? flags | CREATE_BREAKAWAY_FROM_JOB : flags, env, dir, &si, &pi) != 0;
    if (!ok && GetLastError() != ERROR_ACCESS_DENIED) break; /* only a job that forbids leaving retries */
  }
  HeapFree(GetProcessHeap(), 0, env);
  if (!ok) return 0;
  if (!yg__start_image_is_held(app, pi.hProcess) || ResumeThread(pi.hThread) == (DWORD)-1) {
    /* End it and wait for the end. If ending it fails, its thread was never resumed (the image
       check failed, or the resume itself did), so it stays suspended and runs nothing. Either way it
       counts as not started and both handles are closed. */
    if (TerminateProcess(pi.hProcess, 1)) (void)WaitForSingleObject(pi.hProcess, 5000);
    ok = 0;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return ok;
}

/* The download page, through the system's handler for https. */
static int yg__start_shell_open(const char *url) {
  wchar_t what[256];
  size_t i;
  for (i = 0; url[i] && i + 1 < sizeof what / sizeof *what; ++i) what[i] = (wchar_t)url[i];
  if (url[i]) return 0;
  what[i] = 0;
  return (INT_PTR)ShellExecuteW(NULL, L"open", what, NULL, NULL, SW_SHOWNORMAL) > 32;
}

/* The small window: YouGame's own, two buttons. Returns 1 for Get YouGame, 0 otherwise. */
#define YG__START_ID_GET 101
#define YG__START_ID_NOT_NOW 102
static int yg__start_choice;
static int yg__start_done; /* the small window has been destroyed */

static LRESULT CALLBACK yg__start_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_COMMAND:
      if (LOWORD(wp) == YG__START_ID_GET) { yg__start_choice = 1; DestroyWindow(w); return 0; }
      if (LOWORD(wp) == YG__START_ID_NOT_NOW || LOWORD(wp) == IDCANCEL) { DestroyWindow(w); return 0; }
      break;
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_DESTROY: yg__start_done = 1; return 0; /* never a WM_QUIT: that would end the game */
    default: break;
  }
  return DefWindowProcW(w, m, wp, lp);
}

/* ASCII to wide, "\n" to "\r\n". Returns the length written, or 0 if it does not fit. */
static size_t yg__start_widen(const char *s, wchar_t *out, size_t at, size_t cap) {
  for (; *s; ++s) {
    if (*s == '\n') {
      if (at + 1 >= cap) return 0;
      out[at++] = L'\r';
    }
    if (at + 1 >= cap) return 0;
    out[at++] = (wchar_t)(unsigned char)*s;
  }
  out[at] = 0;
  return at;
}

static int yg__start_offer(int which) {
  static const wchar_t *cls = L"YouGameStartOffer";
  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSW wc;
  HWND w, text, get, later;
  HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  MSG msg;
  wchar_t wide[1024];
  size_t at;
  int width = 460, height = 236;
  int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
  at = yg__start_widen(yg__start_texts[which][0], wide, 0, 1024);
  if (at) at = yg__start_widen("\n\n", wide, at, 1024);
  if (at) at = yg__start_widen(yg__start_texts[which][1], wide, at, 1024);
  if (!at) return 0;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = yg__start_wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = cls;
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
  yg__start_choice = 0;
  yg__start_done = 0;
  w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, cls, L"YouGame",
                      WS_POPUP | WS_CAPTION | WS_SYSMENU, x < 0 ? 0 : x, y < 0 ? 0 : y, width, height,
                      NULL, NULL, inst, NULL);
  if (!w) return 0;
  text = CreateWindowExW(0, L"STATIC", wide, WS_CHILD | WS_VISIBLE, 20, 16, width - 46, 130, w,
                         NULL, inst, NULL);
  get = CreateWindowExW(0, L"BUTTON", YG__START_WIDE(YG__START_GET),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        width - 290, 154, 130, 32, w, (HMENU)(INT_PTR)YG__START_ID_GET, inst, NULL);
  later = CreateWindowExW(0, L"BUTTON", YG__START_WIDE(YG__START_LATER),
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                          width - 150, 154, 110, 32, w, (HMENU)(INT_PTR)YG__START_ID_NOT_NOW, inst, NULL);
  if (font) {
    if (text) SendMessageW(text, WM_SETFONT, (WPARAM)font, TRUE);
    if (get) SendMessageW(get, WM_SETFONT, (WPARAM)font, TRUE);
    if (later) SendMessageW(later, WM_SETFONT, (WPARAM)font, TRUE);
  }
  ShowWindow(w, SW_SHOWNORMAL);
  SetForegroundWindow(w);
  if (get) SetFocus(get);
  /* Its own short message loop, until the window is gone: Tab, Enter and Escape work like in a dialog.
     It also pumps the game's windows on this thread. A WM_QUIT it receives is the game's (the player
     closed the game meanwhile): the small window goes, the quit is posted again for the game's own
     loop, and the answer is Not now. */
  while (!yg__start_done) {
    BOOL got = GetMessageW(&msg, NULL, 0, 0);
    if (got == 0 || got == -1) {
      yg__start_choice = 0;
      if (IsWindow(w)) DestroyWindow(w);
      if (got == 0) PostQuitMessage((int)msg.wParam);
      break;
    }
    if (IsWindow(w) && IsDialogMessageW(w, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  UnregisterClassW(cls, inst);
  return yg__start_choice;
}

#elif defined(__APPLE__) || defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <crt_externs.h>
#include <pwd.h>
#include <sys/mount.h>
#include <time.h>
#define YG__START_ENVIRON (*_NSGetEnviron())
#else
#include <dirent.h>
extern char **environ;
#define YG__START_ENVIRON environ
#endif

/* A Mac has a menu bar; a Linux desktop need not have a tray. */
#ifdef __APPLE__
#define YG__START_TRAY_SYSTEM 1
#else
#define YG__START_TRAY_SYSTEM 0
#endif

static void yg__start_sleep(unsigned ms) { poll(NULL, 0, (int)ms); }

static int yg__start_quiet_env(void) {
  const char *v = getenv("YOUGAME_START_QUIET");
  return v && strcmp(v, "1") == 0;
}

/* A stale socket (ECONNREFUSED) is not running. EAGAIN means its listen queue is busy.
   Trust only a socket and a real 0700 parent folder owned by this user; never send any data.
   The connect is non-blocking and waits at most 100 ms. */
static int yg__start_socket(const char *path) {
  struct stat st;
  struct sockaddr_un addr;
  char folder[256];
  const char *slash = strrchr(path, '/');
  uid_t uid = getuid();
  int fd, rc, err, flags;
  size_t n;
  if (!slash || strlen(path) >= sizeof addr.sun_path) return 0;
  n = (size_t)(slash - path);
  if (!n || n >= sizeof folder) return 0;
  memcpy(folder, path, n);
  folder[n] = 0;
  if (lstat(folder, &st) || !S_ISDIR(st.st_mode) || st.st_uid != uid ||
      (st.st_mode & 0777) != 0700) return 0;
  if (lstat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != uid) return 0;
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); return 0; }
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path);
  rc = connect(fd, (const struct sockaddr *)&addr, sizeof addr);
  err = rc == 0 ? 0 : errno;
  if (err == EINPROGRESS) {
    struct pollfd pf;
    socklen_t len = sizeof err;
    pf.fd = fd;
    pf.events = POLLOUT;
    pf.revents = 0;
    if (poll(&pf, 1, 100) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len))
      err = EINPROGRESS;
  }
  close(fd);
  return err == 0 || err == EAGAIN || err == EWOULDBLOCK;
}

static int yg__start_running(int staging) {
  char path[256];
  const char *name = staging ? "yougame-staging" : "yougame";
  unsigned long uid = (unsigned long)getuid();
#ifdef __linux__
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime && runtime[0] == '/') {
    size_t n = strlen(runtime);
    while (n > 1 && runtime[n - 1] == '/') --n;
    if (n < 200) {
      snprintf(path, sizeof path, "%.*s/%s/sdk-v1.sock", (int)n, runtime, name);
      if (yg__start_socket(path)) return 1;
    }
  }
  snprintf(path, sizeof path, "/run/user/%lu/%s/sdk-v1.sock", uid, name);
  if (yg__start_socket(path)) return 1;
#endif
  snprintf(path, sizeof path, "/tmp/%s-%lu/sdk-v1.sock", name, uid);
  return yg__start_socket(path);
}

#ifdef __linux__
/* Linux has no "close everything else" spawn flag everywhere (closefrom_np needs glibc 2.34), so
   each descriptor the game has open, as /proc lists it, is closed in the child. glibc and musl
   ignore a close of one that is already gone. Without /proc the spawn is refused. */
static int yg__start_close_others(posix_spawn_file_actions_t *fa) {
  DIR *d = opendir("/proc/self/fd");
  struct dirent *e;
  int *fds = NULL, ok = 1, own;
  size_t n = 0, cap = 0, i;
  if (!d) return 0;
  own = dirfd(d);
  while (ok && (e = readdir(d)) != NULL) {
    char *end;
    long v = strtol(e->d_name, &end, 10);
    if (end == e->d_name || *end || v <= 2 || v > INT_MAX || v == own) continue;
    if (n == cap) {
      size_t more = cap ? cap * 2 : 64;
      int *grown = (int *)realloc(fds, more * sizeof *fds);
      if (!grown) { ok = 0; break; }
      fds = grown;
      cap = more;
    }
    fds[n++] = (int)v;
  }
  closedir(d);
  for (i = 0; ok && i < n; ++i)
    if (posix_spawn_file_actions_addclose(fa, fds[i])) ok = 0;
  free(fds);
  return ok;
}
#endif

/* Absolute executables and argv arrays only (posix_spawn, never a shell or a PATH search), with
   stdin, stdout and stderr on /dev/null. The child inherits none of the game's other descriptors and
   starts with no blocked signals and default handling for the common ones. */
static int yg__start_spawn(const char *prog, char *const argv[], pid_t *pid) {
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  sigset_t none, dflt;
  int rc, attr_ok = 0;
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
  char **filtered = yg__start_mac_environment(YG__START_ENVIRON);
  if (!filtered) return 0;
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; /* only the descriptors the actions name reach the child */
#endif
  sigemptyset(&none);
  sigemptyset(&dflt);
  sigaddset(&dflt, SIGPIPE);
  sigaddset(&dflt, SIGCHLD);
  sigaddset(&dflt, SIGHUP);
  sigaddset(&dflt, SIGINT);
  sigaddset(&dflt, SIGQUIT);
  sigaddset(&dflt, SIGTERM);
  rc = posix_spawnattr_init(&attr);
  if (!rc) {
    attr_ok = 1;
    rc = posix_spawnattr_setflags(&attr, flags);
    if (!rc) rc = posix_spawnattr_setsigmask(&attr, &none);
    if (!rc) rc = posix_spawnattr_setsigdefault(&attr, &dflt);
  }
  if (!rc) rc = posix_spawn_file_actions_init(&fa);
  if (!rc) {
    rc = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
#ifdef __linux__
    if (!rc && !yg__start_close_others(&fa)) rc = EMFILE;
#endif
    if (!rc) rc = posix_spawn(pid, prog, &fa, &attr, argv,
#ifdef __APPLE__
                             filtered
#else
                             YG__START_ENVIRON
#endif
                             );
    posix_spawn_file_actions_destroy(&fa);
  }
  if (attr_ok) posix_spawnattr_destroy(&attr);
#ifdef __APPLE__
  free(filtered);
#endif
  return rc == 0;
}

#ifdef __APPLE__
static int yg__start_mac_poll_child(void *child) {
  pid_t pid = *(pid_t *)child, rc;
  int status;
  rc = waitpid(pid, &status, WNOHANG);
  if (!rc || (rc < 0 && errno == EINTR)) return 0;
  return rc == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 1 : -1;
}

static void yg__start_mac_stop_child(void *child) {
  pid_t pid = *(pid_t *)child;
  int status;
  (void)kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

static int yg__start_wait(pid_t pid, unsigned deadline) {
  return yg__start_mac_wait(deadline, yg__start_mac_poll_child, yg__start_mac_stop_child, &pid);
}
#else
static int yg__start_wait(pid_t pid) {
  int status;
  pid_t rc;
  do { rc = waitpid(pid, &status, 0); } while (rc < 0 && errno == EINTR);
  return rc == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

#ifdef __linux__
/* xdg-open may live until a browser closes; reap it without holding up the game. */
static void *yg__start_reap(void *arg) {
  pid_t pid = *(pid_t *)arg;
  free(arg);
  (void)yg__start_wait(pid);
  return NULL;
}
#endif

/* The download page, through the system's opener for https (never YouGame's own link type). */
static int yg__start_shell_open(const char *url) {
#ifdef __APPLE__
  const char *prog = "/usr/bin/open";
#else
  const char *prog = access("/usr/bin/xdg-open", X_OK) == 0 ? "/usr/bin/xdg-open" : "/bin/xdg-open";
#endif
  char *argv[3];
  pid_t pid;
  argv[0] = (char *)prog;
  argv[1] = (char *)url;
  argv[2] = NULL;
  if (!yg__start_spawn(prog, argv, &pid)) return 0;
#ifdef __APPLE__
  return yg__start_wait(pid, YG__START_OPEN_MS) == 1;
#else
  {
    pthread_t thread;
    pid_t *job = (pid_t *)malloc(sizeof *job);
    if (job) {
      *job = pid;
      if (pthread_create(&thread, NULL, yg__start_reap, job) == 0) {
        pthread_detach(thread);
        return 1; /* dispatched, as with ShellExecute */
      }
      free(job);
    }
    return yg__start_wait(pid);
  }
#endif
}

#ifdef __linux__
/* Whether one of the desktop's MIME association files names a .desktop handler for `key`
   ("x-scheme-handler/yougame"), in a section that adds one (never "[Removed Associations]"). */
static int yg__start_list_names(const char *file, const char *key) {
  char line[1024];
  size_t k = strlen(key);
  int fd = open(file, O_RDONLY | O_CLOEXEC), take = 0, found = 0;
  FILE *f;
  if (fd < 0) return 0;
  f = fdopen(fd, "r");
  if (!f) { close(fd); return 0; }
  while (!found && fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    if (n && line[n - 1] != '\n' && !feof(f)) { /* too long to be ours: skip the rest of it */
      int c;
      while ((c = fgetc(f)) != EOF && c != '\n') {}
      continue;
    }
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    if (line[0] == '[') {
      take = !strcmp(line, "[Default Applications]") || !strcmp(line, "[Added Associations]") ||
             !strcmp(line, "[MIME Cache]");
    } else if (take && !strncmp(line, key, k) && line[k] == '=') {
      const char *v = line + k + 1, *semi = strchr(v, ';');
      size_t len = semi ? (size_t)(semi - v) : strlen(v);
      found = len > 8 && !memcmp(v + len - 8, ".desktop", 8) && !memchr(v, '/', len);
    }
  }
  fclose(f);
  return found;
}

/* An absolute XDG folder from the environment, else $HOME + fallback. Empty when neither works. */
static void yg__start_xdg(const char *var, const char *fallback, char *out, size_t cap) {
  const char *v = getenv(var), *home = getenv("HOME");
  out[0] = 0;
  if (v && v[0] == '/' && strlen(v) < cap) strcpy(out, v);
  else if (home && home[0] == '/' && (size_t)snprintf(out, cap, "%s/%s", home, fallback) >= cap) out[0] = 0;
}
#endif

#ifdef __APPLE__
/* LaunchServices finds a bundle by flavour, never by a game-supplied path or URL. */
typedef struct yg__start_app {
  CFURLRef url;
  CFBundleRef bundle;
  char path[PATH_MAX];
  int staging;
} yg__start_app;

static CFStringRef yg__start_bundle_id(int staging) {
  return staging ? CFSTR("co.yougame.desktop.staging") : CFSTR("co.yougame.desktop");
}

/* 0 for a non-link, 1 for an allowed link's target, -1 for any refusal or read failure. */
static int yg__start_mac_link_target(const char *path, char *target, size_t cap) {
  struct stat st;
  ssize_t len;
  if (lstat(path, &st)) return -1;
  if (!S_ISLNK(st.st_mode)) return 0;
  if (!yg__start_mac_owner_ok((unsigned long)st.st_uid, (unsigned long)getuid())) return -1;
  len = readlink(path, target, cap - 1);
  if (len < 0 || (size_t)len >= cap - 1) return -1;
  target[len] = 0;
  return 1;
}

static int yg__start_mac_resolve(const char *path, const char *home, char resolved[PATH_MAX]) {
  struct stat st;
  struct statfs volume;
  char parent[PATH_MAX], *slash;
  /* Resolve once. All metadata reads, codesign and open then use this exact resolved string.
     Full running-image binding cannot happen before execution with open/LaunchServices. An
     attacker who can write the bundle can already replace the app the user starts by hand;
     Windows has the same model (its hook additionally checks a suspended image). */
  if (!yg__start_mac_candidate(path, home) || !yg__start_mac_links_ok(path, yg__start_mac_link_target) ||
      !realpath(path, resolved) || !yg__start_mac_candidate(resolved, home) ||
      lstat(resolved, &st) ||
      !yg__start_mac_bundle_ok((unsigned long)st.st_uid, (unsigned long)getuid(),
                               (unsigned)st.st_mode, S_ISDIR(st.st_mode))) return 0;
  /* desktop's installation.installed rejects read-only volumes and unwritable parents too. */
  if (statfs(resolved, &volume) || (volume.f_flags & MNT_RDONLY)) return 0;
  strcpy(parent, resolved);
  slash = strrchr(parent, '/');
  if (!slash) return 0;
  *slash = 0;
  return access(parent, W_OK) == 0;
}

static int yg__start_app_find(int staging, yg__start_app *app) {
  CFArrayRef urls = LSCopyApplicationURLsForBundleIdentifier(yg__start_bundle_id(staging), NULL);
  struct passwd *user = getpwuid(getuid());
  const char *home = user ? user->pw_dir : NULL;
  int best = 0;
  memset(app, 0, sizeof *app);
  app->staging = staging;
  if (urls) {
    CFIndex i;
    for (i = 0; i < CFArrayGetCount(urls); ++i) {
      char path[PATH_MAX], resolved[PATH_MAX];
      CFURLRef url = (CFURLRef)CFArrayGetValueAtIndex(urls, i), resolved_url;
      CFBundleRef bundle;
      int rank;
      if (!CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path) ||
          !yg__start_mac_resolve(path, home, resolved)) continue;
      rank = yg__start_mac_prefer(resolved, home, best);
      if (!rank) continue;
      resolved_url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)resolved,
                                                            (CFIndex)strlen(resolved), true);
      if (!resolved_url) continue;
      bundle = CFBundleCreate(NULL, resolved_url);
      if (!bundle) { CFRelease(resolved_url); continue; }
      if (app->bundle) CFRelease(app->bundle);
      if (app->url) CFRelease(app->url);
      app->url = resolved_url;
      app->bundle = bundle;
      strcpy(app->path, resolved);
      best = rank;
    }
    CFRelease(urls);
  }
  if (app->bundle) return 1;
  if (app->url) CFRelease(app->url);
  app->url = NULL;
  return 0;
}

static void yg__start_app_close(yg__start_app *app) {
  CFRelease(app->bundle);
  CFRelease(app->url);
}

static int yg__start_app_named(yg__start_app *app, int staging) {
  CFStringRef ident = CFBundleGetIdentifier(app->bundle);
  CFTypeRef kind = CFBundleGetValueForInfoDictionaryKey(app->bundle, CFSTR("CFBundlePackageType"));
  return ident && CFEqual(ident, yg__start_bundle_id(staging)) && kind && CFEqual(kind, CFSTR("APPL"));
}

static int yg__start_app_version(yg__start_app *app, unsigned have[3]) {
  char text[32];
  CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(app->bundle, CFSTR("CFBundleShortVersionString"));
  return v && CFGetTypeID(v) == CFStringGetTypeID() &&
    CFStringGetCString((CFStringRef)v, text, sizeof text, kCFStringEncodingASCII) &&
    yg__start_version_parse(text, have);
}

/* Offline code-signature verification, including sealed resources (nested code is sealed by them; no
   --deep, which re-verifies every helper and doubles the time a game waits). Pin the same
   Developer ID team as desktop/src/orca-policy.ts, and this flavour's signed bundle identifier.
   No Gatekeeper assessment, shell, output, or certificate download. */
static int yg__start_app_signed(yg__start_app *app) {
  char requirement[512];
  char *argv[8];
  pid_t pid;
  if (!yg__start_mac_requirement(app->staging, requirement, sizeof requirement)) return 0;
  yg__start_mac_verify_argv(app->path, requirement, argv);
  if (!yg__start_spawn(argv[0], argv, &pid)) return 0;
  return yg__start_wait(pid, YG__START_CODESIGN_MS);
}

/* -g leaves the game in front, -j hides the app, --args reaches Electron's argv. Use the
   verified bundle path (-a), rather than resolving its identifier again after verification. */
static int yg__start_app_launch(yg__start_app *app, int staging) {
  char *argv[8];
  pid_t pid;
  yg__start_mac_open_argv(app->path, argv);
  if (!yg__start_spawn(argv[0], argv, &pid)) return 0;
  return yg__start_mac_opened(yg__start_wait(pid, YG__START_OPEN_MS), staging);
}

static unsigned long yg__start_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
  return ((unsigned long)now.tv_sec * 1000UL + (unsigned long)now.tv_nsec / 1000000UL) & 0xFFFFFFFFUL;
}
#endif

/* Installed = a Mac bundle of this flavour, or an app registered for YouGame's link type. Only looked up: nothing is opened or
   started (Linux reads the association files itself rather than running xdg-mime). */
static int yg__start_installed(int staging) {
#ifdef __APPLE__
  yg__start_app app;
  if (!yg__start_app_find(staging, &app)) return 0;
  yg__start_app_close(&app);
  return 1;
#else
  /* The files xdg-mime and the desktops read for a user's handler (the app's own xdg-settings call
     writes the first), then the caches a packaged install's desktop entry lands in. */
  const char *key = staging ? "x-scheme-handler/yougame-staging" : "x-scheme-handler/yougame";
  char config[256], data[256], path[320];
  yg__start_xdg("XDG_CONFIG_HOME", ".config", config, sizeof config);
  yg__start_xdg("XDG_DATA_HOME", ".local/share", data, sizeof data);
  if (config[0]) {
    snprintf(path, sizeof path, "%s/mimeapps.list", config);
    if (yg__start_list_names(path, key)) return 1;
  }
  if (data[0]) {
    snprintf(path, sizeof path, "%s/applications/mimeapps.list", data);
    if (yg__start_list_names(path, key)) return 1;
    snprintf(path, sizeof path, "%s/applications/mimeinfo.cache", data);
    if (yg__start_list_names(path, key)) return 1;
  }
  return yg__start_list_names("/usr/local/share/applications/mimeinfo.cache", key) ||
         yg__start_list_names("/usr/share/applications/mimeinfo.cache", key);
#endif
}

static int yg__start_offer(int which) {
#ifdef __APPLE__
  CFStringRef title = CFStringCreateWithCString(NULL, yg__start_texts[which][0], kCFStringEncodingASCII);
  CFStringRef body = CFStringCreateWithCString(NULL, yg__start_texts[which][1], kCFStringEncodingASCII);
  CFOptionFlags answer = kCFUserNotificationCancelResponse;
  SInt32 rc = -1;
  if (title && body)
    rc = CFUserNotificationDisplayAlert(0, kCFUserNotificationNoteAlertLevel, NULL, NULL, NULL,
      title, body, CFSTR(YG__START_GET), CFSTR(YG__START_LATER), NULL, &answer);
  if (title) CFRelease(title);
  if (body) CFRelease(body);
  return rc == 0 && (answer & 3) == kCFUserNotificationDefaultResponse;
#else
  char *argv[10];
  char message[1024];
  pid_t pid;
  const char *prog;
  if ((size_t)snprintf(message, sizeof message, "%s\n\n%s", yg__start_texts[which][0],
                       yg__start_texts[which][1]) >= sizeof message) return 0;
  if (access("/usr/bin/zenity", X_OK) == 0) {
    prog = "/usr/bin/zenity";
    argv[0] = (char *)prog;
    argv[1] = (char *)"--question";
    argv[2] = (char *)"--title=YouGame";
    argv[3] = (char *)"--ok-label=" YG__START_GET;
    argv[4] = (char *)"--cancel-label=" YG__START_LATER;
    argv[5] = (char *)"--text";
    argv[6] = message;
    argv[7] = NULL;
  } else if (access("/usr/bin/kdialog", X_OK) == 0) {
    prog = "/usr/bin/kdialog";
    argv[0] = (char *)prog;
    argv[1] = (char *)"--title";
    argv[2] = (char *)"YouGame";
    argv[3] = (char *)"--yes-label";
    argv[4] = (char *)YG__START_GET;
    argv[5] = (char *)"--no-label";
    argv[6] = (char *)YG__START_LATER;
    argv[7] = (char *)"--yesno";
    argv[8] = message;
    argv[9] = NULL;
  } else return 0;
  return yg__start_spawn(prog, argv, &pid) && yg__start_wait(pid);
#endif
}
#endif /* platform implementation */

#if defined(_WIN32) || defined(__APPLE__) || defined(YOUGAME_START_TESTING)
/* Windows and macOS: find the app, check its identity, version and signature, then launch it
   quietly. Windows additionally holds and binds the executable throughout these checks. */
static int yg__start_tray(int staging, const unsigned want[3], unsigned have[3]) {
  yg__start_app app;
  int r, signature;
  if (!yg__start_app_find(staging, &app)) return YG__START_TRAY_REFUSED;
  if (!yg__start_app_named(&app, staging))
    r = YG__START_TRAY_REFUSED;
  else if (!yg__start_app_version(&app, have) || !yg__start_version_at_least(have, want))
    r = YG__START_TRAY_OLD;
  else if ((signature = yg__start_app_signed(&app)) <= 0)
    r = signature < 0 ? YG__START_TRAY_FAILED : YG__START_TRAY_REFUSED;
  else
    r = yg__start_app_launch(&app, staging) ? YG__START_TRAY_STARTED : YG__START_TRAY_FAILED;
  yg__start_app_close(&app);
  return r;
}

#endif

#if defined(_WIN32) || defined(YOUGAME_START_TESTING)
/* The marker's `to` is already installed: the registered program's file version
   (yg__start_installed_version, which never blocks an installer) reaches `to`
   (yg__start_version_reached). A program that cannot be read (an installer may be replacing it
   right now) is not done. */
static int yg__start_update_done(int staging, const char *to) {
  unsigned have[3];
  return yg__start_installed_version(staging, have) && yg__start_version_reached(have, to);
}

/* YG_START_UPDATING, with the marker's version copied to the game's update_to (cut to fit). */
static yg_start_result yg__start_updating(const yg_start_options *o, const char *to) {
  if (o->update_to && o->update_to_cap) {
    size_t n = strlen(to);
    if (n >= o->update_to_cap) n = o->update_to_cap - 1;
    memcpy(o->update_to, to, n);
    o->update_to[n] = 0;
  }
  return YG_START_UPDATING;
}
#endif

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__) || defined(YOUGAME_START_TESTING)
yg_start_result yg_start(const yg_start_options *o) {
  int staging, quiet, installed, text;
#if defined(_WIN32) || defined(YOUGAME_START_TESTING)
  char to[YG__START_TO_CAP];
  yg__start_mark mark;
#endif
  /* Cleared at entry only for a caller that set the magic: a struct that was never zeroed never
     has its update_to written before a real YG_START_UPDATING. */
  if (o && o->update_to_magic == YG_START_UPDATE_TO_MAGIC && o->update_to && o->update_to_cap) o->update_to[0] = 0;
  if (!o || !yg__start_slug_ok(o->game)) return YG_START_UNAVAILABLE;
  staging = o->staging ? 1 : 0;
  quiet = o->quiet || yg__start_quiet_env();
  if (yg__start_running(staging)) return YG_START_RUNNING;
  installed = yg__start_installed(staging);
#if defined(_WIN32) || defined(YOUGAME_START_TESTING)
  /* YouGame quit to install an update: a fact, so quiet runs report it too, and nothing is started
     or shown. Taken whether or not the link type is registered right now, since an installer may
     drop and rewrite it while it runs, unless the installed YouGame already has the marker's version
     and the marker file is at least 20 s old (a finished update, including installer post-steps). Windows only (YG__START_UPDATE_SYSTEM): macOS and
     Linux have no marker, so it is not even looked for there. */
  mark.looked = 0;
  if (YG__START_UPDATE_SYSTEM && yg__start_marker(staging, &mark, to) &&
      (mark.age_ms < 20000 || !yg__start_update_done(staging, to)))
    return yg__start_updating(o, to);
#endif
  if (quiet) return installed ? YG_START_INSTALLED : YG_START_MISSING;
  text = installed ? YG__START_TEXT_NOT_RUNNING : YG__START_TEXT_MISSING;
#if defined(_WIN32) || defined(__APPLE__) || defined(YOUGAME_START_TESTING)
  if (installed && YG__START_TRAY_SYSTEM && !o->no_open) {
    unsigned want[3], have[3] = {0, 0, 0};
    if (yg__start_version_parse(o->tray_from, want)) {
      int tray = yg__start_tray(staging, want, have);
      if (tray == YG__START_TRAY_OLD) text = YG__START_TEXT_UPDATE;
      else if (tray == YG__START_TRAY_FAILED) text = YG__START_TEXT_DIDNT_OPEN;
      else if (tray == YG__START_TRAY_STARTED) {
        /* Polled every 500 ms: the channel first, then (Windows only) the update marker. The wait is bounded by
           the clock as well as by the sleeps it adds up, so slow checks never stretch it. */
        unsigned wait = o->open_wait_ms > 60000 ? 60000 : o->open_wait_ms, waited = 0;
        unsigned long start, elapsed;
        if (wait == 0) return YG_START_OPENED;
        start = yg__start_ms();
        while (waited < wait) {
          unsigned step = wait - waited < 500 ? wait - waited : 500;
          yg__start_sleep(step);
          waited += step;
          elapsed = (yg__start_ms() - start) & 0xFFFFFFFFUL;
          if (elapsed > waited) waited = elapsed < wait ? (unsigned)elapsed : wait;
          if (yg__start_running(staging)) return YG_START_OPENED;
#if defined(_WIN32) || defined(YOUGAME_START_TESTING)
          /* An older YouGame that finds an update quits to install it: say so, not "didn't open".
             The version it was started at (have) tells a leftover marker from a new one. */
          if (YG__START_UPDATE_SYSTEM && yg__start_marker(staging, &mark, to) &&
              (mark.age_ms < 20000 || !yg__start_version_reached(have, to)))
            return yg__start_updating(o, to);
#endif
        }
        text = YG__START_TEXT_DIDNT_OPEN;
      }
    }
  }
#else
  /* Linux: no tray start, so these go unused there. */
  (void)yg__start_version_parse;
  (void)yg__start_version_at_least;
  (void)yg__start_sleep;
#endif
  if (!yg__start_offer(text)) return YG_START_DECLINED;
  if (!yg__start_shell_open(staging ? "https://staging.yougame.co/desktop" : "https://yougame.co/desktop"))
    return YG_START_UNAVAILABLE;
  return YG_START_OFFERED;
}
#else

yg_start_result yg_start(const yg_start_options *options) {
  if (options && options->update_to_magic == YG_START_UPDATE_TO_MAGIC && options->update_to && options->update_to_cap)
    options->update_to[0] = 0;
  (void)yg__start_slug_ok;
  (void)yg__start_version_parse;
  (void)yg__start_version_at_least;
  (void)yg__start_texts;
  return YG_START_UNAVAILABLE;
}
#endif

#endif /* YOUGAME_START_IMPLEMENTATION */

#endif /* YOUGAME_START_H */
