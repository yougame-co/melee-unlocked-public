/*
 * yougame_start.h - the YouGame start hook. One call when your game starts; the game draws nothing.
 *
 *   #define YOUGAME_START_IMPLEMENTATION      (in exactly one .c or .cpp file)
 *   #include "yougame_start.h"
 *
 *   yg_start_options o = {0};
 *   o.game = "your-game-slug";               the game's address on YouGame: yougame.co/g/<slug>
 *   o.tray_from = "0.13.1407";               Windows: the first YouGame version with the tray start
 *   yg_start(&o);                            then carry on starting the game, whatever it returns
 *
 * It never brings up YouGame's client window or a game page, and never brings a running YouGame
 * forward. What it does, in this order:
 *   1. YouGame is running for this user: nothing is shown. YG_START_RUNNING.
 *   2. YouGame is installed but not running.
 *      Windows, when `no_open` is 0, `tray_from` is set, the registered program is the signed
 *      YouGame program at its install location (%LOCALAPPDATA%\Programs\<folder>\<exe>, its version
 *      texts naming the YouGame app, signed by Fluo Inc. through Microsoft's identity verified CA),
 *      its file version is at least `tray_from`, and the game does not run as administrator: it
 *      starts that same file (checked again once started, before it runs) with the tray link
 *      yougame[-staging]://tray, so YouGame starts with no window, only its tray icon.
 *      YG_START_OPENED. With `open_wait_ms` set and YouGame not up in that time, the small window
 *      below says "YouGame didn't open".
 *      Otherwise nothing is started and the small window says why: "YouGame needs an update" when
 *      the installed version is older than `tray_from` (or its file version cannot be read), else
 *      "YouGame isn't running" (also when its version texts cannot be read). macOS and Linux
 *      always take this path: the app has no tray start there yet.
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
 * the game runs as administrator); everywhere the system's opener for one fixed https address (the
 * download page); on Linux also zenity or kdialog (the small window). All by absolute path. On macOS and Linux a game
 * that ignores SIGCHLD or reaps every child itself makes those answers read as failures.
 *
 * Runs that must never show a window or start anything (tests, headless runs): set `quiet`, or the
 * environment variable YOUGAME_START_QUIET=1. Then it only looks (a pipe or socket, the registry or
 * the system's link handler) and reports what it found; it starts no program on any system.
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

#ifdef __cplusplus
extern "C" {
#endif

typedef enum yg_start_result {
  YG_START_RUNNING = 0,     /* YouGame is running: nothing was shown */
  YG_START_OPENED = 1,      /* Windows: YouGame was installed and has been started in the tray */
  YG_START_OFFERED = 2,     /* the small window was shown and the player pressed Get YouGame;
                               the download page is open */
  YG_START_DECLINED = 3,    /* the small window was shown and the player pressed Not now (or closed
                               it), or no window could be shown */
  YG_START_UNAVAILABLE = 4, /* nothing was done: unsupported system, bad options, or an error */
  YG_START_INSTALLED = 5,   /* quiet runs only: installed but not running; nothing was started */
  YG_START_MISSING = 6      /* quiet runs only: not installed, and nothing was shown */
} yg_start_result;

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

/* What a tray start attempt came to (Windows; the test seam drives it everywhere). */
#define YG__START_TRAY_STARTED 0 /* the verified program was started with the tray link */
#define YG__START_TRAY_OLD 1     /* its version is older than tray_from, or could not be read */
#define YG__START_TRAY_REFUSED 2 /* no usable program, not the YouGame app, or not signed */
#define YG__START_TRAY_FAILED 3  /* verified, but starting it failed */

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
#endif

#ifdef YOUGAME_START_TESTING
/* The test translation unit defines these; no OS calls, apps or windows in seam builds. */
typedef struct yg__start_app { int id; } yg__start_app;
static int yg__start_test_windows; /* 1 = take Windows' tray path, 0 = macOS/Linux */
#define YG__START_TRAY_SYSTEM yg__start_test_windows
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
#define YG__START_ENVIRON (*_NSGetEnviron())
#else
#include <dirent.h>
extern char **environ;
#define YG__START_ENVIRON environ
#endif

/* The app has no tray start on macOS or Linux yet: an installed YouGame is never started here. */
#define YG__START_TRAY_SYSTEM 0

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
    if (!rc) rc = posix_spawn(pid, prog, &fa, &attr, argv, YG__START_ENVIRON);
    posix_spawn_file_actions_destroy(&fa);
  }
  if (attr_ok) posix_spawnattr_destroy(&attr);
  return rc == 0;
}

static int yg__start_wait(pid_t pid) {
  int status;
  pid_t rc;
  do { rc = waitpid(pid, &status, 0); } while (rc < 0 && errno == EINTR);
  return rc == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

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
  return yg__start_wait(pid);
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

/* Installed = an app is registered for YouGame's link type. Only looked up: nothing is opened or
   started (Linux reads the association files itself rather than running xdg-mime). */
static int yg__start_installed(int staging) {
#ifdef __APPLE__
  CFURLRef url = CFURLCreateWithString(NULL, staging ? CFSTR("yougame-staging://g/x") : CFSTR("yougame://g/x"), NULL);
  CFURLRef app = NULL;
  int found;
  if (!url) return 0;
  app = LSCopyDefaultApplicationURLForURL(url, kLSRolesAll, NULL);
  CFRelease(url);
  found = app != NULL;
  if (app) CFRelease(app);
  return found;
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

#if defined(_WIN32) || defined(YOUGAME_START_TESTING)
/* Windows' tray start, in the order that keeps one file from check to start: find and hold the
   program, check it is the YouGame app (name and version texts), its version, its signature, then
   start it (bound to the held file). */
static int yg__start_tray(int staging, const unsigned want[3]) {
  yg__start_app app;
  unsigned have[3];
  int r;
  if (!yg__start_app_find(staging, &app)) return YG__START_TRAY_REFUSED;
  if (!yg__start_app_named(&app, staging))
    r = YG__START_TRAY_REFUSED;
  else if (!yg__start_app_version(&app, have) || !yg__start_version_at_least(have, want))
    r = YG__START_TRAY_OLD;
  else if (!yg__start_app_signed(&app))
    r = YG__START_TRAY_REFUSED;
  else
    r = yg__start_app_launch(&app, staging) ? YG__START_TRAY_STARTED : YG__START_TRAY_FAILED;
  yg__start_app_close(&app);
  return r;
}
#endif

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__) || defined(YOUGAME_START_TESTING)
yg_start_result yg_start(const yg_start_options *o) {
  int staging, quiet, installed, text;
  if (!o || !yg__start_slug_ok(o->game)) return YG_START_UNAVAILABLE;
  staging = o->staging ? 1 : 0;
  quiet = o->quiet || yg__start_quiet_env();
  if (yg__start_running(staging)) return YG_START_RUNNING;
  installed = yg__start_installed(staging);
  if (quiet) return installed ? YG_START_INSTALLED : YG_START_MISSING;
  text = installed ? YG__START_TEXT_NOT_RUNNING : YG__START_TEXT_MISSING;
#if defined(_WIN32) || defined(YOUGAME_START_TESTING)
  if (installed && YG__START_TRAY_SYSTEM && !o->no_open) {
    unsigned want[3];
    if (yg__start_version_parse(o->tray_from, want)) {
      int tray = yg__start_tray(staging, want);
      if (tray == YG__START_TRAY_OLD) text = YG__START_TEXT_UPDATE;
      else if (tray == YG__START_TRAY_FAILED) text = YG__START_TEXT_DIDNT_OPEN;
      else if (tray == YG__START_TRAY_STARTED) {
        unsigned wait = o->open_wait_ms > 60000 ? 60000 : o->open_wait_ms, waited = 0;
        if (wait == 0) return YG_START_OPENED;
        while (waited < wait) {
          unsigned step = wait - waited < 500 ? wait - waited : 500;
          yg__start_sleep(step);
          waited += step;
          if (yg__start_running(staging)) return YG_START_OPENED;
        }
        text = YG__START_TEXT_DIDNT_OPEN;
      }
    }
  }
#else
  /* macOS and Linux: no tray start, so these go unused there. */
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
  (void)options;
  (void)yg__start_slug_ok;
  (void)yg__start_version_parse;
  (void)yg__start_version_at_least;
  (void)yg__start_texts;
  return YG_START_UNAVAILABLE;
}
#endif

#endif /* YOUGAME_START_IMPLEMENTATION */

#endif /* YOUGAME_START_H */
