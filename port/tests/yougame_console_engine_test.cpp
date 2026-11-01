#include "yougame_console_engine.h"
#include "native_savestate.h"
#include "yougame_console_menu.h"
#include "yougame_console_card.h"
#include "yougame_console_notice.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdarg>

// Compile the real game/host adapter. Other online ABI exports are unused stubs here.
#define mu_console_abi test_console_abi
#include "../../sourceport/game/shim/mu_online_abi.c"
#undef mu_console_abi
const MuHostApi* mu_host;
void set_test_console_host(const MuHostApi* api) { mu_host = api; }
void* mu_online_state_address() { return nullptr; }
unsigned int mu_online_state_size() { return 0; }
void* mu_online_audio_state_address() { return nullptr; }
unsigned int mu_online_audio_state_size() { return 0; }

using namespace yougame::console;
namespace {
int assertions = 0, cases = 0;
#define CHECK(x) do { ++assertions; if (!(x)) throw std::runtime_error(#x); } while (0)
struct Bus {
  std::map<uint32_t, MuPadStatus> pads[4];
  std::map<uint32_t, uint64_t> digests[4];
  int sends[4]{};
  bool drop = false, lost = false, corrupt = false, echo_digest = false;
  uint64_t release = 0, now = 0;
  unsigned pumps = 0, responsive = 0;
};
struct Loopback : SessionLink {
  Bus& bus; int mine;
  Loopback(Bus& b, int p) : bus(b), mine(p - 1) {}
  void send_pad(uint32_t f, const MuPadStatus& pad) override { bus.pads[mine][f] = pad; ++bus.sends[mine]; }
  std::optional<MuPadStatus> pad(int p, uint32_t f) override {
    if (bus.drop || bus.now < bus.release) return {};
    const auto it = bus.pads[p - 1].find(f);
    return it == bus.pads[p - 1].end() ? std::nullopt : std::optional<MuPadStatus>(it->second);
  }
  void send_digest(uint32_t f, uint64_t d) override { bus.digests[mine][f] = d; if (bus.echo_digest) bus.digests[1-mine][f] = d; }
  std::optional<uint64_t> digest(int p, uint32_t f) override {
    const auto it = bus.digests[p - 1].find(f);
    if (it == bus.digests[p - 1].end()) return {};
    return it->second ^ (bus.corrupt ? 1ull : 0ull);
  }
  void pump(int ms) override { ++bus.pumps; bus.now += ms; }
  std::optional<std::string> lost() const override {
    return bus.lost ? std::optional<std::string>("peer left") : std::nullopt;
  }
};
SessionConfig config(int p, int delay = 3) {
  SessionConfig c; c.session_id = "engine-test"; c.my_port = p; c.ports = {1, 2};
  c.seed = 123; c.epoch_ms = 1793448000000LL; c.input_delay = delay; return c;
}
EngineHooks hooks(Bus& b, std::vector<std::string>& logs, std::vector<std::string>& notices) {
  EngineHooks h;
  h.now_ms = [&] { return b.now; };
  h.responsive = [&] { ++b.responsive; };
  h.log = [&](const std::string& s) { logs.push_back(s); };
  h.notice = [&](const char* s) { notices.emplace_back(s); };
  return h;
}
Pads hardware(unsigned f, int physical = 2) {
  Pads pads{}; for (auto& p : pads) p.err = -1;
  auto& p = pads[physical]; p.err = 0; p.button = static_cast<uint16_t>(f + 1);
  p.stick_x = f % 127; p.stick_y = -(int)(f % 100); p.sub_x = 10; p.sub_y = -20;
  p.trigger_l = f % 256; p.trigger_r = (f + 3) % 256; p.analog_a = 42; p.analog_b = 23;
  return pads;
}
bool equal(const MuPadStatus& a, const MuPadStatus& b) {
  return a.button == b.button && a.stick_x == b.stick_x && a.stick_y == b.stick_y &&
      a.sub_x == b.sub_x && a.sub_y == b.sub_y && a.trigger_l == b.trigger_l &&
      a.trigger_r == b.trigger_r && a.analog_a == b.analog_a && a.analog_b == b.analog_b && a.err == b.err;
}
void boot_barrier() {
  ++cases;
  for (bool arrives : {true,false}) {
    Bus bus; Engine engine; std::vector<std::string> logs, notices;
    auto link = std::make_unique<Loopback>(bus,1); link->frame_zero_barrier = true;
    auto h = hooks(bus,logs,notices);
    h.responsive = [&] {
      ++bus.responsive;
      if (arrives && bus.now >= 8000) bus.digests[1][0] = Engine::state_digest({});
    };
    CHECK(engine.begin(std::move(link),config(1),h, false));
    bus.pads[1][0] = {};
    CHECK(engine.advance(hardware(0),{}) == arrives);
    CHECK(engine.clock_ticks() == 0 && engine.frame() == 0);
    if (arrives) {
      CHECK(bus.now >= 8000 && bus.now < 8010); CHECK(notices.empty());
      CHECK(logs.back() == "console: frame 0 barrier passed");
    } else {
      CHECK(bus.now == 60000);
      CHECK(notices == std::vector<std::string>{"Your friend's game didn't start."});
      CHECK(logs.back() == "console: session ended: your friend's game didn't start");
    }
  }
  for (const char* reason : {"left","away","kicked","host_ended"}) {
    Bus bus; Engine engine; std::vector<std::string> logs, notices;
    CHECK(engine.begin(std::make_unique<Loopback>(bus,1),config(1),hooks(bus,logs,notices), false));
    engine.end(reason);
    CHECK(notices.size() == 1);
    CHECK(notices[0] == (std::string(reason) == "host_ended" ? "The host ended the session." : "Your friend left."));
  }
  { // The seat that pressed Leave ends quietly; only its friend is told.
    Bus bus; Engine engine; std::vector<std::string> logs, notices;
    CHECK(engine.begin(std::make_unique<Loopback>(bus,1),config(1),hooks(bus,logs,notices), false));
    engine.end(kSelfLeft);
    CHECK(notices.empty()); CHECK(logs.back() == "console: session ended: you left");
  }
}
void same_stream() {
  ++cases;
  Bus bus; Engine a, b; std::vector<std::string> logs, notices;
  CHECK(a.begin(std::make_unique<Loopback>(bus, 1), config(1), hooks(bus, logs, notices), false));
  CHECK(b.begin(std::make_unique<Loopback>(bus, 2), config(2), hooks(bus, logs, notices), false));
  for (unsigned f = 0; f < 721; ++f) {
    MuStatePod state{}; state.rng = f * 47; state.scene_major = f / 150; state.menu_selection = f % 5;
    a.retrace(); b.retrace();
    CHECK(a.advance(hardware(f), state)); CHECK(b.advance(hardware(f + 1000, 3), state));
    Pads x, y, repeated;
    a.read(hardware(999), x); b.read(hardware(888), y);
    const auto sends = bus.sends[0];
    a.read(hardware(777), repeated); // PADRead twice in the same body
    CHECK(bus.sends[0] == sends); CHECK(a.frame() == f);
    for (int p = 0; p < 4; ++p) { CHECK(equal(x[p], y[p])); CHECK(equal(x[p], repeated[p])); }
    if (f < 3) CHECK(equal(x[0], MuPadStatus{}));
    else CHECK(equal(x[0], hardware(f - 3)[2]));
    if (f < 3) CHECK(equal(x[1], MuPadStatus{}));
    else CHECK(equal(x[1], hardware(f - 3 + 1000, 3)[3]));
    CHECK(x[2].err == -1 && x[3].err == -1);
    CHECK(a.ticks(9999) == b.ticks(7777));
    CHECK(a.clock_ticks() == uint64_t(f + 1) * (Engine::frame_ticks + Engine::poll_ticks));
    CHECK(a.retrace_count() == f + 1 && b.retrace_count() == f + 1);
    CHECK(a.boot_ticks(9999) == uint64_t(config(1).epoch_ms - 946684800000LL) * 40500);
  }
  CHECK(notices.empty()); CHECK(bus.sends[0] == 724); CHECK(bus.sends[1] == 724);
  CHECK(std::count(logs.begin(), logs.end(), "console: frame 0") == 2);
  CHECK(std::count_if(logs.begin(), logs.end(), [](const std::string& l) {
    return l.rfind("console: frame 600 waited 0 ms max 0 ms; 600 frames in ", 0) == 0 && l.find(" 0 waits over a frame") != std::string::npos;
  }) == 2);
  // Every 60th frame both seats log the same hash line, frames 0 through 720.
  std::vector<std::string> hashes;
  for (const auto& l : logs) if (l.rfind("console: hash ", 0) == 0) hashes.push_back(l);
  CHECK(hashes.size() == 26);
  for (unsigned f = 0; f <= 720; f += 60) {
    const auto prefix = "console: hash " + std::to_string(f) + " ";
    std::vector<std::string> at;
    for (const auto& l : hashes) if (l.rfind(prefix, 0) == 0) at.push_back(l);
    CHECK(at.size() == 2 && at[0] == at[1] && at[0].size() > prefix.size());
  }
  std::cout << logs.front() << '\n' << logs[2] << '\n';
}
void delay_and_reorder() {
  ++cases;
  Bus bus; Engine a; std::vector<std::string> logs, notices;
  CHECK(a.begin(std::make_unique<Loopback>(bus, 1), config(1, 0), hooks(bus, logs, notices), false));
  // Future reports precede the missing frame zero. The engine must ask for the exact frame.
  bus.pads[1][2] = hardware(2)[2]; bus.pads[1][1] = hardware(1)[2]; bus.pads[1][0] = hardware(0)[2];
  bus.release = 35;
  CHECK(a.advance(hardware(0), {})); CHECK(bus.now == 35); CHECK(bus.responsive == 7);
  CHECK(a.clock_ticks() == 0); // real waiting time never advances game time
  Pads out; a.read({}, out); CHECK(equal(out[1], hardware(0)[2]));
  CHECK(a.advance(hardware(1), {})); a.read({}, out); CHECK(equal(out[1], hardware(1)[2]));
}
void clocks_follow_calls() {
  ++cases;
  Bus bus; Engine a, b; std::vector<std::string> logs, notices;
  CHECK(a.begin(std::make_unique<Loopback>(bus, 1), config(1), hooks(bus, logs, notices), false));
  CHECK(b.begin(std::make_unique<Loopback>(bus, 2), config(2), hooks(bus, logs, notices), false));
  for (unsigned f = 0; f < 180; ++f) {
    // Both games do the same loading/OS-time work. Only A waits for reports this frame.
    for (unsigned i = 0; i < f % 9; ++i) {
      CHECK(a.ticks(1) == b.ticks(999999)); a.poll(); b.poll();
    }
    a.retrace(); b.retrace();
    bus.release = bus.now + 25;
    const auto clock_before_wait = a.clock_ticks();
    CHECK(a.advance(hardware(f), {})); CHECK(b.advance(hardware(f + 1000), {}));
    CHECK(a.clock_ticks() == clock_before_wait);
    CHECK(a.clock_ticks() == b.clock_ticks());
    CHECK(a.boot_ticks(1) + a.ticks(1) == b.boot_ticks(999) + b.ticks(999));
    CHECK(a.retrace_count() == f + 1 && a.frame() == f);
  }
  CHECK(bus.now == 4500); CHECK(notices.empty());
}
void timeout_and_loss(bool immediate) {
  ++cases;
  Bus bus; Engine a; std::vector<std::string> logs, notices;
  CHECK(a.begin(std::make_unique<Loopback>(bus, 1), config(1), hooks(bus, logs, notices), false));
  bus.drop = true; bus.lost = immediate;
  bus.digests[1][0] = Engine::state_digest({}); // isolate the pad timeout from digest delivery
  CHECK(!a.advance(hardware(0), {})); CHECK(!a.active()); CHECK(bus.now <= 8000);
  CHECK(immediate || bus.now == 8000);
  Pads out; a.read(hardware(9, 3), out);
  CHECK(equal(out[0], hardware(9, 3)[3]));
  for (int p = 1; p < 4; ++p) CHECK(out[p].err == -1 && out[p].button == 0);
  CHECK(notices.size() == 1 && notices[0] == "Lost connection to your friend.");
  a.end("again"); CHECK(notices.size() == 1);
  CHECK(logs.back() == (immediate ? "console: session ended: peer left" :
      "console: session ended: pad wait timed out at frame 0"));
  std::cout << logs.back() << '\n';
}
void desync() {
  ++cases;
  Bus bus; Engine a; std::vector<std::string> logs, notices;
  CHECK(a.begin(std::make_unique<Loopback>(bus, 1), config(1), hooks(bus, logs, notices), false));
  bus.pads[1][0] = {}; CHECK(a.advance(hardware(0), {}));
  // Late digest arrival is compared on a later body, against the stored boundary digest.
  bus.digests[1][0] = Engine::state_digest({}) ^ 1;
  CHECK(!a.advance(hardware(1), {})); CHECK(!a.active());
  const auto mismatch = std::find_if(logs.begin(), logs.end(), [](const auto& line) {
    return line.rfind("console: desync at frame 0 with port 2 (mine ", 0) == 0;
  });
  CHECK(mismatch != logs.end() && mismatch->find(", theirs ") != std::string::npos);
  CHECK(mismatch[-1] == "console: rollback setting differs between seats");
  CHECK(std::count_if(logs.begin(), logs.end(), [](const auto& line) {
    return line.rfind("console: hash 0 ", 0) == 0;
  }) == 1);
  CHECK(logs.back() == "console: session ended: desync");
  CHECK(notices.size() == 1 && notices[0] == "Your game and your friend's game went out of sync.");
  MuStatePod state{}; auto digest = Engine::state_digest(state);
  CHECK(digest == 0xf05e74aa1eda9c25ull); // switch OFF retains the exact Round A hash
  state.menu_selection = 1; CHECK(digest != Engine::state_digest(state));
  state = {}; state.rng = 1; CHECK(digest != Engine::state_digest(state));
  state = {}; state.scene = 1; CHECK(digest != Engine::state_digest(state));
  state = {}; state.player[0].pos_x = 1; CHECK(digest != Engine::state_digest(state));
  state = {}; state.console_valid = 1; digest = Engine::state_digest(state);
  for (size_t word = 81; word < 103; ++word) {
    auto changed = state;
    reinterpret_cast<uint32_t*>(&changed)[word] = 1;
    CHECK(Engine::state_digest(changed) != digest);
  }
  std::cout << *mismatch << '\n' << logs.back() << '\n';
}
void legacy_digest_layout() {
  ++cases;
  struct LegacyPod {
    uint32_t rng, scene; MuFighterState player[6];
    uint32_t scene_major, match_frame, menu_kind, menu_previous, menu_selection, menu_entering;
  };
  static_assert(sizeof(LegacyPod) == 80 * 4);
  static_assert(offsetof(MuStatePod, console_valid) == sizeof(LegacyPod));
  static_assert(sizeof(MuStatePod) == 103 * 4);
  uint32_t value = 0x12345678;
  // Independent legacy FNV over all 80 words; extension garbage must not enter OFF hashes.
  for (int sample = 0; sample < 100; ++sample) {
    MuStatePod state{}; uint64_t expected = 14695981039346656037ull;
    for (size_t word = 0; word < 103; ++word) {
      value = value * 1664525 + 1013904223;
      std::memcpy(reinterpret_cast<char*>(&state) + word * 4, &value, 4);
      if (word < 80) for (unsigned shift = 0; shift < 32; shift += 8) {
        expected ^= (value >> shift) & 255; expected *= 1099511628211ull;
      }
    }
    state.console_valid = 0;
    CHECK(Engine::state_digest(state) == expected);
  }
}
void rollback_switches() {
  ++cases;
  Bus default_bus; Engine default_engine; std::vector<std::string> log, notices;
  CHECK(default_engine.begin(std::make_unique<Loopback>(default_bus, 1), config(1), hooks(default_bus, log, notices)));
  CHECK(default_engine.rollback_enabled() == kConsoleRollbackDefault);
  CHECK(rollback_setting(nullptr) == kConsoleRollbackDefault);
  CHECK(rollback_setting("") == kConsoleRollbackDefault && rollback_setting("invalid") == kConsoleRollbackDefault);
  CHECK(rollback_setting("1") && !rollback_setting("0"));
  for (const auto modes : {std::pair{false, false}, std::pair{true, true}, std::pair{true, false}}) {
    ++cases;
    Bus bus; Engine a, b; std::vector<std::string> alog, blog, notices;
    // Real readiness barrier, with the other seat admitted by the responsive hook.
    auto link = std::make_unique<Loopback>(bus, 1); link->frame_zero_barrier = true;
    auto ah = hooks(bus, alog, notices);
    ah.responsive = [&] { if (bus.digests[1].empty()) b.advance({}, {}); };
    CHECK(a.begin(std::move(link), config(1), ah, modes.first));
    CHECK(b.begin(std::make_unique<Loopback>(bus, 2), config(2), hooks(bus, blog, notices), modes.second));
    const bool match = modes.first == modes.second;
    CHECK(a.advance({}, {}) == match);
    if (match) {
      CHECK(b.advance({}, {}));
      CHECK(a.active() && b.active() && bus.digests[0].at(0) == bus.digests[1].at(0));
      if (!modes.first) CHECK(bus.digests[0].at(0) == 0xf05e74aa1eda9c25ull);
      else CHECK(bus.digests[0].at(0) != Engine::state_digest({}));
      // The compatibility marker belongs only to zero, even with rollback enabled in menus.
      for (unsigned f = 1; f <= 60; ++f) { CHECK(a.advance({}, {})); CHECK(b.advance({}, {})); }
      CHECK(bus.digests[0].at(60) == Engine::state_digest({}));
      CHECK(bus.digests[0].at(60) == bus.digests[1].at(60));
    } else {
      CHECK(!b.advance({}, {})); CHECK(!a.active() && !b.active());
      for (const auto* log : {&alog, &blog}) {
        const auto setting = std::find(log->begin(), log->end(), "console: rollback setting differs between seats");
        CHECK(setting != log->end());
        CHECK(setting + 2 < log->end() && (setting + 1)->find("console: desync at frame 0 ") == 0);
        CHECK(*(setting + 2) == "console: session ended: desync");
      }
    }
    CHECK(std::count(alog.begin(), alog.end(), modes.first ? "console: rollback on" : "console: rollback off (lockstep)") == 1);
    CHECK(std::count(blog.begin(), blog.end(), modes.second ? "console: rollback on" : "console: rollback off (lockstep)") == 1);
  }
}
void inert() {
  ++cases;
  Engine e; Pads in, out;
  std::memset(in.data(), 0xA5, sizeof in);
  e.read(in, out); CHECK(std::memcmp(in.data(), out.data(), sizeof in) == 0);
  CHECK(!e.advance(in, {})); CHECK(e.ticks(54321) == 54321); CHECK(e.boot_ticks(98765) == 98765);
  e.poll(); e.retrace(); CHECK(e.clock_ticks() == 0); CHECK(e.retrace_count() == 0);
  CHECK(!e.retrace_due()); CHECK(e.ticks(54321) == 54321);
  CHECK(!console_active()); CHECK(!console_active());
  CHECK(!e.rollback_enabled()); CHECK(!e.begin_match()); CHECK(!e.replay(0));
  CHECK(!Engine::valid(SessionConfig{})); auto c = config(1);
  c.ports = {1, 1}; CHECK(!Engine::valid(c)); c = config(1); c.session_id = "../escape"; CHECK(!Engine::valid(c));
  c = config(1); c.input_delay = -1; CHECK(!Engine::valid(c));
}
void retained_inputs() {
  ++cases;
  Bus bus; Engine e; std::vector<std::string> logs, notices;
  CHECK(e.begin(std::make_unique<Loopback>(bus, 1), config(1), hooks(bus, logs, notices), false));
  CHECK(!e.rollback_enabled() && !e.begin_match()); // switch off preserves the lockstep backend
  for (unsigned f = 0; f < 10; ++f) bus.pads[1][f] = hardware(f + 100)[2];
  CHECK(e.advance(hardware(0), {}));
  e.enable_rollback(true); CHECK(e.begin_match());
  CHECK(e.match_origin() == 1 && e.match_epoch() == 1);
  for (unsigned f = 1; f < 6; ++f) CHECK(e.advance(hardware(f), {}));
  const auto sends = bus.sends[0];
  CHECK(e.replay(1)); CHECK(e.frame() == 2); CHECK(bus.sends[0] == sends);
  e.end_match(); // an earlier corrected boundary leaves already issued future samples authoritative
  CHECK(!e.needs_sample());
  CHECK(e.advance(hardware(999), {})); CHECK(bus.sends[0] == sends);
  Pads out; e.read({}, out); CHECK(equal(out[0], hardware(0)[2]));
  CHECK(equal(bus.pads[0].at(6), hardware(3)[2]));
  // Reordered/changed duplicate reports cannot replace a confirmed input in retained history.
  bus.pads[1][4] = hardware(999)[2];
  CHECK(e.begin_match()); CHECK(e.advance({}, {}));
  CHECK(e.replay(0)); e.read({}, out); CHECK(equal(out[1], hardware(104)[2]));
  CHECK(notices.empty());
}
void terminal_wait_failure() {
  for (bool lost : {false, true}) {
    ++cases;
    Bus bus; Engine e; std::vector<std::string> logs, notices;
    CHECK(e.begin(std::make_unique<Loopback>(bus, 1), config(1), hooks(bus, logs, notices), false));
    bus.pads[1][0] = {}; bus.pads[1][1] = {}; bus.digests[1][0] = Engine::state_digest({});
    CHECK(e.advance({}, {}));
    e.enable_rollback(true); CHECK(e.begin_match());
    bus.digests[1][1] = Engine::state_digest({});
    CHECK(e.advance({}, {})); CHECK(e.boundary(0, {})); CHECK(e.finish_body(0));
    // b=0 is confirmed at match entry; create a missing b=1 only after that bootstrap.
    CHECK(e.advance({}, {})); CHECK(e.boundary(1, {})); CHECK(e.finish_body(1));
    const auto clock = e.clock_ticks(); const auto frame = e.frame(); const auto sends = bus.sends[0];
    bus.lost = lost;
    CHECK(!e.confirm(1)); CHECK(!e.active()); CHECK(e.clock_ticks() == clock && e.frame() == frame);
    CHECK(bus.sends[0] == sends); CHECK(notices.size() == 1);
    CHECK(lost || bus.now == 8000);
  }
}
void sparse_ports_and_delay() {
  for (int delay : {1, 3, 7}) {
    ++cases;
    Bus bus; Engine a, b; std::vector<std::string> logs, notices;
    auto ca = config(2, delay), cb = config(4, delay); ca.ports = cb.ports = {2, 4};
    CHECK(a.begin(std::make_unique<Loopback>(bus, 2), ca, hooks(bus, logs, notices), false));
    CHECK(b.begin(std::make_unique<Loopback>(bus, 4), cb, hooks(bus, logs, notices), false));
    for (int f = 0; f < 30; ++f) {
      CHECK(a.advance(hardware(f, 3), {})); CHECK(b.advance(hardware(f + 50, 1), {}));
      Pads x, y; a.read({}, x); b.read({}, y);
      for (int p = 0; p < 4; ++p) CHECK(equal(x[p], y[p]));
      CHECK(x[0].err == -1 && x[2].err == -1);
      CHECK(equal(x[1], f < delay ? MuPadStatus{} : hardware(f - delay, 3)[3]));
      CHECK(equal(x[3], f < delay ? MuPadStatus{} : hardware(f - delay + 50, 1)[1]));
    }
    a.end("test"); Pads out; a.read(hardware(8, 3), out);
    CHECK(out[0].err == -1 && out[2].err == -1 && out[3].err == -1);
    CHECK(equal(out[1], hardware(8, 3)[3]));
  }
}
void four_players() {
  ++cases;
  Bus bus; Engine engines[4]; std::vector<std::string> logs, notices;
  for (int p = 1; p <= 4; ++p) {
    auto c = config(p); c.ports = {1, 2, 3, 4};
    CHECK(engines[p - 1].begin(std::make_unique<Loopback>(bus, p), c, hooks(bus, logs, notices), false));
  }
  for (unsigned f = 0; f < 125; ++f) {
    for (int p = 0; p < 4; ++p) CHECK(engines[p].advance(hardware(f + p * 100), {}));
    Pads expected; engines[0].read({}, expected);
    for (int p = 0; p < 4; ++p) {
      Pads actual; engines[p].read({}, actual);
      for (int q = 0; q < 4; ++q) CHECK(equal(expected[q], actual[q]));
      CHECK(equal(expected[p], f < 3 ? MuPadStatus{} : hardware(f - 3 + p * 100)[2]));
    }
  }
  CHECK(notices.empty());
}
void reentrant_close() {
  ++cases;
  Bus bus; Engine a; std::vector<std::string> logs, notices;
  auto h = hooks(bus, logs, notices);
  h.responsive = [&] { a.end("window closed"); };
  CHECK(a.begin(std::make_unique<Loopback>(bus, 1), config(1), h, false));
  CHECK(!a.advance({}, {})); CHECK(!a.active()); CHECK(bus.now == 0);
}
void missing_digest() {
  ++cases;
  Bus bus; Engine a; std::vector<std::string> logs, notices;
  CHECK(a.begin(std::make_unique<Loopback>(bus, 1), config(1), hooks(bus, logs, notices), false));
  bus.pads[1][0] = {}; CHECK(a.advance(hardware(0), {}));
  bus.now = 8000; bus.pads[1][1] = {};
  CHECK(!a.advance(hardware(1), {})); CHECK(!a.active());
  CHECK(logs.back() == "console: session ended: state digest timed out");
}
void menu_hint_and_stalls() {
  ++cases;
  for (bool lockstep : {false,true}) for (bool rollback : {false,true})
    for (unsigned major : {1u,2u,8u,0x10u}) for (unsigned minor : {0u,1u,2u,3u})
      for (size_t count : {2u,4u})
        CHECK(honest_menu(lockstep,rollback,count,major,minor) ==
              (lockstep && rollback && count == 2 && (major == 2 || major == 0x10) && minor <= 1));
  CHECK(std::string(kMenuHint) == "Menus are delay-based for now. Matches use rollback.");
  MenuHintAnimation hint;
  CHECK(hint.alpha(false,1) == 0); CHECK(hint.alpha(true,2) == 0);
  CHECK(hint.alpha(true,2.1) > 0 && hint.alpha(true,2.1) < 1);
  CHECK(hint.alpha(true,2.75) > hint.alpha(true,2.375));
  CHECK(hint.alpha(true,3.49) > hint.alpha(true,3.125));
  CHECK(hint.alpha(true,3.5) == 1); CHECK(hint.alpha(false,4) == 0);
  CHECK(hint.alpha(true,5) == 1); // repeated CSS entry never pulses again
  ++cases;
  Bus bus; Engine a; std::vector<std::string> logs, notices;
  CHECK(a.begin(std::make_unique<Loopback>(bus,1),config(1,2),hooks(bus,logs,notices)));
  bus.echo_digest = true;
  CHECK(a.menus_lockstep());
  auto run = [&](unsigned f,uint64_t at,uint64_t wait) {
    bus.now = at; bus.release = at+wait; bus.pads[1][f] = {};
    CHECK(a.advance(hardware(f),{}));
  };
  // Nonzero start avoids the rollback-tagged frame-zero digest from being checked here.
  run(0,1,25);
  run(1,30001,0); run(2,60001,40);
  CHECK(std::count(logs.begin(),logs.end(),"console: menu stalls 2/min (longest 40 ms, delay 2)") == 1);
  run(3,61001,0);
  CHECK(std::count_if(logs.begin(),logs.end(),[](const std::string& s) { return s.find("console: menu stalls ") == 0; }) == 1);
  run(4,120041,0);
  CHECK(std::count(logs.begin(),logs.end(),"console: menu stalls 0/min (longest 0 ms, delay 2)") == 1);
  CHECK(a.begin_match()); CHECK(!a.menus_lockstep());
  run(5,180001,0);
  CHECK(std::count_if(logs.begin(),logs.end(),[](const std::string& s) { return s.find("console: menu stalls ") == 0; }) == 2);
  a.end_match(); CHECK(a.menus_lockstep());
  run(6,240001,0); // a fresh menu window, excluding match time
  CHECK(std::count_if(logs.begin(),logs.end(),[](const std::string& s) { return s.find("console: menu stalls ") == 0; }) == 2);
}
void card(const std::filesystem::path& root) {
  ++cases;
  auto data = root / ("console-engine-test-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(data);
  std::filesystem::path path; CHECK(prepare_card(data, config(1), path));
  CHECK(path == data / "console" / "engine-test" / "card"); CHECK(std::filesystem::is_empty(path));
  std::ofstream(path / "session-save.gci") << "test";
  CHECK(!prepare_card(data, config(1), path));
  auto c = config(1); c.session_id = "../../ordinary-card"; CHECK(!prepare_card(data, c, path));
  c = config(1); c.session_id = "symlink";
  std::error_code ec;
  std::filesystem::create_directory_symlink(data, data / "console" / "symlink", ec);
  if (!ec) CHECK(!prepare_card(data, c, path)); // Windows may require a developer-mode grant
  // Leave these isolated fixtures for inspection; never delete pre-existing files.
}
#include "yougame_console_rollback_test.inc"
}
int32_t mu_slippi_command(uint8_t command, const uint8_t* payload, uint32_t size,
                         uint8_t*, uint32_t, uint32_t* response_size) {
  using namespace rollback_test;
  CHECK(size == 32); CHECK(command == 0xB1 || command == 0xB2);
  for (unsigned i = 4; i < size; ++i) CHECK(payload[i] == 0);
  *response_size = 0;
  if (command == 0xB1) {
    current->snapshots.capture(be32(payload)); current->session.capture_clock(be32(payload)); return 0;
  }
  if (current->missing) current->snapshots.end();
  if (!current->snapshots.load(be32(payload))) return -1;
  std::copy(std::begin(current->state.rumble), std::end(current->state.rumble), HSD_Rumble_804C22E0);
  return current->session.restore_clock(be32(payload)) ? 0 : -1;
}
int main(int argc, char** argv) {
  try {
    boot_barrier(); same_stream(); delay_and_reorder(); clocks_follow_calls();
    timeout_and_loss(false); timeout_and_loss(true); desync(); inert();
    sparse_ports_and_delay(); four_players(); reentrant_close(); missing_digest();
    retained_inputs(); terminal_wait_failure(); rollback_switches(); legacy_digest_layout();
    rollback_test::all(); menu_hint_and_stalls();
    if (argc > 1) card(argv[1]);
    std::cout << cases << " cases, " << assertions << " assertions passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
