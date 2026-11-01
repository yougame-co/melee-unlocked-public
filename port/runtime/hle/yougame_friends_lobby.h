// Friends phase A: thread-safe presentation/exit state and frozen CSS selection.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_friends.h"
#include <atomic>
#include <charconv>
#include <algorithm>
#include <optional>
namespace yougame::friends {
inline constexpr const char* kRoomOpen = "Friends room open: waiting for your friend";
// The app's hello cap "lobby-lines": its page draws the lobby lines, so MU draws none (band or close toast).
inline std::atomic<bool> page_lobby_lines{false};
inline std::string host_left_line(const std::string& name) { return (name.empty() ? "Your friend" : name) + " left: lobby closed"; }
struct CssPick {
  int character = 0, costume = 0, pad = 0;
  bool valid() const { return character >= 0 && character < 26 && costume >= 0 && costume < 6 && pad >= 0 && pad < 4; }
  std::string settings() const {
    return "friends1:" + std::to_string(character) + ":" + std::to_string(costume) + ":" + std::to_string(pad);
  }
};
inline std::optional<CssPick> css_pick(const std::string& s) {
  if (s.compare(0,9,"friends1:") != 0) return {};
  CssPick pick;
  int* fields[] = {&pick.character,&pick.costume,&pick.pad};
  const char* at = s.data()+9; const char* end = s.data()+s.size();
  for (int n = 0; n < 3; ++n) {
    const auto result = std::from_chars(at,end,*fields[n]);
    if (result.ec != std::errc()) return {};
    at = result.ptr;
    if (n < 2) { if (at == end || *at++ != ':') return {}; }
  }
  // Canonical decimal fields only; reject suffixes, leading zeros and invalid fighter/pad values.
  return at == end && pick.valid() && s == pick.settings() ? std::optional<CssPick>(pick) : std::nullopt;
}
inline std::string closed_line(const json& m) {
  if (!m.is_object() || m.value("t",json()) != "roomClosed" || m.value("reason",json()) != "host_left") return {};
  auto name = text(m,"name",40);
  return host_left_line(name);
}
class Lobby {
 public:
  void begin(bool host) {
    std::lock_guard<std::mutex> l(mutex_);
    host_ = host; held_ = true; reported_ = false; return_ = false; loading_ = shown_ = invite_ = leave_ = false; room_.clear(); line_.clear(); pick_ = {}; me_.clear(); host_id_.clear(); host_name_.clear(); host_user_.clear(); reenter_ = false;
  }
  void opened(const std::string& code) { std::lock_guard<std::mutex> l(mutex_); if (!leave_) room_ = code; }
  void report_ready() { std::lock_guard<std::mutex> l(mutex_); reported_ = true; }
  bool reported() const { std::lock_guard<std::mutex> l(mutex_); return reported_; }
  std::string room() const { std::lock_guard<std::mutex> l(mutex_); return room_; }
  bool held() const { std::lock_guard<std::mutex> l(mutex_); return held_; }
  bool host() const { std::lock_guard<std::mutex> l(mutex_); return host_ && held_; }
  bool loading() const { std::lock_guard<std::mutex> l(mutex_); return loading_; }
  bool shown() const { std::lock_guard<std::mutex> l(mutex_); return shown_; }
  void shown(CssPick p) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!held_ || leave_) return;
    if (!shown_) invite_ = host_;
    shown_ = true; if (!loading_ && p.valid()) pick_ = p;
  }
  std::string freeze(const std::string& name) {
    std::lock_guard<std::mutex> l(mutex_);
    loading_ = true; line_ = (name.empty() ? "Your friend" : name) + " joined: loading together";
    return pick_.settings();
  }
  bool ready() const { std::lock_guard<std::mutex> l(mutex_); return !host_ || shown_; }
  void invite() { std::lock_guard<std::mutex> l(mutex_); if (held_ && host_ && shown_ && !loading_) invite_ = true; }
  bool take_invite() { std::lock_guard<std::mutex> l(mutex_); return std::exchange(invite_,false); }
  std::string line() const { std::lock_guard<std::mutex> l(mutex_); return held_ ? (host_ && shown_ ? (loading_ ? line_ : kRoomOpen) : line_) : std::string(); }
  void leave(std::string line = {}) {
    std::lock_guard<std::mutex> l(mutex_); leave_ = true; invite_ = false;
    if (!held_) return; // a stray page Leave still blocks a pending relaunch join, but tears nothing down
    return_ = true; held_ = false; room_.clear(); line_ = std::move(line);
  }
  // Rooms' away roster retains the held host; only a final removal closes a guest.
  // Called only after the live transport verified the frozen account and slot.
  void rebound(const std::string& old_id, const std::string& new_id) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!held_ || leave_) return;
    if (host_id_ == old_id) host_id_ = new_id;
    if (me_ == old_id) me_ = new_id;
  }
  void observe(const json& m) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!held_ || leave_) return;
    const auto t = text(m,"t",16);
    if (t == "welcome" && m.count("me") && m.count("host")) {
      me_ = text(m,"me",128); host_id_ = text(m,"host",128);
      host_ = !me_.empty() && me_ == host_id_;
    }
    const json* roster = nullptr;
    if (m.count("players") && m["players"].is_array()) roster = &m["players"];
    else if (m.count("participants") && m["participants"].is_array()) roster = &m["participants"];
    if (roster) for (const auto& p : *roster)
      if (text(p,"id",128) == host_id_ || text(p,"connectionId",128) == host_id_) {
        const auto name = text(p,"name",40); if (!name.empty()) host_name_ = name;
        const auto user = text(p,"user",128); if (!user.empty()) host_user_ = user;
      }
    auto closed = closed_line(m);
    if (!host_ && closed.empty() && roster && !host_id_.empty() &&
        (t == "leave" || t == "lobby" || t == "back" || t == "join" || t == "welcome") &&
        std::none_of(roster->begin(),roster->end(),[&](const json& p) {
          return text(p,"id",128) == host_id_ || text(p,"connectionId",128) == host_id_ ||
              (!host_user_.empty() && text(p,"user",128) == host_user_); // a held host back under a new id
        })) closed = host_left_line(host_name_);
    if (!host_ && !closed.empty()) close_locked(std::move(closed));
  }
  void closed(int code) {
    std::lock_guard<std::mutex> l(mutex_);
    if (held_ && !host_ && code == 4012)
      close_locked(host_left_line(host_name_));
  }
  void session_stopped(bool retry = true) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!held_ || leave_) return;
    loading_ = false; reenter_ = retry;
    if (host_) { shown_ = false; reported_ = false; }
    else line_ = retry ? "Your friend disconnected. Waiting for the room to reconnect." :
                        "Lost the room connection. Leave and reopen the invite to play again.";
  }
  bool resume() {
    std::lock_guard<std::mutex> l(mutex_);
    if (!held_ || leave_) return false; // never erase an authoritative closure racing the menu tick
    loading_ = shown_ = reported_ = reenter_ = false;
    return true;
  }
  std::string take_reenter() {
    std::lock_guard<std::mutex> l(mutex_);
    return held_ && std::exchange(reenter_,false) ? room_ : std::string();
  }
  bool cancelled() const { std::lock_guard<std::mutex> l(mutex_); return leave_; }
  bool take_return(std::string& line) {
    std::lock_guard<std::mutex> l(mutex_); if (!std::exchange(return_,false)) return false; line = line_; return true;
  }
 private:
  void close_locked(std::string line) {
    leave_ = return_ = true; held_ = invite_ = reenter_ = false; room_.clear(); line_ = std::move(line);
  }
  mutable std::mutex mutex_;
  bool reenter_ = false;
  std::string me_, host_id_, host_name_, host_user_;
  bool reported_ = false;
  bool host_ = false, held_ = false, shown_ = false, loading_ = false, invite_ = false, leave_ = false, return_ = false;
  std::string room_, line_; CssPick pick_;
};
inline Lobby lobby;
} // namespace yougame::friends
