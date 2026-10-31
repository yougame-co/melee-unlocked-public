// Windows toplevel embed policy, independent of Windows headers for synthetic tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_embed_view.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace yougame::embed_toplevel_policy {
enum class Backend { D3D11, D3D12, OpenGL };
// Win32 constants kept here so the policy can be exercised on non-Windows hosts.
inline constexpr uint32_t popup = 0x80000000u, toolwindow = 0x80u,
                          no_redirection = 0x00200000u;
struct Styles { uint32_t style, exstyle; };
inline constexpr Styles styles(bool toplevel, bool flip, Backend backend,
                               uint32_t legacy_style = 0x00cf0000u, uint32_t legacy_exstyle = 0) {
  return toplevel ? Styles{popup, toolwindow | (flip && backend != Backend::OpenGL ? no_redirection : 0)}
                  : Styles{legacy_style, legacy_exstyle};
}
inline bool enabled(bool embedded, std::string_view flag) { return embedded && flag == "1"; }
inline uint32_t app_pid(std::string_view text) {
  if (text.empty()) return 0;
  uint32_t pid = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), pid);
  // UINT32_MAX is ASFW_ANY, not an app PID. Never grant foreground access to everybody.
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         pid != UINT32_MAX ? pid : 0;
}
inline uint32_t preferred_app_pid(uint32_t owner, uint32_t env) {
  return owner && owner != UINT32_MAX ? owner : env;
}
inline bool emit_focus_return(bool foreground, bool focused) { return foreground || focused; }
inline bool rect(std::string_view line, bool toplevel, yg_embed_box& box) {
  if (line.substr(0, 5) != "rect ") return false;
  line.remove_prefix(5);
  int values[4]{};
  for (int i = 0; i < 4; ++i) {
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    if (line.empty()) return false;
    const auto result = std::from_chars(line.data(), line.data() + line.size(), values[i]);
    if (result.ec != std::errc{}) return false;
    line.remove_prefix(result.ptr - line.data());
    if (i < 3 && (line.empty() || line.front() != ' ')) return false;
  }
  while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
  if (!line.empty() || values[2] <= 0 || values[3] <= 0 ||
      (!toplevel && (values[0] < 0 || values[1] < 0))) return false;
  box = {values[0], values[1], toplevel ? std::min(values[2], 16384) : values[2],
         toplevel ? std::min(values[3], 16384) : values[3]};
  return true;
}
inline uint64_t pack(int first, int second) {
  return (uint64_t(uint32_t(first)) << 32) | uint32_t(second);
}
inline int first(uint64_t value) { return int32_t(value >> 32); }
inline int second(uint64_t value) { return int32_t(value); }
inline constexpr const char* kFocusReturn = "yg {\"focusReturn\":1}";
// The app places this window and brings it forward itself (SetForegroundWindow on "focus"), so it is told which window.
inline std::string hwnd_line(uintptr_t hwnd) { return "yg {\"toplevelHwnd\":" + std::to_string(hwnd) + "}"; }
inline std::string focus_return_message(uint32_t pid) {
  return "embed: toplevel focus return pid=" + (pid ? std::to_string(pid) : "none");
}
class FocusReturn {
 public:
  void focused() { returned_ = false; }
  // Eligible blur/hide commands emit the protocol line; duplicates log only once.
  bool changed() { const bool changed = !returned_; returned_ = true; return changed; }
 private:
  bool returned_ = false;
};
} // namespace yougame::embed_toplevel_policy
