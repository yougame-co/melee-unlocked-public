// Tournament-only absolute delivery budget. Packaged-task futures never join on destruction.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <future>
#include <mutex>
#include <algorithm>
#include <thread>
#include <string>
#include <utility>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace yougame::tourney {
using DeliveryClock = std::chrono::steady_clock;
using Deadline = DeliveryClock::time_point;
inline Deadline delivery_deadline() { return DeliveryClock::now() + std::chrono::seconds(5); }
// Preserve the ordinary bridge's mutex type and behavior. Only tournament callers poll
// try_lock, and never wait past their delivery deadline even if another writer is stalled.
template<class Mutex> bool lock_until(std::unique_lock<Mutex>& lock, Deadline deadline) {
  while (DeliveryClock::now() < deadline) {
    if (lock.try_lock()) return true;
    std::this_thread::sleep_until(std::min(deadline, DeliveryClock::now() + std::chrono::milliseconds(1)));
  }
  return false;
}
template<class F> auto detached(F work) {
  std::packaged_task<decltype(work())()> task(std::move(work));
  auto done = task.get_future().share();
  std::thread(std::move(task)).detach();
  return done;
}
#ifndef _WIN32
// Caller serializes stdout. The descriptor flags are restored before releasing that lock.
// Only pipe writes of at most PIPE_BUF are attempted: they are atomic or return EAGAIN.
inline bool pipe_write_until(int fd, const std::string& data, Deadline deadline) {
  const long atomic_size = fpathconf(fd, _PC_PIPE_BUF);
  if (atomic_size <= 0 || data.size() > static_cast<size_t>(atomic_size)) return false;
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
  struct Restore { int fd, flags; ~Restore() { fcntl(fd, F_SETFL, flags); } } restore{fd, flags};
  while (DeliveryClock::now() < deadline) {
    const auto wrote = write(fd, data.data(), data.size());
    if (wrote == static_cast<ssize_t>(data.size())) return true;
    if (wrote >= 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - DeliveryClock::now()).count();
    if (left <= 0) return false;
    pollfd p{fd, POLLOUT, 0};
    if (poll(&p, 1, static_cast<int>(left)) < 0 && errno != EINTR) return false;
  }
  return false;
}
#endif
} // namespace yougame::tourney
