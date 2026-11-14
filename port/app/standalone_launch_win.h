// Windows: a plain double-click on melee_source.exe (or melee_source_compat.exe) starts the game.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Started with no arguments at all, the Source Port used to print its usage to a console that a
// double-click does not have and exit, so a player saw nothing. Now it finds the player's own Melee
// NTSC 1.02 disc (the one it remembered, else melee.iso beside the exe, else a file picker), remembers
// it, and starts a copy of itself exactly the way MeleeUnlocked.bat and the YouGame app do: settings, saves, replays,
// the log and the shader cache in %LOCALAPPDATA%\MeleeUnlocked-YouGame, Sys beside the exe, the same
// flags. Any argument at all (the app, the .bat, a shortcut, a test) skips this entirely.
#pragma once
#if defined(_WIN32) && defined(MELEE_SOURCE_PORT)
#include <windows.h>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace standalone_launch {
namespace fs = std::filesystem;

inline fs::path exe_dir() {
  std::wstring self(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, self.data(), (DWORD)self.size());
  if (n == 0 || n >= self.size()) return {};
  self.resize(n);
  return fs::path(self).parent_path();
}

inline fs::path data_dir() {
  wchar_t local[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return fs::path(local) / L"MeleeUnlocked-YouGame";
}

// Melee NTSC 1.02: game ID GALE01 at the start of the disc, disc revision 2 at offset 7.
inline bool is_melee_102(const fs::path& iso) {
  std::error_code ec;
  if (!fs::is_regular_file(iso, ec)) return false;
  FILE* f = _wfopen(iso.c_str(), L"rb");
  if (!f) return false;
  unsigned char head[8] = {};
  const size_t got = std::fread(head, 1, sizeof head, f);
  std::fclose(f);
  return got == sizeof head && std::memcmp(head, "GALE01", 6) == 0 && head[7] == 2;
}

inline fs::path remembered(const fs::path& ini) {
  FILE* f = _wfopen(ini.c_str(), L"rb");
  if (!f) return {};
  char line[4096];
  fs::path found;
  while (std::fgets(line, sizeof line, f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.rfind("iso=", 0) == 0) found = fs::u8path(s.substr(4));
  }
  std::fclose(f);
  return found;
}

inline void remember(const fs::path& ini, const fs::path& iso) {
  const fs::path tmp = fs::path(ini).concat(L".tmp");
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return;
  std::fprintf(f, "iso=%s\n", iso.u8string().c_str());
  std::fclose(f);
  MoveFileExW(tmp.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING);
}

inline fs::path pick() {
  for (;;) {
    wchar_t file[32768] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"Melee disc image (*.iso, *.gcm)\0*.iso;*.gcm\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    ofn.lpstrTitle = L"Melee Unlocked: choose your Super Smash Bros. Melee NTSC 1.02 disc (ISO)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};   // closed or cancelled: start nothing
    const fs::path chosen(file);
    if (is_melee_102(chosen)) return chosen;
    MessageBoxW(nullptr,
                L"That file is not a Super Smash Bros. Melee NTSC 1.02 disc image.\n\n"
                L"Melee Unlocked needs your own copy of Melee, version 1.02 (game ID GALE01), as an .iso file. "
                L"Choose it again.",
                L"Melee Unlocked", MB_OK | MB_ICONWARNING);
  }
}

// One argument quoted the way the C runtime splits it (backslashes before a quote are doubled).
inline std::wstring quote(const std::wstring& arg) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') { ++slashes; continue; }
    out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

// A no-argument start: find the disc, then start this same exe with the .bat's command line and
// return. The game itself runs in that copy, so its own relaunches (settings restart, the updater,
// mod looks, all built from GetCommandLineW) keep the data folder and flags. Returns the exit code.
inline int run() {
  const fs::path exe = exe_dir(), data = data_dir();
  if (exe.empty() || data.empty()) {
    MessageBoxW(nullptr, L"Melee Unlocked could not find its own folder or %LOCALAPPDATA%.", L"Melee Unlocked",
                MB_OK | MB_ICONERROR);
    return 1;
  }
  std::error_code ec;
  for (const wchar_t* sub : {L"User\\Slippi", L"User\\GC\\CardA", L"Replays"}) fs::create_directories(data / sub, ec);
  const fs::path ini = data / L"standalone.ini";
  fs::path iso = remembered(ini);
  if (!is_melee_102(iso)) iso = exe / L"melee.iso";
  if (!is_melee_102(iso)) iso = pick();
  if (iso.empty()) return 0;   // the player closed the picker
  remember(ini, iso);
  std::wstring self(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, self.data(), (DWORD)self.size());
  if (n == 0 || n >= self.size()) return 1;
  self.resize(n);
  const std::vector<std::wstring> args = {
      self, L"--iso", iso.wstring(), L"--threaded-renderer",
      L"--settings-path", (data / L"port-settings.ini").wstring(),
      L"--sys-dir", (exe / L"Sys").wstring(),
      L"--user-dir", (data / L"User\\Slippi").wstring(),
      L"--replay-dir", (data / L"Replays").wstring(),
      L"--card-dir", (data / L"User\\GC\\CardA").wstring(),
      L"--log-file", (data / L"melee.log").wstring(),
      L"--shader-cache", (data / L"shadercache").wstring(),
      L"--vanilla-only"};
  std::wstring cmd;
  for (const auto& arg : args) cmd += (cmd.empty() ? L"" : L" ") + quote(arg);
  STARTUPINFOW si{}; si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  // Working folder: the exe's own, as the .bat's cd /d "%~dp0".
  if (!CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.c_str(), &si, &pi)) {
    MessageBoxW(nullptr, L"Melee Unlocked could not start the game.", L"Melee Unlocked", MB_OK | MB_ICONERROR);
    return 1;
  }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  return 0;
}
}  // namespace standalone_launch
#endif
