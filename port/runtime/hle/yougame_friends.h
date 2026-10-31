// YouGame friends protocol and presentation state, shared with window-free tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace yougame::friends {
using json = nlohmann::json;
inline constexpr const char* kHint = "Leave empty to invite a YouGame friend";
inline constexpr const char* kVersion = "Your friend is on a different version";
inline constexpr const char* kJoined = "Joined";
inline constexpr const char* kFailed = "Couldn't reach your friend";
// Accept the explicit rooms refusal/notice reason; never guess from free-form text.
inline bool version_reason(const json& value) {
  const auto reason = value.find("reason");
  return value.is_object() && reason != value.end() && reason->is_string() && *reason == "version";
}
struct InviteEdge {
  uint16_t previous = 0;
  bool need_release = false;
  // connected = the pad is present AND the game has the keyboard/pads. After it was not (the page's
  // friend picker had focus, a hot-plug), an A or START still held must be let go before a press counts:
  // otherwise a held button reopened the picker every time focus came back (quality, pc2 10-30).
  bool poll(bool connected, uint16_t buttons, bool code, bool empty, bool entry) {
    if (!connected) { previous = 0; need_release = true; return false; }
    if (need_release) {
      if (!(buttons & 0x1100)) need_release = false;
      previous = buttons;
      return false;
    }
    const uint16_t pressed = buttons & ~previous;
    previous = buttons;
    return (code && empty && (pressed & 0x1000)) || (entry && (pressed & 0x1100));
  }
};
inline constexpr const char* kWaiting = "Waiting for your friend...";
inline constexpr const char* kTeamWaiting = "Waiting for your team...";
inline constexpr const char* kTeamCodeError = "Team codes need 3 to 9 letters or numbers";
inline constexpr int kPickerTimeout = 600000;

// The game's Direct code field is Shift-JIS: ASCII letters and digits with the # as the full-width
// 0x81 0x94 (convert_connect_code_for_game), and the on-screen keyboard can also give full-width
// letters, digits or spaces. YouGame's search is UTF-8 JSON, so the code goes in as plain ASCII
// ("LAIN#504"): the raw bytes made the request's dump() throw type_error.316, shown on CSS.
// Anything outside that set returns false so the search fails with a readable message.
inline bool direct_code_from_game(const std::string& sj, std::string* out) {
  std::string code;
  for (size_t i = 0; i < sj.size(); ++i) {
    const auto c = static_cast<unsigned char>(sj[i]);
    if (c == 0) break;
    if (c < 0x80) {
      if (c >= 'a' && c <= 'z') code += static_cast<char>(c - 'a' + 'A');
      else if (c != ' ') code += static_cast<char>(c);
      continue;
    }
    if (i + 1 >= sj.size()) return false;
    const auto d = static_cast<unsigned char>(sj[++i]);
    if (c == 0x81 && d == 0x94) code += '#';
    else if (c == 0x81 && d == 0x40) continue;
    else if (c == 0x82 && d >= 0x4F && d <= 0x58) code += static_cast<char>('0' + d - 0x4F);
    else if (c == 0x82 && d >= 0x60 && d <= 0x79) code += static_cast<char>('A' + d - 0x60);
    else if (c == 0x82 && d >= 0x81 && d <= 0x9A) code += static_cast<char>('A' + d - 0x81);
    else return false;
  }
  for (const char ch : code)
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '#')) return false;
  if (out) *out = code;
  return true;
}

inline bool search_code(int mode, const std::string& raw, std::string* out) {
  std::string code;
  const bool folded = direct_code_from_game(raw, &code);
  if (!folded && (mode == 2 || (mode == 3 && !raw.empty()))) return false;
  if (out) *out = code;
  return true;
}

inline bool team_room_code(const std::string& folded, std::string* out) {
  std::string code;
  for (char c : folded) {
    if (c == '#') continue;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    code += c;
  }
  if (code.size() < 3 || code.size() > 9) return false;
  if (out) *out = "mut" + code;
  return true;
}
inline json team_ticket(const std::string& compatibility, const std::string& code) {
  if (code.size() < 6 || code.size() > 12 || code.compare(0, 3, "mut") != 0)
    throw std::runtime_error(kTeamCodeError);
  for (char c : code)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) throw std::runtime_error(kTeamCodeError);
  return {{"type", "mpTicket"}, {"players", 4}, {"mode", "teams"},
          {"room", code}, {"ranked", false},
          {"lobby", {{"version", 1}, {"minPlayers", 4}, {"maxLocalPlayers", 1}, {"compatibility", compatibility}}}};
}

inline std::string text(const json& value, const char* key, size_t max) {
  if (!value.is_object()) return {};
  const auto it = value.find(key);
  if (it == value.end() || !it->is_string()) return {};
  const auto& s = it->get_ref<const std::string&>();
  if (s.empty() || s.size() > max * 4) return {};
  size_t count = 0;
  for (size_t i = 0; i < s.size();) {
    const auto lead = static_cast<unsigned char>(s[i++]);
    uint32_t cp = lead;
    int tails = 0;
    if (lead >= 0xC2 && lead <= 0xDF) { cp &= 0x1F; tails = 1; }
    else if (lead >= 0xE0 && lead <= 0xEF) { cp &= 0x0F; tails = 2; }
    else if (lead >= 0xF0 && lead <= 0xF4) { cp &= 0x07; tails = 3; }
    else if (lead >= 0x80) return {};
    if (i + tails > s.size()) return {};
    for (int n = 0; n < tails; ++n) {
      const auto c = static_cast<unsigned char>(s[i++]);
      if ((c & 0xC0) != 0x80) return {};
      cp = (cp << 6) | (c & 0x3F);
    }
    if ((tails == 1 && cp < 0x80) || (tails == 2 && cp < 0x800) ||
        (tails == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp < 32 || (cp >= 127 && cp <= 159) ||
        ++count > max) return {};
  }
  return s;
}
inline std::string room_code(const json& value, const char* key) {
  auto s = text(value, key, 16);
  if (s.size() < 4 || s.size() > 16) return {};
  for (unsigned char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return {};
  return s;
}
inline std::string room(const json& value) { return room_code(value, "room"); }
inline std::string picker_room(const json& value) { return room_code(value, "code"); }
inline bool empty_code(bool yougame, int mode, const std::string& code) {
  return yougame && mode == 2 && code.empty();
}
// The picker supplies the host code; invitees repeat it in the `room` field.
inline json ticket(const std::string& compatibility, const std::string& code) {
  if (room(json{{"room", code}}).empty()) throw std::runtime_error("Invalid YouGame friends room");
  return {{"type", "mpTicket"}, {"players", 2}, {"mode", "singles"},
          {"room", code}, {"ranked", false},
          {"lobby", {{"version", 1}, {"minPlayers", 2}, {"maxLocalPlayers", 1}, {"compatibility", compatibility}}}};
}
inline bool native_ready(bool yougame, int mode, const std::string& code, const std::string& invite) {
  return !(yougame && (!invite.empty() || empty_code(true, mode, code)));
}
// The ordinary Direct winner path locks in on START without another code/stage screen.
// A random first stage is already supplied by native_start_match, as for other searches.
inline constexpr int kFirstCssWinner = 1;
inline json picker() { return {{"type", "mpFriend"}, {"mode", "singles"}, {"players", 2}}; }
inline bool version_request(const json& request) {
  return request.value("type", "") == "mpFriend" ||
      (request.value("type", "") == "mpTicket" && request.value("mode", "") == "singles" &&
       request.value("players", 0) == 2 && !request.value("ranked", false) &&
       !request.count("direct") && !room(request).empty());
}
inline std::string expire_joined(const std::string& status, uint64_t since, uint64_t now) {
  return status == kJoined && now - since >= 10000 ? std::string() : status;
}
// Shared request path: no ticket can be sent before a valid picker answer (or invite).
template <class Send, class Cancelled>
std::string resolve_room(const std::string& invite, Send send, Cancelled cancelled) {
  if (cancelled()) throw std::runtime_error("Cancelled");
  const auto code = invite.empty() ? picker_room(send(picker(), kPickerTimeout))
                                   : room(json{{"room", invite}});
  if (cancelled()) throw std::runtime_error("Cancelled");
  if (code.empty()) throw std::runtime_error("Invalid YouGame friends room");
  return code;
}
template <class Send, class Cancelled, class Starting>
json request_ticket(const std::string& compatibility, const std::string& invite,
                    json& request, Send send, Cancelled cancelled, Starting starting) {
  const auto code = resolve_room(invite, send, cancelled);
  request = ticket(compatibility, code);
  starting(code);  // track our room before the page can push its own URL's join
  return send(request, 30000);
}
// Restrict scene changes to the initialized main menu or online CSS/stage/results menus.
// Every match (including local play), replay and other scene remains untouched.
inline bool menu_safe(int major, int minor) { return major == 1 || (major == 8 && (minor == 0 || minor == 1 || minor == 3)); }

class Events {
 public:
  void hello(const json& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hello_seen_) return;
    hello_seen_ = true;
    const auto code = room(value);
    if (pending_.empty() && code != current_room_) pending_ = code;  // a newer push wins over the launch invite
  }
  bool push(const json& value, uint64_t now) {
    const auto type = text(value, "type", 16);
    if (type == "join") {
      const auto code = room(value);
      if (code.empty()) return false;
      std::lock_guard<std::mutex> lock(mutex_);
      if (code == current_room_) return false;
      pending_ = code;
      return true;
    }
    if (type == "notice" && version_reason(value)) {
      std::lock_guard<std::mutex> lock(mutex_);
      version_notice_ = true; picker_error_ = kVersion;
      toast_ = kVersion; toast_until_ = now + 6000;
      return true;
    }
    if (type != "notice" || text(value, "kind", 16) != "invite") return false;
    const auto from = text(value, "from", 40), line = text(value, "text", 80);
    if (from.empty() || line.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    toast_ = from + " " + line;
    toast_until_ = now + 6000;
    return true;
  }
  bool report_version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !std::exchange(version_logged_, true);
  }
  bool take_version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(version_notice_, false);
  }
  std::string take_join(bool safe) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!safe) return {};
    return std::exchange(pending_, {});
  }
  std::string toast(uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return now < toast_until_ ? toast_ : std::string();
  }
  void current_room(const std::string& code) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_room_ = code;
    if (!code.empty() && pending_ == code) pending_.clear();
  }
  void picker_error(std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    picker_error_ = std::move(error);
  }
  std::string picker_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return picker_error_;
  }
  void online_menu(bool visible, bool selected = false) {
    std::lock_guard<std::mutex> lock(mutex_);
    online_menu_ = visible; friend_entry_ = visible && selected;
    if (!visible) host_requested_ = false;
  }
  bool online_menu() const { std::lock_guard<std::mutex> lock(mutex_); return online_menu_; }
  bool friend_entry() const { std::lock_guard<std::mutex> lock(mutex_); return friend_entry_; }
  bool picker_context() const { std::lock_guard<std::mutex> lock(mutex_); return code_screen_ || online_menu_; }
  // What asked for the picker last ("pad N buttons XXXX" or the keyboard/overlay path), for the entry log line.
  std::string trigger() const { std::lock_guard<std::mutex> lock(mutex_); return trigger_; }
  bool poll_pad(int port, bool connected, uint16_t buttons) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (port < 0 || port >= 4) return false;
    if (!edges_[port].poll(connected, buttons, code_screen_, code_empty_, friend_entry_)) return false;
    char why[40];
    std::snprintf(why, sizeof why, "pad %d buttons %04x", port + 1, buttons);
    trigger_ = why;
    host_requested_ = true; picker_error_.clear(); version_notice_ = version_logged_ = false; return true;
  }
  void request_host() {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((code_screen_ && code_empty_) || online_menu_) {
      trigger_ = "Enter or the overlay button";
      host_requested_ = true; picker_error_.clear(); version_notice_ = version_logged_ = false;
    }
  }
  bool take_host() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(host_requested_, false);
  }
  void code_empty(bool empty) { std::lock_guard<std::mutex> lock(mutex_); code_empty_ = empty; }
  bool code_empty() const { std::lock_guard<std::mutex> lock(mutex_); return code_empty_; }
  void code_screen(bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    code_screen_ = visible;
    if (!visible) { host_requested_ = false; picker_error_.clear(); }
  }
  bool code_screen() const { std::lock_guard<std::mutex> lock(mutex_); return code_screen_; }
 private:
  mutable std::mutex mutex_;
  InviteEdge edges_[4];
  std::string trigger_;
  bool online_menu_ = false, friend_entry_ = false, version_notice_ = false, version_logged_ = false;
  bool hello_seen_ = false, code_screen_ = false, code_empty_ = false, host_requested_ = false;
  std::string pending_, toast_, current_room_, picker_error_;
  uint64_t toast_until_ = 0;
};
inline Events events;
}  // namespace yougame::friends
