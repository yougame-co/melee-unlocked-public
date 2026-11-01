// Friends Leave scheduling, with slow offline backend operations.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_console_entry.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
using namespace yougame::console;
using Clock = std::chrono::steady_clock;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
static long long elapsed(Clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-since).count();
}
struct Backend {
  std::atomic<bool> flushed{false}, closed{false};
  std::atomic<int> reports{0};
  int delay = 1000;
  void flush() { std::this_thread::sleep_for(std::chrono::milliseconds(delay)); flushed = true; }
  void close() { std::this_thread::sleep_for(std::chrono::milliseconds(delay)); closed = true; }
};
static void slow_leave() {
  FriendsRetire retire;
  auto current = std::make_shared<Backend>(), old = current;
  std::promise<void> flushing;
  auto started = flushing.get_future();
  const auto since = Clock::now();
  auto release = retire.begin([b = current, &flushing] {
    flushing.set_value();
    b->flush(); ++b->reports; b->close();
  });
  CHECK(elapsed(since) < 100);
  CHECK(!retire.wait(0)); // retirement is published before Engine::end
  CHECK(started.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  // Model Engine::end having joined its worker, then a new Friends entry installing a backend.
  const auto returning = Clock::now();
  release();
  CHECK(elapsed(returning) < 100);
  current = std::make_shared<Backend>();
  std::atomic<bool> attached{false}, waited{false}, closed_before_attach{false};
  auto next = std::async(std::launch::async,[&] {
    waited = retire.wait(5000);
    closed_before_attach = old->closed.load();
    attached = true;
  });
  CHECK(started.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(next.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
  CHECK(!attached && !old->closed);
  CHECK(next.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
  next.get();
  CHECK(waited && attached && closed_before_attach);
  CHECK(old->flushed && old->closed && old->reports == 1);
  CHECK(!current->flushed && !current->closed && current->reports == 0);
  CHECK(retire.wait(0));
  std::printf("Friends slow leave: caller <100 ms, flush+close %lld ms; %d checks passed\n",elapsed(since),checks);
}
static void serial_retire() {
  FriendsRetire retire;
  std::promise<void> finish;
  auto ready = finish.get_future().share();
  std::atomic<int> order{0};
  std::atomic<bool> ordered{false}, destroyed{false};
  struct Lifetime {
    std::atomic<bool>& destroyed;
    explicit Lifetime(std::atomic<bool>& flag) : destroyed(flag) {}
    ~Lifetime() { destroyed = true; }
  };
  auto owned = std::make_shared<Lifetime>(destroyed);
  auto first = retire.begin([ready,owned,&order] { ready.wait(); order = 1; });
  owned.reset(); first();
  auto second = retire.begin([&] { ordered = order == 1 && destroyed; order = 2; });
  second();
  CHECK(!retire.wait(20));
  CHECK(order == 0);
  finish.set_value();
  CHECK(retire.wait(1000));
  CHECK(ordered && order == 2 && destroyed);
  // An unexpected cleanup exception must still release the next entry/host exit.
  auto throwing = retire.begin([] { throw 1; }); throwing();
  CHECK(retire.wait(1000));
  std::printf("Friends retire sequencing: %d checks passed\n",checks);
}
static void exit_wait() {
  FriendsRetire retire;
  CHECK(retire.wait(2000)); // quitting without a Leave has nothing to join
  auto b = std::make_shared<Backend>(); b->delay = 80;
  auto release = retire.begin([b] { b->flush(); b->close(); }); release();
  const auto since = Clock::now();
  CHECK(!retire.wait(20));
  CHECK(elapsed(since) < 100); // bounded exit wait can time out
  CHECK(retire.wait(2000)); // the production exit budget gives cleanup its chance
  // An attach whose own backend starts retiring stops waiting at once instead of after 5 s.
  std::promise<void> hold; auto held = hold.get_future().share();
  auto blocked = retire.begin([held] { held.wait(); }); blocked();
  const auto stopping = Clock::now();
  CHECK(!retire.wait(5000,[] { return true; }));
  CHECK(elapsed(stopping) < 200);
  hold.set_value();
  CHECK(retire.wait(1000));
  CHECK(b->flushed && b->closed);
  std::printf("Friends exit wait: %d checks passed\n",checks);
}
int main(int argc,char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  if (mode == "all" || mode == "slow") slow_leave();
  if (mode == "all" || mode == "sequence") serial_retire();
  if (mode == "all" || mode == "exit") exit_wait();
}
