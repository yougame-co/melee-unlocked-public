"""Exercise the production paused CSS adoption loop at each game-thread failure point."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from yougame_console_wiring_test import body

ROOT = Path(__file__).resolve().parents[2]


class CssJoinAdoption(unittest.TestCase):
    def test_install_failure_and_peer_abort_release_the_lock_and_engine(self):
        source = (ROOT / 'port/app/source_host.cpp').read_text()
        fixture = r'''
#include "yougame_css_join.h"
#include <cassert>
#include <stdexcept>
#include <thread>
#include <cstdarg>
#include "friends_handoff_transfer.h"
namespace cj=yougame::console::css_join;
using namespace yougame::console;
static int failure, abort_at, ends, ons, offs, phase;
static std::atomic<bool> exiting{false};
static bool release_ok, threaded;
static int threaded_fault, delivered;
static std::vector<std::string> diagnostics;
static uint64_t wall;
static const auto main_thread=std::this_thread::get_id();
struct EngineHooks {
  std::function<void(const std::string&)> log;
  std::function<void(const char*)> notice;
  std::function<void()> responsive;
  std::function<uint64_t()> host_ticks;
  std::function<void(uint64_t)> host_catch_up;
};
static void console_host_catch_up(uint64_t) {}
struct FakeEngine {
  bool active_=false;
  std::unique_ptr<SessionLink> link;
  bool can_begin(const SessionConfig&) { return failure!=1; }
  bool begin(std::unique_ptr<SessionLink>&& incoming,const SessionConfig&,EngineHooks,bool) {
    if (failure==2) return false;
    link=std::move(incoming); active_=true;
    if (failure==3) throw std::runtime_error("injected");
    return true;
  }
  bool adopt_css_clock(uint64_t,uint32_t) {
    if (failure==4) return false;
    if (failure==5) throw std::runtime_error("injected");
    return true;
  }
  void css_join_message(const nlohmann::json& m) { assert(std::this_thread::get_id()==main_thread); if (active_ && link) link->css_join_message(m); }
  bool css_join_release() {
    assert(std::this_thread::get_id()==main_thread);
    if (!active_ || !link || link->lost()) return false;
    link->css_join_release();
    if (threaded && threaded_fault==6) end("peer left immediately after release");
    return true;
  }
  bool active() { return active_; }
  void end(const char*) { if (active_) ++ends; active_=false; link.reset(); }
} engine;
namespace yougame::console { FakeEngine& console_engine() { return engine; } }
static const bool kConsoleRollbackDefault=false;
struct FakeLink : SessionLink {
  bool handed=false;
  ~FakeLink() override { assert(std::this_thread::get_id()==main_thread); }
  void css_join_message(const nlohmann::json&) override { assert(std::this_thread::get_id()==main_thread); ++delivered; }
  void css_join_release() override { assert(std::this_thread::get_id()==main_thread); handed=true; }
  void send_pad(uint32_t,const MuPadStatus&) override {}
  std::optional<MuPadStatus> pad(int,uint32_t) override { return {}; }
  void send_digest(uint32_t,uint64_t) override {}
  std::optional<uint64_t> digest(int,uint32_t) override { return {}; }
  void pump(int) override {}
  std::optional<std::string> lost() const override { assert(std::this_thread::get_id()==main_thread); if (failure==6 && engine.active_) return "your friend disconnected"; return {}; }

};
static bool g_console_boot=false, g_replaying=false;
static int g_online_test_mode=-1, g_console_host_pad=0;
namespace source_p2p { bool requested() { return false; } }
namespace screen_label { void show(const char*,double) {} }
namespace native_savestate { uint64_t hash_spans(int) { return 0; } }
struct Boot { bool ready() { return true; } };
struct { Boot boot; bool armed=true,pending=false,catching_up=false; uint64_t frame=6; int spans=0; std::vector<uint8_t> card; } g_css_origin;
static int practice(int,int32_t* out,int) { out[0]=2; out[1]=0; return 0; }
static int capture_mode=0;
static MuCssState state() {
  MuCssState s{}; s.version=MU_CSS_VERSION; s.size=MU_CSS_SIZE;
  s.rules[1]=0x34; s.rules[4]=4; s.rules[6]=10;
  mu_css_write_le(s.rules+20,0xffffffff,4); mu_css_write_le(s.item_mask,~0ull,8); mu_css_write_le(s.stage_mask,0xffffffff,4);
  for (auto& p:s.ports) { p.fighter=255; p.kind=3; p.tag_index=120; p.auto_handicap=p.handicap=p.cpu=5; }
  return s;
}
static int css_state(int op,void* bytes,unsigned) {
  if (op==MU_CSS_CAPTURE || op==MU_CSS_CAPTURE_CHECKED || op==MU_CSS_IDENTITY) {
    auto s=state();
    if (capture_mode==1) mu_css_write_le(s.ports[1].cursor_x,0x41e40000,4); // 28.5: retail runtime refuses
    if (capture_mode==2) mu_css_write_le(s.ports[1].cursor_x,0x41f00000,4); // 30: wire refuses
    *static_cast<MuCssState*>(bytes)=s;
    return capture_mode ? MU_CSS_NOT_TRANSFERABLE : 0;
  }
  return 0;
}
struct { decltype(&::practice) practice=::practice; decltype(&::css_state) css_state=::css_state; } g_game;
namespace host {
struct Cpu { uint64_t tb=99; } cpu_value,*cpu=&cpu_value;
bool exit_requested() { return exiting; }
uint32_t retrace_count() { return 10; }
void log(const char* text,...) {
  char line[256]; va_list args; va_start(args,text); vsnprintf(line,sizeof line,text,args); va_end(args);
  diagnostics.emplace_back(line);
  if (std::string(text)=="css-join lock: on") ++ons;
  if (std::string(text)=="css-join lock: off") ++offs;
}
void window_pump() {
  if (threaded) {
    if (threaded_fault==1 && engine.active_) exiting=true;
    return;
  }
  auto ch=std::atomic_load(&cj::pending);
  auto r=std::atomic_load(&ch->result);
  if (!r || r->serial!=unsigned(phase)) return;
  if (!r->ok || abort_at==phase) { ch->cancelled=true; return; }
  if (abort_at==10) { exiting=true; return; }
  if (abort_at==11) { wall=100000; return; }
  auto q=std::make_shared<cj::Request>(); q->serial=++phase;
  q->frame=6;
  q->operation=phase==2?cj::Operation::Prepare:phase==3?cj::Operation::Plug:cj::Operation::Release;
  std::atomic_store(&ch->request,std::shared_ptr<const cj::Request>(q));
}
}
uint64_t GetTickCount64() { return wall; }
bool queue_css_origin_reset(const std::vector<uint8_t>&) { return true; }
int css_join_tick() {
''' + body(source, 'css_join_tick') + r'''
}
// Run the real setup protocol concurrently with the production main-thread adoption loop.
// Any Link method/destructor on setup's thread aborts the fixture.
struct ProtocolIO {
  std::deque<nlohmann::json> messages;
  uint64_t clock() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  bool cancelled() { return exiting.load(); } bool room_closed() { return false; }
  std::function<void(bool)> css_lock=[](bool on) { cj::locked=on; };
  std::function<bool(int)> flush_room=[](int) { return true; };
  void wait(int n) { std::this_thread::sleep_for(std::chrono::milliseconds(n)); }
  void log(const std::string&) {}
  void queue(cj::Message m) { auto j=cj::envelope(m,"session","host"); j["from"]="guest"; messages.push_back(j); }
  bool send_room(const nlohmann::json& j) {
    auto received=j; received["from"]="host"; cj::Message m;
    assert(cj::receive(received,"session","host",m));
    if (m.kind==cj::Kind::Installed && threaded_fault>=2 && threaded_fault<=5) {
      if (threaded_fault==2) messages.push_back({{"t","sessionEnded"},{"sessionId","session"}});
      if (threaded_fault==3) messages.push_back({{"t","msg"},{"from","guest"},{"d",{{"kind","mu-console-end"},{"v",1},{"sessionId","session"}}}});
      if (threaded_fault==4) messages.push_back({{"t","roomClosed"},{"reason","host_left"}});
      if (threaded_fault==5) queue({cj::Kind::Abort,0,{}});
      return true;
    }
    if (m.kind==cj::Kind::Identity) queue({m.kind,0,cj::identity(state().ports[1],1)});
    else if (m.kind==cj::Kind::OriginReady || m.kind==cj::Kind::Hash || m.kind==cj::Kind::PlugIn || m.kind==cj::Kind::Installed) queue(m);
    messages.push_back({{"t","presence"}});
    return true;
  }
  bool next_room(nlohmann::json& out,int n) {
    if (!messages.empty()) { out=messages.front(); messages.pop_front(); return true; }
    wait(n); return false;
  }
};
int main() {
  // Preflight, begin refusal/exception, clock refusal/exception; peer abort before/after
  // either vote and after adoption; window close and paused-loop timeout; full release.
  const int cases[][2]={{1,0},{2,0},{3,0},{4,0},{5,0},{6,0},{0,1},{0,2},{0,3},{0,10},{0,11},{0,0}};
  for (auto& c:cases) {
    failure=c[0]; abort_at=c[1]; ends=ons=offs=0; wall=0; exiting=false;
    g_console_boot=false; engine.active_=false; phase=1; cj::locked=false;
    auto ch=std::make_shared<cj::Channel>();
    auto q=std::make_shared<cj::Request>(); q->serial=1; q->frame=6; q->operation=cj::Operation::Apply;
    std::atomic_store(&ch->request,std::shared_ptr<const cj::Request>(q));
    std::atomic_store(&cj::pending,ch);
    auto launch=std::make_shared<cj::Launch>(); launch->link=std::make_unique<FakeLink>();
    std::atomic_store(&cj::launch,launch);
    assert(css_join_tick()==0);
    assert(!cj::locked && ons==1 && offs==1 && !ch->working);
    if (!failure && !abort_at) { assert(engine.active_ && g_console_boot && !ch->cancelled); }
    else {
      assert(!engine.active_ && !g_console_boot && ch->cancelled);
      assert(ends==((failure>=3 || abort_at==3)?1:0));
    }
  }
  // Stable but unsupported CSS leaves scene=1 for ordinary START and immediately refuses Capture.
  for (capture_mode=1;capture_mode<=2;++capture_mode) {
    auto ch=std::make_shared<cj::Channel>();
    auto q=std::make_shared<cj::Request>(); q->serial=1; q->host=true; q->operation=cj::Operation::Capture;
    std::atomic_store(&ch->request,std::shared_ptr<const cj::Request>(q));
    std::atomic_store(&cj::pending,ch); exiting=false; diagnostics.clear();
    assert(css_join_tick()==0 && cj::scene==1);
    auto r=std::atomic_load(&ch->result); assert(r && !r->ok);
    if (capture_mode==2) assert(diagnostics.back()=="css-join invalid: ports[1].cursor_x=30");
    ch->cancelled=true; css_join_tick();
    assert(!cj::locked && !ch->working);
  }
  capture_mode=0; threaded=true;
  for (threaded_fault=0;threaded_fault<=6;++threaded_fault) {
    failure=0; exiting=false; wall=0; delivered=0; ends=0; g_console_boot=false; g_css_origin.frame=6;
    engine.end("reset");
    auto ch=std::make_shared<cj::Channel>(); std::atomic_store(&cj::pending,ch);
    auto launch=std::make_shared<cj::Launch>(); launch->link=std::make_unique<FakeLink>();
    std::atomic_store(&cj::launch,launch);
    bool joined=false; std::atomic<bool> done{false};
    std::thread setup([&] {
      ProtocolIO io; SessionConfig c; c.session_id="session"; c.my_port=1;
      joined=cj::synchronize(io,true,2,"guest",c,ch); done=true;
    });
    while (!done || ch->working) { css_join_tick(); ++g_css_origin.frame; std::this_thread::yield(); }
    setup.join();
    assert(joined==(threaded_fault==0 || threaded_fault==6));
    assert(!cj::locked && !ch->working && delivered>0);
    if (threaded_fault==6) assert(joined && !engine.active_ && !engine.link);
    else if (joined) assert(engine.active_ && static_cast<FakeLink*>(engine.link.get())->handed);
    else assert(!engine.active_ && !g_console_boot);
    engine.end("reset");
  }

}
'''
        with tempfile.TemporaryDirectory(prefix='css-adoption-') as work:
            path, exe = Path(work) / 'adoption.cpp', Path(work) / 'adoption'
            path.write_text(fixture)
            subprocess.run([os.environ.get('CXX') or 'c++', '-std=c++17',
                            '-I' + str(ROOT / 'port/runtime/hle'), '-I' + str(ROOT / 'port/third_party'),
                            str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
