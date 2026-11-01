// Offline production capture + transfer timing tests. No socket or game process.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "friends_handoff_capture.h"
#include <chrono>
#include <future>
#include <thread>
#include <random>
#include <stdexcept>
#include <cstdio>
using namespace yougame::console;
using namespace yougame::console::handoff_join;
struct TransferIO {
  std::function<uint64_t()> clock;
  std::function<bool()> cancelled,room_closed;
  std::function<bool(int)> flush_room;
  std::function<void(const std::string&)> log;
  std::function<bool(Json&,int)> next_room;
  std::function<bool(const Json&)> send_room;
};
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while (0)
static void capture_equivalence() {
  // Representative 40 MiB spans, including ~6.36 MiB incompressible memory.
  Buffer memory(40*1024*1024,0x31); std::mt19937 rng(45);
  for (size_t i=0;i<6360000;++i) memory[i]=uint8_t(rng());
  std::vector<native_savestate::Span> spans{{memory.data(),12345},{memory.data()+12345,uint32_t(memory.size()-12345)}};
  handoff_state::Identity id{12345,{}}; handoff_state::State state{20467,2,0,{1,2,3}};
  handoff_state::Sizes sizes; std::string why; Buffer old;
  const auto start=yougame::transport::now_ms();
  CHECK(handoff_state::encode(id,spans,state,old,sizes,why));
  const auto old_ms=yougame::transport::now_ms()-start;
  auto storage=std::make_shared<Buffer>(memory.size()+state.host.size());
  Result r; CHECK(capture_base(id,spans,state,r,why,storage));
  CHECK(storage->empty()); // ownership of preallocated storage moved into immutable capture
  CHECK(!r.bytes && bool(r.finish_base)); // game thread has done NO encoding
  const auto game_ms=r.ms;
  // Memory and host record may advance (or be unloaded) before worker encoding starts.
  std::fill(memory.begin(),memory.end(),0x77); state.host={9}; ++state.frame;
  const auto caller=std::this_thread::get_id();
  auto work=std::async(std::launch::async,[r,caller]() mutable {
    CHECK(std::this_thread::get_id()!=caller);
    auto finish=std::move(r.finish_base); finish(r); return r;
  });
  auto encoded=work.get(); CHECK(encoded.reason.empty() && encoded.bytes);
  CHECK(*encoded.bytes==old); CHECK(card_sha(*encoded.bytes)==card_sha(old));
  Chunks chunks;
  const auto sha=card_sha(old); const size_t n=(old.size()+kCardChunk-1)/kCardChunk;
  for (size_t k=0;k<n;++k) {
    // Hash calculated once per sender in production; avoid rehashing 6 MiB per chunk here.
    auto d=Json{{"kind","mu-hj-base"},{"i",k},{"n",n},{"sha256",sha},
      {"data",card_encode(old.data()+k*kCardChunk,std::min(kCardChunk,old.size()-k*kCardChunk))}};
    CHECK(d["data"]==card_encode(encoded.bytes->data()+k*kCardChunk,std::min(kCardChunk,encoded.bytes->size()-k*kCardChunk)));
    CHECK(chunks.receive(d));
  }
  CHECK(chunks.complete() && *chunks.finish()==old);
  std::printf("B: 40 MiB raw, %zu stored; old %llu ms; game copy %llu ms; worker %llu ms; %zu identical chunks/checksum\n",
    old.size(),(unsigned long long)old_ms,(unsigned long long)game_ms,(unsigned long long)encoded.worker_ms,n);
  // Raw codec and empty/nonempty host records also preserve the old format.
  for (bool compressed : {false,true}) {
    Buffer small(65537); for (auto& b:small) b=uint8_t(rng());
    spans={{small.data(),uint32_t(small.size())}}; state.host.clear();
    CHECK(handoff_state::encode(id,spans,state,old,sizes,why,compressed));
    handoff_state::Capture c; CHECK(handoff_state::capture(id,spans,state,c,why));
    Buffer fresh; CHECK(handoff_state::encode_capture(c,fresh,sizes,why,compressed)); CHECK(fresh==old);
  }
}
static void paced_transfer(size_t size, bool guest) {
  // Virtual monotonic clock, but REAL sender/receiver/chunks/pacer and message ordering.
  uint64_t now=1, sent_at=0; unsigned serial=0; size_t base_count=0,delta_count=0; bool finished_base=false;
  std::deque<std::pair<uint64_t,Json>> inbox; std::vector<std::string> logs;
  auto channel=std::make_shared<Channel>(); SessionConfig config; config.session_id="timing";
  auto bytes=std::make_shared<const Buffer>(size,0x31);
  const auto sha=card_sha(*bytes); const size_t n=(size+kCardChunk-1)/kCardChunk;
  auto push=[&](Json d,uint64_t at=0) { d["v"]=1; d["match"]="timing";
    inbox.emplace_back(at,Json{{"t","msg"},{"from","peer"},{"d",d}}); };
  auto enqueue=[&](const char* kind) {
    RoomPacer pacer; uint64_t at=now;
    for(size_t k=0;k<n;++k) {
      const size_t count=std::min(kCardChunk,size-k*kCardChunk);
      push({{"kind",kind},{"i",k},{"n",n},{"sha256",sha},{"data",card_encode(bytes->data()+k*kCardChunk,count)}},at);
      at=pacer.after(at,count);
    }
  };
  TransferIO io;
  io.clock=[&] {return now;}; io.cancelled=[] {return false;}; io.room_closed=[] {return false;};
  io.flush_room=[](int ms) {CHECK(ms>0);return true;}; io.log=[&](const std::string& s) {logs.push_back(s);};
  if(guest) {auto q=std::make_shared<Request>();q->serial=1;q->operation=Operation::GuestCSS;channel->request=q; enqueue("mu-hj-base");}
  io.next_room=[&](Json& m,int wait) {
    const auto q=std::atomic_load(&channel->request);
    if(q && q->serial!=serial) {
      auto r=std::make_shared<Result>();r->serial=q->serial;r->frame=907;r->hash="hash";
      if(q->operation==Operation::Base) r->finish_base=[&](Result& out) {
        CHECK(!finished_base); finished_base=true; out.bytes=bytes; out.worker_ms=7;
      };
      if(q->operation==Operation::PauseDelta) r->bytes=bytes;
      if(q->operation==Operation::PauseDelta) channel->paused_at=now;
      if(q->operation==Operation::Load) {CHECK(*q->base==*bytes && *q->delta==*bytes);}
      std::shared_ptr<const Result> out=r;std::atomic_store(&channel->result,out);serial=q->serial;
    }
    if(inbox.empty() || inbox.front().first>now+wait) {now+=wait;return false;}
    now=std::max(now,inbox.front().first);m=std::move(inbox.front().second);inbox.pop_front();return true;
  };
  io.send_room=[&](const Json& m) {
    const auto& d=m["d"];const auto kind=d["kind"].get<std::string>();
    if(kind=="mu-hj-base" || kind=="mu-hj-delta") {
      CHECK(!guest && d["sha256"]==sha && d["n"]==n);
      auto& count=kind=="mu-hj-base"?base_count:delta_count; CHECK(d["i"]==count); ++count;
      if(count==1) sent_at=now;
      if(count==n) {
        CHECK(now-sent_at>10000);
        if(kind=="mu-hj-base") {push({{"kind","mu-hj-base-ok"},{"sha256",sha}});push({{"kind","mu-hj-at-css"}});}
        else push({{"kind","mu-hj-loaded"},{"frame",907},{"hash","hash"}});
      }
    }
    if(kind=="mu-hj-at-css" && guest) enqueue("mu-hj-delta");
    if(kind=="mu-hj-loaded") push({{"kind","mu-hj-refuse"},{"reason","start-at-N-unavailable"}});
    return true;
  };
  transfer(io,!guest,"peer",config,channel);
  CHECK(logs.back()=="handoff-join: refuse start-at-N-unavailable");
  CHECK(guest || (finished_base && base_count==n && delta_count==n));
  CHECK(std::count_if(logs.begin(),logs.end(),[](const std::string& s) {return s.find("handoff-join: deadline ")==0;})==2);
  CHECK(transfer_budget(size)>10000 && transfer_budget(size)<=200000);
  CHECK(transfer_budget(stored_limit*2)==transfer_budget(stored_limit));
  std::printf("C: %s %zu bytes, %zu chunks each base/delta; budget %llu ms, both transfers completed in %llu ms\n",
    guest?"receiver":"sender",size,n,(unsigned long long)transfer_budget(size),(unsigned long long)(now-1));
}
static void pause_guard() {
  for (size_t size : {size_t(6360000),stored_limit}) {
    for (Operation op : {Operation::PauseDelta,Operation::Load}) {
      Channel channel; Result result; result.bytes=std::make_shared<Buffer>(size);
      uint64_t now=123, pumps=0;
      wait_paused(op,result,channel,now,[&] {return now;},[] {return false;},[&] {++now; ++pumps;});
      const auto expected=op==Operation::PauseDelta ? transfer_budget(size) : 20000;
      CHECK(pumps==expected && now==123+expected);
      std::printf("pause guard: %zu bytes, %s, %llu ms\n",size,
          op==Operation::PauseDelta ? "delta" : "load",(unsigned long long)pumps);
    }
  }
  // All early-outs still bypass the guard; cancellation/exit are checked on every pump.
  for (int stop=0;stop<5;++stop) {
    Channel channel; Result result; uint64_t now=0;
    if (stop==0) result.reason="failed";
    if (stop==1) channel.cancelled=true;
    wait_paused(Operation::PauseDelta,result,channel,0,[&] {return now;},
        [&] {return stop==2 || (stop==4 && now==17);},[&] {++now; if(stop==3 && now==17) channel.cancelled=true;});
    CHECK(now==uint64_t(stop<3 ? 0 : 17));
  }
  Channel channel; Result result; uint64_t now=0;
  wait_paused(Operation::PauseDelta,result,channel,0,[&] {return now;},[] {return false;},[&] {++now;});
  CHECK(now==20000); // no captured delta retains the old fallback
}
int main(int argc,char** argv) {
  try {
    if(argc<2 || std::string(argv[1])=="--pause") pause_guard();
    if(argc<2 || std::string(argv[1])=="--capture") capture_equivalence();
    if(argc<2 || std::string(argv[1])=="--transfer")
      for(size_t size:{size_t(6360000),stored_limit}) for(bool guest:{false,true}) paced_transfer(size,guest);
    std::printf("Friends timing: %u checks passed\n",checks);return 0;
  } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
