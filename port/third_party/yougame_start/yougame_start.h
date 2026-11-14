/*
 * yougame_start.h - the YouGame start hook. One call when your game starts; the game draws nothing.
 *
 *   #define YOUGAME_START_IMPLEMENTATION      (in exactly one .c or .cpp file)
 *   #include "yougame_start.h"
 *
 *   yg_start_options o = {0};
 *   o.game = "your-game-slug";               the game's address on YouGame: yougame.co/g/<slug>
 *   yg_start(&o);                            then carry on starting the game, whatever it returns
 *
 * What it does, in this order:
 *   1. YouGame is running for this user: nothing is shown. YG_START_RUNNING.
 *   2. YouGame is installed but not open: it opens YouGame on the game's page. YG_START_OPENED.
 *   3. YouGame is not on this computer (or was asked to open and did not): its own small window offers "Get YouGame" (opens YouGame's
 *      download page in the browser) or "Not now". YG_START_OFFERED or YG_START_DECLINED.
 * It never blocks play: "Not now" and every failure just return, and the game starts as usual.
 *
 * It only looks and opens; it does not talk to YouGame. Online calls are yougame.h's job (or the
 * game's own client): they find YouGame once it is open, with no restart of the game.
 *
 * Windows only in this version. On other systems yg_start returns YG_START_UNAVAILABLE and shows
 * nothing. It downloads nothing and starts no program itself: it opens YouGame's own link type and
 * one fixed https address, through the system.
 *
 * Runs that must never show a window or open anything (tests, headless runs): set `quiet`, or the
 * environment variable YOUGAME_START_QUIET=1. Then it only reports what it found.
 *
 * MIT licence (see LICENSE beside this file).
 */
#ifndef YOUGAME_START_H
#define YOUGAME_START_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum yg_start_result {
  YG_START_RUNNING = 0,     /* YouGame is open: nothing was shown */
  YG_START_OPENED = 1,      /* YouGame was installed and has been asked to open */
  YG_START_OFFERED = 2,     /* not installed: the player pressed Get YouGame; the download page is open */
  YG_START_DECLINED = 3,    /* not installed: the player pressed Not now (or closed the window) */
  YG_START_UNAVAILABLE = 4, /* nothing was done: unsupported system, bad options, or an error */
  YG_START_INSTALLED = 5,   /* quiet runs only: installed but not open, and nothing was opened */
  YG_START_MISSING = 6      /* quiet runs only: not installed, and nothing was shown */
} yg_start_result;

typedef struct yg_start_options {
  const char *game;      /* required: the game's slug, a-z 0-9 and '-', at most 80 characters */
  int staging;           /* 1 = YouGame's staging app and site (test builds); 0 = YouGame */
  int quiet;             /* 1 = never show a window and never open anything; only report */
  unsigned open_wait_ms; /* after opening an installed YouGame, wait up to this long for it to be
                            running before returning (0 = return at once, the usual choice; at most
                            60000). If it is not running by then, the Get YouGame window is shown. */
  int no_open;           /* 1 = never open an installed YouGame: show the offer window instead */
} yg_start_options;

/* Call once, on the thread that starts the game, before or after the game's own window exists. */
yg_start_result yg_start(const yg_start_options *options);

#ifdef __cplusplus
}
#endif

#ifdef YOUGAME_START_IMPLEMENTATION

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <wchar.h>

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

static int yg__start_quiet_env(void) {
  wchar_t v[8];
  DWORD n = GetEnvironmentVariableW(L"YOUGAME_START_QUIET", v, 8);
  return n == 1 && v[0] == L'1';
}

/* The app's local channel for this Windows user: \\.\pipe\yougame[-staging]-sdk-v1-<user SID>.
   Only its presence is checked; nothing connects to it here. */
static int yg__start_running(int staging) {
  HANDLE tok = NULL;
  BYTE buf[256];
  DWORD len = 0;
  LPWSTR sid = NULL;
  wchar_t name[400];
  int up = 0;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
  if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &len) &&
      ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid) && sid) {
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

/* Installed = YouGame's link type has an open command for this user (the app's installer writes it). */
static int yg__start_key_has_command(HKEY root, const wchar_t *path) {
  HKEY k;
  DWORD type = 0, size = 0;
  int ok = 0;
  if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
  if (RegQueryValueExW(k, NULL, NULL, &type, NULL, &size) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t)) ok = 1;
  RegCloseKey(k);
  return ok;
}

static int yg__start_installed(int staging) {
  return yg__start_key_has_command(HKEY_CURRENT_USER, staging
           ? L"Software\\Classes\\yougame-staging\\shell\\open\\command"
           : L"Software\\Classes\\yougame\\shell\\open\\command") ||
         yg__start_key_has_command(HKEY_CLASSES_ROOT, staging
           ? L"yougame-staging\\shell\\open\\command"
           : L"yougame\\shell\\open\\command");
}

static int yg__start_shell_open(const wchar_t *what) {
  return (INT_PTR)ShellExecuteW(NULL, L"open", what, NULL, NULL, SW_SHOWNORMAL) > 32;
}

/* yougame[-staging]://g/<slug>: the one link shape the app accepts; it opens on the game's page. */
static int yg__start_open_app(const char *game, int staging) {
  wchar_t url[160];
  size_t i, n;
  wcscpy(url, staging ? L"yougame-staging://g/" : L"yougame://g/");
  n = wcslen(url);
  for (i = 0; game[i]; i++) url[n + i] = (wchar_t)game[i]; /* the slug is ASCII (checked) */
  url[n + i] = 0;
  return yg__start_shell_open(url);
}

/* The offer: YouGame's own small window, two buttons. Returns 1 for Get YouGame, 0 otherwise. */
#define YG__START_ID_GET 101
#define YG__START_ID_NOT_NOW 102
static int yg__start_choice;

static LRESULT CALLBACK yg__start_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_COMMAND:
      if (LOWORD(wp) == YG__START_ID_GET) { yg__start_choice = 1; DestroyWindow(w); return 0; }
      if (LOWORD(wp) == YG__START_ID_NOT_NOW || LOWORD(wp) == IDCANCEL) { DestroyWindow(w); return 0; }
      break;
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
  }
  return DefWindowProcW(w, m, wp, lp);
}

static int yg__start_offer(int installed) {
  static const wchar_t *cls = L"YouGameStartOffer";
  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSW wc;
  HWND w, text, get, later;
  HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  MSG msg;
  int width = 460, height = 236;
  int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = yg__start_wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = cls;
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
  yg__start_choice = 0;
  w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, cls, L"YouGame",
                      WS_POPUP | WS_CAPTION | WS_SYSMENU, x < 0 ? 0 : x, y < 0 ? 0 : y, width, height,
                      NULL, NULL, inst, NULL);
  if (!w) return 0;
  text = CreateWindowExW(0, L"STATIC",
    installed
      ? L"YouGame didn't open\r\n\r\n"
        L"This game uses YouGame for online matches, friends and feedback. "
        L"YouGame is on this computer, but it didn't open just now.\r\n\r\n"
        L"Open YouGame yourself, or Get YouGame opens its download page to install it again. "
        L"Not now starts the game."
      : L"Play online with YouGame\r\n\r\n"
        L"This game uses YouGame for online matches, friends and feedback. "
        L"YouGame isn't on this computer yet.\r\n\r\n"
        L"Get YouGame opens YouGame's download page. It installs the YouGame app; a smaller helper is coming.",
    WS_CHILD | WS_VISIBLE, 20, 16, width - 46, 130, w, NULL, inst, NULL);
  get = CreateWindowExW(0, L"BUTTON", L"Get YouGame", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        width - 290, 154, 130, 32, w, (HMENU)(INT_PTR)YG__START_ID_GET, inst, NULL);
  later = CreateWindowExW(0, L"BUTTON", L"Not now", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                          width - 150, 154, 110, 32, w, (HMENU)(INT_PTR)YG__START_ID_NOT_NOW, inst, NULL);
  if (font) {
    if (text) SendMessageW(text, WM_SETFONT, (WPARAM)font, TRUE);
    if (get) SendMessageW(get, WM_SETFONT, (WPARAM)font, TRUE);
    if (later) SendMessageW(later, WM_SETFONT, (WPARAM)font, TRUE);
  }
  ShowWindow(w, SW_SHOWNORMAL);
  SetForegroundWindow(w);
  if (get) SetFocus(get);
  /* Its own short message loop: Tab, Enter and Escape work like in a dialog. */
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (IsWindow(w) && IsDialogMessageW(w, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  UnregisterClassW(cls, inst);
  return yg__start_choice;
}

yg_start_result yg_start(const yg_start_options *o) {
  int staging, quiet, installed = 0;
  if (!o || !yg__start_slug_ok(o->game)) return YG_START_UNAVAILABLE;
  staging = o->staging ? 1 : 0;
  quiet = o->quiet || yg__start_quiet_env();
  if (yg__start_running(staging)) return YG_START_RUNNING;
  if (yg__start_installed(staging)) {
    installed = 1;
    unsigned wait = o->open_wait_ms > 60000 ? 60000 : o->open_wait_ms, waited = 0;
    if (quiet) return YG_START_INSTALLED;
    if (!o->no_open) {
    if (yg__start_open_app(o->game, staging)) {
      if (wait == 0) return YG_START_OPENED;
      while (waited < wait) {
        Sleep(500);
        waited += 500;
        if (yg__start_running(staging)) return YG_START_OPENED;
      }
    }
    }
    /* It did not open (or did not come up in time): fall through to the offer, never a dead end. */
  } else if (quiet) {
    return YG_START_MISSING;
  }
  if (!yg__start_offer(installed)) return YG_START_DECLINED;
  if (!yg__start_shell_open(staging ? L"https://staging.yougame.co/desktop" : L"https://yougame.co/desktop"))
    return YG_START_UNAVAILABLE;
  return YG_START_OFFERED;
}

#else /* not Windows */

yg_start_result yg_start(const yg_start_options *options) {
  (void)options;
  return YG_START_UNAVAILABLE;
}

#endif /* _WIN32 */

#endif /* YOUGAME_START_IMPLEMENTATION */

#endif /* YOUGAME_START_H */
