// Real console protocol + real broker Stream, entirely offline.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include "yougame_console_link.h"
#include "yougame_console_engine.h"
#include "yougame_console_entry.h"
#include "yougame_agent_test.h"
#include <cstdio>
#include <cstdlib>
#include <future>
#include <atomic>
#include <condition_variable>
using namespace yougame::console;
using yougame::transport::str;
static std::atomic<int> checks{0};
static int cases = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
struct Fixture {
  uint64_t now = 1;
  int players, seat, conn = 0, state = 2, start_delay = 3;
  int host_seat = 0;
  json lobby_roster, console_roster;
  bool auto_delay = true, reply_delay = true;
  uint64_t reported_rtt = 40;
  std::string path, session;
  bool closed = false, attach_ok = true, fill = true, reply_start = true, reply_candidates = true;
  unsigned packets = 0;
  uint64_t start_at = 0;
  std::vector<int> waits;
  std::vector<json> requests, messages;
  std::vector<std::string> logs;
  std::deque<std::pair<unsigned,Bytes>> inbox;
  std::vector<std::pair<unsigned,Bytes>> sent;
  std::function<void(unsigned,const Bytes&)> deliver;
  std::function<void()> on_wait;
  std::function<void(json&)> alter_start;
  yougame::transport::Events events{[this](int ms) { wait(ms); }};
  yougame::transport::Stream stream{events,[this](const json& q,int ms) { return request(q,ms); },[this] { return now; }};
  Fixture(int n = 2,int s = 0) : players(n),seat(s) {}
  json roster() const {
    if (!lobby_roster.is_null()) return lobby_roster;
    json r = json::array();
    for (int n = 0; n < (fill ? players : 1); ++n) r.push_back({{"slot",n},{"localIndex",0},{"id","p"+std::to_string(n)},
      {"connectionId","c"+std::to_string(n)}});
    return r;
  }
  json console_start() const {
    auto participants = roster();
    for (auto& p : participants) { p["port"] = p["slot"].get<int>()+1; p["user"] = "user"; p["name"] = "Player"; p.erase("localIndex"); }
    if (!console_roster.is_null()) participants = console_roster;
    return {{"sessionId","session-fixed"},{"seed","fedcba9876543210"},{"startedAt",1793443200123LL},
      {"inputDelay",start_delay},{"settingsDigest",""},{"participants",participants}};
  }
  void push(const json& m) { events.push({{"conn",conn},{"event","message"},{"data",m.dump()}}); }
  void wait(int ms) { CHECK(ms >= 0); waits.push_back(ms); now += uint64_t(ms); if (on_wait) on_wait(); }
  json request(const json& q,int ms) {
    CHECK(ms > 0); requests.push_back(q);
    const auto type = str(q,"type");
    if (type == "mpTicket") return {{"ticket","fake-ticket"},{"room",q.value("room","") == "new" ? "" : q.value("room","")}};
    if (type == "netOpen") {
      conn = seat+10; events.push({{"conn",conn},{"event","open"}});
      push({{"t","welcome"},{"me","c"+std::to_string(seat)},{"host","c"+std::to_string(host_seat)},{"revision",7},{"participants",roster()},{"room",{{"code","ABCDEF"}}}});
      if (q.count("session") && reply_start)
        push({{"t","welcome"},{"me","c"+std::to_string(seat)},{"host","c"+std::to_string(host_seat)},{"revision",7},{"room",{{"console",console_start()}}}});
      else if (seat != host_seat && reply_start) push({{"t","start"},{"matchId","session-fixed"},{"console",console_start()},{"participants",roster()}});
      return {{"conn",conn}};
    }
    if (type == "netSend") {
      const auto m = json::parse(q["data"].get<std::string>()); messages.push_back(m);
      if (str(m,"t") == "begin" && reply_start) {
        auto s = console_start(); start_at = now;
        json start = {{"t","start"},{"matchId","session-fixed"},{"console",s},{"participants",roster()}};
        if (alter_start) alter_start(start);
        push(start);
      }
      if (str(m,"t") == "msg" && m.count("d") && str(m["d"],"kind") == "mu-cand" && reply_candidates)
        push({{"t","msg"},{"from",m["to"]},{"d",m["d"]}});
    }
    if (type == "directOpen") return {{"link",unsigned(q["peer"].get<std::string>()[1]-'0'+1)}};
    return json::object();
  }
  SessionIO io() {
    SessionIO i;
    i.request = [this](const json& q,int ms) { return request(q,ms); };
    i.clock = [this] { return now; }; i.random = [] { return UINT64_C(0xfedcba9876543210); };
    i.open_room = [this](const std::string& t,const std::string& r,int ms) { return stream.open(t,r,ms,session); };
    i.next_room = [this](json& m,int ms) { return stream.next(m,ms); };
    i.send_room = [this](const json& m) { return stream.send(m); };
    i.room_closed = [this] { return closed || stream.closed(); };
    i.connection = [this] { return conn; }; i.attach = [this](int) { return attach_ok; };
    i.state = [this](unsigned) { return state; }; i.route = [this](unsigned id) { return path.empty() ? (id%2 ? "direct" : "relay") : path; };
    i.send = [this](unsigned id,const Bytes& b) { CHECK(b.size() <= 1200); ++packets; sent.emplace_back(id,b); if (deliver) deliver(id,b);
      if (auto_delay && reply_delay && b.size() == 20 && b[0] == 0xc1) {
        auto reply = b; reply[1] = uint8_t(id);
        auto value = [&](uint64_t v) { for (int n = 19; n >= 12; --n) { reply[n] = uint8_t(v); v >>= 8; } };
        if (b[2] == 1) { reply[2] = 2; inbox.emplace_back(id,reply); reply[2] = 3; value(reported_rtt); inbox.emplace_back(id,reply); }
        if (seat == host_seat && b[2] == 4) { reply[2] = 5; inbox.emplace_back(id,reply); }
        if (seat != host_seat && b[2] == 3) { reply[2] = 4; value(reported_rtt); reply[3] = uint8_t(measured_delay(reported_rtt,3)); inbox.emplace_back(id,reply); }
        if (seat != host_seat && b[2] == 5) { reply[2] = 6; inbox.emplace_back(id,reply); }
      } };
    i.receive = [this](unsigned& id,Bytes& b) {
      if (inbox.empty()) return false;
      id = inbox.front().first; b = std::move(inbox.front().second); inbox.pop_front(); return true;
    };
    i.wait = [this](int ms) { wait(ms); }; i.close = [] {};
    i.log = [this](const std::string& s) { logs.push_back(s); }; i.compatibility = "fake-console1";
    return i;
  }
  // Joiners use the code the host made (rooms never makes one for "new").
  json input() const { return {{"room",seat ? yougame::console::new_room_code(UINT64_C(0xfedcba9876543210)) : "new"},{"players",players}}; }
  std::unique_ptr<SessionLink> start(SessionConfig& c) { std::string e; auto l = start_session(input(),c,e,io()); CHECK(l && e.empty()); return l; }
};
static json shared(const SessionConfig& c) {
  return {{"room",c.room},{"session",c.session_id},{"ports",c.ports},{"seed",c.seed},{"epoch",c.epoch_ms},{"delay",c.input_delay},{"settings",c.settings_digest}};
}
static void intake() {
  ++cases; json out;
  CHECK(parse_request({{"room","new"},{"players",2}},out));
  CHECK(out["type"] == "consoleSession");
  const json app = {{"type","consoleSession"},{"room","ABCDEF"},{"players",4},{"port",4},{"sessionId","s"},{"seed",UINT64_MAX},{"epochMs",INT64_MAX},{"inputDelay",3}};
  CHECK(parse_request(app,out) && out == app);
  auto hex_app = app; hex_app["seed"] = "fedcba9876543210"; CHECK(parse_request(hex_app,out)); CHECK(out["seed"] == UINT64_C(0xfedcba9876543210));
  for (auto key : {"players","port","seed","epochMs","inputDelay"}) for (const auto& bad : {json(-1),json(2.5),json(true),json("3"),json(nullptr)}) {
    auto j = app; j[key] = bad; CHECK(!parse_request(j,out));
  }
  for (const auto& bad : {json::array(),json(nullptr),json("new")}) CHECK(!parse_request(bad,out));
  for (auto field : {"room","sessionId","type"}) { auto j = app; j[field] = "../invalid"; CHECK(!parse_request(j,out)); }
  for (int n : {1,5,INT32_MAX}) { auto j = app; j["players"] = n; CHECK(!parse_request(j,out)); }
  for (auto field : {"port","epochMs","inputDelay"}) { auto j = app; j[field] = UINT64_MAX; CHECK(!parse_request(j,out)); }
  auto settings = yougame::agent_test::map_keys({{"console",{{"room","new"},{"players",4}}}});
  CHECK(settings.console_request == json({{"type","consoleSession"},{"room","new"},{"players",4}}));
  settings = yougame::agent_test::map_keys({{"console",{{"room","ABCDEF"},{"players",2}}}});
  CHECK(settings.console_request["room"] == "ABCDEF");
  CHECK(yougame::agent_test::map_keys({{"console",{{"room","new"},{"players",5}}}}).console_request.is_null());
  CHECK(yougame::agent_test::map_keys({{"console",{{"room","new"},{"players",2}}}},yougame::agent_test::Engine::Static).console_request.is_null());
  CHECK(yougame::agent_test::map_keys({{"console",{{"room","new"},{"players",2},{"seed",1}}}}).console_request.is_null());
  CHECK(launch_request(app)); CHECK(take_request() == app); CHECK(!take_request());
  auto newer = app; newer["sessionId"] = "newer"; CHECK(launch_request(newer)); launch_hello({{"launch",app}}); CHECK(take_request() == newer);
  launch_hello({{"launch",app}}); CHECK(take_request() == app);
  CHECK(requests.claim()); CHECK(!launch_request(app)); CHECK(!requests.claim()); requests.release();
}
static void starts() {
  for (int count : {2,3,4}) {
    ++cases; json agreed;
    for (int seat = 0; seat < count; ++seat) {
      Fixture f(count,seat); SessionConfig c; auto link = f.start(c);
      CHECK(c.my_port == seat+1); CHECK(c.ports.size() == size_t(count));
      for (int n = 0; n < count; ++n) CHECK(c.ports[n] == n+1);
      if (seat == 0) agreed = shared(c); else CHECK(agreed == shared(c));
      const auto& ticket = f.requests.front();
      CHECK(ticket["type"] == "mpTicket" && ticket["mode"] == "console" && ticket["players"] == count);
      CHECK(ticket["lobby"]["minPlayers"] == count && ticket["lobby"]["maxLocalPlayers"] == 1);
      CHECK(ticket["room"] == yougame::console::new_room_code(UINT64_C(0xfedcba9876543210)));
      CHECK(std::count_if(f.requests.begin(),f.requests.end(),[](const json& q) { return str(q,"type") == "directOpen"; }) == count-1);
      CHECK(std::count_if(f.messages.begin(),f.messages.end(),[](const json& q) { return str(q,"t") == "begin"; }) == (seat ? 0 : 1));
      CHECK(std::count_if(f.logs.begin(),f.logs.end(),[](const std::string& s) { return s.find("console: link to port ") == 0; }) == count-1);
    }
  }
}
static void failure_cases() {
  ++cases;
  for (int mode = 0; mode < 7; ++mode) {
    Fixture f; auto request = f.input();
    if (mode == 0) f.fill = false;
    if (mode == 1) f.reply_start = false;
    if (mode == 2) f.reply_candidates = false;
    if (mode == 3) f.state = 1;
    if (mode == 4) f.state = 3;
    if (mode == 5) request["port"] = 2;
    if (mode == 6) f.attach_ok = false;
    SessionConfig c; c.room = "unchanged"; std::string error;
    auto link = start_session(request,c,error,f.io()); CHECK(!link && !error.empty()); CHECK(c.room == "unchanged");
    if (mode < 2) { CHECK(error == "your friend didn't join"); CHECK(f.now >= 300001 && f.now <= 300051); }
    if (mode == 0) CHECK(std::none_of(f.messages.begin(),f.messages.end(),[](const json& q) { return str(q,"t") == "begin"; }));
    if (mode == 2 || mode == 3) { CHECK(error == "couldn't connect to your friend"); CHECK(f.now-f.start_at >= 20000 && f.now-f.start_at <= 20010); }
    CHECK(f.logs.back().find("console: start failed:") == 0 || f.logs.back().find("console: session ended:") == 0);
  }
}
static void host_waits_for_seats() {
  ++cases; Fixture f(4); f.fill = false;
  f.on_wait = [&] {
    if (f.now < 501 || f.fill) return;
    CHECK(std::none_of(f.messages.begin(),f.messages.end(),[](const json& q) { return str(q,"t") == "begin"; }));
    f.fill = true; f.push({{"t","join"},{"participants",f.roster()},{"revision",8},{"host","c0"}});
  };
  SessionConfig c; auto l = f.start(c); CHECK(f.start_at >= 501);
  for (const auto& m : f.messages) if (str(m,"t") == "begin") CHECK(m["revision"] == 8);
}
static void agent_create_join() {
  ++cases;
  for (bool host : {true,false}) {
    Fixture f(2,host ? 0 : 1);
    auto settings = yougame::agent_test::map_keys({{"console",{{"room",host ? "new" : "ABCDEF"},{"players",2}}}});
    SessionConfig c; std::string error;
    auto l = start_session(settings.console_request,c,error,f.io()); CHECK(l && error.empty());
    CHECK(c.my_port == (host ? 1 : 2));
    const auto hosted = yougame::console::new_room_code(UINT64_C(0xfedcba9876543210));
    CHECK(f.requests[0]["room"] == (host ? hosted : "ABCDEF"));
    CHECK(std::count(f.logs.begin(),f.logs.end(),"agent: console host room " + hosted) == (host ? 1 : 0));
  }
}
static void invalid_start() {
  ++cases;
  for (int bad = 0; bad < 7; ++bad) {
    Fixture f;
    f.alter_start = [bad](json& j) {
      if (bad == 0) j["participants"][1]["slot"] = 0;
      if (bad == 1) j["participants"][1]["connectionId"] = "c0";
      if (bad == 2) j["console"]["participants"][1]["port"] = 1;
      if (bad == 3) j["console"]["sessionId"] = "../path";
      if (bad == 4) j["console"]["seed"] = "bad-hex";
      if (bad == 5) j["console"]["startedAt"] = -1;
      if (bad == 6) j["console"]["inputDelay"] = 13;
    };
    SessionConfig c; std::string e; CHECK(!start_session(f.input(),c,e,f.io())); CHECK(e == "invalid console session start");
  }
}
static MuPadStatus sample(uint32_t frame,int port) {
  MuPadStatus p{}; p.button = uint16_t(frame); p.stick_x = int8_t(frame); p.stick_y = -47; p.sub_x = -127;
  p.sub_y = int8_t(port); p.trigger_l = 251; p.trigger_r = 128; p.analog_a = 7; p.analog_b = 9; p.err = -1; return p;
}
static void same_pad(const MuPadStatus& a,const MuPadStatus& b) {
  CHECK(a.button == b.button && a.stick_x == b.stick_x && a.stick_y == b.stick_y && a.sub_x == b.sub_x && a.sub_y == b.sub_y &&
      a.trigger_l == b.trigger_l && a.trigger_r == b.trigger_r && a.analog_a == b.analog_a && a.analog_b == b.analog_b && a.err == b.err);
}
static void sparse_roster(Fixture& f,const std::vector<uint64_t>& slots) {
  auto lobby = f.roster(); auto frozen = f.console_start()["participants"];
  for (size_t n = 0; n < slots.size(); ++n) {
    lobby[n]["slot"] = frozen[n]["slot"] = slots[n];
    frozen[n]["port"] = n+1;
  }
  f.lobby_roster = lobby; f.console_roster = frozen;
}
static void sparse_starts() {
  for (const auto& slots : {std::vector<uint64_t>{0,2},std::vector<uint64_t>{0,2,3}})
    for (int host : {0,int(slots.size())-1}) for (bool rejoin : {false,true}) {
      ++cases; const int count = int(slots.size());
      std::vector<std::unique_ptr<Fixture>> fixtures;
      std::vector<std::unique_ptr<SessionLink>> links;
      for (int seat = 0; seat < count; ++seat) {
        auto f = std::make_unique<Fixture>(count,seat); f->host_seat = host; sparse_roster(*f,slots);
        // Start's lobby roster has no ports and may arrive in a different order.
        std::reverse(f->lobby_roster.begin(),f->lobby_roster.end());
        auto request = f->input();
        if (rejoin) {
          f->session = "session-fixed";
          request = {{"room","ABCDEF"},{"players",count},{"sessionId",f->session},{"port",seat+1},
            {"seed","fedcba9876543210"},{"epochMs",1793443200123LL},{"inputDelay",3}};
        }
        SessionConfig c; std::string error;
        auto link = start_session(request,c,error,f->io());
        if (!link) std::fprintf(stderr,"sparse count %d host %d rejoin %d seat %d: %s\n",count,host,int(rejoin),seat,error.c_str());
        CHECK(link && error.empty());
        CHECK(c.my_port == seat+1);
        CHECK(c.ports == (count == 2 ? std::vector<int>{1,2} : std::vector<int>{1,2,3}));
        CHECK(std::count_if(f->messages.begin(),f->messages.end(),[](const json& m) { return str(m,"t") == "begin"; }) == (!rejoin && seat == host ? 1 : 0));
        for (int peer = 0; peer < count; ++peer) if (peer != seat) {
          CHECK(std::count_if(f->requests.begin(),f->requests.end(),[&](const json& q) {
            return str(q,"type") == "directOpen" && str(q,"peer") == "c"+std::to_string(peer);
          }) == 1);
          CHECK(std::any_of(f->logs.begin(),f->logs.end(),[&](const std::string& line) {
            return line.find("console: link to port " + std::to_string(peer+1) + " up via ") == 0;
          }));
        }
        fixtures.push_back(std::move(f)); links.push_back(std::move(link));
      }
      for (int seat = 0; seat < count; ++seat) fixtures[seat]->deliver = [&,seat](unsigned id,const Bytes& b) {
        CHECK(id >= 1 && id <= unsigned(count)); CHECK(b[1] == seat+1);
        fixtures[id-1]->inbox.emplace_back(unsigned(seat+1),b);
      };
      for (int seat = 0; seat < count; ++seat) {
        links[seat]->send_pad(0,sample(0,seat+1)); links[seat]->send_digest(0,100+seat);
      }
      for (auto& link : links) link->pump(0);
      for (int seat = 0; seat < count; ++seat) for (int peer = 0; peer < count; ++peer) if (peer != seat) {
        auto pad = links[seat]->pad(peer+1,0); CHECK(pad); same_pad(*pad,sample(0,peer+1));
        CHECK(links[seat]->digest(peer+1,0) == uint64_t(100+peer)); CHECK(!links[seat]->lost());
      }
    }
}
static void sparse_invalid_rosters() {
  ++cases;
  for (int bad = 0; bad < 18; ++bad) {
    Fixture f; sparse_roster(f,{0,2});
    f.alter_start = [bad](json& m) {
      auto& p = m["console"]["participants"][1];
      if (bad == 0) p["port"] = 1;
      if (bad == 1) p["port"] = 0;
      if (bad == 2) p["port"] = 3;
      if (bad == 3) p.erase("port");
      if (bad == 4) p["slot"] = 0;
      if (bad == 5) p["connectionId"] = "c0";
      if (bad == 6) p["id"] = "p0";
      if (bad == 7) p["port"] = 1.5;
      if (bad == 8) p["port"] = true;
      if (bad == 9) p["port"] = "2";
      if (bad == 10) p["slot"] = -1;
      if (bad == 11) p["slot"] = 2.5;
      if (bad == 12) p.erase("slot");
      if (bad == 13) m["participants"][1]["id"] = "changed";
      if (bad == 14) m["participants"][1]["connectionId"] = "changed";
      if (bad == 15) m["participants"][1]["slot"] = 3;
      if (bad == 16) m["participants"][1]["localIndex"] = 1;
      if (bad == 17) p["port"] = nullptr;
    };
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,f.io())); CHECK(error == "invalid console session start");
  }
}
static void sparse_relaunch() {
  ++cases;
  for (int seat : {0,1}) {
    Fixture first(2,seat); sparse_roster(first,{0,2});
    SessionConfig config; std::string error; json launch; auto io = first.io();
    io.prepared = [&](const SessionConfig& c) {
      CHECK(c.my_port == seat+1);
      CHECK(handoff(c,true,{},0,"",[&](const json& m) { launch = m["request"]; },[](const std::string&) {},[](const char*) {}));
    };
    CHECK(!start_session(first.input(),config,error,std::move(io))); CHECK(error.empty());
    CHECK(launch["port"] == seat+1);
    Fixture second(2,seat); sparse_roster(second,{0,2}); second.session = launch["sessionId"];
    auto link = start_session(launch,config,error,second.io()); CHECK(link && error.empty());
    CHECK(config.my_port == seat+1);
    CHECK(std::none_of(second.messages.begin(),second.messages.end(),[](const json& m) { return str(m,"t") == "begin"; }));
  }
  // Slot identifiers have no bound related to player count or a native int port.
  Fixture large; sparse_roster(large,{0,UINT64_MAX}); SessionConfig c;
  auto link = large.start(c); CHECK(c.my_port == 1);
}
static void sparse_peer_logs() {
  ++cases; Fixture f; sparse_roster(f,{0,2}); auto io = f.io();
  const auto request = io.request;
  io.request = [&](const json& q,int ms) {
    if (str(q,"type") == "directOpen") return json{{"refused","not_allowed"}};
    return request(q,ms);
  };
  SessionConfig c; std::string error;
  CHECK(!start_session(f.input(),c,error,std::move(io))); CHECK(error == "couldn't connect to your friend");
  CHECK(std::count(f.logs.begin(),f.logs.end(),"console: link open to port 2 refused (not_allowed)") == 1);
  Fixture reopened; sparse_roster(reopened,{0,2}); auto retry_io = reopened.io();
  const auto retry_request = retry_io.request; int opens = 0;
  retry_io.request = [&](const json& q,int ms) {
    if (str(q,"type") == "directOpen") ++opens;
    return retry_request(q,ms);
  };
  retry_io.state = [&](unsigned) { return opens == 1 ? 3 : 2; };
  auto link = start_session(reopened.input(),c,error,std::move(retry_io)); CHECK(link && error.empty());
  CHECK(opens == 2);
  CHECK(std::count(reopened.logs.begin(),reopened.logs.end(),"console: link to port 2 opened again (1)") == 1);
}
static void mesh(int count,bool loss) {
  ++cases;
  std::vector<std::unique_ptr<Fixture>> f;
  std::vector<std::unique_ptr<SessionLink>> links;
  for (int s = 0; s < count; ++s) { f.push_back(std::make_unique<Fixture>(count,s)); SessionConfig c; links.push_back(f.back()->start(c)); }
  std::map<std::pair<int,unsigned>,unsigned> counters;
  for (int s = 0; s < count; ++s) f[s]->deliver = [&,s](unsigned id,const Bytes& b) {
    // Deterministic 50% packet loss, reverse delivery on surviving packets.
    const auto n = counters[{s,id}]++;
    if (loss && n%4 < 2) return;
    auto& q = f[id-1]->inbox;
    if (loss) q.emplace_front(unsigned(s+1),b); else q.emplace_back(unsigned(s+1),b);
  };
  constexpr uint32_t frames = 2400; // traverse the 2048-frame ring boundary
  std::vector<std::vector<uint32_t>> next(count,std::vector<uint32_t>(count,0));
  auto collect = [&] {
    for (int s = 0; s < count; ++s) for (int peer = 0; peer < count; ++peer) if (s != peer) {
      while (next[s][peer] < frames) {
        auto p = links[s]->pad(peer+1,next[s][peer]); if (!p) break;
        same_pad(*p,sample(next[s][peer],peer+1)); ++next[s][peer];
      }
    }
  };
  for (uint32_t frame = 0; frame < frames; ++frame) {
    for (int s = 0; s < count; ++s) { f[s]->now += 17; links[s]->send_pad(frame,sample(frame,s+1)); }
    if (frame%60 == 0) for (int s = 0; s < count; ++s) links[s]->send_digest(frame,UINT64_C(0xfedcba9800000000)+frame);
    for (int s = 0; s < count; ++s) links[s]->pump(0);
    collect();
    for (const auto& l : links) CHECK(!l->lost());
  }
  // Allow recovery of the last dropped packet without producing new input.
  for (int n = 0; n < 200; ++n) { for (int s = 0; s < count; ++s) { f[s]->now += 20; links[s]->pump(0); } collect(); }
  for (int s = 0; s < count; ++s) for (int peer = 0; peer < count; ++peer) if (s != peer) {
    if (next[s][peer] != frames) std::fprintf(stderr,"mesh count %d loss %d seat %d peer %d: received %u/%u\n",count,int(loss),s,peer,next[s][peer],frames);
    CHECK(next[s][peer] == frames);
    for (uint32_t frame = 0; frame < frames; frame += 60) CHECK(links[s]->digest(peer+1,frame) == UINT64_C(0xfedcba9800000000)+frame);
    CHECK(!links[s]->lost());
    // The native rollback adapter revisits historical pads after their transport ACK. Reverse
    // reads through the seven-frame horizon must remain exact and cannot issue/retransmit input.
    const auto sent = f[s]->sent.size();
    for (int repeat = 0; repeat < 3; ++repeat) for (uint32_t back = 1; back <= 7; ++back) {
      auto historical = links[s]->pad(peer + 1, frames - back);
      CHECK(historical); same_pad(*historical, sample(frames - back, peer + 1));
    }
    CHECK(f[s]->sent.size() == sent);
  }
  // ACKs stop pad and digest payload retransmission. Idle packets are header only.
  for (auto& fixture : f) { fixture->now += 3001; fixture->sent.clear(); }
  for (int n = 0; n < 30; ++n) for (int s = 0; s < count; ++s) { f[s]->now += 20; links[s]->pump(0); }
  for (const auto& fixture : f) {
    CHECK(!fixture->sent.empty());
    for (const auto& packet : fixture->sent) CHECK(packet.second.size() == 20);
    CHECK(std::count_if(fixture->logs.begin(),fixture->logs.end(),[](const std::string& s) { return s.find("console: port ") == 0; }) == 4*(count-1));
  }
}
static void departures_and_waits() {
  ++cases;
  for (const char* event : {"closed","silent","link"}) {
    Fixture f; SessionConfig c; auto l = f.start(c); l->pump(0);
    if (std::string(event) == "silent") {
      l->send_digest(0,42);
      auto packet = f.sent.back().second; packet[1] = 2;
      packet[12] = packet[13] = packet[14] = packet[15] = 255; // no pad ACK
      f.inbox.emplace_back(2,packet); l->pump(0);
      f.now += 4999; l->pump(0); CHECK(!l->lost()); ++f.now; }
    else if (std::string(event) == "link") f.state = 3;
    else if (std::string(event) == "closed") f.closed = true;
    else f.push({{"t",event},{"player",{{"id","c1"}}},{"reason","left"}});
    l->pump(0); CHECK(l->lost());
    const auto expected = "your friend disconnected";
    CHECK(*l->lost() == expected);
    CHECK(str(f.messages.back()["d"],"kind") == "mu-console-end"); CHECK(f.messages.back()["d"]["reason"] == expected);
  }
  Fixture f; SessionConfig c; auto l = f.start(c);
  for (int wait : {0,1,5,10,99,1000}) { const auto before = f.now; l->pump(wait); CHECK(f.now-before <= uint64_t(wait)); if (wait) CHECK(f.now > before); }
  const auto before = f.now;
  f.sent.clear();
  for (int n = 0; n < 100; ++n) l->pump(10);
  CHECK(f.now-before == 1000);
  const auto keepalives = std::count_if(f.sent.begin(),f.sent.end(),[](const auto& p) { return p.second[0] == 1; });
  CHECK(keepalives >= 10 && keepalives <= 12);
  f.inbox.push_back({2,Bytes{2}}); f.inbox.push_back({2,Bytes{2}}); l->pump(0);
  CHECK(std::count_if(f.logs.begin(),f.logs.end(),[](const std::string& s) { return s.find("dropped packet version") != std::string::npos; }) == 1);
  CHECK(!l->pad(2,0));
  for (const char* event : {"leave","left","away","kicked"}) {
    f.push({{"t",event},{"player",{{"id","c1"}}}}); l->pump(0); CHECK(!l->lost());
  }
  // Lobby arrivals/reservations do not mutate or interrupt the frozen session.
  auto r = f.roster(); r[1]["connectionId"] = "replacement"; f.push({{"t","join"},{"participants",r}}); l->pump(0); CHECK(!l->lost());
}
static void common_end_reason() {
  ++cases;
  Fixture a,b(2,1); SessionConfig ca,cb; auto la = a.start(ca), lb = b.start(cb);
  const json data = {{"kind","mu-console-end"},{"v",1},{"sessionId",cb.session_id},{"reason","your friend disconnected"}};
  b.push({{"t","msg"},{"from","outsider"},{"d",data}}); lb->pump(0); CHECK(!lb->lost());
  a.push({{"t","sessionEnded"},{"sessionId",ca.session_id},{"port",2},{"reason","left"}}); la->pump(0); CHECK(la->lost());
  b.push({{"t","msg"},{"from","c0"},{"d",a.messages.back()["d"]}}); lb->pump(0); CHECK(lb->lost() == la->lost());
}
static void final_rooms_contract() {
  ++cases;
  Fixture f; SessionConfig c; auto link = f.start(c);
  const auto begin = *std::find_if(f.messages.begin(),f.messages.end(),[](const json& m) { return str(m,"t") == "begin"; });
  CHECK(begin == json({{"t","begin"},{"revision",7},{"console",true},{"inputDelay",3},{"settingsDigest",""}}));
  f.push({{"t","sessionEnded"},{"sessionId","other"},{"reason","left"},{"port",2}}); link->pump(0); CHECK(!link->lost());
  f.push({{"t","sessionEnded"},{"sessionId",c.session_id},{"reason","host_ended"},{"port",1}}); link->pump(0);
  CHECK(link->lost() == "host_ended"); CHECK(f.messages.back()["d"]["reason"] == "host_ended");
  Fixture during_delay; bool ended = false;
  during_delay.on_wait = [&] {
    if (ended || during_delay.now < 300) return;
    ended = true;
    during_delay.push({{"t","sessionEnded"},{"sessionId","session-fixed"},{"reason","host_ended"},{"port",1}});
  };
  std::string start_error;
  CHECK(!start_session(during_delay.input(),c,start_error,during_delay.io())); CHECK(start_error == "host_ended");
  for (int variant = 0; variant < 8; ++variant) {
    Fixture shape; shape.alter_start = [variant](json& m) {
      if (variant == 0) m.erase("participants"); // console roster is authoritative
      if (variant == 1) m["console"]["seed"] = "FEDCBA9876543210";
      if (variant == 2) m["console"]["inputDelay"] = 0;
      if (variant == 3) m["console"]["settingsDigest"] = "bad digest";
      if (variant == 4) m["console"]["participants"][1]["port"] = 3;
      if (variant == 5) m["matchId"] = "different-session";
      if (variant == 6) m["console"]["settingsDigest"] = std::string(129,'a');
      if (variant == 7) m["console"]["settingsDigest"] = nullptr;
    };
    std::string error; auto result = start_session(shape.input(),c,error,shape.io()); CHECK(bool(result) == (variant == 0));
  }
}
static void boot_liveness() {
  ++cases;
  for (int count : {2,4}) {
    std::vector<std::unique_ptr<Fixture>> fixtures;
    std::vector<std::unique_ptr<SessionLink>> links;
    for (int seat = 0; seat < count; ++seat) {
      fixtures.push_back(std::make_unique<Fixture>(count,seat));
      SessionConfig c; links.push_back(fixtures.back()->start(c));
    }
    for (int seat = 0; seat < count; ++seat) fixtures[seat]->deliver = [&,seat](unsigned id,const Bytes& b) {
      fixtures[id-1]->inbox.emplace_back(seat+1,b);
    };
    // Keep servicing links through a boot gap. Neutral pads are not readiness.
    for (auto& link : links) link->send_pad(0,{});
    for (int elapsed = 0; elapsed <= 8000; elapsed += 100) {
      for (auto& fixture : fixtures) fixture->now += 100;
      if (elapsed == 2000) for (int seat = 0; seat < count-1; ++seat) links[seat]->send_digest(0,42);
      if (elapsed == 8000) links.back()->send_digest(0,42);
      for (auto& link : links) { link->pump(0); CHECK(!link->lost()); }
      for (auto& link : links) link->pump(0);
      if (elapsed < 8000) for (const auto& fixture : fixtures)
        CHECK(std::none_of(fixture->logs.begin(),fixture->logs.end(),[](const auto& line) { return line.find("liveness armed") != std::string::npos; }));
    }
    for (const auto& fixture : fixtures) CHECK(std::count(fixture->logs.begin(),fixture->logs.end(),"console: all peers at frame 0; liveness armed") == 1);
    for (int seat = 0; seat < count; ++seat) for (int peer = 0; peer < count; ++peer)
      if (seat != peer) CHECK(links[seat]->digest(peer+1,0) == 42);
  }
  Fixture f; SessionConfig c; auto link = f.start(c);
  link->send_digest(0,42);
  const auto began = f.now;
  f.now = began+59999; link->pump(0); CHECK(!link->lost());
  ++f.now; link->pump(0); CHECK(link->lost() == kBootTimeout);
  CHECK(f.now-began == 60000);
  CHECK(f.logs.back() == std::string("console: session ended: ")+kBootTimeout);
}
static void background_boot_service() {
  ++cases; Fixture f; SessionConfig c; std::string error;
  auto io = f.io(); const auto began = yougame::transport::now_ms();
  io.clock = [&] { return f.now + yougame::transport::now_ms()-began; };
  io.background_service = true;
  std::atomic<unsigned> packets{0}; const auto send = io.send;
  io.send = [&](unsigned id,const Bytes& b) { send(id,b); ++packets; };
  auto link = start_session(f.input(),c,error,std::move(io)); CHECK(link && error.empty());
  const auto before = packets.load();
  // No engine, body, or explicit pump runs while content/library loading holds the caller.
  std::this_thread::sleep_for(std::chrono::milliseconds(240));
  CHECK(packets.load() >= before+2); CHECK(!link->lost());
  link.reset(); // joins service before destroying the backend/fake
  CHECK(std::none_of(f.messages.begin(),f.messages.end(),[](const json& m) { return str(m,"t") == "leave"; }));
}
static void interruption_shapes() {
  ++cases;
  for (const char* reason : {"left","kicked","away","host_ended"}) {
    Fixture f; SessionConfig c; auto link = f.start(c);
    f.push({{"t","sessionEnded"},{"sessionId",c.session_id},{"reason",reason},{"port",2}});
    link->pump(0); CHECK(link->lost() == reason);
    CHECK(f.logs.back() == std::string("console: session ended: ")+reason);
    CHECK(f.messages.back()["d"]["reason"] == reason);
  }
  Fixture mixed; mixed.alter_start = [](json& m) {
    m = {{"t","command-error"},{"action","begin"},{"error","Console sessions require the same lobby compatibility for every seat"},{"code","console_mixed_builds"}};
  };
  SessionConfig c; std::string error;
  CHECK(!start_session(mixed.input(),c,error,mixed.io())); CHECK(error == kMixedBuilds);
  CHECK(std::count(mixed.logs.begin(),mixed.logs.end(),"console: command-error console_mixed_builds") == 1);
  for (int delay : {1,8}) {
    Fixture f; f.start_delay = delay; auto request = f.input(); request["inputDelay"] = delay;
    CHECK(start_session(request,c,error,f.io()));
    for (const auto& m : f.messages) if (str(m,"t") == "begin") CHECK(m["inputDelay"] == delay);
  }
  Fixture digest; digest.alter_start = [](json& m) { m["console"]["settingsDigest"] = "rules:v1_abc-123"; };
  CHECK(start_session(digest.input(),c,error,digest.io())); CHECK(c.settings_digest == "rules:v1_abc-123");
}
static void relaunch_no_leave() {
  ++cases; Fixture first; SessionConfig config; std::string error; json launch;
  auto io = first.io();
  io.prepared = [&](const SessionConfig& c) {
    CHECK(handoff(c,true,{},0,"",[&](const json& m) { launch = m["request"]; },[](const std::string&) {},[](const char*) {}));
  };
  CHECK(!start_session(first.input(),config,error,std::move(io))); CHECK(error.empty());
  first.stream.close(1000,1000); // ConsoleBackend's clean close after handoff
  CHECK(first.requests.back()["type"] == "netClose" && first.requests.back()["code"] == 1000);
  CHECK(std::none_of(first.messages.begin(),first.messages.end(),[](const json& m) { return str(m,"t") == "leave" || (m.count("d") && str(m["d"],"kind") == "mu-console-end"); }));
  Fixture second; second.session = launch["sessionId"];
  auto link = start_session(launch,config,error,second.io()); CHECK(link && error.empty());
  CHECK(first.roster()[0]["id"] == second.roster()[0]["id"]);
  CHECK(config.my_port == launch["port"]);
  CHECK(std::none_of(second.messages.begin(),second.messages.end(),[](const json& m) { return str(m,"t") == "begin" || str(m,"t") == "leave"; }));
  for (const auto& q : second.requests) if (str(q,"type") == "netOpen") CHECK(q["session"] == launch["sessionId"]);
}
static void rejoin() {
  ++cases;
  for (int seat : {0,1}) {
    Fixture f(2,seat); f.session = "session-fixed";
    auto request = json{{"type","consoleSession"},{"room","ABCDEF"},{"players",2},{"port",seat+1},
      {"sessionId","session-fixed"},{"seed","fedcba9876543210"},{"epochMs",1793443200123LL},{"inputDelay",3}};
    f.reply_candidates = false;
    bool gap = false;
    f.on_wait = [&] {
      if (f.now < 60001 || gap) return;
      gap = true; f.reply_candidates = true;
      f.push({{"t","away"},{"reason","dropped"}});
    };
    SessionConfig c; std::string error;
    auto link = start_session(request,c,error,f.io()); CHECK(link && error.empty()); CHECK(gap);
    CHECK(std::none_of(f.messages.begin(),f.messages.end(),[](const json& m) { return str(m,"t") == "begin"; }));
    for (const auto& q : f.requests) if (str(q,"type") == "netOpen") CHECK(q["session"] == "session-fixed");
    CHECK(c.my_port == seat+1 && c.session_id == "session-fixed");
    // Each full launch field is an expectation checked against Rooms' frozen object.
    for (auto key : {"sessionId","seed","epochMs","inputDelay","port"}) {
      Fixture changed(2,seat); changed.session = "session-fixed"; auto bad = request;
      if (std::string(key) == "sessionId") bad[key] = "other";
      else if (std::string(key) == "seed") bad[key] = "0000000000000000";
      else bad[key] = std::string(key) == "port" ? 2-seat : std::string(key) == "inputDelay" ? 4 : 123;
      CHECK(!start_session(bad,c,error,changed.io())); CHECK(error == "console session start changed");
    }
  }
}
static void routes_and_delay() {
  ++cases;
  for (auto path : {"lan","direct","relay","unknown"}) {
    auto n = direct_notice({{"link",9},{"event","up"},{"path",path},{"rttMs",42}});
    CHECK(n.link == 9 && n.state == 2 && n.path == path && n.rtt == 42);
    Fixture f; f.path = path; SessionConfig c; auto l = f.start(c);
    CHECK(std::find_if(f.logs.begin(),f.logs.end(),[&](const std::string& s) { return s.find(std::string("up via ")+path) != std::string::npos; }) != f.logs.end());
    f.path = "lan"; l->pump(0);
    if (std::string(path) != "lan") CHECK(std::count(f.logs.begin(),f.logs.end(),"console: link to port 2 now lan") == 1);
    l->pump(0); CHECK(std::count(f.logs.begin(),f.logs.end(),"console: link to port 2 now lan") <= 1);
  }
  auto n = direct_notice({{"link",9},{"event","up"}}); CHECK(n.state == 2 && n.path == "unknown");
  n = direct_notice({{"link",9},{"event","path"},{"path","direct"},{"rttMs",8}}); CHECK(!n.state && n.path == "direct" && n.rtt == 8);
  n = direct_notice({{"link",9},{"event","up"},{"via","relay"}}); CHECK(n.path == "unknown");
  CHECK(measured_delay(0,1) == 2); CHECK(measured_delay(33,1) == 2); CHECK(measured_delay(34,1) == 3);
  CHECK(measured_delay(66,1) == 3); CHECK(measured_delay(67,1) == 4); CHECK(measured_delay(200,1) == 7);
  CHECK(measured_delay(300,1) == 8); CHECK(measured_delay(0,7) == 7);
  Fixture measured; measured.reported_rtt = 200; SessionConfig c; auto l = measured.start(c); CHECK(c.input_delay == 7);
  CHECK(std::count(measured.logs.begin(),measured.logs.end(),"console: input delay 7 (worst rtt 200 ms)") == 1);
  for (int seat : {0,1}) {
    Fixture silent(2,seat); silent.reply_delay = false; std::string error;
    const auto start_at = silent.now;
    CHECK(!start_session(silent.input(),c,error,silent.io())); CHECK(error == "couldn't agree on a delay");
    CHECK(silent.now-start_at >= 3500 && silent.now-start_at < 3600);
  }
  Fixture minimum; minimum.start_delay = 7; minimum.reported_rtt = 0;
  auto min_link = minimum.start(c); CHECK(c.input_delay == 7);
  Fixture helper; auto io = helper.io(); io.rtt = [](unsigned) { return std::vector<uint64_t>{230}; };
  std::string error; auto h = start_session(helper.input(),c,error,std::move(io)); CHECK(h && c.input_delay == 8);
}
static void human_room_start() {
  ++cases;
  for (int seat : {0,1}) {
    Fixture f(2,seat); auto io = f.io(); bool prepared = false, prompt = false;
    io.confirm_start = [&] { return f.now >= 1001; };
    io.waiting = [&](const std::string& text,bool ready) {
      if (seat == 0 && ready) { CHECK(text == "Press START to play together"); prompt = true; }
    };
    io.prepared = [&](const SessionConfig& c) { CHECK(c.my_port == seat+1); prepared = true; };
    SessionConfig c; std::string error; auto l = start_session(f.input(),c,error,std::move(io));
    CHECK(!l && prepared && error.empty()); CHECK(seat || (prompt && f.start_at >= 1001));
    CHECK(std::none_of(f.requests.begin(),f.requests.end(),[](const json& q) { return str(q,"type") == "directOpen"; }));
    CHECK(std::none_of(f.messages.begin(),f.messages.end(),[](const json& m) { return str(m,"t") == "leave"; }));
  }
}
// Four actual startup instances. The worst pair is ports 3/4, away from the host.
// Drop the first report/offer/ACK/commit in each direction to exercise retransmission.
static void real_delay_mesh() {
  ++cases;
  struct Packet { uint64_t due; unsigned from; Bytes bytes; };
  std::mutex mutex; std::array<std::deque<Packet>,4> queues;
  std::map<std::pair<int,int>,int> drops;
  std::array<SessionConfig,4> configs;
  std::array<std::string,4> errors;
  std::array<std::unique_ptr<Fixture>,4> fixtures;
  std::vector<std::future<std::unique_ptr<SessionLink>>> starts;
  for (int s = 0; s < 4; ++s) fixtures[s] = std::make_unique<Fixture>(4,s);
  for (int s = 0; s < 4; ++s) {
    fixtures[s]->auto_delay = false;
    auto io = fixtures[s]->io(); io.clock = yougame::transport::now_ms;
    io.wait = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    io.send = [&,s](unsigned id,const Bytes& b) {
      std::lock_guard<std::mutex> lock(mutex);
      if (b[0] == 0xc1 && b[2] >= 3 && drops[{s*4+int(id),b[2]}]++ == 0) return;
      const uint64_t latency = s >= 2 && id >= 3 ? 70 : 10;
      queues[id-1].push_back({yougame::transport::now_ms()+latency,unsigned(s+1),b});
    };
    io.receive = [&,s](unsigned& id,Bytes& b) {
      std::lock_guard<std::mutex> lock(mutex); auto& q = queues[s];
      auto p = std::find_if(q.begin(),q.end(),[](const Packet& p) { return p.due <= yougame::transport::now_ms(); });
      if (p == q.end()) return false;
      id = p->from; b = std::move(p->bytes); q.erase(p); return true;
    };
    starts.push_back(std::async(std::launch::async,[&,s,io = std::move(io)]() mutable {
      return start_session(fixtures[s]->input(),configs[s],errors[s],std::move(io));
    }));
  }
  std::vector<std::unique_ptr<SessionLink>> links;
  // Keep completed hosts pumping commits while other machines finish their handshake.
  while (links.size() < 4) {
    for (int s = 0; s < 4; ++s) if (starts[s].valid() && starts[s].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      auto l = starts[s].get(); CHECK(l && errors[s].empty()); links.push_back(std::move(l));
    }
    for (auto& l : links) l->pump(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int s = 0; s < 4; ++s) {
    CHECK(configs[s].input_delay == configs[0].input_delay); CHECK(configs[s].input_delay >= 6);
    CHECK(std::count_if(fixtures[s]->logs.begin(),fixtures[s]->logs.end(),[](const std::string& line) { return line.find("console: input delay ") == 0; }) == 1);
  }
}
static void ack_and_pacing(bool measure_only) {
  ++cases;
  Fixture f; SessionConfig c; auto link = f.start(c);
  f.now += 3001; link->pump(0); // finish the host's startup commit repetition
  f.sent.clear(); link->send_pad(0,sample(0,1));
  CHECK(f.sent.size() == 1 && f.sent.back().second[2] == 1); // sent before any pump
  auto incoming = f.sent.back().second; incoming[1] = 2;
  f.inbox.emplace_back(2,incoming); link->pump(0);
  const auto ack_pads = f.sent.back().second[2];
  CHECK(f.sent.size() == 2 && f.sent.back().second[15] == 0); // immediate ACK of frame 0
  f.now += 16; link->send_pad(1,sample(1,1));
  const auto new_pads = f.sent.back().second[2];
  f.now += 34; link->pump(0);
  const auto at_50 = f.sent.back().second;
  std::printf("ack/pacing: pads in ACK %u, pads on next sample %u, retry at 50 ms %s\n",
    unsigned(ack_pads),unsigned(new_pads),at_50[2] == 1 && at_50[23] == 0 ? "yes" : "no");
  if (!measure_only) { CHECK(ack_pads == 0); CHECK(new_pads == 1); CHECK(at_50[2] == 1 && at_50[23] == 0); }
  // If work already received the awaited input, return without another timed sleep.
  const auto before = f.now; f.inbox.emplace_back(2,incoming); link->pump(10);
  if (!measure_only) CHECK(f.now == before);
  // An ACK-only packet must not provoke another ACK.
  Bytes ack(incoming.begin(),incoming.begin()+20); ack[2] = ack[3] = 0;
  const auto count = f.sent.size(); f.inbox.emplace_back(2,ack); link->pump(0);
  CHECK(f.sent.size() == count);
  // Another service/engine caller must enter while pump is asleep on the doorbell.
  Fixture sleeping; auto io = sleeping.io(); std::atomic<bool> block{false};
  std::promise<void> entered, release; auto released = release.get_future().share();
  io.wait = [&](int ms) { if (block) { entered.set_value(); released.wait(); } else sleeping.wait(ms); };
  std::string error; auto idle = start_session(sleeping.input(),c,error,std::move(io)); CHECK(idle && error.empty());
  idle->pump(0); // drain any final startup agreement ACK before testing an empty wait
  block = true;
  auto waiter = std::async(std::launch::async,[&] { idle->pump(10); });
  CHECK(entered.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  auto sender = std::async(std::launch::async,[&] { idle->send_pad(0,{}); idle->pump(0); });
  const bool concurrent = sender.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
  release.set_value(); waiter.get(); sender.get();
  std::printf("mutex: send_pad and service pump completed during doorbell wait: %s\n",concurrent ? "yes" : "no");
  if (!measure_only) CHECK(concurrent);
}
// A delayed datagram ring, with the production Link service worker and two real Engines.
// No sockets: the clock/pacer models 60 Hz bodies and each packet spends 15 ms in flight.
static void latency(bool measure_only) {
  ++cases;
  struct Packet { uint64_t due; unsigned from; Bytes bytes; };
  struct Metrics { uint64_t wait = 0, resent = 0, rtt = 0, max_rtt = 0, samples = 0; };
  std::mutex mutex;
  std::condition_variable doorbell;
  std::array<std::deque<Packet>,2> queues;
  std::array<std::map<uint32_t,uint64_t>,2> first_sent;
  std::array<uint32_t,2> acked{UINT32_MAX,UINT32_MAX};
  std::array<std::atomic<unsigned>,2> window{};
  std::array<std::array<Metrics,2>,2> metrics{};
  auto word = [](const Bytes& b,size_t at) {
    uint32_t v = 0; for (int n = 0; n < 4; ++n) v = (v << 8) | b[at+n]; return v;
  };
  std::array<std::unique_ptr<Fixture>,2> fixtures;
  std::array<SessionConfig,2> configs;
  std::array<std::string,2> errors;
  std::array<std::future<std::unique_ptr<SessionLink>>,2> starts;
  for (int s = 0; s < 2; ++s) {
    fixtures[s] = std::make_unique<Fixture>(2,s);
    fixtures[s]->start_delay = 4;
    fixtures[s]->auto_delay = false;
    auto io = fixtures[s]->io();
    io.clock = yougame::transport::now_ms;
    io.background_service = true;
    io.send = [&,s](unsigned id,const Bytes& b) {
      std::lock_guard<std::mutex> lock(mutex);
      CHECK(id == unsigned(2-s) && b.size() <= 1200);
      const auto now = yougame::transport::now_ms();
      if (b[0] == kWireVersion) for (unsigned n = 0; n < b[2]; ++n) {
        const auto f = word(b,20+n*15);
        if (!first_sent[s].emplace(f,now).second) ++metrics[s][window[s]].resent;
      }
      queues[1-s].push_back({now+15,unsigned(s+1),b});
      doorbell.notify_all();
    };
    io.receive = [&,s](unsigned& id,Bytes& b) {
      std::lock_guard<std::mutex> lock(mutex);
      auto& q = queues[s]; const auto now = yougame::transport::now_ms();
      if (q.empty() || q.front().due > now) return false;
      id = q.front().from; b = std::move(q.front().bytes); q.pop_front();
      if (b[0] == kWireVersion) {
        const auto ack = word(b,12);
        if (ack != UINT32_MAX && (acked[s] == UINT32_MAX || ack > acked[s])) {
          // Same first-send-to-contiguous-ACK estimator as Link::receive.
          auto& m = metrics[s][window[s]];
          CHECK(first_sent[s].count(ack));
          m.rtt = now-first_sent[s].at(ack); m.max_rtt = std::max(m.max_rtt,m.rtt); ++m.samples;
          acked[s] = ack;
        }
      }
      return true;
    };
    io.wait = [&,s](int ms) {
      std::unique_lock<std::mutex> lock(mutex);
      const auto until = yougame::transport::now_ms()+uint64_t(ms);
      for (;;) {
        const auto now = yougame::transport::now_ms();
        if (now >= until || (!queues[s].empty() && queues[s].front().due <= now)) return;
        const auto wake = queues[s].empty() ? until : std::min(until,queues[s].front().due);
        doorbell.wait_for(lock,std::chrono::milliseconds(wake-now));
      }
    };
    starts[s] = std::async(std::launch::async,[&,s,io = std::move(io)]() mutable {
      return start_session(fixtures[s]->input(),configs[s],errors[s],std::move(io));
    });
  }
  std::array<Engine,2> engines;
  bool finishing = false;
  for (int s = 0; s < 2; ++s) {
    auto link = starts[s].get(); CHECK(link && errors[s].empty()); CHECK(configs[s].input_delay == 4);
    EngineHooks h;
    h.log = [&,s](const std::string& line) { CHECK(line.find("desync") == std::string::npos); };
    h.notice = [&](const char*) { CHECK(finishing); };
    CHECK(engines[s].begin(std::move(link),configs[s],std::move(h)));
  }
  const auto epoch = std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
  std::array<std::future<void>,2> bodies;
  for (int s = 0; s < 2; ++s) bodies[s] = std::async(std::launch::async,[&,s] {
    for (uint32_t f = 0; f < 1200; ++f) {
      std::this_thread::sleep_until(epoch+std::chrono::microseconds(uint64_t(f)*1000000/60));
      window[s] = f/600;
      Pads hw{}; for (auto& p : hw) p.err = -1;
      hw[0] = sample(f,s+1); hw[0].err = 0;
      MuStatePod state{}; state.rng = f*47; state.menu_selection = f%5;
      const auto began = yougame::transport::now_ms();
      CHECK(engines[s].advance(hw,state));
      const auto elapsed = yougame::transport::now_ms()-began;
      { std::lock_guard<std::mutex> lock(mutex); metrics[s][f/600].wait += elapsed; }
      CHECK(engines[s].frame() == f);
      Pads out; engines[s].read({},out);
      for (int p = 0; p < 2; ++p) {
        auto expected = f < 4 ? MuPadStatus{} : sample(f-4,p+1); expected.err = 0;
        same_pad(out[p],expected);
      }
    }
  });
  for (auto& body : bodies) body.get();
  finishing = true;
  for (auto& engine : engines) engine.end("latency test complete"); // join before inspecting metrics/logs
  for (int s = 0; s < 2; ++s) for (int w = 0; w < 2; ++w) {
    const auto& m = metrics[s][w];
    std::printf("latency seat %d frames %d-%d: advance/wait <= %llu ms, game rtt %llu ms (max %llu, %llu samples), resent %llu (%.3f/frame)\n",
      s,w*600,(w+1)*600-1,(unsigned long long)m.wait,(unsigned long long)m.rtt,
      (unsigned long long)m.max_rtt,(unsigned long long)m.samples,(unsigned long long)m.resent,double(m.resent)/600);
    CHECK(m.samples > 500);
    if (!measure_only) { CHECK(m.wait < 500); CHECK(m.max_rtt <= 60); CHECK(m.resent < 3000); }
  }
  for (const auto& f : fixtures) for (const auto& line : f->logs)
    if (line.find("console: port ") == 0) std::printf("%s\n",line.c_str());
}
int main(int argc,char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--pacing") {
    ack_and_pacing(argc > 2 && std::string(argv[2]) == "--measure"); return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--latency") {
    latency(argc > 2 && std::string(argv[2]) == "--measure");
    std::printf("yougame_console_latency: %d cases, %d checks passed\n",cases,checks.load()); return 0;
  }
  ack_and_pacing(false);
  boot_liveness(); background_boot_service(); interruption_shapes(); relaunch_no_leave(); final_rooms_contract(); rejoin(); routes_and_delay(); human_room_start(); real_delay_mesh();
  intake(); starts(); agent_create_join(); host_waits_for_seats(); failure_cases(); invalid_start(); mesh(2,false); mesh(2,true); mesh(4,true); departures_and_waits(); common_end_reason();
  sparse_starts(); sparse_invalid_rosters(); sparse_relaunch(); sparse_peer_logs();
  std::printf("yougame_console_link: %d cases, %d checks passed\n",cases,checks.load());
}
