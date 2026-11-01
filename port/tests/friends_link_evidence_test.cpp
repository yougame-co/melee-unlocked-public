// Exercise the real Link's room-carried vs direct decision from route reports.
// Including the implementation keeps its private peer type out of production headers.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_console_link.cpp"
#include <cstdio>
#include <stdexcept>
using namespace yougame::console;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while (0)
struct Fixture {
  uint64_t now=100;
  int connection=10, opens=0, closes=0;
  bool rejoining=false;
  std::string path="unknown";
  std::deque<json> room;
  std::deque<Bytes> packets;
  SessionIO io() {
    SessionIO i;
    i.clock=[&] {return now;}; i.connection=[&] {return connection;};
    i.room_rejoining=[&] {return rejoining;}; i.room_closed=[] {return false;};
    i.state=[](unsigned) {return 2;}; i.route=[&](unsigned) {return path;};
    i.request=[&](const json& q,int) {
      if (str(q,"type")=="directClose") ++closes;
      if (str(q,"type")=="directOpen") {++opens; return json{{"link",2}};}
      return json::object();
    };
    i.next_room=[&](json& m,int) {if(room.empty()) return false; m=room.front(); room.pop_front(); return true;};
    i.receive=[&](unsigned& id,Bytes& b) {if(packets.empty()) return false; id=2; b=packets.front(); packets.pop_front(); return true;};
    i.send=[](unsigned,const Bytes&) {}; i.send_room=[](const json&) {return true;};
    i.log=[](const std::string&) {}; return i;
  }
  void prepare(Link& link) {
    link.config.session_id="evidence"; link.config.my_port=1; link.config.ports={1,2};
    link.frozen=json::array({{{"port",1},{"slot",0},{"connectionId","self"}},
                             {{"port",2},{"slot",1},{"connectionId","peer"}}});
    link.identities=link.frozen;
    Peer p{}; p.port=2; p.link=2; p.connection="peer"; p.heard=now; p.room_connection=connection;
    link.peers.push_back(std::move(p)); link.service();
  }
  void presence(bool away) {
    room.push_back({{"t",away ? "away" : "back"},{"participants",json::array({
      {{"port",2},{"slot",1},{"connectionId","peer"},{"away",away}}})}});
  }
};
static void room_only(bool away, bool recover) {
  Fixture f; Link link(f.io()); f.prepare(link); link.pump(0);
  CHECK(!link.reconnecting());
  if (away) f.presence(true); else f.rejoining=true;
  const auto began=f.now; link.pump(0);
  CHECK(link.reconnecting()); CHECK(f.opens==0 && f.closes==0);
  f.now+=3370; link.pump(0);
  CHECK(!link.lost() && link.liveness_clock(f.now)==began);
  if (recover) {
    if (away) f.presence(false); else {f.rejoining=false; ++f.connection;}
    for (int n=0;n<3;++n) link.pump(0);
    CHECK(!link.reconnecting() && !link.lost()); CHECK(f.opens==1 && f.closes==1);
  } else {
    f.now=began+74999; link.pump(0); CHECK(!link.lost());
    f.now=began+75000; link.pump(0); CHECK(link.lost()=="your friend disconnected");
  }
  std::printf("room only: %s, %s passed\n",away ? "peer away" : "own rejoin",recover ? "recovered" : "75 s give-up");
}
// Pings and pad datagrams arrive over a room-carried link too: they must not count as direct.
static void datagrams_not_evidence() {
  Fixture f; Link link(f.io()); f.prepare(link); link.pump(0);
  Bytes c={0xc1,2,1,0}; put(c,tag("evidence"),8); put(c,f.now,8);
  Bytes d={kWireVersion,2,0,0}; put(d,tag("evidence"),8); put(d,kNone,4); put(d,kNone,4);
  f.packets.push_back(c); f.packets.push_back(d); link.pump(0); link.pump(0);
  f.rejoining=true; link.pump(0); CHECK(link.reconnecting());
  std::printf("room only: received control and data datagrams are not direct evidence\n");
}
static void direct_survives(const std::string& evidence, bool away) {
  Fixture f; Link link(f.io()); f.prepare(link);
  // An initial unknown report must not erase later evidence, and later unknown
  // reports must not erase evidence already observed on this peer.
  link.pump(0);
  f.path=evidence; link.pump(0);
  f.path="unknown"; link.pump(0);
  if (away) f.presence(true); else f.rejoining=true;
  link.pump(0); CHECK(!link.reconnecting() && !link.lost());
  f.now+=3370; link.pump(0); CHECK(link.liveness_clock(f.now)==f.now);
  if (away) f.presence(false); else {f.rejoining=false; ++f.connection;}
  for (int n=0;n<3;++n) link.pump(0);
  CHECK(!link.reconnecting() && !link.lost());
  // A new room connection reopens every link, as before (the link was opened on the old one).
  CHECK(away ? f.opens==0 && f.closes==0 : f.opens==1 && f.closes==1);
  std::printf("direct evidence: unknown -> %s -> unknown keeps running through %s\n",
      evidence.c_str(),away ? "peer away/back" : "room rejoin (reopened on the new connection)");
}
int main() {
  try {
    for (bool away : {false,true}) {
      for (bool recover : {false,true}) room_only(away,recover);
      for (const char* evidence : {"lan","direct","relay"}) direct_survives(evidence,away);
    }
    datagrams_not_evidence();
    std::printf("Friends link evidence: %u checks passed\n",checks); return 0;
  } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1;}
}
