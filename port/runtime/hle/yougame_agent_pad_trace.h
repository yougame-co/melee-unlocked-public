// Game-poll diagnostics for staging builds only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef YOUGAME_AGENT_TEST_HOOK
#include "yougame_agent_paths.h"
#include <array>
#include <chrono>
#include <mutex>

namespace yougame::agent_test {
struct TracePad {
  int buttons = 0, x = 0, y = 0, cx = 0, cy = 0, left = 0, right = 0, error = -1;
  std::string source = "none";
  bool operator==(const TracePad& p) const {
    return buttons == p.buttons && x == p.x && y == p.y && cx == p.cx && cy == p.cy &&
        left == p.left && right == p.right && error == p.error && source == p.source;
  }
};
class PadTrace {
 public:
  ~PadTrace() { stop(); }
  void stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) std::fclose(file_);
    file_ = nullptr;
    seen_ = {};
  }
  bool arm(const std::string& name, const agent_paths::Root& root) {
    stop();
    if (name.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    file_ = agent_paths::open(root.path, name, agent_paths::Mode::Append, false, root);
    return file_ != nullptr;
  }
  void poll(int port, const TracePad& pad) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    record(ms, port, pad);
  }
  void record(uint64_t ms, int port, const TracePad& pad) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_ || port < 0 || port >= 4 || (seen_[port] && last_[port] == pad)) return;
    last_[port] = pad;
    seen_[port] = true;
    std::fprintf(file_, "%llu port %d src %s buttons %04X stick %d,%d\n",
        static_cast<unsigned long long>(ms), port + 1, pad.source.c_str(), static_cast<unsigned>(pad.buttons), pad.x, pad.y);
    std::fflush(file_);
  }
 private:
  std::mutex mutex_;
  FILE* file_ = nullptr;
  std::array<TracePad, 4> last_{};
  std::array<bool, 4> seen_{};
};
inline PadTrace pad_trace;
}  // namespace yougame::agent_test
#endif
