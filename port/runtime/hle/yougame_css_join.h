// Small Friends join protocol on the existing console room/link. No sockets or match rollback.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../abi/mu_css_join.h"
#include "yougame_console_card.h"
#include "yougame_friends_lobby.h"
#include <mutex>
#include <cstdio>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <deque>
#include <memory>

namespace yougame::console::css_join {
using json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
enum class Kind : uint8_t { Hold=1, OriginReady, CssState, Hash, PlugIn, Identity, Offer, Accept, Abort, Installed };
struct Message { Kind kind = Kind::Hold; uint32_t frame = 0; Bytes bytes; };
inline std::string invalid_line(const MuCssError& e) {
  std::string field = e.port >= 0 ? "ports["+std::to_string(e.port)+"]." : "";
  field += e.field ? e.field : "state";
  if (e.offset >= 0) field += "["+std::to_string(e.offset)+"]";
  if (e.field && std::string(e.field)=="tag") return "css-join invalid: "+field;
  char value[64]; std::snprintf(value,sizeof value,"%.9g",e.value);
  return "css-join invalid: "+field+"="+value;
}
inline bool valid(const Message& m,std::string* rejection=nullptr) {
  switch (m.kind) {
  case Kind::Hold: case Kind::OriginReady: case Kind::PlugIn: case Kind::Offer: case Kind::Accept: case Kind::Abort: case Kind::Installed:
    return m.bytes.empty();
  case Kind::CssState: {
    MuCssState s{};
    if (m.bytes.size() != sizeof s) {
      if (rejection) *rejection="css-join invalid: state.size="+std::to_string(m.bytes.size());
      return false;
    }
    std::memcpy(&s,m.bytes.data(),sizeof s);
    MuCssError error{};
    if (mu_css_validate(&s,&error)) return true;
    if (rejection) *rejection=invalid_line(error);
    return false;
  }
  case Kind::Identity: {
    if (m.bytes.size()!=1+sizeof(MuCssIdentity)) {
      if (rejection) *rejection="css-join invalid: identity.size="+std::to_string(m.bytes.size());
      return false;
    }
    MuCssError error{};
    if (m.frame) mu_css_reject(&error,"frame",-1,-1,m.frame);
    else if (m.bytes[0]>=4) mu_css_reject(&error,"port",-1,-1,m.bytes[0]);
    else if (mu_css_identity_check(m.bytes.data()+1,m.bytes[9],m.bytes[10],&error,m.bytes[0])) return true;
    if (rejection) *rejection=invalid_line(error);
    return false;
  }
  case Kind::Hash:
    return m.bytes.size() == 16 && std::all_of(m.bytes.begin(),m.bytes.end(),[](uint8_t c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
  }
  return false;
}
inline Bytes encode(const Message& m) {
  if (!valid(m)) return {};
  Bytes out(10+m.bytes.size());
  out[0]='M'; out[1]='C'; out[2]='J'; out[3]=2; out[4]=uint8_t(m.kind); out[5]=0;
  mu_css_write_le(out.data()+6,m.frame,4);
  std::copy(m.bytes.begin(),m.bytes.end(),out.begin()+10); return out;
}
inline bool decode(const Bytes& bytes, Message& out,std::string* rejection=nullptr) {
  if (bytes.size() < 10 || bytes.size() > 10+MU_CSS_SIZE || bytes[0]!='M' || bytes[1]!='C' ||
      bytes[2]!='J' || bytes[3]!=2 || bytes[5]) return false;
  Message m{Kind(bytes[4]),uint32_t(mu_css_read_le(bytes.data()+6,4)),Bytes(bytes.begin()+10,bytes.end())};
  if (!valid(m,rejection)) return false;
  out = std::move(m); return true;
}
inline json envelope(const Message& m, const std::string& match, const std::string& peer) {
  const auto b=encode(m);
  return {{"t","msg"},{"to",peer},{"d",{{"kind","mu-css-join"},{"v",2},{"match",match},{"data",card_encode(b.data(),b.size())}}}};
}
inline bool receive(const json& m,const std::string& match,const std::string& peer,Message& out,std::string* rejection=nullptr) {
  if (!m.is_object() || m.value("t",json())!="msg" || m.value("from",json())!=peer ||
      !m.count("d") || !m["d"].is_object()) return false;
  const auto& d=m["d"];
  if (d.value("kind",json())!="mu-css-join" || d.value("v",json())!=2 || d.value("match",json())!=match ||
      !d.count("data") || !d["data"].is_string() || d["data"].get_ref<const std::string&>().size()>352) return false;
  Bytes b; return card_decode(d["data"].get<std::string>(),b) && decode(b,out,rejection);
}
inline Bytes bytes(const MuCssState& s) {
  const auto* p=reinterpret_cast<const uint8_t*>(&s); return {p,p+sizeof s};
}
inline Bytes identity(const MuCssPort& p,int port) {
  Bytes b(11); b[0]=uint8_t(port); std::copy(p.tag,p.tag+8,b.begin()+1); b[9]=p.flags&7; b[10]=p.auto_handicap; return b;
}
inline void merge_identity(MuCssState& s,const Message& m) {
  auto& p=s.ports[m.bytes[0]];
  std::copy(m.bytes.begin()+1,m.bytes.begin()+9,p.tag); p.flags=(p.flags&~7)|m.bytes[9]; p.auto_handicap=m.bytes[10];
  p.tag_index=(p.flags&4) ? 0 : 120; // foreign numeric indices have no meaning
}
inline std::atomic<int> scene{0}; // 1 stable CSS, 2 fight/results, 0 other; simulation writes
inline std::atomic<bool> on_css{false};
inline std::atomic<bool> locked{false}; // room thread requests; game only observes on CSS
inline std::atomic<bool> available{false}, relaunching{false};
enum class Operation { Capture, Origin, Apply, Prepare, Plug, Release };
struct Request {
  unsigned serial=0; Operation operation=Operation::Capture;
  uint32_t frame=0; uint64_t seed=0; bool host=false; int port=0; Bytes card; MuCssState state{}; SessionConfig config;
};
struct Result { unsigned serial=0; bool ok=false; uint32_t frame=0; MuCssState state{}; std::string hash; };
struct Channel {
  std::shared_ptr<const Request> request;
  std::shared_ptr<const Result> result;
  std::atomic<bool> cancelled{false}, working{false};
  std::recursive_mutex main_mutex; // main adoption versus setup reclaim after cancellation
  std::mutex room_mutex;
  std::deque<json> room_messages;
  void queue_room(const json& m) {
    std::lock_guard<std::mutex> lock(room_mutex); room_messages.push_back(m);
  }
  std::deque<json> take_room() {
    std::lock_guard<std::mutex> lock(room_mutex);
    std::deque<json> out; out.swap(room_messages); return out;
  }
};
inline std::shared_ptr<Channel> pending;
struct Launch { std::unique_ptr<SessionLink> link; SessionConfig config; };
inline std::shared_ptr<Launch> launch;
using Begin=std::function<std::shared_ptr<Channel>()>;
// Preparation thread owns protocol state. Commands/results are immutable, game-thread-only
// capture/apply. The native stack stays paused between OriginReady, Apply/Hash and PlugIn.
template<class IO>
bool synchronize(IO& io,bool host,int other_port,const std::string& peer,const SessionConfig& config,
                 const std::shared_ptr<Channel>& channel) {
  bool success=false;
  const uint64_t until=io.clock()+30000;
  auto emit=[&](Message m) {
    std::string rejection;
    if (!valid(m,&rejection)) { if (!rejection.empty()) io.log(rejection); return false; }
    return io.send_room(envelope(m,config.session_id,peer));
  };
  std::deque<Message> inbox;
  bool ended=false;
  auto pump=[&](int wait) {
    json m; Message got; std::string rejection;
    if (!io.next_room(m,wait)) return;
    if (receive(m,config.session_id,peer,got,&rejection)) {
      if (got.kind==Kind::Abort) ended=true;
      else inbox.push_back(std::move(got));
    } else {
      if (!rejection.empty()) io.log(rejection);
      // A malformed join from the frozen peer is a refusal, never an ignored name.
      if (m.is_object() && m.value("t",json())=="msg" && m.value("from",json())==peer &&
          m.count("d") && m["d"].is_object() && m["d"].value("kind",json())=="mu-css-join") ended=true;
      // Decide terminal controls without touching the Link, even while next_room was
      // blocked across main's ownership transfer or destruction of the provisional engine.
      if (!friends::closed_line(m).empty() ||
          (m.value("t",json())=="sessionEnded" && m.value("sessionId",json())==config.session_id) ||
          (m.value("t",json())=="msg" && m.value("from",json())==peer &&
           m.count("d") && m["d"].is_object() && m["d"].value("kind",json())=="mu-console-end" &&
           m["d"].value("v",json())==1 && m["d"].value("sessionId",json())==config.session_id)) ended=true;
      channel->queue_room(m);
    }
    if (ended && io.css_lock) io.css_lock(false);
  };
  auto next=[&](Kind kind,Message& out,uint64_t deadline=0) {
    while (io.clock()<(deadline ? deadline : until) && !io.cancelled() && !io.room_closed() && !ended && !channel->cancelled.load(std::memory_order_acquire)) {
      if (inbox.empty()) pump(10);
      if (ended) return false;
      if (inbox.empty()) continue;
      Message got=std::move(inbox.front()); inbox.pop_front();
      if (got.kind==Kind::Offer) { emit({Kind::Accept,0,{}}); continue; }
      if (got.kind==Kind::Accept) continue;
      if (got.kind!=kind) return false; // out-of-order/conflicting control: fail closed
      out=std::move(got); return true;
    }
    return false;
  };
  unsigned serial=0;
  auto command=[&](Operation op,const MuCssState& state,uint32_t frame,uint64_t deadline=0) -> std::shared_ptr<const Result> {
    auto q=std::make_shared<Request>(); q->serial=++serial; q->operation=op; q->state=state;
    q->frame=frame; q->seed=config.seed; q->host=host; q->port=config.my_port-1; q->card=config.card_bytes; q->config=config;
    std::atomic_store(&channel->request,std::shared_ptr<const Request>(q));
    while (io.clock()<(deadline ? deadline : until) && !io.cancelled() && !io.room_closed() && !ended && !channel->cancelled.load(std::memory_order_acquire)) {
      // Release is the final ownership acknowledgement. Stop consuming room traffic
      // before publishing it: main drains the queue, then the live service owns the room.
      if (op!=Operation::Release) pump(0);
      if (ended) return {};
      auto r=std::atomic_load(&channel->result);
      if (r && r->serial==serial) return r->ok ? r : nullptr;
      io.wait(1);
    }
    return {};
  };
  bool aborted=false;
  auto abort=[&](const char* reason) {
    aborted=true;
    io.log(std::string("css-join abort: ")+reason);
    emit({Kind::Abort,0,{}});
    if (io.flush_room && !io.room_closed()) io.flush_room(1000);
    if (io.css_lock) io.css_lock(false);
    return false;
  };
  struct Cleanup {
    std::shared_ptr<Channel> c; bool& success; std::function<void()> notify;
    ~Cleanup() {
      if (!success) {
        try { notify(); } catch (...) {}
        c->cancelled.store(true,std::memory_order_release);
      }
    }
  } cleanup{channel,success,[&] {
    if (!aborted) { try { abort("protocol exception"); } catch (...) {} }
    if (io.css_lock) io.css_lock(false);
  }};
  MuCssState state{};
  auto captured=command(Operation::Capture,state,0);
  if (!captured) return abort("capture");
  const int local_port=config.my_port-1;
  if (host) state=captured->state;
  if (!emit({Kind::Identity,0,identity(captured->state.ports[local_port],local_port)})) return abort("protocol");
  Message remote;
  if (!next(Kind::Identity,remote)) return abort("protocol");
  if (remote.bytes[0]!=other_port-1) {
    io.log("css-join invalid: identity.port="+std::to_string(remote.bytes[0])); return abort("protocol");
  }
  if (host) merge_identity(state,remote);
  auto origin=command(Operation::Origin,state,0);
  if (!origin || !emit({Kind::OriginReady,origin->frame,{}})) return abort("protocol");
  if (!next(Kind::OriginReady,remote)) return abort("protocol");
  uint32_t k=0;
  if (host) {
    if (std::max(origin->frame,remote.frame)>UINT32_MAX-2) return abort("protocol");
    k=std::max(origin->frame,remote.frame)+2;
    if (!emit({Kind::CssState,k,bytes(state)})) return abort("protocol");
  } else {
    if (!next(Kind::CssState,remote) || remote.frame<origin->frame || remote.frame-origin->frame>600) return abort("protocol");
    k=remote.frame; std::memcpy(&state,remote.bytes.data(),sizeof state);
    // The host must echo OUR identity, including rumble off. Refuse a peer overwriting it.
    if (identity(state.ports[local_port],local_port)!=identity(captured->state.ports[local_port],local_port)) {
      const auto& got=state.ports[local_port];
      const auto& own=captured->state.ports[local_port];
      MuCssError error{};
      if ((got.flags & 7)!=(own.flags & 7)) mu_css_reject(&error,"flags",local_port,-1,got.flags & 7);
      else if (got.auto_handicap!=own.auto_handicap) mu_css_reject(&error,"auto_handicap",local_port,-1,got.auto_handicap);
      else for (int n=0;n<8;++n) if (got.tag[n]!=own.tag[n]) { mu_css_reject(&error,"tag",local_port,n,0); break; }
      io.log(invalid_line(error)); return abort("protocol");
    }
  }
  io.log("css-join state bytes="+std::to_string(sizeof state)+" frame="+std::to_string(k));
  auto applied=command(Operation::Apply,state,k);
  if (!applied || applied->frame!=k || applied->hash.size()!=16 ||
      !emit({Kind::Hash,k,Bytes(applied->hash.begin(),applied->hash.end())})) return abort("protocol");
  if (!next(Kind::Hash,remote) || remote.frame!=k || std::string(remote.bytes.begin(),remote.bytes.end())!=applied->hash) {
    io.log("css-join hash mismatch frame="+std::to_string(k)); return abort("protocol");
  }
  io.log("css-join hash ok frame="+std::to_string(k));
  // Check every begin() admission condition on the paused game thread before voting.
  if (!command(Operation::Prepare,state,k)) return abort("engine preflight");
  if (!emit({Kind::PlugIn,k,{}}) || !next(Kind::PlugIn,remote) || remote.frame!=k) return abort("protocol");
  if (!command(Operation::Plug,state,k)) return abort("engine begin");
  // Keep the game paused and the caller's relaunch bookkeeping alive until BOTH engines
  // acknowledge installation. An abort here tears down the provisional engine on main.
  const uint64_t release_until=io.clock()+3000;
  if (!emit({Kind::Installed,k,{}}) || !next(Kind::Installed,remote,release_until) || remote.frame!=k)
    return abort("engine installation acknowledgement");
  if (!command(Operation::Release,state,k,release_until)) return abort("engine release");
  success=true; return true;
}
} // namespace yougame::console::css_join
