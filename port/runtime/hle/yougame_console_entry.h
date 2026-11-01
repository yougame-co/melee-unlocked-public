// Friends waiting-panel state and manual relaunch persistence. No simulation state.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_console_request.h"
#include "yougame_console.h"
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
namespace yougame::console {
inline constexpr const char* kHostStart = "Press START to play together";
inline constexpr const char* kGuestWait = "Waiting for the host";
inline constexpr const char* kManualStart = "Ready! Close the game and press Play again to start together.";
inline bool relaunch_cap(const json& j) {
  auto p = j.find("caps");
  return p != j.end() && p->is_array() && std::find(p->begin(), p->end(), json("relaunch")) != p->end();
}
inline std::string seed_hex(uint64_t seed) {
  std::ostringstream s; s << std::hex << std::setw(16) << std::setfill('0') << seed; return s.str();
}
inline json session_launch(const SessionConfig& c) {
  return {{"type","consoleSession"},{"room",c.room},{"sessionId",c.session_id},{"port",c.my_port},
    {"players",c.ports.size()},{"seed",seed_hex(c.seed)},{"epochMs",c.epoch_ms},{"inputDelay",c.input_delay}};
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
  if (!accept(parsed,consume)) return false;
  log("console: pending launch taken"); return true;
}
template<class Send, class Log, class Notice>
bool handoff(const SessionConfig& c, bool relaunch, const std::filesystem::path& data,
             int64_t now, const std::string& build, Send send, Log log, Notice notice) {
  if (relaunch) {
    log("console: requesting relaunch for session " + c.session_id);
    send(relaunch_message(c)); notice("Starting together..."); return true;
  }
  if (pending_write(data,session_launch(c),now,build)) {
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
    status_ = friends::kWaiting; for (auto& edge : edges_) edge.need_release = true;
    return true;
  }
  void finished() { std::lock_guard<std::mutex> lock(mutex_); running_ = false; }
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
    if (!active_ || port < 0 || port >= 4) return false;
    if (connected && (buttons & 0x10)) { cancelled_ = true; active_ = false; status_.clear(); }
    if (edges_[port].poll(connected,buttons,true,true,false) && ready_) start_ = true;
    return true; // the waiting panel owns this pad; don't reopen the Friends picker
  }
 private:
  mutable std::mutex mutex_;
  bool running_ = false, active_ = false, cancelled_ = false, ready_ = false, start_ = false;
  std::string status_;
  friends::InviteEdge edges_[4];
};
inline Entry entry;
void friends_entry(const std::string& room);
void take_pending_launch();
} // namespace yougame::console
