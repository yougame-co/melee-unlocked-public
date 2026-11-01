// Friends waiting-panel state and manual relaunch persistence. No simulation state.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_console_request.h"
#include "yougame_friends_lobby.h"
#include "yougame_console.h"
#include "yougame_console_card.h"
#include "yougame_console_io.h"
#include "yougame_console_notice.h"
#include <atomic>
#include <functional>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <future>
#include <thread>
namespace yougame::console {
// Published before sending the relaunch request. The stdio reader only writes the
// bounded reason; the Friends entry thread owns status, logging and cleanup.
struct RelaunchWait {
  const std::string session;
  const int64_t handed_at;
  std::atomic<int> dropped{0};
  RelaunchWait(std::string id, int64_t at) : session(std::move(id)), handed_at(at) {}
  void push(const json& p) {
    if (friends::text(p,"type",32) != "relaunchDropped" || friends::text(p,"sessionId",64) != session) return;
    const auto reason = friends::text(p,"reason",32);
    int empty = 0;
    dropped.compare_exchange_strong(empty,reason == "ticket" ? 1 : reason == "renewal_failed" ? 2 : 3);
  }
  const char* reason() const {
    switch (dropped.load()) { case 1: return "ticket"; case 2: return "renewal_failed"; case 3: return "internal"; default: return nullptr; }
  }
  bool timed_out(int64_t now) const { return now - handed_at >= 45000; }
};
inline constexpr const char* kRelaunchDropped = "Couldn't start playing together. Press Play again.";
// Publish retirement before stopping the simulation's datagram worker. The returned
// release callback lets cleanup start only after that worker has been joined.
class FriendsRetire {
 public:
  template<class Work> std::function<void()> begin(Work work) {
    auto done = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    std::shared_future<void> previous;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      previous = retired_; retired_ = done->get_future().share();
    }
    std::thread([previous, ready = release->get_future(), done, work = std::move(work)]() mutable {
      if (previous.valid()) previous.wait();
      ready.wait();
      // Destroy captured resources before publishing completion (their destructors
      // can close a process-global link channel too).
      { auto cleanup = std::move(work); try { cleanup(); } catch (...) {} }
      done->set_value();
    }).detach();
    return [release] { release->set_value(); };
  }
  // stop (optional) ends the wait early, e.g. when the waiter's own backend starts retiring.
  bool wait(int ms, const std::function<bool()>& stop = {}) const {
    std::shared_future<void> retired;
    { std::lock_guard<std::mutex> lock(mutex_); retired = retired_; }
    if (!retired.valid()) return true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (retired.wait_for(std::chrono::milliseconds(stop ? 20 : ms)) != std::future_status::ready) {
      if ((stop && stop()) || std::chrono::steady_clock::now() >= until) return false;
    }
    return true;
  }
 private:
  mutable std::mutex mutex_;
  std::shared_future<void> retired_;
};
inline constexpr const char* kHostStart = "Press START to play together";
inline constexpr const char* kGuestWait = "Waiting for the host";
inline constexpr const char* kManualStart = "Ready! Close the game and press Play again to start together.";
inline bool has_cap(const json& j, const char* cap) {
  auto p = j.find("caps");
  return p != j.end() && p->is_array() && std::find(p->begin(), p->end(), json(cap)) != p->end();
}
inline bool relaunch_cap(const json& j) { return has_cap(j, "relaunch"); }
// Read the hello reply's caps: relaunch, and lobby-lines (the page draws the Friends lobby lines).
inline void read_caps(const json& j, std::atomic<bool>& relaunch) {
  relaunch.store(relaunch_cap(j));
  friends::page_lobby_lines.store(has_cap(j, "lobby-lines"), std::memory_order_release);
}
inline std::string seed_hex(uint64_t seed) {
  std::ostringstream s; s << std::hex << std::setw(16) << std::setfill('0') << seed; return s.str();
}
inline json session_launch(const SessionConfig& c) {
  json launch = {{"type","consoleSession"},{"room",c.room},{"sessionId",c.session_id},{"port",c.my_port},
    {"players",c.ports.size()},{"seed",seed_hex(c.seed)},{"epochMs",c.epoch_ms},{"inputDelay",c.input_delay}};
  // Exactly the app's eight consoleSession fields (desktop/src/console-session.ts drops any other
  // key, "consoleSession dropped: fields"). The rejoin reads cardDigest from the room's frozen start.
  return launch;
}
inline json relaunch_message(const SessionConfig& c) { return {{"type","relaunch"},{"request",session_launch(c)}}; }
// Fingerprint the actual host executable, not its marketing version or path.
inline std::string build_fingerprint(const std::filesystem::path& exe) {
  std::ifstream f(exe, std::ios::binary); if (!f) return {};
  uint64_t hash = 14695981039346656037ull; char bytes[65536];
  while (f.read(bytes,sizeof bytes) || f.gcount())
    for (std::streamsize n = 0; n < f.gcount(); ++n) hash = (hash ^ uint8_t(bytes[n])) * 1099511628211ull;
  return f.eof() ? seed_hex(hash) : std::string();
}
inline std::filesystem::path left_path(const std::filesystem::path& data, const std::string& session) {
  if (data.empty() || session_id(json{{"sessionId",session}}).empty()) return {};
  return data / "console" / ("left-" + session);
}
inline bool session_left(const std::filesystem::path& data, const std::string& session) {
  const auto path = left_path(data,session); std::error_code ec;
  return !path.empty() && std::filesystem::exists(path,ec);
}
inline bool mark_session_left(const std::filesystem::path& data, const std::string& session) {
  const auto path = left_path(data,session); if (path.empty()) return false;
  std::error_code ec;
  if (std::filesystem::is_symlink(path.parent_path(),ec) || std::filesystem::is_symlink(path,ec)) return false;
  std::filesystem::create_directories(path.parent_path(),ec); if (ec) return false;
  std::ofstream file(path,std::ios::binary|std::ios::trunc); file << "left\n"; file.close(); return bool(file);
}
inline bool pending_write(const std::filesystem::path& data, const json& request,
                          int64_t now, const std::string& build) {
  if (data.empty() || build.empty()) return false;
  json parsed; if (!parse_request(request,parsed) || !parsed.count("sessionId")) return false;
  std::error_code ec; const auto dir = data / "console", path = dir / "pending.json";
  if (std::filesystem::is_symlink(dir,ec) || std::filesystem::is_symlink(path,ec)) return false;
  std::filesystem::create_directories(dir,ec); if (ec) return false;
  auto record = request; record["createdAt"] = now; record["build"] = build;
  std::ofstream f(path,std::ios::binary|std::ios::trunc); if (!f) return false;
  f << record.dump(); f.close(); return bool(f);
}
template<class Accept, class Log>
bool pending_take(const std::filesystem::path& data, int64_t now, const std::string& build, Accept accept, Log log) {
  if (data.empty() || build.empty()) return false;
  std::error_code ec; const auto path = data / "console" / "pending.json";
  if (std::filesystem::is_symlink(data / "console",ec) || std::filesystem::is_symlink(path,ec) ||
      !std::filesystem::is_regular_file(path,ec) || std::filesystem::file_size(path,ec) > 4096 || ec) return false;
  std::ifstream f(path); auto record = json::parse(f,nullptr,false); f.close();
  if (!record.is_object() || !number(record,"createdAt",0,INT64_MAX)) return false;
  if (record.value("build",json()) != build) { log("console: pending launch ignored: build changed"); return false; }
  const auto created = record["createdAt"].get<int64_t>();
  if (now < created || now-created >= 90000) { log("console: pending launch ignored: expired"); return false; }
  record.erase("createdAt"); record.erase("build"); json parsed;
  if (!parse_request(record,parsed) || !parsed.count("sessionId")) return false;
  // The queue calls consume under its intake lock, before publishing this request.
  // A failed delete cannot leave a launch queued to repeat on the next boot.
  auto consume = [&] {
    if (std::filesystem::remove(path,ec) && !ec) return true;
    log("console: couldn't consume pending launch"); return false;
  };
  if (session_left(data,session_id(parsed))) { consume(); return false; }
  if (!accept(parsed,consume)) return false;
  log("console: pending launch taken"); return true;
}
struct HandoffIO {
  std::function<uint64_t()> clock = [] {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  };
  std::function<bool()> cancelled = [] { return false; };
  // Detached workers must own every dependency of these callbacks.
  std::function<bool(const std::filesystem::path&,const SessionConfig&)> choice = write_card_choice;
  std::function<bool(const std::filesystem::path&,const json&,int64_t,const std::string&)> pending = pending_write;
};
template<class Send, class Log, class Notice>
bool handoff(const SessionConfig& c, bool relaunch, const std::filesystem::path& data,
             int64_t now, const std::string& build, Send send, Log log, Notice notice, HandoffIO io = {}) {
  const auto until = io.clock()+1000;
  if (!card_io_until(io.clock,io.cancelled,until,false,
      [write = io.choice, data, card = c] { return write(data,card); })) {
    log("console: save choice unavailable; rejoin will agree default unlocks");
    log(std::string("console: ") + kDefaultCard);
  }
  if (io.cancelled()) return false;
  if (relaunch) {
    log("console: requesting relaunch for session " + c.session_id);
    send(relaunch_message(c)); notice("Starting together..."); return true;
  }
  // Manual relaunch still needs its pending request when the choice timed out.
  if (card_io_until(io.clock,io.cancelled,io.clock()+1000,false,
      [write = io.pending, data, request = session_launch(c), now, build] {
        return write(data,request,now,build);
      })) {
    log("console: pending launch saved for session " + c.session_id);
    log(std::string("console: ") + kManualStart); notice(kManualStart); return true;
  }
  log("console: couldn't save pending launch");
  notice("Couldn't save the session. Please invite your friend again."); return false;
}
class Entry {
 public:
  bool begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ || running_) return false;
    running_ = true; active_ = true; cancelled_ = false; ready_ = false; start_ = false;
    for (auto& buttons : lobby_buttons_) buttons = 0;
    status_ = friends::kWaiting; for (auto& edge : edges_) edge.need_release = true;
    return true;
  }
  void finished() { std::lock_guard<std::mutex> lock(mutex_); running_ = false; active_ = false; }
  bool available() const { std::lock_guard<std::mutex> lock(mutex_); return !active_ && !running_; }
  bool active() const { std::lock_guard<std::mutex> lock(mutex_); return active_; }
  bool cancelled() const { std::lock_guard<std::mutex> lock(mutex_); return cancelled_; }
  void stop() { std::lock_guard<std::mutex> lock(mutex_); cancelled_ = true; active_ = false; status_.clear(); }
  void status(const std::string& text, bool can_start = false) {
    std::lock_guard<std::mutex> lock(mutex_); if (cancelled_) return; status_ = text;
    if (can_start != ready_) { start_ = false; for (auto& edge : edges_) edge.need_release = true; }
    ready_ = can_start;
  }
  std::string status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
  bool start() const { std::lock_guard<std::mutex> lock(mutex_); return start_; }
  bool poll(int port, bool connected, uint16_t buttons) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (port < 0 || port >= 4) return false;
    if (friends::lobby.host() && friends::lobby.shown()) {
      // Local VS owns A/B/sticks/costumes (B un-picks; backing out of CSS leaves in tick()).
      // B cancels only the shared load; Z reopens this room's picker.
      const uint16_t edge = connected ? buttons & ~lobby_buttons_[port] : 0;
      lobby_buttons_[port] = connected ? buttons : 0;
      if ((edge & 0x200) && friends::lobby.loading()) { friends::lobby.leave(); cancelled_ = true; return true; }
      if (edge & 0x10) { friends::lobby.invite(); return true; }
      return friends::lobby.loading() || (buttons & 0x1000); // no solo START
    }
    if (friends::lobby.held() && !friends::lobby.host() && !friends::lobby.line().empty()) {
      const uint16_t edge = connected ? buttons & ~lobby_buttons_[port] : 0;
      lobby_buttons_[port] = connected ? buttons : 0;
      if (edge & 0x200) { friends::lobby.leave(); cancelled_ = true; return true; }
    }
    if (!active_) {
      if (friends::events.join_pending() && connected && (buttons & 0x10)) {
        friends::events.take_join(true); return true;
      }
      return false; // keep playing if the invite is waiting for a safe scene
    }
    if (connected && (buttons & 0x10)) { cancelled_ = true; active_ = false; status_.clear(); }
    if (edges_[port].poll(connected,buttons,true,true,false) && ready_) start_ = true;
    return true; // the waiting panel owns this pad; don't reopen the Friends picker
  }
 private:
  mutable std::mutex mutex_;
  bool running_ = false, active_ = false, cancelled_ = false, ready_ = false, start_ = false;
  std::string status_;
  friends::InviteEdge edges_[4];
  uint16_t lobby_buttons_[4]{};
};
inline Entry entry;
inline std::string waiting_status() {
  if (friends::lobby.host()) return {}; // corner line over CSS, never a waiting panel
  const auto status = entry.status();
  // Show the accepted invite immediately, including the safe-scene settling gap.
  return friends::events.join_pending() ? kGuestWait : status;
}
void friends_entry(const std::string& room, bool host_reenter = false, bool held_reenter = false);
std::function<void()> leave_friends_room();
void close_friends_room(std::function<void()> release);
// Host shutdown: give a running Leave retire up to ms to finish (the process ends with
// TerminateProcess/_exit, so atexit handlers never run).
bool wait_friends_retired(int ms);
void take_pending_launch();
} // namespace yougame::console
