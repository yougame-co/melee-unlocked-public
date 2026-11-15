// Match hints without an app, network, identity or game state.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_match_state.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
  using namespace yougame::match_state;
  Pending p;
  CHECK(p.take(0, 100) == -1); // offline/replay: never opened, no close or heartbeat
  CHECK(p.take(1, 100) == 1);
  CHECK(p.take(1, 100 + kRenewMs - 1) == -1);
  CHECK(p.take(1, 100 + kRenewMs) == 1);
  CHECK(p.take(2, 101 + kRenewMs) == 0);
  CHECK(p.take(2, 20 * kRenewMs) == -1);
  // The ticket need not change. Every new match has a closed edge before its next open.
  CHECK(p.take(5, 20 * kRenewMs) == 1);
  CHECK(p.take(5, 20 * kRenewMs) == 0);
  CHECK(p.take(5, 20 * kRenewMs) == 1);

  std::mutex mutex;
  std::condition_variable wake;
  bool entered = false, release = false;
  std::vector<int> sent;
  const auto game_thread = std::this_thread::get_id();
  Worker worker;
  worker.start([&](int open) {
    CHECK(std::this_thread::get_id() != game_thread);
    std::unique_lock<std::mutex> lock(mutex);
    sent.push_back(open);
    entered = true;
    wake.notify_one();
    wake.wait(lock, [&] { return release; }); // stand-in for an app that stopped answering
    throw std::runtime_error("refused"); // even a refusal cannot stop later close/open delivery
  });
  worker.set(false);
  worker.set(true);
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(wake.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
  }
  // These must complete while the sender is still blocked, including repeated frame-1 starts.
  worker.set(true);
  worker.set(false);
  worker.set(true);
  worker.set(false);
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(sent == std::vector<int>{1});
    release = true;
  }
  wake.notify_one();
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(wake.wait_for(lock, std::chrono::seconds(2), [&] { return sent.size() == 4; }));
    CHECK((sent == std::vector<int>{1, 0, 1, 0}));
  }
  worker.stop();
  return 0;
}
