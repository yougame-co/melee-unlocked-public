// Real console protocol + real broker Stream, entirely offline.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include "yougame_console_link.h"
#include "yougame_console_engine.h"
#include "yougame_console_entry.h"
#include "yougame_console_card.h"
#include "yougame_agent_test.h"
#include "yougame_update.h"
#include <cstdio>
#include <set>
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
  std::function<json(const json&)> direct_open;
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
    if (type == "directOpen") return direct_open ? direct_open(q) : json{{"link",unsigned(q["peer"].get<std::string>()[1]-'0'+1)}};
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
static void failed_session_new_invite() {
  ++cases;
  yougame::friends::Events invites;
  invites.current_room("OldRoom");
  Fixture failed(2,1); failed.attach_ok = false;
  auto old = failed.input(); old["room"] = "OldRoom";
  SessionConfig config; std::string error;
  CHECK(!start_session(old,config,error,failed.io()) && !error.empty());
  invites.current_room({}); // the production console IO close releases/retire this room
  invites.menu_scene(yougame::friends::menu_safe(2,0));
  CHECK(invites.push({{"type","join"},{"room","NewRoom"}},1));
  CHECK(!invites.push({{"type","join"},{"room","OldRoom"}},2));
  CHECK(invites.join_pending());
  Fixture retry(2,1); auto next = retry.input(); next["room"] = invites.take_join(true);
  CHECK(next["room"] == "NewRoom");
  auto link = start_session(next,config,error,retry.io()); CHECK(link && error.empty());
  CHECK(config.room == "NewRoom");
  CHECK(retry.requests.front()["room"] == "NewRoom");
  CHECK(!invites.push({{"type","join"},{"room","OldRoom"}},3));

  // A fresh invite lets the guest retry the SAME live room after a failed join.
  ++cases; yougame::friends::Events same;
  same.current_room("OldRoom");
  CHECK(!start_session(old,config,error,failed.io()) && !error.empty());
  same.current_room({});
  CHECK(!same.push({{"type","join"},{"room","OldRoom"}},4)); // stale page push, no invite
  CHECK(same.push({{"type","notice"},{"kind","invite"},{"from","Host"},{"text","Play with me"}},5));
  CHECK(same.push({{"type","join"},{"room","OldRoom"}},6));
  Fixture same_retry(2,1); auto same_request = same_retry.input(); same_request["room"] = same.take_join(true);
  same.current_room(same_request["room"]);
  CHECK(same_request["room"] == "OldRoom");
  auto same_link = start_session(same_request,config,error,same_retry.io());
  CHECK(same_link && error.empty() && config.room == "OldRoom");
  same.current_room({});
  CHECK(!same.push({{"type","join"},{"room","OldRoom"}},7)); // the new generation also retires
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
    l->pump(0);
    if (std::string(event) != "closed") { CHECK(!l->lost() && l->reconnecting()); f.closed = true; l->pump(0); }
    CHECK(l->lost());
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
  // An unverified replacement cannot inherit the frozen peer's port.
  auto r = f.roster(); r[1]["connectionId"] = "replacement"; f.push({{"t","join"},{"participants",r}}); l->pump(0);
  CHECK(l->lost() == "your friend disconnected");
}
static void common_end_reason() {
  ++cases;
  Fixture a,b(2,1); SessionConfig ca,cb; auto la = a.start(ca), lb = b.start(cb);
  const json data = {{"kind","mu-console-end"},{"v",1},{"sessionId",cb.session_id},{"reason","your friend disconnected"}};
  b.push({{"t","msg"},{"from","outsider"},{"d",data}}); lb->pump(0); CHECK(!lb->lost());
  a.push({{"t","sessionEnded"},{"sessionId",ca.session_id},{"port",2},{"reason","left"}}); la->pump(0); CHECK(la->lost());
  b.push({{"t","msg"},{"from","c0"},{"d",a.messages.back()["d"]}}); lb->pump(0); CHECK(lb->lost() == la->lost());
}
// The desktop app's consoleSession check (YouGame desktop/src/console-session.ts consoleSessionError,
// apps up to 0.13.1225: exactly these 8 fields; 1226 adds an optional cardDigest) and bridge.ts's
// relaunch envelope (exactly {type, request}, at most 1024 bytes). A dropped relaunch hangs Friends.
static std::string app_console_session_error(const json& r) {
  static const std::set<std::string> fields = {"type","room","sessionId","port","players","seed","epochMs","inputDelay"};
  if (!r.is_object()) return "request";
  for (auto it = r.begin(); it != r.end(); ++it) if (!fields.count(it.key())) return "fields";
  auto integer = [&](const char* k) { return r.count(k) && r[k].is_number_integer(); };
  if (str(r,"type") != "consoleSession") return "type";
  const auto room = str(r,"room");
  if (room.size() < 4 || room.size() > 16 || room.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) return "room";
  const auto id = str(r,"sessionId");
  if (id.empty() || id.size() > 64 || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) return "sessionId";
  if (!integer("players") || r["players"].get<int64_t>() < 2 || r["players"].get<int64_t>() > 4) return "players";
  if (!integer("port") || r["port"].get<int64_t>() < 1 || r["port"].get<int64_t>() > r["players"].get<int64_t>()) return "port";
  const auto seed = str(r,"seed");
  if (seed.size() != 16 || seed.find_first_not_of("0123456789abcdef") != std::string::npos) return "seed";
  if (!integer("epochMs") || r["epochMs"].get<int64_t>() < 0) return "epochMs";
  if (!integer("inputDelay") || r["inputDelay"].get<int64_t>() < 1 || r["inputDelay"].get<int64_t>() > 8) return "inputDelay";
  return {};
}
static void app_launch_contract() {
  for (const bool shared : {true,false}) for (const size_t players : {size_t(2),size_t(4)}) {
    ++cases;
    SessionConfig c; c.room = "ABCDEF"; c.session_id = "b1c99ca4-63b7-4502-969e-c74c8b547c7a"; c.my_port = 2;
    c.ports.clear(); for (size_t p = 1; p <= players; ++p) c.ports.push_back(int(p));
    c.seed = 0x0123456789abcdefull; c.epoch_ms = 1793443200123LL; c.input_delay = 2;
    c.default_card = !shared;
    if (shared) c.card_digest = std::string(64,'a');
    const auto launch = session_launch(c);
    CHECK(app_console_session_error(launch).empty());
    const auto relaunch = relaunch_message(c);
    CHECK(relaunch.size() == 2 && relaunch.count("type") && relaunch.count("request") && relaunch.dump().size() <= 1024);
    CHECK(app_console_session_error(relaunch["request"]).empty());
  }
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
// No Engine/body pump: these calls model the transport worker while gameplay is at 5 fps.
static void watchdog_receive_clock() {
  ++cases; Fixture f; SessionConfig c; auto link = f.start(c);
  link->send_digest(0,42);
  auto heartbeat = f.sent.back().second; heartbeat[1] = 2;
  heartbeat[12] = heartbeat[13] = heartbeat[14] = heartbeat[15] = 255;
  f.inbox.emplace_back(2,heartbeat); link->pump(0);
  CHECK(!link->lost());
  // Receiving at 5 Hz keeps the transport alive, regardless of simulation progress.
  for (int frame = 0; frame < 40; ++frame) {
    f.now += 200; f.inbox.emplace_back(2,heartbeat); link->pump(0); CHECK(!link->lost());
  }
  // A nearly-five-second foreground gap does not end a peer that just sent again.
  f.now += 4999; f.inbox.emplace_back(2,heartbeat); link->pump(0); CHECK(!link->lost());
  f.now += 4999; link->pump(0); CHECK(!link->lost());
  ++f.now; link->pump(0); CHECK(!link->lost() && link->reconnecting());
  const auto diagnostic = "console: link to port 2 lost; waiting for the room to come back";
  CHECK(std::count(f.logs.begin(),f.logs.end(),diagnostic) == 1);
  auto held = f.roster(); held[1]["away"] = true;
  f.push({{"t","away"},{"participants",held}}); link->pump(0);
  f.now += 74999; link->pump(0); CHECK(!link->lost());
  ++f.now; link->pump(0); CHECK(link->lost() == "your friend disconnected");
  CHECK(f.logs[f.logs.size()-2] == "console: relink gave up after 75000 ms (room did not return)");
  CHECK(f.logs.back() == "console: session ended: your friend disconnected");
  // A closed helper link now enters recovery immediately, before the silence threshold.
  Fixture closed; auto other = closed.start(c); closed.state = 3; other->pump(0);
  CHECK(!other->lost() && other->reconnecting());
  CHECK(closed.logs.back() == diagnostic);
}
static void relink_failures() {
  auto ends_sent = [](const Fixture& f) {
    return std::count_if(f.messages.begin(),f.messages.end(),[](const json& m) {
      return str(m,"t") == "msg" && m.count("d") && str(m["d"],"kind") == "mu-console-end";
    });
  };
  auto logged = [](const Fixture& f,const std::string& line) { return std::find(f.logs.begin(),f.logs.end(),line) != f.logs.end(); };
  for (int mode = 0; mode < 10; ++mode) {
    ++cases; Fixture f; bool cancel = false, rejoining = mode < 7;
    auto io = f.io(); io.room_rejoining = [&] { return rejoining; }; io.cancelled = [&] { return cancel; };
    SessionConfig c; std::string error; auto link = start_session(f.input(),c,error,std::move(io)); CHECK(link);
    // The app refuses every reopen once direct links are off: give up now, not after 75 s.
    if (mode == 7) f.direct_open = [](const json&) { return json{{"link",0},{"refused","no_direct"}}; };
    f.state = 3; link->pump(0); const auto lost_at = f.now;
    CHECK(!link->lost() && link->reconnecting());
    if (mode == 0 || mode == 6) {
      if (mode == 6) rejoining = false; // room returned, helper never comes up
      f.now += 74999; link->pump(0); CHECK(!link->lost());
      ++f.now; link->pump(0);
      CHECK(link->lost() == "your friend disconnected");
      CHECK(f.logs[f.logs.size()-2] == std::string("console: relink gave up after 75000 ms (") +
          (mode == 0 ? "room did not return)" : "link did not come up)"));
    } else if (mode == 1) {
      auto released = f.roster(); released.erase(released.begin()+1);
      f.push({{"t","leave"},{"participants",released}}); link->pump(0); CHECK(link->lost());
      CHECK(f.now == lost_at);
    } else if (mode == 2) {
      cancel = true; link->pump(0); CHECK(link->lost() == kSelfLeft && f.now == lost_at);
      CHECK(ends_sent(f) == 0); // the room's leave/sessionEnded carries Leave; the friend keeps a notice
    } else if (mode == 3) {
      f.push({{"t","sessionEnded"},{"sessionId","other"},{"reason","left"}}); link->pump(0); CHECK(!link->lost());
      f.push({{"t","sessionEnded"},{"sessionId",c.session_id},{"reason","left"}}); link->pump(0); CHECK(link->lost() == "left");
    } else if (mode == 4) {
      for (uint32_t frame = 0; frame < kHistory; ++frame) link->send_pad(frame,sample(frame,1));
      CHECK(!link->lost()); link->send_pad(kHistory,{}); CHECK(link->lost() == "your friend disconnected");
      CHECK(ends_sent(f) == 1);
    } else if (mode == 7) {
      f.now += 100; link->pump(0);
      CHECK(link->lost() == "your friend disconnected");
      CHECK(logged(f,"console: relink gave up after 100 ms (direct links unavailable)"));
    } else if (mode == 8) {
      // No away roster, rejoin or re-seat: the friend's seat is not being held.
      while (!link->lost() && f.now-lost_at < 30000) { f.now += 100; link->pump(0); }
      CHECK(link->lost() == "your friend disconnected" && f.now-lost_at == 20000);
      CHECK(logged(f,"console: relink gave up after 20000 ms (seat not held)"));
    } else if (mode == 9) {
      f.push({{"t","msg"},{"from","c1"},{"d",{{"kind","mu-console-end"},{"v",1},{"sessionId",c.session_id},{"reason",kSelfLeft}}}});
      link->pump(0); CHECK(link->lost() == "left");
    } else {
      for (uint32_t frame = 0; frame < kHistory; ++frame) link->send_digest(frame,frame);
      CHECK(!link->lost()); link->send_digest(kHistory,0); CHECK(link->lost() == "your friend disconnected");
    }
  }
  // A returned room can have lost the session while the socket was away.
  for (bool replaced : {false,true}) {
    ++cases; Fixture f; SessionConfig c; auto link = f.start(c);
    f.state = 3; link->pump(0); CHECK(link->reconnecting());
    json welcome = {{"t","welcome"},{"participants",f.roster()}};
    if (replaced) { welcome["console"] = f.console_start(); welcome["console"]["sessionId"] = "another-session"; }
    f.push(welcome); link->pump(0); CHECK(link->lost() == "your friend disconnected");
  }
}
// Two real transports and Engines, deterministic room/helper fakes. The only room
// that drops is B's; A learns away/back through its still-connected room. Each
// helper allocates a fresh local id and becomes up only after BOTH games open.
static void relink_async_leave() {
  ++cases; Fixture f; SessionConfig c; std::string error;
  auto io = f.io(); const auto request = io.request;
  std::atomic<bool> live{false}, closed{false}, cancel{false};
  std::promise<void> entered, release, cleaned;
  auto released = release.get_future().share();
  io.request = [&](const json& q,int ms) {
    if (!live) return request(q,ms);
    CHECK(ms > 0 && ms <= 1000);
    if (str(q,"type") == "directOpen") { entered.set_value(); released.wait(); return json{{"link",20}}; }
    if (str(q,"type") == "directClose" && q["link"] == 20) cleaned.set_value();
    return json::object();
  };
  io.background_service = true;
  io.state = [&](unsigned) { return closed ? 3 : 2; };
  io.cancelled = [&] { return cancel.load(); };
  auto link = start_session(f.input(),c,error,std::move(io)); CHECK(link && error.empty());
  live = closed = true;
  CHECK(entered.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  auto sender = std::async(std::launch::async,[&] { link->send_pad(0,{}); link->pump(0); });
  const bool responsive = sender.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
  cancel = true; link->pump(0); CHECK(link->lost() == kSelfLeft);
  sender.get(); link.reset(); // must not wait for the bridge request; Leave takes effect now
  release.set_value();
  CHECK(cleaned.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(responsive); // late successful open is closed by the abandoned attempt
}
static void relink_engines(int dropped_seat, bool match = false) {
  ++cases;
  Fixture a,b(2,1); std::array<Fixture*,2> f{&a,&b};
  std::array<SessionConfig,2> c;
  std::array<SessionLink*,2> links{};
  std::array<unsigned,2> ids{2,1};
  std::array<bool,2> opened{false,false};
  bool dropped = false, back = false;
  std::array<int,2> rooms{10,11}; int stalls = 0;
  std::array<Engine,2> engines;
  std::array<yougame::friends::Lobby,2> lobbies;
  std::array<std::vector<std::string>,2> notices, hashes;
  for (int s = 0; s < 2; ++s) {
    lobbies[s].begin(s == 0);
    lobbies[s].observe({{"t","welcome"},{"me","c"+std::to_string(s)},{"host","c0"},{"participants",f[s]->roster()}});
    auto io = f[s]->io();
    io.rebound = [&,s](const std::string& old_id,const std::string& new_id) { lobbies[s].rebound(old_id,new_id); };
    io.connection = [&,s] { return rooms[s]; };
    io.room_rejoining = [&,s] { return s == dropped_seat && dropped && !back; };
    io.state = [&,s](unsigned id) {
      if (!dropped) return 2;
      if (id <= 2) return 3;
      return opened[0] && opened[1] ? 2 : 1;
    };
    const auto send = io.send;
    io.send = [&,s,send](unsigned id,const Bytes& bytes) {
      if (!links[s]) { send(id,bytes); return; } // startup's fake delay agreement
      if (dropped && (!back || !opened[0] || !opened[1])) return;
      f[1-s]->inbox.emplace_back(ids[1-s],bytes);
    };
    f[s]->direct_open = [&,s](const json& q) {
      if (!dropped) return json{{"link",ids[s]}};
      CHECK(back && str(q,"peer") == (1-s == dropped_seat ? "c9" : "c"+std::to_string(1-s)));
      CHECK(q["conn"] == rooms[s]);
      opened[s] = true; ids[s] = unsigned(20+s); return json{{"link",ids[s]}};
    };
    std::string error; auto link = start_session(f[s]->input(),c[s],error,std::move(io)); CHECK(link && error.empty());
    links[s] = link.get();
    EngineHooks h; h.now_ms = [&,s] { return f[s]->now; };
    h.notice = [&,s](const char* text) { notices[s].emplace_back(text); };
    h.log = [&,s](const std::string& line) { CHECK(line.find("desync") == std::string::npos); if (line.find("console: hash ") == 0) hashes[s].push_back(line); };
    h.responsive = [&] {
      CHECK(dropped);
      for (auto* seat : f) seat->now += 1000;
      if (++stalls == 15) {
        back = true; rooms[dropped_seat] = 99;
        auto roster = a.console_start()["participants"]; roster[dropped_seat]["connectionId"] = "c9"; roster[dropped_seat]["id"] = "new-id";
        f[1-dropped_seat]->push({{"t","back"},{"participants",roster}});
        auto welcome = b.console_start(); welcome["participants"] = roster;
        f[dropped_seat]->push({{"t","welcome"},{"me","c9"},{"room",{{"console",welcome}}}});
      }
      for (auto& engine : engines) engine.poll();
    };
    CHECK(engines[s].begin(std::move(link),c[s],std::move(h),false));
  }
  // Seed only the initial digest rendezvous so sequential Engines can meet at 0.
  for (auto* link : links) link->send_digest(0,Engine::state_digest({}));
  for (int n = 0; n < 4; ++n) for (auto* link : links) link->pump(0);
  for (uint32_t frame = 0; frame < 660; ++frame) {
    if (match && frame == 30) for (auto& engine : engines) {
      engine.enable_rollback(true); CHECK(engine.begin_match());
    }
    if (frame == 60) {
      dropped = true;
      auto roster = a.roster(); roster[dropped_seat]["away"] = true;
      f[1-dropped_seat]->push({{"t","away"},{"participants",roster}});
    }
    for (int s = 0; s < 2; ++s) {
      Pads hw{}; for (auto& pad : hw) pad.err = -1; hw[0] = sample(frame,s+1); hw[0].err = 0;
      const bool advanced = engines[s].advance(hw,{});
      if (!advanced) std::fprintf(stderr,"relink engine %d frame %u: %s; transport %s\n",s,frame,
          notices[s].empty() ? "no notice" : notices[s].back().c_str(),f[s]->logs.back().c_str());
      if (!advanced) for (const auto& line : f[s]->logs) std::fprintf(stderr,"%s\n",line.c_str());
      CHECK(advanced); CHECK(engines[s].frame() == frame);
      if (match && frame >= 30) {
        const auto body = frame-30;
        CHECK(engines[s].confirm(body)); // terminal causal wait uses the same paused deadline
        CHECK(engines[s].replay(body)); // read corrected, confirmed pads after any prediction
        CHECK(engines[s].validated(body));
        CHECK(engines[s].boundary(body,{})); CHECK(engines[s].finish_body(body));
      }
      Pads out; engines[s].read({},out);
      for (int p = 0; p < 2; ++p) {
        auto expected = frame < unsigned(c[s].input_delay) ? MuPadStatus{} : sample(frame-c[s].input_delay,p+1); expected.err = 0;
        same_pad(out[p],expected);
      }
      for (auto* link : links) link->pump(0);
    }
    for (auto* seat : f) seat->now += 17;
  }
  CHECK(stalls >= 15 && back && opened[0] && opened[1]);
  CHECK(hashes[0] == hashes[1] && hashes[0].size() == (match ? 12u : 11u));
  for (int s = 0; s < 2; ++s) {
    auto roster = a.roster(); roster[dropped_seat]["connectionId"] = "c9";
    lobbies[s].observe({{"t","back"},{"participants",roster}});
    CHECK(!lobbies[s].cancelled() && lobbies[s].held());
    CHECK(engines[s].active() && !links[s]->lost() && !links[s]->reconnecting());
    CHECK(std::find(notices[s].begin(),notices[s].end(),kReconnecting) != notices[s].end());
    CHECK(notices[s].back().empty());
    CHECK(std::count_if(f[s]->logs.begin(),f[s]->logs.end(),[](const std::string& line) { return line.find(" lost; waiting for the room") != std::string::npos; }) == 1);
    CHECK(std::count_if(f[s]->logs.begin(),f[s]->logs.end(),[](const std::string& line) { return line.find(" reopened via ") != std::string::npos; }) == 1);
  }
  for (auto& engine : engines) engine.end("test complete");
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
// 11-01: a throw before the seat wait logged only "link setup threw". The log now names the stage and the
// exception category (never its text, which can carry a ticket).
static void setup_throw_names_step() {
  struct Case { const char* type; int kind; const char* line; };
  for (const Case k : {Case{"mpTicket",0,"console: setup threw at ticket (runtime_error)"},
                       Case{"mpTicket",1,"console: setup threw at ticket (update_required)"},
                       Case{"mpTicket",2,"console: setup threw at ticket (json)"},
                       Case{"mpTicket",3,"console: setup threw at ticket (other)"}}) {
    ++cases; Fixture f; auto io = f.io();
    const auto request = io.request;
    io.request = [&](const json& q,int ms) -> json {
      if (str(q,"type") == k.type) {
        if (k.kind == 0) throw std::runtime_error("refused: secret-ticket-text");
        if (k.kind == 1) throw yougame::update::Required();
        if (k.kind == 2) return json::array().at(3);
        throw 7;
      }
      return request(q,ms);
    };
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,std::move(io)));
    CHECK(error == (k.kind == 1 ? std::string(yougame::update::kMessage) : std::string("couldn't connect to your friend")));
    CHECK(std::count(f.logs.begin(),f.logs.end(),std::string(k.line)) == 1);
    CHECK(std::count(f.logs.begin(),f.logs.end(),std::string("console: update required")) == (k.kind == 1 ? 1 : 0));
    CHECK(std::none_of(f.logs.begin(),f.logs.end(),[](const std::string& s) { return s.find("secret-ticket-text") != std::string::npos; }));
  }
  ++cases; Fixture a; auto io = a.io();
  io.attach = [](int) -> bool { throw std::runtime_error("attach"); };
  SessionConfig c; std::string error;
  CHECK(!start_session(a.input(),c,error,std::move(io)));
  CHECK(std::count(a.logs.begin(),a.logs.end(),std::string("console: setup threw at attach (runtime_error)")) == 1);
}
static Bytes save_fixture() {
  Bytes bytes(64+11*8192,0x5a);
  std::copy_n("GALE01",6,bytes.begin());
  std::fill(bytes.begin()+8,bytes.begin()+40,0);
  std::copy_n("SuperSmashBros0110290334",24,bytes.begin()+8);
  bytes[0x38] = 0; bytes[0x39] = 11;
  return bytes;
}
static void card_transfers() {
  const auto save = save_fixture(); CHECK(melee_gci(save));
  CHECK(card_sha(Bytes{'a','b','c'}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const auto digest = card_sha(save);
  const auto root = std::filesystem::current_path() / ("round37-card-test-"+std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  auto prepared = [&](const SessionConfig& c) {
    CHECK(c.card_bytes == save && c.card_digest == digest);
    std::filesystem::path card;
    CHECK(prepare_card(root,c,card));
    std::ifstream in(card / "GALE01-SuperSmashBros0110290334.gci",std::ios::binary);
    Bytes written((std::istreambuf_iterator<char>(in)),{}); CHECK(written == save);
    CHECK(!prepare_card(root,c,card)); // never reuse the prepared directory
  };
  ++cases;
  Fixture host; auto io = host.io(); bool handed = false, flushed = false;
  io.host_save = [&] { return save; };
  io.persist_card = [&](const SessionConfig& c) { return stage_card(root,c); };
  io.flush_room = [&](int ms) { CHECK(ms > 0); flushed = true; return true; };
  io.wait = [](int) { CHECK(false); }; // no datagram helper exists in the waiting process
  io.prepared = [&](const SessionConfig& c) { CHECK(flushed); prepared(c); handed = true; };
  const auto host_send = io.send_room;
  io.send_room = [&](const json& m) {
    if (m.count("d") && str(m["d"],"kind") == "mu-card" && m["d"]["i"] == 0)
      host.push({{"t","msg"},{"from","c1"},{"d",{{"kind","mu-card-verdict"},{"v",1},{"match","session-fixed"},{"ok",true}}}});
    return host_send(m);
  };
  host.start_delay = 2;
  host.alter_start = [&](json& s) { s["console"]["cardDigest"] = digest; };
  auto request = host.input(); request["inputDelay"] = 2;
  SessionConfig config; std::string error;
  CHECK(!start_session(request,config,error,std::move(io))); CHECK(handed && error.empty());
  Bytes sent; size_t chunks = 0;
  for (const auto& m : host.messages) {
    if (str(m,"t") == "begin") { CHECK(m["inputDelay"] == 2); CHECK(m["cardDigest"] == digest); }
    if (str(m,"t") != "msg" || str(m["d"],"kind") != "mu-card") continue;
    CHECK(m.dump().size() < 16*1024 && m["to"] == "c1");
    CHECK(m["d"]["match"] == "session-fixed" && m["d"]["v"] == 1);
    CHECK(m["d"]["i"] == chunks && m["d"]["n"] == (save.size()+kCardChunk-1)/kCardChunk);
    Bytes bytes; CHECK(card_decode(m["d"]["data"],bytes)); CHECK(bytes.size() <= 12*1024);
    sent.insert(sent.end(),bytes.begin(),bytes.end()); ++chunks;
  }
  CHECK(chunks > 1 && sent == save);
  CHECK(std::count(host.logs.begin(),host.logs.end(),"console: card sent "+std::to_string(chunks)+" chunks sha "+digest.substr(0,8)) == 1);
  SessionConfig restored; restored.session_id = "session-fixed"; restored.card_digest = digest;
  CHECK(load_card(root,restored) && restored.card_bytes == save);
  CHECK(!stage_card(root,restored)); // no overwrite through a repeated handoff
  auto launch = session_launch(restored); json parsed;
  launch["room"] = "ABCDEF"; launch["port"] = 1; launch["players"] = 2; launch["epochMs"] = 1793443200123LL; launch["inputDelay"] = 2;
  // The app relays exactly its eight consoleSession fields; the rejoin takes cardDigest from the frozen start.
  CHECK(session_launch(restored).size() == 8 && !session_launch(restored).count("cardDigest"));
  CHECK(parse_request(launch,parsed) && !parsed.count("cardDigest"));

  // An unusable host save never refuses START: no cardDigest, no chunks, a default-card start.
  for (const Bytes& bad : {Bytes{}, Bytes(100,0)}) {
    ++cases;
    Fixture plain; auto pio = plain.io(); bool started = false; int reads = 0;
    pio.host_save = [&] { ++reads; return bad; };
    pio.prepared = [&](const SessionConfig& c) { CHECK(c.card_digest.empty() && c.card_bytes.empty()); started = true; };
    plain.start_delay = 2;
    auto plain_request = plain.input(); plain_request["inputDelay"] = 2;
    SessionConfig plain_config; std::string plain_error;
    CHECK(!start_session(plain_request,plain_config,plain_error,std::move(pio))); CHECK(started && plain_error.empty());
    CHECK(reads == 1);
    for (const auto& m : plain.messages) {
      if (str(m,"t") == "begin") CHECK(!m.count("cardDigest"));
      CHECK(!(m.count("d") && m["d"].is_object() && str(m["d"],"kind") == "mu-card"));
    }
    const auto want = "console: couldn't share the host's save (" + std::to_string(bad.size()) + " bytes); every seat starts with default unlocks";
    CHECK(std::count(plain.logs.begin(),plain.logs.end(),want) == 1);
    CHECK(std::count(plain.logs.begin(),plain.logs.end(),std::string("console: no shared save in the start; this seat starts with default unlocks")) == 1);
  }

  ++cases;
  Fixture rejoin; rejoin.start_delay = 2; rejoin.session = "session-fixed";
  auto resume = rejoin.io(); bool loaded = false;
  resume.host_save = []() -> Bytes { CHECK(false); return {}; };
  resume.restore_card = [&](SessionConfig& c) { loaded = true; return load_card(root,c); };
  const auto next_resume = resume.next_room;
  resume.next_room = [&](json& m,int ms) {
    const bool got = next_resume(m,ms);
    if (got && m.count("room") && m["room"].count("console")) m["room"]["console"]["cardDigest"] = digest;
    return got;
  };
  launch["seed"] = "fedcba9876543210";
  auto resumed = start_session(launch,config,error,std::move(resume));
  CHECK(resumed && error.empty() && loaded && config.card_bytes == save);
  CHECK(std::none_of(rejoin.messages.begin(),rejoin.messages.end(),[](const json& m) {
    return str(m,"t") == "begin" || (m.count("d") && str(m["d"],"kind") == "mu-card");
  }));

  // Game-local choice survives both handoff forms; missing/unreadable/stale files fail open.
  for (int seat : {0,1}) for (int mode = 0; mode < 9; ++mode) {
    ++cases; Fixture r(2,seat); r.start_delay = 2; r.session = "session-fixed";
    auto rio = r.io(); bool restored_card = false;
    auto marked = launch; marked.erase("cardDigest"); marked["port"] = seat+1;
    const auto data = root / ("choice-"+std::to_string(seat)+"-"+std::to_string(mode)); std::filesystem::create_directory(data);
    SessionConfig choice = restored; choice.default_card = mode == 0; choice.card_digest = digest;
    CHECK(write_card_choice(data,choice));
    const auto marker = data / "console" / choice.session_id / "save-choice.json";
    if (mode == 1 || mode == 7) std::filesystem::remove(marker);
    if (mode == 2) std::ofstream(marker) << "broken";
    if (mode == 3) std::ofstream(marker) << json({{"sessionId","other"},{"digest",digest}});
    const auto next = rio.next_room;
    rio.next_room = [&](json& m,int ms) {
      const bool got = next(m,ms);
      if (got && m.count("room") && m["room"].count("console")) m["room"]["console"]["cardDigest"] = digest;
      // A peer with a missing marker vetoes our valid staged host save as well.
      if (got && m.count("d") && str(m["d"],"kind") == "mu-cand") {
        if (mode == 6 || mode == 8) m["d"]["cardDigest"] = "";
        // The marker arrives only AFTER this launch read its missing choice and sent
        // its original empty digest. It must never revise that default vote.
        if (mode == 7) CHECK(write_card_choice(data,choice));
        if (mode == 1 || mode == 7) m["d"]["cardDigest"] = digest; // Our missing choice vetoes a host-ready peer.
      }
      return got;
    };
    rio.restore_card = [&](SessionConfig& c) {
      if (!take_card_choice(data,c)) return false;
      restored_card = true; c.card_bytes = save;
      if (mode == 4) c.card_bytes.back() ^= 1;
      return true;
    };
    auto link = start_session(marked,config,error,std::move(rio));
    CHECK(link && error.empty());
    CHECK(config.default_card == (mode != 5));
    CHECK((config.card_digest == digest && config.card_bytes == save) == (mode == 5));
    CHECK(restored_card == (mode == 4 || mode == 5 || mode == 6 || mode == 8));
    if (mode == 7) CHECK(take_card_choice(data,choice)); // late record was never consulted by this launch
    CHECK(!take_card_choice(data,choice)); // consumed choices never authorize another host-card boot
    CHECK(session_launch(config).size() == 8 && !session_launch(config).count("card"));
    CHECK(pending_write(data,session_launch(config),1000,"test-build"));
    CHECK(pending_take(data,1001,"test-build",[&](const json& q,auto consume) {
      CHECK(q.size() == 8 && !q.count("card")); return consume();
    },[](const std::string&) {}));
  }
  auto bad_marker = launch; bad_marker["card"] = "default"; CHECK(!parse_request(bad_marker,parsed));

  // Replay both actual local choices through the relaunch rendezvous. One seat can have
  // the host final while its peer timed out: both receive the other's ORIGINAL digest.
  auto check_default_pair = [&](const SessionConfig& host_choice, const SessionConfig& guest_choice) {
    const SessionConfig choices[] = {host_choice,guest_choice};
    for (int seat : {0,1}) {
      ++cases; Fixture f(2,seat); f.session = "session-fixed"; f.start_delay = 2;
      auto io = f.io();
      const auto next = io.next_room;
      io.next_room = [&](json& m,int ms) {
        const bool got = next(m,ms);
        if (got && m.count("room") && m["room"].count("console")) m["room"]["console"]["cardDigest"] = digest;
        if (got && m.count("d") && str(m["d"],"kind") == "mu-cand") m["d"]["cardDigest"] = choices[1-seat].card_digest;
        return got;
      };
      io.restore_card = [&](SessionConfig& c) {
        if (choices[seat].card_digest.empty()) return false;
        c.card_bytes = save; return true;
      };
      auto marked = launch; marked["port"] = seat+1;
      SessionConfig actual; std::string e;
      auto playing = start_session(marked,actual,e,std::move(io));
      CHECK(playing && e.empty() && actual.default_card && actual.card_digest.empty() && actual.card_bytes.empty());
      CHECK(std::count(f.logs.begin(),f.logs.end(),std::string("console: ")+kDefaultCard) == 1);
    }
  };

  // Run the guest against the real host decision, relaying its actual verdict and the host's final.
  // Good out-of-order delivery/duplicate; missing, corrupt, excessive count/total, malformed
  // index/base64, absent digest, closure, cancellation, write failure, no final and host disk error.
  for (int mode = 0; mode < 20; ++mode) {
    ++cases; Fixture guest(2,1); auto g = guest.io(); bool ready = false, persisted = false, discarded = false;
    SessionConfig guest_prepared, host_prepared; bool host_ready = false;
    g.host_save = []() -> Bytes { CHECK(false); return {}; }; // guest never reads its own save
    const auto next = g.next_room; bool injected = false;
    const auto id = "guest-"+std::to_string(mode);
    g.next_room = [&](json& m,int ms) {
      const bool got = next(m,ms);
      if (got && m.count("console")) {
        m["console"]["sessionId"] = id; m["matchId"] = id;
        if (mode != 8) m["console"]["cardDigest"] = digest;
        if (!injected) {
          injected = true;
          if (mode == 16) guest.push({{"t","msg"},{"from","c0"},{"d",{
              {"kind","mu-card-final"},{"v",1},{"match",id},{"use","default"}}}});
          const size_t count = mode == 4 ? (kCardLimit+kCardChunk-1)/kCardChunk : chunks;
          for (size_t n = count; n-- > 0;) {
            if (mode == 1 && n == 0) continue;
            const size_t offset = mode == 4 ? 0 : n*kCardChunk;
            json d = {{"kind","mu-card"},{"v",1},{"match",id},{"i",n},{"n",count},
                {"data",card_encode(save.data()+offset,std::min(kCardChunk,save.size()-offset))}};
            if (mode == 2 && n == chunks-1) {
              auto corrupt = save; corrupt.back() ^= 1;
              d["data"] = card_encode(corrupt.data()+offset,std::min(kCardChunk,corrupt.size()-offset));
            }
            if (mode == 3) d["n"] = 99999;
            if (mode == 5) d["i"] = count;
            if (mode == 6) d["data"] = "%%%=";
            if (mode == 7) d["data"] = std::string(20000,'A');
            if (mode == 9) continue; // completely absent: 20 s timeout
            guest.push({{"t","msg"},{"from","c0"},{"d",d}});
            if (mode == 0) guest.push({{"t","msg"},{"from","c0"},{"d",d}});
          }
        }
      }
      return got;
    };
    g.persist_card = [&](const SessionConfig& c) {
      persisted = true;
      if (mode == 12) return false;
      if (mode == 15) throw std::runtime_error("disk error");
      return stage_card(root,c);
    };
    g.discard_card = [&](const SessionConfig& c) { discarded = true; CHECK(discard_staged_card(root,c)); };
    g.prepared = [&](const SessionConfig& c) { guest_prepared = c; ready = true; };
    const auto send = g.send_room;
    g.send_room = [&](const json& m) {
      const bool sent = send(m);
      if (!m.count("d") || str(m["d"],"kind") != "mu-card-verdict") return sent;
      CHECK(m["to"] == "c0" && m["d"]["v"] == 1 && m["d"]["match"] == id);
      const bool ok = mode == 0 || mode == 13 || mode == 14 || mode == 17 || mode == 18 || mode == 19;
      CHECK(m["d"]["ok"] == ok);
      Fixture h; auto hio = h.io(); hio.host_save = [&] { return save; };
      h.alter_start = [&](json& s) { s["console"]["sessionId"] = id; s["matchId"] = id; s["console"]["cardDigest"] = digest; };
      const auto host_root = root / ("host-"+id); std::filesystem::create_directory(host_root);
      hio.persist_card = [&](const SessionConfig& c) { return mode != 14 && stage_card(host_root,c); };
      hio.discard_card = [&](const SessionConfig& c) { CHECK(discard_staged_card(host_root,c)); };
      hio.prepared = [&](const SessionConfig& c) { host_prepared = c; host_ready = true; };
      std::vector<uint64_t> finals;
      int flushes = 0; hio.flush_room = [&](int ms) { CHECK(ms > 0); ++flushes; return true; };
      const auto hs = hio.send_room;
      hio.send_room = [&](const json& msg) {
        const bool sent_host = hs(msg);
        if (msg.count("d") && str(msg["d"],"kind") == "mu-card" && msg["d"]["i"] == 0)
          h.push({{"t","msg"},{"from","c1"},{"d",m["d"]}}); // must survive pacing
        if (msg.count("d") && str(msg["d"],"kind") == "mu-card-final") {
          CHECK((flushes >= 1 && flushes <= 3) && !host_ready);
          if (finals.size() < 2) finals.push_back(h.now);
          if (mode != 13) guest.push({{"t","msg"},{"from","c0"},{"d",msg["d"]}});
        }
        return sent_host;
      };
      std::string he; SessionConfig hc;
      CHECK(!start_session(h.input(),hc,he,std::move(hio)) && he.empty());
      CHECK(host_ready && flushes == 3);
      CHECK(finals.size() == 2 && finals[1] == finals[0]+1000);
      const auto use = ok && mode != 14 ? "host" : "default";
      CHECK(std::count(h.logs.begin(),h.logs.end(),std::string("console: card verdicts ")+(ok ? "1" : "0")+"/1 ok; using "+use) == 1);
      CHECK(std::count(h.logs.begin(),h.logs.end(),std::string("console: card final ")+use) == 1);
      if (mode == 13) {
        // Spoofed, stale, invalid-version and invalid-use finals cannot authorize handoff.
        for (int invalid = 0; invalid < 4; ++invalid) {
          json d = {{"kind","mu-card-final"},{"v",1},{"match",id},{"use","default"}};
          if (invalid == 1) d["match"] = "stale";
          if (invalid == 2) d["v"] = 2;
          if (invalid == 3) d["use"] = "other";
          guest.push({{"t","msg"},{"from",invalid == 0 ? "intruder" : "c0"},{"d",d}});
        }
      }
      return mode == 17 ? false : sent;
    };
    if (mode == 18 || mode == 19) {
      const auto normal_next = g.next_room;
      g.next_room = [&,normal_next](json& m,int ms) {
        // A permanently nonempty zero-wait queue must hit the drain cap.
        if (ms == 0) { if (mode == 19) guest.now += 40000; m = {{"t","ping"}}; return true; }
        return normal_next(m,ms);
      };
    }
    if (mode == 10) g.room_closed = [&] { return injected; };
    if (mode == 11) g.cancelled = [&] { return injected; };
    CHECK(!start_session(guest.input(),config,error,std::move(g)));
    if (mode == 8) { CHECK(ready && !persisted && error.empty() && !host_ready); }
    else if (mode == 11) { CHECK(!ready && error == "cancelled" && !host_ready); }
    else if (mode == 10 || mode == 13 || mode == 17 || mode == 18 || mode == 19) {
      CHECK(ready && error.empty() && guest_prepared.default_card);
      CHECK(guest_prepared.card_digest.empty());
      if (mode == 13) CHECK(guest.now == 40001 && host_ready && host_prepared.card_digest == digest);
      if (mode == 13 || mode == 17 || mode == 18 || mode == 19) check_default_pair(host_prepared,guest_prepared);
    } else {
      CHECK(ready && host_ready && error.empty());
      CHECK(guest_prepared.card_digest == host_prepared.card_digest && guest_prepared.card_bytes == host_prepared.card_bytes);
      if (mode == 0) { CHECK(persisted && !discarded); CHECK(guest_prepared.card_digest == digest && guest_prepared.card_bytes == save); }
      else {
        CHECK(discarded && guest_prepared.default_card && host_prepared.default_card);
        CHECK(guest_prepared.card_digest.empty() && guest_prepared.card_bytes.empty());
        CHECK(!std::filesystem::exists(root / id / "incoming.gci"));
        CHECK(session_launch(guest_prepared).size() == 8 && session_launch(host_prepared).size() == 8);
        CHECK(write_card_choice(root,guest_prepared) && !take_card_choice(root,guest_prepared));
      }
      CHECK(std::count(guest.logs.begin(),guest.logs.end(),std::string("console: card verdict ")+(mode == 0 || mode == 14 ? "ok" : "fail")+" sent") == 1);
      CHECK(std::count(guest.logs.begin(),guest.logs.end(),std::string("console: card final ")+(mode == 0 ? "host" : "default")) == 1);
      if (mode == 1 || mode == 9) {
        CHECK(guest.now == 21501);
        CHECK(std::any_of(guest.logs.begin(),guest.logs.end(),[](const std::string& s) { return s.find("chunks sha ") != std::string::npos && s.find(" timeout") != std::string::npos; }));
      }
      if (mode == 2) CHECK(std::any_of(guest.logs.begin(),guest.logs.end(),[](const std::string& s) { return s.find(" mismatch") != std::string::npos; }));
    }
  }
  // Host timeout and local send/flush/disk failures all select default. Invalid verdicts
  // never count, and duplicate ok verdicts cannot replace a missing seat.
  for (int mode = 0; mode < 14; ++mode) {
    ++cases; Fixture h; auto hio = h.io(); bool ready = false; int flushes = 0;
    hio.host_save = [&] { return save; };
    h.alter_start = [&](json& s) { s["console"]["cardDigest"] = digest; };
    hio.persist_card = [&](const SessionConfig&) -> bool {
      if (mode == 5) throw std::runtime_error("disk error");
      return mode != 2;
    };
    hio.flush_room = [&](int ms) {
      CHECK(ms > 0); ++flushes;
      if (flushes == 2 && (mode == 7 || mode == 11)) return false;
      if ((flushes == 2 && mode == 8) || (flushes == 3 && mode == 13)) h.now += uint64_t(ms);
      if (flushes == 3 && mode == 12) return false;
      return flushes != 1 || mode != 3;
    };
    hio.prepared = [&](const SessionConfig& c) {
      CHECK(c.default_card && c.card_digest.empty() && c.card_bytes.empty()); ready = true;
      if (mode >= 9) { SessionConfig other; other.card_digest = digest; other.card_bytes = save; check_default_pair(c,other); }
    };
    const auto hn = hio.next_room; bool late_verdict = false;
    hio.next_room = [&](json& m,int ms) {
      const bool got = hn(m,ms);
      if (!got && mode == 6 && !late_verdict && h.start_at && h.now >= h.start_at+25000) {
        late_verdict = true;
        m = {{"t","msg"},{"from","c1"},{"d",{{"kind","mu-card-verdict"},{"v",1},{"match","session-fixed"},{"ok",true}}}};
        return true;
      }
      return got;
    };
    const auto hs = hio.send_room; int final_sends = 0;
    hio.send_room = [&](const json& m) {
      if (m.count("d") && str(m["d"],"kind") == "mu-card-final") {
        ++final_sends;
        if ((mode == 9 && final_sends == 1) || (mode == 10 && final_sends == 2)) return false;
      }
      if (m.count("d") && str(m["d"],"kind") == "mu-card" && m["d"]["i"] == 0) {
        if (mode == 0 || mode == 6) {
          for (int invalid = 0; invalid < 4; ++invalid) {
            json d = {{"kind","mu-card-verdict"},{"v",1},{"match","session-fixed"},{"ok",true}};
            if (invalid == 1) d["match"] = "stale";
            if (invalid == 2) d["v"] = 2;
            if (invalid == 3) d["ok"] = "true";
            h.push({{"t","msg"},{"from",invalid == 0 ? "intruder" : "c1"},{"d",d}});
          }
        } else h.push({{"t","msg"},{"from","c1"},{"d",{{"kind","mu-card-verdict"},{"v",1},{"match","session-fixed"},{"ok",mode != 1 && (mode < 7 || mode >= 9)}}}});
        if (mode == 4) return false;
      }
      return hs(m);
    };
    CHECK(!start_session(h.input(),config,error,std::move(hio)));
    CHECK(error.empty() && ready);
    if (mode == 0 || mode == 6) CHECK(h.now == h.start_at+26000);
    CHECK(std::count(h.logs.begin(),h.logs.end(),std::string("console: card final default")) == 1);
  }
  ++cases;
  Fixture doubles(4); auto four = doubles.io();
  four.host_save = []() -> Bytes { CHECK(false); return {}; };
  auto link = start_session(doubles.input(),config,error,std::move(four)); CHECK(link && error.empty());
  CHECK(config.ports == std::vector<int>({1,2,3,4}));
  for (const auto& m : doubles.messages) if (str(m,"t") == "begin") CHECK(!m.count("cardDigest") && m["inputDelay"] == 3);
  std::filesystem::remove_all(root);
}
// Exercise the actual broker close envelope through Stream and console startup.
static void room_upgrade_refusals() {
  const std::string mismatch = "the rooms server answered 403: This room uses a different lobby protocol or configuration";
  CHECK(std::string(kMixedBuilds) == "Your friend is on a different version of Melee Unlocked. Both of you need the latest update to play together.");
  CHECK(session_notice(kMixedBuilds) == std::string(kMixedBuilds));
  for (int mode = 0; mode < 6; ++mode) {
    ++cases; Fixture f(2,1); auto io = f.io();
    const std::string reason = mode == 0 || mode == 3 ? mismatch :
        mode == 1 ? "the rooms server answered 403: ticket expired" : "";
    yougame::transport::Stream stream(f.events,[&](const json& q,int) -> json {
      if (str(q,"type") == "netOpen") {
        if (mode == 4) f.events.push({{"conn",77},{"event","error"},{"code","refused"},{"message",mismatch}});
        f.events.push({{"conn",77},{"event","close"},{"code",mode == 3 ? 1008 : 1006},{"reason",reason}});
        return {{"conn",77}};
      }
      return {};
    },[&] { return f.now; });
    io.open_room = [&](const std::string& t,const std::string& r,int ms) {
      return mode == 5 ? false : stream.open(t,r,ms);
    };
    io.room_mismatch = [&] { return stream.lobby_mismatch(); };
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,std::move(io)));
    CHECK(error == (mode == 0 ? kMixedBuilds : "couldn't open your friend's room"));
    CHECK(stream.lobby_mismatch() == (mode == 0));
  }
  ++cases; Fixture f;
  CHECK(f.stream.open("fake-ticket","ABCDEF",100));
  f.events.push({{"conn",f.conn},{"event","close"},{"code",1006},{"reason",mismatch}});
  json m;
  while (f.stream.next(m,0)) {}
  CHECK(f.stream.lobby_mismatch());
  f.seat = 1; // broker connection IDs are never reused
  CHECK(f.stream.open("fake-ticket","ABCDEF",100));
  CHECK(!f.stream.lobby_mismatch()); // new attempts never inherit a previous refusal
}
// The worker remains blocked after START's save deadline. Its owned result is discarded;
// no session stack is referenced by it, and tests release it before leaving the case.
static void blocked_card_io() {
  const auto save = save_fixture(); const auto digest = card_sha(save);
  for (int mode : {0,1,2,3,4}) { // host read/persist, guest persist, host/guest discard
    ++cases; Fixture f(2,mode == 2 || mode == 4 ? 1 : 0); auto io = f.io();
    struct Work { std::atomic<bool> entered{false}, release{false}, done{false}; };
    auto work = std::make_shared<Work>();
    auto blocked = [work] {
      work->entered = true;
      while (!work->release) std::this_thread::yield();
      work->done = true;
    };
    io.clock = [&] { return f.now + (work->entered ? (mode >= 3 ? 1000 : 20000) : 0); };
    io.host_save = mode == 0 ? std::function<Bytes()>([blocked,save] { blocked(); return save; }) : std::function<Bytes()>([save] { return save; });
    if (mode != 0) {
      f.alter_start = [&](json& m) { m["console"]["cardDigest"] = digest; };
      if (mode < 3) io.persist_card = [blocked](const SessionConfig&) { blocked(); return true; };
      else {
        io.persist_card = [](const SessionConfig&) { return false; };
        io.discard_card = [blocked](const SessionConfig&) { blocked(); };
      }
      if (mode == 2 || mode == 4) {
        const auto next = io.next_room; auto injected = std::make_shared<bool>(false);
        io.next_room = [&,next,injected](json& m,int ms) {
          const bool got = next(m,ms);
          if (got && m.count("console") && !*injected) {
            *injected = true; m["console"]["cardDigest"] = digest;
            const size_t n = (save.size()+kCardChunk-1)/kCardChunk;
            for (size_t chunk = 0; chunk < n; ++chunk) {
              const size_t offset = chunk*kCardChunk;
              f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-card"},{"v",1},{"match","session-fixed"},
                  {"i",chunk},{"n",n},{"data",card_encode(save.data()+offset,std::min(kCardChunk,save.size()-offset))}}}});
            }
            f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-card-final"},{"v",1},{"match","session-fixed"},{"use","host"}}}});
          }
          return got;
        };
      }
    }
    bool ready = false;
    auto marker = std::make_shared<Work>();
    const auto session_clock = io.clock;
    io.prepared = [&](const SessionConfig& c) {
      ready = true; CHECK(work->entered && !work->done && c.card_digest.empty() && c.card_bytes.empty());
      if (mode < 3) return;
      CHECK(c.default_card);
      // Both filesystem workers stay blocked while the app receives its relaunch request.
      HandoffIO hio;
      hio.clock = [&] { return session_clock() + (marker->entered ? 1000 : 0); };
      hio.choice = [marker](const std::filesystem::path&,const SessionConfig& card) {
        marker->entered = true;
        while (!marker->release) std::this_thread::yield();
        CHECK(card.default_card && card.card_digest.empty()); marker->done = true; return true;
      };
      hio.pending = [](const std::filesystem::path&,const json& request,int64_t,const std::string&) {
        CHECK(request["sessionId"] == "session-fixed" && request.size() == 8); return true;
      };
      bool sent = false, manual_ready = false; std::vector<std::string> logs;
      CHECK(handoff(c,mode == 3,{},0,"",[&](const json& q) {
        CHECK(marker->entered && !marker->done && !work->done && q["type"] == "relaunch"); sent = true;
      },[&](const std::string& line) { logs.push_back(line); },[&](const char* text) {
        manual_ready = std::string(text) == kManualStart;
      },hio));
      CHECK(sent == (mode == 3) && manual_ready == (mode == 4));
      CHECK(std::count(logs.begin(),logs.end(),std::string("console: ")+kDefaultCard) == 1);
      CHECK(std::count(f.logs.begin(),f.logs.end(),std::string("console: ")+kDefaultCard) == 1);
    };
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,std::move(io)) && ready && error.empty());
    work->release = true; marker->release = true;
    while (!work->done || (mode >= 3 && !marker->done)) std::this_thread::yield();
  }
}
static void card_final_repeats() {
  const auto save = save_fixture(); const auto digest = card_sha(save);
  for (int mode = 0; mode < 6; ++mode) {
    ++cases; Fixture f(2,1); auto io = f.io();
    const auto next = io.next_room; bool injected = false, verdict = false, first = false, repeat = false, ready = false;
    std::atomic<bool> slow_persist{false};
    auto push_final = [&](const char* use) {
      f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-card-final"},{"v",1},{"match","session-fixed"},{"use",use}}}});
    };
    io.next_room = [&](json& m,int ms) {
      if (slow_persist.exchange(false)) f.now += 2000;
      const bool got = next(m,ms);
      if (got && m.count("console") && !injected) {
        injected = true; m["console"]["cardDigest"] = digest;
        if (mode == 5) { first = repeat = true; push_final("host"); }
        const size_t n = (save.size()+kCardChunk-1)/kCardChunk;
        for (size_t chunk = 0; chunk < n; ++chunk) {
          const size_t offset = chunk*kCardChunk;
          f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-card"},{"v",1},{"match","session-fixed"},
              {"i",chunk},{"n",n},{"data",card_encode(save.data()+offset,std::min(kCardChunk,save.size()-offset))}}}});
        }
        if (mode == 5) push_final("default"); // remains queued behind the last chunk
      }
      return got;
    };
    io.persist_card = [&](const SessionConfig&) { if (mode == 5) slow_persist = true; return true; };
    const auto send = io.send_room;
    io.send_room = [&](const json& m) {
      if (m.count("d") && str(m["d"],"kind") == "mu-card-verdict") verdict = true;
      return send(m);
    };
    const uint64_t first_at = mode == 2 ? 32001 : 101;
    f.on_wait = [&] {
      if (verdict && !first && f.now >= first_at) { first = true; push_final(mode == 4 ? "default" : "host"); }
      if (first && !repeat && mode != 3 && f.now >= first_at+1000) {
        repeat = true; push_final(mode == 1 ? "default" : "host");
      }
    };
    io.prepared = [&](const SessionConfig& c) {
      ready = true;
      if (mode == 1 || mode == 4 || mode == 5) CHECK(c.default_card && c.card_bytes.empty() && c.card_digest.empty());
      else CHECK(c.card_bytes == save && !c.default_card);
    };
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,std::move(io)));
    if (mode == 1 || mode == 4 || mode == 5) CHECK(ready && error.empty() && repeat);
    else {
      CHECK(ready && error.empty());
      CHECK(f.now >= first_at+1500 && f.now < 40001);
      CHECK(repeat == (mode != 3));
    }
  }
}
static void friends_order_phase_a() {
  ++cases;
  Fixture f; f.fill = false; auto io = f.io();
  std::vector<std::string> order; bool css = false, prepared = false, roster_reported = false;
  const auto request = io.request;
  io.request = [&](const json& q,int ms) {
    if (str(q,"type") == "mpTicket") { CHECK(q["queue"] == "private"); order.push_back("ticket"); }
    return request(q,ms);
  };
  io.friends_lobby = true;
  io.opened = [&](const std::string& room) {
    CHECK(order == std::vector<std::string>{"ticket"});
    CHECK(room == new_room_code(UINT64_C(0xfedcba9876543210)));
    order.push_back("room");
  };
  io.roster = [&](const std::string&,const json& r,const std::string& me,int rev) {
    CHECK(me == "c0" && (rev == 7 || rev == 8)); CHECK(!r.empty()); roster_reported = true;
  };
  io.css_ready = [&] { return css; };
  io.confirm_start = [] { CHECK(false); return false; }; // Friends never asks for START
  io.waiting = [](const std::string&,bool) { CHECK(false); }; // no waiting panel
  const yougame::friends::CssPick pick{18,3,2};
  io.freeze_pick = [&](const std::string& name) {
    CHECK(css && roster_reported && name == "Marth"); order.push_back("arrival"); return pick.settings();
  };
  f.alter_start = [&](json& m) { m["console"]["settingsDigest"] = pick.settings(); };
  io.prepared = [&](const SessionConfig& c) {
    CHECK(c.settings_digest == pick.settings()); CHECK(yougame::friends::css_pick(c.settings_digest)->pad == 2);
    prepared = true;
  };
  f.on_wait = [&] {
    // Simulate more than ten minutes of local CSS; no sleeps or real network.
    if (!css) { CHECK(order.back() == "room"); css = true; order.push_back("css");
      CHECK(yougame::friends::picker(new_room_code(UINT64_C(0xfedcba9876543210)))["room"] != "new");
      order.push_back("picker"); f.now = 660001; }
    else if (!f.fill) {
      CHECK(std::none_of(f.messages.begin(),f.messages.end(),[](const json& m) { return str(m,"t") == "begin"; }));
      f.fill = true; auto r = f.roster(); r[1]["name"] = "Marth";
      f.push({{"t","join"},{"participants",r},{"revision",8},{"host","c0"}});
    }
  };
  SessionConfig c; std::string error;
  CHECK(!start_session(f.input(),c,error,std::move(io))); CHECK(error.empty() && prepared && f.start_at > 600000);
  CHECK(order == (std::vector<std::string>{"ticket","room","css","picker","arrival"}));
  for (const auto& m : f.messages) if (str(m,"t") == "begin") CHECK(m["settingsDigest"] == pick.settings());
  // A roomClosed message ends the session without accepting another roster/reconnect.
  Fixture guest(2,1); auto link = guest.start(c);
  guest.push({{"t","roomClosed"},{"reason","host_left"},{"host","c0"},{"name","Luigi"}});
  link->pump(0); CHECK(link->lost() == "Luigi left: lobby closed");
  // Full-roster validation still rejects duplicates even on automatic Friends startup.
  Fixture bad; auto badio = bad.io(); badio.friends_lobby = true;
  bad.alter_start = [](json& m) { m["console"]["participants"][1]["port"] = 1; };
  CHECK(!start_session(bad.input(),c,error,std::move(badio))); CHECK(error == "invalid console session start");
}

static void friends_reenter_held_room() {
  ++cases;
  Fixture f; auto io = f.io(); io.friends_lobby = io.held_reenter = true;
  bool previous = true, prepared = false;
  io.previous_session_active = [&] { return previous; };
  const auto send = io.send_room;
  io.send_room = [&](const json& m) { if (str(m,"t") == "begin") CHECK(!previous); return send(m); };
  f.on_wait = [&] {
    if (f.now >= 200 && previous) {
      previous = false;
      f.push({{"t","sessionEnded"},{"sessionId","old-session"},{"reason","away"}});
    }
  };
  io.prepared = [&](const SessionConfig& c) { CHECK(c.room == "room123"); prepared = true; };
  // Even a stale frozen start must not relaunch the interrupted session.
  const auto open = io.open_room;
  io.open_room = [&](const std::string& ticket,const std::string& room,int ms) {
    CHECK(room == "room123");
    const bool ok = open(ticket,room,ms);
    f.push({{"t","lobby"},{"console",f.console_start()},{"participants",f.roster()}});
    return ok;
  };
  SessionConfig c;std::string error;
  CHECK(!start_session({{"room","room123"},{"players",2}},c,error,std::move(io)));
  CHECK(prepared && error.empty() && f.now >= 200);
  CHECK(f.requests.front()["room"] == "room123");
}
static void friends_guest_waits_for_next_start() {
  ++cases;
  Fixture f(2,1); f.reply_start = false;
  auto io = f.io(); io.friends_lobby = io.held_reenter = true;
  bool previous = true, prepared = false;
  io.previous_session_active = [&] { return previous; };
  f.on_wait = [&] {
    if (f.now >= 200 && previous) {
      previous = false;
      f.push({{"t","sessionEnded"},{"sessionId","old-session"},{"reason","host_ended"}});
      f.push({{"t","start"},{"matchId","session-fixed"},{"console",f.console_start()},{"participants",f.roster()}});
    }
  };
  io.prepared = [&](const SessionConfig& c) { CHECK(c.room == "room123" && c.my_port == 2); prepared = true; };
  SessionConfig c;std::string error;
  CHECK(!start_session({{"room","room123"},{"players",2}},c,error,std::move(io)));
  CHECK(prepared && error.empty());
  CHECK(std::none_of(f.messages.begin(),f.messages.end(),[](const json& m) { return str(m,"t") == "begin"; }));
}
#include "friends_handoff_join_test.inc"
int main(int argc,char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--handoff-off-trace") {
    friends_handoff_off_trace(); return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--handoff-join") {
    friends_handoff_agreement(); friends_handoff_flow(); friends_handoff_chunks(); friends_handoff_live_transfer(); friends_handoff_guest_transfer();
    std::printf("Friends handoff agreement: %d cases, %d checks passed\n",cases,checks.load()); return 0;
  }
  friends_guest_waits_for_next_start();
  friends_reenter_held_room();
  friends_order_phase_a();
  if (argc > 1 && std::string(argv[1]) == "--blocked-card-io") {
    blocked_card_io();
    std::printf("blocked card IO: %d cases, %d checks passed\n",cases,checks.load()); return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--room-refusals") {
    room_upgrade_refusals();
    std::printf("console room refusals: %d cases, %d checks passed\n",cases,checks.load()); return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--card-agreement") {
    card_transfers(); card_final_repeats(); blocked_card_io();
    std::printf("console card agreement: %d cases, %d checks passed\n",cases,checks.load()); return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--pacing") {
    ack_and_pacing(argc > 2 && std::string(argv[2]) == "--measure"); return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--latency") {
    latency(argc > 2 && std::string(argv[2]) == "--measure");
    std::printf("yougame_console_latency: %d cases, %d checks passed\n",cases,checks.load()); return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--watchdog") {
    watchdog_receive_clock(); background_boot_service();
    std::printf("console watchdog: %d cases, %d checks passed\n",cases,checks.load()); return 0;
  }
  room_upgrade_refusals();
  relink_failures(); relink_async_leave(); relink_engines(0); relink_engines(1); relink_engines(1,true);
  watchdog_receive_clock();
  card_transfers(); card_final_repeats(); blocked_card_io();
  ack_and_pacing(false);
  failed_session_new_invite();
  boot_liveness(); background_boot_service(); interruption_shapes(); relaunch_no_leave(); app_launch_contract(); final_rooms_contract(); rejoin(); routes_and_delay(); human_room_start(); real_delay_mesh();
  intake(); starts(); agent_create_join(); host_waits_for_seats(); failure_cases(); invalid_start(); mesh(2,false); mesh(2,true); mesh(4,true); departures_and_waits(); common_end_reason();
  sparse_starts(); sparse_invalid_rosters(); sparse_relaunch(); sparse_peer_logs(); setup_throw_names_step();
  std::printf("yougame_console_link: %d cases, %d checks passed\n",cases,checks.load());
}
