// Boot, launch persistence, and the actual Friends entry policies, entirely offline.
#include <cstdint>
#include "yougame_console_boot.h"
#include "yougame_console_entry.h"
#include "yougame_parity.h"
#include <thread>
#include <cstdio>
#include <cstdlib>
using namespace yougame::console;
static int checks = 0, cases = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
struct FakeLink : SessionLink {
  void send_pad(uint32_t,const MuPadStatus&) override {}
  std::optional<MuPadStatus> pad(int,uint32_t) override { return {}; }
  void send_digest(uint32_t,uint64_t) override {}
  std::optional<uint64_t> digest(int,uint32_t) override { return {}; }
  void pump(int) override {}
  std::optional<std::string> lost() const override { return {}; }
};
static SessionConfig config() {
  SessionConfig c; c.room = "ABCDEF"; c.session_id = "test-session"; c.ports = {1,2}; c.seed = UINT64_MAX;
  c.epoch_ms = 1793443200123LL; return c;
}
static std::vector<std::string> signals;
static void signal(const char* s) { signals.emplace_back(s); }
static void boot_paths() {
  ++cases;
  for (int mode = 0; mode < 4; ++mode) {
    std::vector<std::string> events;
    int pumps = 0, begins = 0; bool ready = false;
    auto request = mode == 3 ? std::optional<json>() : std::optional<json>(session_launch(config()));
    bool online = boot(request, [&](const json&,SessionConfig& c,std::string& error) -> std::unique_ptr<SessionLink> {
      CHECK(ready); std::this_thread::sleep_for(std::chrono::milliseconds(16)); c = config();
      if (mode == 1) { error = "fake room failure"; return nullptr; }
      return std::make_unique<FakeLink>();
    }, [&](auto l,const auto& c) { ++begins; CHECK(l && c.session_id == "test-session"); events.push_back("begin"); return mode != 2; },
       [&] { ready = true; events.push_back("ready"); }, [&] { ++pumps; },
       [&](const char* n) { events.push_back(n); }, [&](const std::string& l) { events.push_back(l); });
    events.push_back("first simulation frame");
    CHECK(online == (mode == 0)); CHECK(begins == (mode == 0 || mode == 2 ? 1 : 0));
    if (mode != 3) { CHECK(pumps >= 2); CHECK(events.front() == "ready"); }
    if (mode == 1) CHECK(events[1].find("fake room failure") != std::string::npos);
    if (mode == 2) CHECK(events[3].find("Couldn't start playing together") == 0);
  }
  yougame::parity::Signals ready;
  ready.start(true,signal); ready.console_boot_ready(); ready.console_boot_ready(); ready.presented(true);
  CHECK(signals.size() == 2 && signals[0].find("caps") != std::string::npos && signals[1] == yougame::parity::kReady);
}
static void boot_notices() {
  ++cases;
  for (const auto& reason : {std::string(kMixedBuilds),std::string("left"),std::string("away"),std::string("kicked"),std::string("host_ended"),std::string(kBootTimeout)}) {
    std::string shown; bool began = false;
    CHECK(!boot(std::optional<json>(session_launch(config())),
      [&](const json&,SessionConfig&,std::string& error) -> std::unique_ptr<SessionLink> { error = reason; return nullptr; },
      [&](auto,const auto&) { began = true; return true; }, [] {}, [] {},
      [&](const char* message) { shown = message; }, [](const std::string&) {}));
    CHECK(!began); CHECK(shown == session_notice(reason));
  }
}
static void files(const std::filesystem::path& data) {
  ++cases; std::vector<json> sent; std::vector<std::string> logs; std::string notice;
  auto send = [&](const json& q) { sent.push_back(q); };
  auto log = [&](const std::string& s) { logs.push_back(s); };
  auto show = [&](const char* s) { notice = s; };
  auto path = data / "console" / "pending.json";
  const auto c = config();
  CHECK(relaunch_cap({{"caps",{"ready","relaunch"}}}));
  CHECK(!relaunch_cap({{"caps",{"ready"}}})); CHECK(!relaunch_cap({{"caps","relaunch"}}));
  { // lobby-lines: the page draws the Friends lobby lines; an older app's hello turns MU's back on.
    std::atomic<bool> relaunch{false};
    read_caps({{"caps",{"relaunch","lobby-lines"}}}, relaunch);
    CHECK(relaunch.load()); CHECK(yougame::friends::page_lobby_lines.load());
    read_caps({{"caps",{"ready"}}}, relaunch);
    CHECK(!relaunch.load()); CHECK(!yougame::friends::page_lobby_lines.load());
    CHECK(yougame::friends::host_left_line("") == "Your friend left: lobby closed");
  }
  CHECK(handoff(c,true,data,1000,"build-a",send,log,show));
  CHECK(sent.size() == 1 && sent[0] == relaunch_message(c)); CHECK(!std::filesystem::exists(path));
  CHECK(sent[0]["request"]["seed"] == "ffffffffffffffff");
  CHECK(handoff(c,false,data,1000,"build-a",send,log,show));
  CHECK(sent.size() == 1 && notice == kManualStart && std::filesystem::exists(path));
  json taken;
  auto accept = [&](const json& q,const auto& consume) { if (!consume()) return false; taken = q; return true; };
  CHECK(!pending_take(data,1001,"build-b",accept,log)); CHECK(taken.is_null()); CHECK(std::filesystem::exists(path));
  CHECK(!pending_take(data,91000,"build-a",accept,log)); CHECK(taken.is_null());
  CHECK(!pending_take(data,999,"build-a",accept,log)); CHECK(taken.is_null());
  CHECK(pending_take(data,90999,"build-a",accept,log)); CHECK(taken["seed"] == UINT64_MAX);
  CHECK(!std::filesystem::exists(path)); CHECK(!pending_take(data,90999,"build-a",accept,log));
  CHECK(pending_write(data,session_launch(c),2000,"build-a"));
  CHECK(!pending_take(data,2001,"build-a",[](const json&,const auto&) { return false; },log)); CHECK(std::filesystem::exists(path));
  CHECK(pending_take(data,2001,"build-a",accept,log));
  CHECK(!handoff(c,false,{},1000,"",send,log,show));
  Requests queue;
  CHECK(pending_write(data,session_launch(c),3000,"build-a"));
  CHECK(queue.push({{"room","new"},{"players",2}})); CHECK(queue.pending());
  auto enqueue = [&](const json& q,const auto& consume) { return queue.push(q,true,consume); };
  CHECK(!pending_take(data,3001,"build-a",enqueue,log)); CHECK(std::filesystem::exists(path));
  CHECK(queue.take());
  CHECK(pending_take(data,3001,"build-a",enqueue,log)); CHECK(queue.take()->at("sessionId") == c.session_id);
  CHECK(!std::filesystem::exists(path)); CHECK(!queue.take());
  const auto exe = data / "fake-executable";
  { std::ofstream f(exe); f << "build one"; }
  const auto first = build_fingerprint(exe); CHECK(!first.empty());
  { std::ofstream f(exe); f << "build two"; }
  CHECK(first != build_fingerprint(exe)); CHECK(build_fingerprint(data / "absent").empty());
}
static void relaunch_dropped() {
  ++cases;
  RelaunchWait wait("prepared",1000);
  CHECK(!wait.reason() && !wait.timed_out(45999) && wait.timed_out(46000));
  wait.push({{"type","relaunchDropped"},{"sessionId","another"},{"reason","ticket"}});
  CHECK(!wait.reason());
  std::thread reader([&] { wait.push({{"type","relaunchDropped"},{"sessionId","prepared"},{"reason","ticket"}}); });
  reader.join(); CHECK(std::string(wait.reason()) == "ticket");
  Entry e; CHECK(e.begin()); e.status(kRelaunchDropped);
  CHECK(e.status() == "Couldn't start playing together. Press Play again.");
  for (const auto& reason : {json("renewal_failed"),json("unexpected secret text"),json(nullptr),json(7)}) {
    RelaunchWait other("prepared",1000);
    other.push({{"type","relaunchDropped"},{"sessionId","prepared"},{"reason",reason}});
    CHECK(std::string(other.reason()) == (reason == "renewal_failed" ? "renewal_failed" : "internal"));
  }
  RelaunchWait missing("prepared",1000);
  missing.push({{"type","relaunchDropped"},{"sessionId","prepared"}});
  CHECK(std::string(missing.reason()) == "internal");
}
static void waiting() {
  ++cases; Entry e; CHECK(e.begin()); CHECK(!e.begin());
  e.status(kHostStart,true); CHECK(e.poll(0,true,0x1000)); CHECK(!e.start());
  e.poll(0,true,0); e.poll(0,true,0x1000); CHECK(e.start());
  e.status(yougame::friends::kWaiting,false); CHECK(!e.start());
  e.status(kHostStart,true); e.poll(0,true,0x1000); CHECK(!e.start());
  e.poll(0,true,0); e.poll(0,true,0x1000); CHECK(e.start());
  e.poll(0,true,0x10); CHECK(e.cancelled() && !e.active());
  CHECK(!e.begin()); e.finished();
  CHECK(e.begin()); e.status(kGuestWait,false); e.poll(0,true,0); e.poll(0,true,0x1000); CHECK(!e.start());
  e.stop(); CHECK(e.status().empty());
}
int main(int argc,char** argv) {
  CHECK(argc == 2);
  auto data = std::filesystem::path(argv[1]) / ("console-integration-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  CHECK(std::filesystem::create_directory(data));
  boot_paths(); boot_notices(); files(data); waiting(); relaunch_dropped();
  std::filesystem::remove_all(data); // only this test's unique fixture
  std::printf("yougame_console_integration: %d cases, %d checks passed\n",cases,checks);
}
