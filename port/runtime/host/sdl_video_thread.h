// SDL video and native embed operations belong to the window's creating thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace host {
class SdlVideoThread {
 public:
  enum class Call { Pump, EmbedCommands, Event, LoadingShow, LoadingClose, Destroy, Title };

  void claim() {
    std::lock_guard<std::mutex> lock(mutex_);
    owner_ = std::this_thread::get_id();
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ == std::this_thread::get_id()) owner_ = {};
  }
  template<class Log> bool allow(Call call, Log log) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (owner_ == std::this_thread::get_id()) return true;
    }
    // Shared by all calling threads, and deliberately not reset across window lifetimes.
    const auto bit = uint32_t{1} << static_cast<unsigned>(call);
    if (!(skipped_.fetch_or(bit, std::memory_order_relaxed) & bit))
      log("window: pump skipped on a non-video thread");
    return false;
  }

 private:
  std::mutex mutex_;
  std::thread::id owner_{};
  std::atomic<uint32_t> skipped_{0};
};
} // namespace host
