// Synthetic stamps and controllers only; no game, device, file or stdout pipe needed.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_controller_relay.h"
#include "yougame_relay_rumble.h"
#include "yougame_agent_rumble.h"
#include "yougame_box_present.h"
#include "yougame_parity.h"
#ifdef YOUGAME_AGENT_TEST_HOOK
#include "yougame_agent_test.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <string>
namespace {
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace yougame::relay_pads;
void timing() {
  static Timing t;
  char line[384];
  CHECK(!t.report(30000, line, sizeof line));
  t.arrival(100, true, 99.125, true); t.poll(100.25); t.poll(100.75);
  t.arrival(101, true, 100, true); t.poll(101.5);
  t.arrival(102, false, 0, true);
  t.arrival(103, true, 104, true); t.poll(104);
  t.arrival(105, true, 103, true);
  t.arrival(106, true, 105, true); t.poll(109);
  CHECK(!t.report(30099, line, sizeof line));
  CHECK(t.report(30100, line, sizeof line));
  CHECK(std::string(line) == "[relay-pads] 30 s: lines=6 hs=5 arrival-hs p50=1.000 p95=2.000 max=2.000 poll-arrival p50=0.500 p95=3.000 max=3.000 neg=1 overwritten=2");
  CHECK(!t.report(30101, line, sizeof line));
  CHECK(t.report(60100, line, sizeof line));
  CHECK(std::string(line) == "[relay-pads] 30 s: lines=0 hs=0 arrival-hs p50=0.000 p95=0.000 max=0.000 poll-arrival p50=0.000 p95=0.000 max=0.000 neg=0 overwritten=0");
  t.arrival(60101, false, 0, false);
  CHECK(!t.report(100000, line, sizeof line));
  t.arrival(100001, false, 0, true); t.arrival(100002, false, 0, true); t.poll(100003);
  CHECK(t.report(130001, line, sizeof line));
  CHECK(std::string(line).find("lines=2 hs=0") != std::string::npos);
  CHECK(std::string(line).find("overwritten=1") != std::string::npos);
  // Detaching a full window preserves its output and the live pending arrival.
  Timing live, completed;
  live.arrival(100, true, 99, true); live.poll(102);
  live.arrival(110, true, 107, true);
  CHECK(!live.take_report(30099, completed));
  CHECK(live.take_report(30100, completed));
  CHECK(!live.due(30101));
  live.poll(115);  // belongs to the next window even though arrival preceded the swap
  live.arrival(30110, true, 30106, true); live.poll(30111);
  completed.format(line, sizeof line);
  CHECK(std::string(line) == "[relay-pads] 30 s: lines=2 hs=2 arrival-hs p50=1.000 p95=3.000 max=3.000 poll-arrival p50=2.000 p95=2.000 max=2.000 neg=0 overwritten=0");
  CHECK(live.take_report(60100, completed));  // reuse the previous destination buffers
  completed.format(line, sizeof line);
  CHECK(std::string(line) == "[relay-pads] 30 s: lines=1 hs=1 arrival-hs p50=4.000 p95=4.000 max=4.000 poll-arrival p50=1.000 p95=5.000 max=5.000 neg=0 overwritten=0");
  // Exercise the bounded ring at and beyond capacity before swapping and sorting.
  live.reset();
  for (size_t i = 0; i < Timing::capacity + 10; ++i) {
    live.arrival(70000, true, 69998, true); live.poll(70001);
  }
  CHECK(live.take_report(100000, completed));
  completed.format(line, sizeof line);
  CHECK(std::string(line) == "[relay-pads] 30 s: lines=262154 hs=262154 arrival-hs p50=2.000 p95=2.000 max=2.000 poll-arrival p50=1.000 p95=1.000 max=1.000 neg=0 overwritten=0");
  Frame frame; std::string reason;
  CHECK(parse("yg-pads {\"seq\":0,\"t\":0,\"hs\":123.125,\"pads\":[]}", frame, reason));
  CHECK(frame.has_hs && frame.hs == 123.125);
  for (const char* hs : {"null", "true", "-1", "\"123\"", "1e999"})
    CHECK(!parse(std::string("yg-pads {\"seq\":0,\"t\":0,\"hs\":") + hs + ",\"pads\":[]}", frame, reason));
}
void rumble() {
  CHECK(rumble_line({0,65535,32768,500}) == "yg {\"rumble\":{\"seat\":0,\"strong\":65535,\"weak\":32768,\"ms\":500}}");
  CHECK(rumble_line({9,99999,-7,9000}) == "yg {\"rumble\":{\"seat\":3,\"strong\":65535,\"weak\":0,\"ms\":5000}}");
  CHECK(rumble_line({-1,0,0,5000}) == "yg {\"rumble\":{\"seat\":0,\"strong\":0,\"weak\":0,\"ms\":0}}");
  CHECK(rumble_line({1,1,2,-1}).find("\"ms\":0") != std::string::npos);
  std::vector<std::string> logs;
  State relay([&](const std::string& line) { logs.push_back(line); });
  CHECK(relay.accept("yg-pads {\"seq\":0,\"t\":0,\"pads\":[]}",0));
  bool native_called=false;
  relay.rumble([&] { native_called=true; },true);
  CHECK(!native_called && logs.size()==1); // implemented relay output must not warn "no rumble yet"
  Rumble r; RumbleCommand c;
  r.set(-1,true,0); r.set(4,true,0); r.set(0,false,0); CHECK(!r.take(c));
  r.set(0,true,100); CHECK(r.take(c)); CHECK(c.seat==0 && c.strong==65535 && c.weak==32768 && c.ms==5000);
  r.set(0,true,200); r.tick(4099); CHECK(!r.take(c));
  r.tick(4100); CHECK(r.take(c)); CHECK(c.ms==5000); CHECK(!r.take(c));
  r.set(0,false,4101); CHECK(r.take(c)); CHECK(c.strong==0 && c.weak==0 && c.ms==0);
  r.set(0,false,4200); r.tick(9000); CHECK(!r.take(c));
  r.pulse(2,10000); CHECK(r.take(c)); CHECK(c.seat==2 && c.strong==65535 && c.weak==65535 && c.ms==300);
  r.tick(10299); CHECK(!r.take(c)); r.tick(10300); CHECK(r.take(c)); CHECK(c.seat==2 && c.ms==0 && c.strong==0 && c.weak==0);
  for (int i=0;i<1000;++i) r.set(1,i%2==0,20000+i);
  int count=0; while(r.take(c)) ++count;
  CHECK(count==64 && c.seat==1 && c.ms==0); // bounded queue retains the final stop
}
void mapping() {
  Frame f;
  f.pads[3].present=true; f.pads[3].kind="ps"; f.pads[3].family_index=0;
  f.pads[0].present=true; f.pads[0].kind="switch"; f.pads[0].family_index=2;
  f.pads[1].present=true; f.pads[1].kind="xbox"; f.pads[1].family_index=0;
  CHECK(seat_for(f,host::CaptureDevice::DS4Pad,0)==3);
  CHECK(seat_for(f,host::CaptureDevice::SwitchPro,2)==0);
  CHECK(seat_for(f,host::CaptureDevice::XInputPad,0)==1);
  CHECK(seat_for(f,host::CaptureDevice::DS4Pad,3)==-1);
  CHECK(seat_for(f,host::CaptureDevice::None,0)==-1);
  CHECK(seat_for(f,host::CaptureDevice::DS4Pad,-1)==-1);
  CHECK(family_line(3,f.pads[3],1)=="relay pads: slot 3 kind ps family 0 -> DS4 bindings, port 2");
  CHECK(family_line(0,f.pads[0],0)=="relay pads: slot 0 kind switch family 2 -> SWPRO bindings, port 1");
  CHECK(family_line(1,f.pads[1],3)=="relay pads: slot 1 kind xbox family 0 -> XInput bindings, port 4");
  RouteLog routes;
  routes.begin(f);
  CHECK(routes.changed(f,host::CaptureDevice::DS4Pad,0,1)==family_line(3,f.pads[3],1));
  CHECK(routes.changed(f,host::CaptureDevice::DS4Pad,0,2).empty()); // duplicate assignment
  routes.begin(f); CHECK(routes.changed(f,host::CaptureDevice::DS4Pad,0,1).empty());
  routes.begin(f); CHECK(routes.changed(f,host::CaptureDevice::DS4Pad,0,2)==family_line(3,f.pads[3],2));
  routes.begin(f); CHECK(routes.changed(f,host::CaptureDevice::DS4Pad,0,1)==family_line(3,f.pads[3],1));
  routes.begin(f); routes.begin(f); // no routed port for a poll, then reassign
  CHECK(!routes.changed(f,host::CaptureDevice::DS4Pad,0,1).empty());
  Pad other; other.kind="hid"; CHECK(family_line(0,other,0).empty());
  other.kind="other"; CHECK(family_line(0,other,0).empty());
  f.pads[3].family_index=kNoFamilyIndex; CHECK(seat_for(f,host::CaptureDevice::DS4Pad,0)==-1);
  // Explicit port routing and the existing automatic fallback both resolve to a family/index.
  host::InputDeviceFacts facts{}; facts.ds4[0]=true;
  std::array<host::PortSource,4> ports{}; ports[0]={host::DeviceKind::DS4Pad,0};
  const auto source=host::effective_port_source(ports,facts,0);
  CHECK(source.kind==host::DeviceKind::DS4Pad && source.index==0);
  f.pads[3].family_index=0;
  CHECK(seat_for(f,host::controller_relay::family_of(source.kind),source.index)==3);
  ports[0]={host::DeviceKind::Keyboard,0};
  const auto fallback=host::effective_port_source(ports,facts,0);
  CHECK(seat_for(f,host::controller_relay::family_of(fallback.kind),fallback.index)==3);
}
void present() {
  using yougame::box_present::choose;
  for(bool embedded:{false,true}) for(bool vsync:{false,true}) for(bool full:{false,true}) for(bool agent:{false,true}) {
    const auto p=choose(embedded,vsync,full,agent,true,true,!full);
    CHECK(p.interval==(vsync && !(embedded && !full && agent) ? 1u : 0u));
    CHECK(p.flags==(vsync || full ? 0u : yougame::box_present::allow_tearing));
    CHECK(p.compositor==(embedded && vsync && !full && agent));
    CHECK(p.agent==p.compositor);
  }
  CHECK(yougame::box_present::message(choose(true,true,false,false))==nullptr); // default: interval 1, no line
  CHECK(std::string(yougame::box_present::message(choose(true,true,false,true)))==
      "present: embedded v-sync via compositor (interval 0, agent test)");
  CHECK(yougame::box_present::announcement(choose(true,true,false,true))!=nullptr);
  CHECK(yougame::box_present::announcement(choose(true,true,false,true))==nullptr);
  CHECK(choose(true,true,false,true,false).interval==1); // D3D11 blit fallback, even for the experiment
  CHECK(choose(true,true,false,true,true,false).interval==1); // pacing gate
  CHECK(choose(false,false,false,false,true,true,false).flags==0);
  CHECK(choose(false,false,false,false,true,true,true,true).interval==1); // VRR path
}
void pulse() {
#ifdef YOUGAME_AGENT_TEST_HOOK
  const auto settings = yougame::agent_test::map_keys({{"rumbleTest",true},{"boxPresent","compositor"}});
  CHECK(settings.rumble_test && settings.box_present_compositor);
  CHECK(!yougame::agent_test::map_keys({{"boxPresent","vsync"}}).box_present_compositor); // the default
  const auto invalid = yougame::agent_test::map_keys({{"rumbleTest","true"},{"boxPresent","other"}});
  CHECK(!invalid.rumble_test && !invalid.box_present_compositor);
  yougame::agent_test::RumblePulse p;
  std::array<bool,4> seats{}; std::vector<int> sent; std::vector<std::string> logs;
  auto send=[&](int seat){sent.push_back(seat);}; auto log=[&](const char* line){logs.emplace_back(line);};
  p.arm(100); seats[0]=seats[3]=true;
  p.tick(5099,seats,send,log); CHECK(sent.empty());
  p.tick(5100,seats,send,log); CHECK(sent==std::vector<int>({0,3}));
  CHECK(logs==std::vector<std::string>({"agent test: rumble pulse sent to slot 0","agent test: rumble pulse sent to slot 3"}));
  p.tick(5200,seats,send,log); CHECK(sent.size()==2);
  p.arm(0); seats={}; p.tick(5000,seats,send,log); CHECK(logs.size()==2);
  seats[2]=true; p.tick(59999,seats,send,log); CHECK(sent.back()==2);
  p.arm(0); seats={}; p.tick(60000,seats,send,log);
  CHECK(logs.back()=="agent test: rumble pulse: no relayed seats");
#endif
}
}
int main(int argc, char** argv) {
  const std::string test = argc > 1 ? argv[1] : "all";
  if (test=="all" || test=="timing") timing();
  if (test=="all" || test=="rumble") rumble();
  if (test=="all" || test=="family") mapping();
  if (test=="all" || test=="present") present();
  if (test=="all" || test=="pulse") pulse();
  CHECK(std::string(yougame::parity::kCaps).find("\"pads\",\"rumble\"")!=std::string::npos);
  std::printf("yougame relay output (%s): %d/%d checks passed\n",test.c_str(),checks,checks);
}
