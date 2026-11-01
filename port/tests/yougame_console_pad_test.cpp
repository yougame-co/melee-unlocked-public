// Compile the production PADRead/console boundary against only SDK type stubs.
#include "yougame_console_engine.h"
#include <dolphin/pad.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
int mu_online_motor_port(s32 port, u32) { return port; }
int mu_console_rollback_active(void);
void mu_apply_seed(uint32_t);
void OSSetArenaLo(void*) {}
void OSSetArenaHi(void*) {}
#include "../../sourceport/game/shim/mu_os.c"
#include "../../sourceport/game/shim/mu_vi.c"
#include "../../sourceport/game/shim/mu_pad.c"
#include "../../sourceport/game/shim/mu_console_pad.c"
PadLibData HSD_PadLibData{};
}
const MuHostApi* mu_host;
using namespace yougame::console;
namespace {
Engine engine;
Pads hardware{};
int checks, seeds, alarm_samples, vi_callbacks, bodies, once_fired;
bool rollback_body = false, rollback_setup = false;
uint64_t ordinary_ticks = 0;
struct RawRows {
  PADStatus* operator[](size_t i) { return HSD_PadLibData.queue[i].stat; }
} raw;
int rumble_interprets = 0;

#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Fake : SessionLink {
  void send_pad(uint32_t, const MuPadStatus&) override {}
  std::optional<MuPadStatus> pad(int, uint32_t f) override { MuPadStatus p{}; p.button = f + 500; return p; }
  void send_digest(uint32_t, uint64_t) override {}
  std::optional<uint64_t> digest(int, uint32_t) override { return {}; }
  void pump(int) override {}
  std::optional<std::string> lost() const override { return {}; }
};
void read(MuPadStatus out[4]) { Pads pads; engine.read(hardware, pads); std::copy(pads.begin(), pads.end(), out); }
int32_t begin() { return engine.advance(hardware, {}); }
int32_t seed(uint32_t* out) { if (!engine.active()) return 0; *out = 123; return 1; }
uint64_t ticks() { return engine.ticks(ordinary_ticks); }
uint64_t boot() { return engine.boot_ticks(123456); }
int32_t alarm_clock(uint64_t* simulation, uint64_t* hardware) {
  if (!engine.rollback_enabled()) return 0;
  *simulation = engine.servicing() ? engine.alarm_ticks() : engine.clock_ticks();
  *hardware = engine.alarm_ticks(); return 1;
}
int32_t service_clock(int32_t op, uint32_t body, MuConsoleInputs*) {
  if (!engine.rollback_enabled() || op != MU_CONSOLE_SERVICE) return 0;
  engine.service_clock(body != 0); return 1;
}
void time_wait(unsigned steps, bool explicit_poll);
void clock_event(void*, intptr_t) {
  const auto before = engine.clock_ticks();
  time_wait(12, false);
  CHECK(engine.clock_ticks() == before);
}
uint32_t retraces() { return engine.active() ? engine.retrace_count() : 123; }
uint32_t field() { return retraces() & 1; }
// The same console branches as source_host.cpp; actual OS alarm/VI shims execute below.
void retrace() { engine.retrace(); mu_fire_alarms(engine.alarm_ticks()); mu_vi_retrace(); }
void poll() {
  engine.poll();
  if (engine.retrace_due()) retrace();
  else mu_fire_alarms(engine.alarm_ticks());
}
void pad_alarm(OSAlarm*, OSContext*) { ++alarm_samples; HSD_PadRenewRawStatus(0); }
void once_alarm(OSAlarm*, OSContext*) { ++once_fired; }
void vi_callback(u32 n) { CHECK(n == engine.retrace_count()); ++vi_callbacks; }
void time_wait(unsigned steps, bool explicit_poll) {
  const auto start = OSGetTime();
  unsigned polls = 0;
  while (OSGetTime() - start < OSTime(steps * Engine::poll_ticks)) {
    if (explicit_poll) mu_poll();
    CHECK(++polls <= steps);
  }
  CHECK(polls == (explicit_poll ? steps / 2 : steps - 1));
}
void off_alarm_equivalence() {
  // No console rollback: retain absolute/relative/periodic scheduling and interrupt delivery.
  CHECK(!engine.rollback_enabled());
  OSInitAlarm();
  const auto now = ticks(), epoch = boot();
  OSAlarm relative{}, absolute{}, periodic{};
  OSCreateAlarm(&relative); OSCreateAlarm(&absolute); OSCreateAlarm(&periodic);
  OSSetAlarm(&relative, 11, once_alarm);
  OSSetAbsAlarm(&absolute, epoch + now + 13, once_alarm);
  OSSetPeriodicAlarm(&periodic, epoch + now - 20, 10, once_alarm);
  CHECK(relative.fire == OSTime(now + 11) && relative.period == 0);
  CHECK(absolute.fire == OSTime(now + 13) && absolute.period == 0);
  CHECK(periodic.start == OSTime(now - 20) && periodic.fire == OSTime(now + 10));
  CHECK(periodic.period == 10);
  const auto fired = once_fired;
  mu_fire_alarms(now + 11); CHECK(once_fired == fired + 2);
  CHECK(periodic.fire == OSTime(now + 20));
  mu_fire_alarms(now + 13); CHECK(once_fired == fired + 3);
  mu_alarms_hold(); periodic.fire = 0; periodic.period = 0; mu_alarms_release();
  CHECK(periodic.fire == OSTime(now + 20) && periodic.period == 10);
  OSCancelAlarm(&periodic); CHECK(!OSCheckAlarmQueue());
  const auto disabled = OSDisableInterrupts();
  mu_post([](void*, intptr_t) { ++once_fired; }, nullptr, 0);
  mu_deliver_pending(); CHECK(once_fired == fired + 3);
  OSRestoreInterrupts(disabled); CHECK(once_fired == fired + 4);
  CHECK(!engine.servicing());
}
void fields(const PADStatus& game, const MuPadStatus& host) {
  CHECK(game.button == host.button); CHECK(game.err == host.err);
  CHECK(game.stickX == host.stick_x && game.stickY == host.stick_y);
  CHECK(game.substickX == host.sub_x && game.substickY == host.sub_y);
  CHECK(game.triggerLeft == host.trigger_l && game.triggerRight == host.trigger_r);
  CHECK(game.analogA == host.analog_a && game.analogB == host.analog_b);
}
}
extern "C" void mu_apply_seed(uint32_t value) { CHECK(value == 123); ++seeds; }
extern "C" int mu_console_rollback_active(void) { return rollback_body; }
extern "C" int mu_console_rumble_owned(void) { return rollback_body || rollback_setup; }
extern "C" int mu_online_pad_renew(PADStatus* p) { PADRead(p); return 0; }
extern "C" void HSD_PadRumbleInterpret(void) { ++rumble_interprets; }
extern "C" void HSD_PadRenewRawStatus(BOOL) {
  if (!mu_console_rumble_owned()) HSD_PadRumbleInterpret();
  auto& q = HSD_PadLibData;
  PADRead(raw[q.qwrite]);
  q.qwrite = (q.qwrite + 1) % 16;
  if (q.qcount == 16) q.qread = (q.qread + 1) % 16;
  else ++q.qcount;
}
int main() {
  MuHostApi host{}; host.pad_read = read; host.console_frame_begin = begin; host.console_seed = seed;
  host.version = MU_HOST_API_VERSION; host.ticks = ticks; host.boot_time = boot; host.poll = poll;
  host.console_alarm_clock = alarm_clock;
  host.console_rollback = service_clock;
  host.vi_wait_retrace = retrace; host.vi_retrace_count = retraces; host.vi_next_field = field;
  mu_host = &host;
  for (auto& p : hardware) p.err = -1;
  hardware[3] = {42, 80, -50, -70, 30, 255, 23, 45, 67, 0};
  PADStatus pads[4]{};
  CHECK(!mu_console_frame_begin()); CHECK(!mu_console_active()); CHECK(seeds == 0);
  HSD_PadLibData = {3, 7, 4};
  int count = 4;
  CHECK(!mu_console_engine_gate(&count)); CHECK(count == 4);
  CHECK(HSD_PadLibData.qread == 3 && HSD_PadLibData.qwrite == 7 && HSD_PadLibData.qcount == 4);
  CHECK(OSGetTime() == 123456); CHECK(OSGetTick() == 0);
  ordinary_ticks = 999; CHECK(OSGetTime() == 124455); CHECK(OSGetTick() == 999);
  off_alarm_equivalence();
  HSD_PadLibData = {};
  CHECK(PADRead(pads) == 0x10000000u);
  for (int i = 0; i < 4; ++i) fields(pads[i], hardware[i]);
  SessionConfig config; config.session_id = "sdk-pad-test"; config.ports = {1, 2};
  config.epoch_ms = 1793448000000LL; config.seed = 123;
  EngineHooks hooks; hooks.now_ms = [] { return uint64_t(0); };
  CHECK(engine.begin(std::make_unique<Fake>(), config, hooks, false));
  // Boot/loading and even an in-body pure OSGetTime spin make progress without a retrace/body.
  time_wait(12, false); time_wait(12, true);
  CHECK(engine.frame() == 0 && engine.retrace_count() == 0);
  VISetPreRetraceCallback(vi_callback);
  OSAlarm alarm{}; OSCreateAlarm(&alarm);
  OSSetPeriodicAlarm(&alarm, OSGetTime() + Engine::frame_ticks, Engine::frame_ticks, pad_alarm);
  for (int f = 0; f < 8; ++f) {
    hardware[3].button = f + 10;
    CHECK(mu_console_frame_begin()); CHECK(mu_console_active()); CHECK(seeds == 1);
    CHECK(PADRead(pads) == 0xC0000000u);
    const auto before = engine.frame(); PADStatus again[4]{};
    hardware[3].button = 999;
    CHECK(PADRead(again) == 0xC0000000u); CHECK(engine.frame() == before);
    for (int i = 0; i < 4; ++i) {
      MuPadStatus value{pads[i].button, pads[i].stickX, pads[i].stickY,
          pads[i].substickX, pads[i].substickY, pads[i].triggerLeft, pads[i].triggerRight,
          pads[i].analogA, pads[i].analogB, pads[i].err};
      fields(again[i], value);
    }
    CHECK(pads[0].button == (f < 3 ? 0 : f - 3 + 10));
    CHECK(pads[1].button == f + 500); CHECK(pads[2].err == -1 && pads[3].err == -1);
  }
  HSD_PadLibData = {}; // all initial samples drained: the original scene-gate deadlock
  for (int scene = 0; scene < 4; ++scene) { // menus, loading, match, paused match use the same loop
    if (scene) {
      time_wait(20, false); time_wait(20, true);
      // Loading can produce a backlog. It must never become a multi-body catch-up batch.
      for (int i = 0; i < 20; ++i) VIWaitForRetrace();
      CHECK(HSD_PadLibData.qcount == 16);
    }
    for (int f = 0; f < 60; ++f) {
      if (f == 40) VIWaitForRetrace(); // rendering can already have waited for this body's VI
      if (f == 10) OSCancelAlarm(&alarm); // scheduler cannot depend on the sampling period
      int queued;
      unsigned waits = 0;
      const auto before = engine.clock_ticks();
      const auto vi_before = engine.retrace_count();
      for (;;) { // gm_801A4D34: query raw queue, gate, then poll if it is still empty
        queued = HSD_PadLibData.qcount;
        if (mu_console_engine_gate(&queued)) break;
        mu_poll(); CHECK(++waits < 2);
      }
      CHECK(waits == 0); CHECK(queued == 1); CHECK(HSD_PadLibData.qcount == 1);
      const unsigned new_vi = ((scene && f == 0) || f == 40) ? 0 : 1;
      CHECK(engine.clock_ticks() == before + new_vi * Engine::frame_ticks);
      CHECK(engine.retrace_count() == vi_before + new_vi);
      mu_poll(); // lb_800195D0, also called once after the gate succeeds
      for (int i = 0; i < queued; ++i) {
        const auto previous_frame = engine.frame();
        CHECK(mu_console_frame_begin()); CHECK(engine.frame() == previous_frame + 1);
        auto& q = HSD_PadLibData;
        CHECK(q.qcount == 1);
        const auto& confirmed = raw[q.qread];
        CHECK(confirmed[1].button == engine.frame() + 500);
        CHECK(confirmed[2].err == -1 && confirmed[3].err == -1);
        q.qread = (q.qread + 1) % 16; --q.qcount; ++bodies;
        if (f == 30) time_wait(14, false); // OS-time polling inside a body must also terminate
      }
      CHECK(HSD_PadLibData.qcount == 0);
      if (f == 11)
        OSSetPeriodicAlarm(&alarm, OSGetTime() + Engine::frame_ticks, Engine::frame_ticks, pad_alarm);
    }
    // Alarm/VI waits outside bodies still work, without assigning a lockstep frame.
    const auto saved_frame = engine.frame();
    OSAlarm once{}; OSCreateAlarm(&once);
    OSSetAlarm(&once, 5 * Engine::poll_ticks, once_alarm);
    const int once_before = once_fired;
    for (int i = 0; i < 5; ++i) mu_poll();
    CHECK(once_fired == once_before + 1);
    CHECK(engine.frame() == saved_frame);
  }
  CHECK(bodies == 240); CHECK(engine.frame() == 247); CHECK(seeds == 1);
  // A loading loop using only HOST_POLL must drive VI even without a scene gate or body.
  const auto loading_frame = engine.frame(), loading_vi = engine.retrace_count();
  unsigned loading_polls = 0;
  while (engine.retrace_count() == loading_vi) {
    mu_poll(); CHECK(++loading_polls <= 330);
  }
  CHECK(engine.frame() == loading_frame); CHECK(engine.retrace_count() == loading_vi + 1);
  CHECK(vi_callbacks == int(engine.retrace_count())); CHECK(alarm_samples >= 280);
  // An earlier native rollback hook owns the injected queue. The unconditional TE hook must
  // leave it byte-for-byte intact and cannot advance, reseed, send, or replace it on replay.
  rollback_setup = true;
  const auto before_setup_rumble = rumble_interprets;
  int setup_ticket = 0;
  CHECK(mu_console_engine_gate(&setup_ticket) && setup_ticket == 1);
  CHECK(rumble_interprets == before_setup_rumble);
  rollback_setup = false;
  rollback_body = true;
  const auto replay_frame = engine.frame();
  HSD_PadLibData = {2, 3, 1}; raw[2][1].button = 0x1234;
  CHECK(!mu_console_frame_begin()); CHECK(engine.frame() == replay_frame);
  CHECK(HSD_PadLibData.qread == 2 && HSD_PadLibData.qwrite == 3 && HSD_PadLibData.qcount == 1);
  CHECK(raw[2][1].button == 0x1234); CHECK(seeds == 1);
  const auto scripts = rumble_interprets;
  int ticket = 0;
  CHECK(mu_console_engine_gate(&ticket) && ticket == 1);
  CHECK(rumble_interprets == scripts); // VI/alarm and fallback ticket cannot interpret scripts
  mu_console_replace_raw(); CHECK(rumble_interprets == scripts);
  mu_console_rumble_step(); CHECK(rumble_interprets == scripts + 1);
  rollback_body = false;
  // The simulation clock restores; absolute/relative/periodic hardware schedules do not.
  OSCancelAlarm(&alarm);
  engine.enable_rollback(true); CHECK(engine.begin_match());
  CHECK(engine.advance(hardware, {})); CHECK(engine.boundary(0, {}));
  engine.ticks(0); engine.poll(); engine.capture_clock(0);
  mu_post(clock_event, nullptr, 0); mu_deliver_pending();
  OSAlarm relative{}, absolute{}, periodic{};
  OSCreateAlarm(&relative); OSCreateAlarm(&absolute); OSCreateAlarm(&periodic);
  const auto sim = engine.clock_ticks(), service = engine.alarm_ticks();
  OSSetAlarm(&relative, 5 * Engine::poll_ticks, once_alarm);
  OSSetAbsAlarm(&absolute, boot() + sim + 7 * Engine::poll_ticks, once_alarm);
  OSSetPeriodicAlarm(&periodic, boot() + sim + 9 * Engine::poll_ticks,
                     Engine::frame_ticks, once_alarm);
  CHECK(relative.fire == OSTime(service + 5 * Engine::poll_ticks));
  CHECK(absolute.fire == OSTime(service + 7 * Engine::poll_ticks));
  CHECK(periodic.fire == OSTime(service + 9 * Engine::poll_ticks));
  CHECK(engine.clock_ticks() == sim); // alarm registration peeks; no simulation reads
  CHECK(engine.finish_body(0));
  const auto original_deadline = periodic.fire;
  mu_alarms_hold();
  CHECK(engine.restore_clock(0));
  relative.fire = absolute.fire = periodic.fire = 0; // snapshot's obsolete hardware fields
  mu_alarms_release();
  CHECK(periodic.fire == original_deadline && periodic.period == OSTime(Engine::frame_ticks));
  CHECK(relative.fire == OSTime(service + 5 * Engine::poll_ticks));
  CHECK(absolute.fire == OSTime(service + 7 * Engine::poll_ticks));
  CHECK(engine.replay(0)); CHECK(engine.boundary(0, {}));
  engine.ticks(0); engine.poll(); CHECK(engine.clock_ticks() == sim);
  CHECK(engine.finish_body(0)); CHECK(engine.confirm(0)); CHECK(engine.validated(1)); CHECK(engine.commit_match(0, {}));
  OSCancelAlarm(&relative); OSCancelAlarm(&absolute); OSCancelAlarm(&periodic);
  engine.end("test"); CHECK(!mu_console_active()); CHECK(PADRead(pads) == 0x80000000u);
  CHECK(pads[0].button == 999); CHECK(pads[1].err == -1 && pads[2].err == -1 && pads[3].err == -1);
  CHECK(OSGetTime() == 124455); CHECK(OSGetTick() == 999);
  std::printf("console PAD/OS/VI shims: %d checks passed (8 pad frames + %d scene bodies, 4 scenes, time waits, alarms, inactive, loss)\n", checks, bodies);
}
