#include "yougame_css_join.h"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
using namespace yougame::console;
namespace cj=css_join;
static std::atomic<int> checks{0};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
static MuCssState state() {
  MuCssState s{}; s.version=MU_CSS_VERSION; s.size=MU_CSS_SIZE;
  s.rules[1]=0x34; s.rules[6]=10; mu_css_write_le(s.rules+20,0xffffffff,4);
  mu_css_write_le(s.item_mask,~0ull,8); mu_css_write_le(s.stage_mask,0xffffffff,4);
  s.rules[2]=1; s.rules[4]=4; s.rules[8]=8;
  for (auto& p:s.ports) { p.fighter=255; p.kind=3; p.tag_index=120; p.auto_handicap=5; p.handicap=9; p.cpu=9; }
  s.ports[0].fighter=2; s.ports[0].kind=0; s.ports[0].costume=1;
  return s;
}
struct Pipe {
  std::mutex mutex; std::deque<nlohmann::json> messages[2]; std::vector<std::string> logs[2];
  int applies[2]{}, plugs[2]{}; MuCssState applied[2]{}; bool mismatch=false, tamper=false;
  std::atomic<bool> stop{false};
};
struct IO {
  Pipe& p; int side;
  uint64_t clock() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  bool cancelled() { return false; } bool room_closed() { return false; }
  std::function<void(bool)> css_lock=[](bool) {};
  std::function<bool(int)> flush_room=[](int) { return true; };
  void wait(int n) { std::this_thread::sleep_for(std::chrono::milliseconds(n)); }
  bool send_room(nlohmann::json j) {
    cj::Message m; CHECK(cj::receive(nlohmann::json{{"t","msg"},{"from",side?"guest":"host"},{"d",j["d"]}},"session",side?"guest":"host",m));
    std::lock_guard<std::mutex> l(p.mutex);
    if (m.kind==cj::Kind::PlugIn) ++p.plugs[side];
    if (!side && p.tamper && m.kind==cj::Kind::CssState) {
      MuCssState changed{}; std::memcpy(&changed,m.bytes.data(),sizeof changed);
      changed.ports[1].flags |= 1; // malicious host tries to enable our rumble
      j=cj::envelope({cj::Kind::CssState,m.frame,cj::bytes(changed)},"session","guest");
    }
    j["from"]=side?"guest":"host"; p.messages[1-side].push_back(j); return true;
  }
  bool next_room(nlohmann::json& out,int n) {
    { std::lock_guard<std::mutex> l(p.mutex); if (!p.messages[side].empty()) {
      out=std::move(p.messages[side].front()); p.messages[side].pop_front(); return true; } }
    wait(n); return false;
  }
  void log(const std::string& s) { std::lock_guard<std::mutex> l(p.mutex); p.logs[side].push_back(s); }
};
static void session(bool mismatch,bool tamper=false) {
  Pipe pipe; pipe.mismatch=mismatch; pipe.tamper=tamper;
  std::shared_ptr<cj::Channel> ch[2]={std::make_shared<cj::Channel>(),std::make_shared<cj::Channel>()};
  std::thread game[2];
  for (int side=0;side<2;++side) game[side]=std::thread([&,side] {
    unsigned serial=0;
    while (!pipe.stop.load()) {
      auto q=std::atomic_load(&ch[side]->request);
      if (!q || q->serial==serial) { std::this_thread::yield(); continue; }
      serial=q->serial;
      auto r=std::make_shared<cj::Result>(); r->serial=serial; r->ok=true; r->frame=4;
      if (q->operation==cj::Operation::Capture) {
        r->state=state();
        if (side) { auto& own=r->state.ports[1]; std::memcpy(own.tag,"\x82\x66\x82\x74",4); own.flags=4; own.tag_index=7; }
      } else if (q->operation==cj::Operation::Apply) {
        ++pipe.applies[side]; pipe.applied[side]=q->state; r->frame=q->frame;
        r->hash=side && mismatch ? "0000000000000001" : "abcdef0123456789";
      }
      std::atomic_store(&ch[side]->result,std::shared_ptr<const cj::Result>(r));
    }
  });
  bool ok[2]{};
  std::thread work[2];
  for (int side=0;side<2;++side) work[side]=std::thread([&,side] {
    IO io{pipe,side}; SessionConfig c; c.my_port=side+1; c.session_id="session";
    ok[side]=cj::synchronize(io,!side,2-side,side?"host":"guest",c,ch[side]);
  });
  for (auto& t:work) t.join();
  pipe.stop=true; for (auto& t:game) t.join();
  if (tamper) {
    CHECK(!ok[0] && !ok[1]); CHECK(pipe.applies[0]==1 && pipe.applies[1]==0);
    CHECK(pipe.plugs[0]==0 && pipe.plugs[1]==0); return;
  }
  CHECK(ok[0]==!mismatch && ok[1]==!mismatch);
  CHECK(pipe.applies[0]==1 && pipe.applies[1]==1);
  CHECK(std::memcmp(&pipe.applied[0],&pipe.applied[1],sizeof(MuCssState))==0);
  CHECK(pipe.applied[0].ports[1].flags==4); // own tag, BOTH rumble choices off, echoed by host
  CHECK(std::memcmp(pipe.applied[0].ports[1].tag,"\x82\x66\x82\x74",4)==0);
  CHECK(pipe.applied[0].rules[4]==4 && pipe.applied[0].rules[8]==8);
  CHECK(pipe.plugs[0]==(mismatch?0:1) && pipe.plugs[1]==(mismatch?0:1));
  CHECK(ch[0]->cancelled.load()==mismatch && ch[1]->cancelled.load()==mismatch);
  for (int side=0;side<2;++side) CHECK(std::find(pipe.logs[side].begin(),pipe.logs[side].end(),
      mismatch ? "css-join hash mismatch frame=6" : "css-join hash ok frame=6")!=pipe.logs[side].end());
}

static void rejection_tests() {
  const auto good=state();
  auto reject=[&](size_t at,unsigned value) {
    auto bad=good; reinterpret_cast<unsigned char*>(&bad)[at]=value; CHECK(!mu_css_valid(&bad));
  };
  for (auto at:{0,1,46,47}) reject(at,254);
  for (unsigned i=0;i<20;++i) reject(offsetof(MuCssState,rules)+i,255);
  reject(offsetof(MuCssState,rules)+4,0); reject(offsetof(MuCssState,rules)+6,4);
  auto bad=good; mu_css_write_le(bad.rules+20,98,4); CHECK(!mu_css_valid(&bad));
  for (unsigned value=5;value<255;++value) reject(offsetof(MuCssState,item_frequency),value);
  for (unsigned value:{0u,1u,2u,3u,4u,255u}) { bad=good; bad.item_frequency=value; CHECK(mu_css_valid(&bad)); }
  for (auto at:{offsetof(MuCssState,teams),offsetof(MuCssState,ready)}) reject(at,2);
  reject(offsetof(MuCssState,stage)+1,1); reject(offsetof(MuCssState,stage),21);
  bad=good; bad.item_mask[7]=0; CHECK(!mu_css_valid(&bad));
  bad=good; bad.stage_mask[3]=0; CHECK(!mu_css_valid(&bad));
  bad=good; mu_css_write_le(bad.item_mask,0x7fffffff,8); CHECK(mu_css_valid(&bad));
  bad=good; mu_css_write_le(bad.item_mask,0x10000000,8); CHECK(mu_css_valid(&bad));
  bad=good; mu_css_write_le(bad.item_mask,0xefffffff,8); CHECK(!mu_css_valid(&bad));
  const size_t port=offsetof(MuCssState,ports);
  for (auto at:{2,3,4,5,6,7,40,41,42,43,45,47,49,50}) reject(port+at,254);
  for (auto at:{4,5,50}) reject(port+at,0);
  reject(port+43,2); reject(port+45,32); reject(port+49,40); reject(port+48,6); reject(port+7,48);
  for (unsigned f=0;f<6;++f) {
    bad=good; mu_css_write_le(reinterpret_cast<unsigned char*>(&bad.ports[0])+16+4*f,0x7f800000,4); CHECK(!mu_css_valid(&bad));
    bad=good; mu_css_write_le(reinterpret_cast<unsigned char*>(&bad.ports[0])+16+4*f,0x42800001,4); CHECK(!mu_css_valid(&bad));
  }
  const unsigned char valid_tag[8]={0x82,0x60,0x82,0x61,0,0,0,0};
  CHECK(mu_css_tag_valid(valid_tag));
  const unsigned char tags[][8]={{'%'},{'%','n'},{'A'},{0x82,0x7f},{0x82,0x60,0,0,1},{0x82,0x60,0,0x25},{0,0x82,0x60},{0x82},{0x20},{0x81,0x40}};
  for (const auto& tag:tags) {
    CHECK(!mu_css_tag_valid(tag));
    bad=good; std::copy(tag,tag+8,bad.ports[0].tag); bad.ports[0].flags=4;
    CHECK(!mu_css_valid(&bad));
    cj::Message m{cj::Kind::Identity,0,cj::identity(bad.ports[0],0)};
    CHECK(!cj::valid(m));
  }
  for (unsigned value:{0u,10u,255u}) {
    auto own=good.ports[0]; own.auto_handicap=value;
    CHECK(!cj::valid({cj::Kind::Identity,0,cj::identity(own,0)}));
  }
}

// Deterministic fault walk through the real protocol, without sockets, sleeps or a game.
struct FaultIO {
  std::shared_ptr<cj::Channel> channel=std::make_shared<cj::Channel>();
  int fail_op=-1, fail_send=-1, end_op=-1, end_kind=-1;
  bool session_end=false, timeout=false, locked=true, late_release=false, invalid_peer=false;
  unsigned handled=0, off=0, aborts=0, forwarded=0; uint64_t now=0;
  std::deque<nlohmann::json> messages;
  std::vector<std::string> logs;
  uint64_t clock() { return timeout ? 40000 : now; }
  bool cancelled() { return false; } bool room_closed() { return false; }
  void wait(int n) {
    now+=n;
    auto q=std::atomic_load(&channel->request);
    if (q && q->operation==cj::Operation::Release && q->serial!=handled) {
      handled=q->serial;
      auto r=std::make_shared<cj::Result>(); r->serial=q->serial;
      r->ok=int(q->operation)!=fail_op && int(q->operation)!=end_op;
      std::atomic_store(&channel->result,std::shared_ptr<const cj::Result>(r));
    }
  }
  std::function<void(bool)> css_lock=[this](bool on) { if (locked && !on) ++off; locked=on; };
  std::function<bool(int)> flush_room=[](int) { return true; };
  void log(const std::string& s) { logs.push_back(s); }
  nlohmann::json peer_end() {
    if (session_end) return {{"t","sessionEnded"},{"sessionId","session"}};
    return {{"t","msg"},{"from","guest"},{"d",{{"kind","mu-console-end"},{"sessionId","session"},{"v",1},{"reason","left"}}}};
  }
  void enqueue(cj::Message m) {
    auto j=cj::envelope(m,"session","host"); j["from"]="guest";
    if (invalid_peer && m.kind==cj::Kind::Identity) {
      auto b=cj::encode(m); b[11]=0x25;
      j["d"]["data"]=card_encode(b.data(),b.size());
    }
    messages.push_back(j);
  }
  bool send_room(const nlohmann::json& j) {
    auto received=j; received["from"]="host"; cj::Message m;
    CHECK(cj::receive(received,"session","host",m));
    if (late_release && m.kind==cj::Kind::Installed) now+=2;
    if (m.kind==cj::Kind::Abort) { ++aborts; return true; }
    if (int(m.kind)==fail_send) return false;
    if (int(m.kind)==end_kind) { messages.push_back(peer_end()); return true; }
    switch (m.kind) {
    case cj::Kind::Identity: enqueue({m.kind,0,cj::identity(state().ports[1],1)}); break;
    case cj::Kind::OriginReady: enqueue({m.kind,4,{}}); break;
    case cj::Kind::Hash: case cj::Kind::PlugIn: case cj::Kind::Installed: enqueue(m); break;
    default: break;
    }
    // Unrelated room traffic must also reach Link::room_message.
    messages.push_back({{"t","presence"}});
    return true;
  }
  bool next_room(nlohmann::json& out,int n) {
    auto q=std::atomic_load(&channel->request);
    if (q && q->serial!=handled) {
      handled=q->serial;
      if (late_release && q->operation==cj::Operation::Plug) now=29999;
      if (int(q->operation)==end_op) { out=peer_end(); return true; }
      auto r=std::make_shared<cj::Result>(); r->serial=q->serial;
      r->ok=int(q->operation)!=fail_op; r->frame=q->operation==cj::Operation::Apply?q->frame:4;
      r->state=state(); r->hash="abcdef0123456789";
      std::atomic_store(&channel->result,std::shared_ptr<const cj::Result>(r));
    }
    if (!messages.empty()) { out=messages.front(); messages.pop_front(); return true; }
    wait(n); return false;
  }
  bool run() {
    SessionConfig config; config.my_port=1; config.session_id="session";
    bool ok=cj::synchronize(*this,true,2,"guest",config,channel);
    forwarded=channel->take_room().size();
    if (!ok) {
      CHECK(channel->cancelled.load() && !locked && off==1 && aborts==1);
      CHECK(std::any_of(logs.begin(),logs.end(),[](const auto& s) { return s.find("css-join abort: ")==0; }));
    }
    return ok;
  }
};
static void failure_walk() {
  for (auto op:{cj::Operation::Capture,cj::Operation::Origin,cj::Operation::Apply,
                cj::Operation::Prepare,cj::Operation::Plug,cj::Operation::Release}) {
    FaultIO io; io.fail_op=int(op); CHECK(!io.run());
    for (bool session_end:{false,true}) {
      FaultIO peer; peer.end_op=int(op); peer.session_end=session_end;
      CHECK(!peer.run()); CHECK(peer.now<100);
    }
  }
  for (auto kind:{cj::Kind::Identity,cj::Kind::OriginReady,cj::Kind::CssState,
                  cj::Kind::Hash,cj::Kind::PlugIn,cj::Kind::Installed}) {
    FaultIO io; io.fail_send=int(kind); CHECK(!io.run());
    for (bool session_end:{false,true}) {
      FaultIO peer; peer.end_kind=int(kind); peer.session_end=session_end;
      CHECK(!peer.run()); CHECK(peer.now<100);
    }
  }
  FaultIO ok; CHECK(ok.run() && ok.forwarded>0);
  FaultIO late; late.late_release=true; CHECK(late.run() && late.now>=30001);
  FaultIO invalid; invalid.invalid_peer=true; CHECK(!invalid.run());
  CHECK(std::find(invalid.logs.begin(),invalid.logs.end(),"css-join invalid: ports[1].tag[0]")!=invalid.logs.end());
}
int main() {
  CHECK(MU_CSS_VERSION==2 && MU_CSS_SIZE==252 && sizeof(MuCssIdentity)==10);
  CHECK(offsetof(MuCssState,shield01)==2 && offsetof(MuCssState,shield23)==45 && offsetof(MuCssState,reserved)==47);
  CHECK(sizeof(MuCssState)==252 && sizeof(MuCssPort)==51);
  CHECK(offsetof(MuCssState,rules)==4 && offsetof(MuCssState,item_frequency)==28);
  CHECK(offsetof(MuCssState,item_mask)==29 && offsetof(MuCssState,stage_mask)==37);
  CHECK(offsetof(MuCssState,teams)==41 && offsetof(MuCssState,stage)==42);
  CHECK(offsetof(MuCssState,ready)==44 && offsetof(MuCssState,ports)==48);
  CHECK(offsetof(MuCssPort,tag)==8 && offsetof(MuCssPort,cursor_x)==16);
  CHECK(offsetof(MuCssPort,cursor_state)==40 && offsetof(MuCssPort,auto_handicap)==50);
  auto s=state(); CHECK(mu_css_valid(&s));
  cj::Message decoded;
  for (auto kind:{cj::Kind::Hold,cj::Kind::OriginReady,cj::Kind::PlugIn,cj::Kind::Offer,cj::Kind::Accept,cj::Kind::Abort,cj::Kind::Installed}) {
    auto b=cj::encode({kind,0x78563412,{}}); CHECK(b.size()==10 && b[6]==0x12 && b[9]==0x78);
    CHECK(cj::decode(b,decoded) && decoded.kind==kind && decoded.frame==0x78563412);
    b.push_back(0); CHECK(!cj::decode(b,decoded));
  }
  auto b=cj::encode({cj::Kind::CssState,123,cj::bytes(s)}); CHECK(b.size()==262 && cj::decode(b,decoded));
  for (size_t size=0;size<b.size();++size) CHECK(!cj::decode(cj::Bytes(b.begin(),b.begin()+size),decoded));
  for (size_t at:{size_t(0),size_t(1),size_t(2),size_t(3),size_t(4),size_t(5),size_t(10),size_t(11),size_t(57)}) {
    auto bad=b; bad[at]=255; CHECK(!cj::decode(bad,decoded));
  }
  auto closed=s; closed.ports[3].token_colour=255; CHECK(mu_css_valid(&closed));
  auto bad=s; bad.ports[3].kind=2; CHECK(!mu_css_valid(&bad));
  bad=s; mu_css_write_le(bad.ports[0].cursor_x,0x7fc00000,4); CHECK(!mu_css_valid(&bad));
  bad=s; bad.ports[0].token_state=5; CHECK(!mu_css_valid(&bad));
  bad=s; bad.ports[0].held_token=8; CHECK(mu_css_valid(&bad)); // handicap slider
  auto hash=cj::encode({cj::Kind::Hash,6,cj::Bytes(16,'a')}); CHECK(cj::decode(hash,decoded));
  hash.back()='G'; CHECK(!cj::decode(hash,decoded)); hash.pop_back(); CHECK(!cj::decode(hash,decoded));
  auto invalid_identity=cj::Message{cj::Kind::Identity,1,cj::identity(s.ports[0],0)};
  CHECK(!cj::valid(invalid_identity));
  auto msg=cj::envelope({cj::Kind::CssState,6,cj::bytes(s)},"session","guest"); msg["from"]="host";
  CHECK(cj::receive(msg,"session","host",decoded)); CHECK(!cj::receive(msg,"wrong","host",decoded));
  CHECK(!cj::receive(msg,"session","intruder",decoded)); msg["d"]["v"]=1; CHECK(!cj::receive(msg,"session","host",decoded));
  msg["d"]["v"]=2; msg["d"]["data"]=std::string(353,'A'); CHECK(!cj::receive(msg,"session","host",decoded));
  // Version-1 peers cannot enter the changed installation handshake.
  auto legacy=cj::encode({cj::Kind::Offer,0,{}}); legacy[3]=1; CHECK(!cj::decode(legacy,decoded));
  // m-ex cursor range and runtime costumes are admitted by the wire validator.
  bad=s; mu_css_write_le(bad.ports[1].cursor_x,0x41e40000,4); CHECK(mu_css_valid(&bad)); // 28.5
  bad.ports[1].fighter=30; bad.ports[1].costume=8; CHECK(mu_css_valid(&bad));
  MuCssError error{}; bad=s; mu_css_write_le(bad.ports[1].cursor_x,0x41f00000,4); // 30
  CHECK(!mu_css_validate(&bad,&error)); CHECK(cj::invalid_line(error)=="css-join invalid: ports[1].cursor_x=30");
  bad=s; bad.ports[0].tag[0]=0x25; bad.ports[0].flags=4; bad.ports[0].tag_index=0;
  CHECK(!mu_css_validate(&bad,&error)); CHECK(cj::invalid_line(error)=="css-join invalid: ports[0].tag[0]");
  auto wire=cj::encode({cj::Kind::CssState,0,cj::bytes(s)});
  wire[10+offsetof(MuCssState,ports)+offsetof(MuCssPort,cursor_flags)]=2;
  std::string peer_rejection; CHECK(!cj::decode(wire,decoded,&peer_rejection));
  CHECK(peer_rejection=="css-join invalid: ports[0].cursor_flags=2");
  auto invalid=cj::Message{cj::Kind::Identity,0,cj::identity(bad.ports[0],0)};
  std::string rejected; CHECK(!cj::valid(invalid,&rejected)); CHECK(rejected=="css-join invalid: ports[0].tag[0]");
  rejection_tests(); failure_walk();
  session(false); session(true); session(false,true);
  std::printf("css join: %d checks passed (layout, codecs, identity echo, K/hash/plug, mismatch fallback)\n",checks.load());
}
