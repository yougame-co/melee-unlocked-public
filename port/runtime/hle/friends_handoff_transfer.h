// Live Friends pre-copy protocol. The room thread only moves immutable buffers.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "friends_handoff_join.h"
#include "yougame_console_request.h"
#include "yougame_transport.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>

namespace yougame::console::handoff_join {
using transport::str;
using Buffer = std::vector<uint8_t>;
constexpr size_t stored_limit = 24 * 1024 * 1024;
inline bool quiescent(unsigned disc_depth, unsigned callbacks) { return !disc_depth && !callbacks; }
enum class Operation { GuestCSS, Base, PauseDelta, Load };
struct Request {
  unsigned serial = 0;
  Operation operation = Operation::GuestCSS;
  uint64_t seed = 0;
  std::shared_ptr<Buffer> storage; // allocated by worker, exclusively filled by game before result publication
  std::shared_ptr<const Buffer> base, delta;
};
struct Result {
  unsigned serial = 0;
  std::string reason, hash;
  uint64_t frame = 0, raw = 0, ms = 0;
  size_t pages = 0;
  std::shared_ptr<const Buffer> bytes;
  // Executed by the preparation worker; captures owned bytes only.
  std::function<void(Result&)> finish_base;
  uint64_t worker_ms = 0;
};
struct Channel {
  std::shared_ptr<const Request> request;
  std::shared_ptr<const Result> result;
  std::atomic<bool> cancelled{false}, paused{false}, working{false};
  std::atomic<uint64_t> paused_at{0};
};
// Main thread claims a session exactly once; no worker ever receives pointers to game spans.
inline std::shared_ptr<Channel> pending_channel;
using Begin = std::function<std::shared_ptr<Channel>()>;
inline std::string frame_hash(uint64_t n) {
  char text[17]; std::snprintf(text,sizeof text,"%016llx",(unsigned long long)n); return text;
}
// Bounded, duplicate-aware chunk receiver. No allocation from untrusted chunk counts above limit.
class Chunks {
 public:
  bool receive(const Json& d) {
    if (!number(d,"n",1,(stored_limit+kCardChunk-1)/kCardChunk) ||
        !number(d,"i",0,d["n"].get<size_t>()-1) || !d.count("data") || !d["data"].is_string() ||
        !d.count("sha256") || !d["sha256"].is_string() || !card_hash(d["sha256"].get<std::string>())) return false;
    const auto n = d["n"].get<size_t>(), index = d["i"].get<size_t>();
    const auto sha = d["sha256"].get<std::string>();
    if (chunks_.empty()) { chunks_.resize(n); sha_ = sha; }
    if (chunks_.size() != n || sha_ != sha) return false;
    Buffer b;
    if (!card_decode(d["data"].get<std::string>(),b) || b.empty() ||
        (index+1 < n && b.size() != kCardChunk)) return false;
    if (!chunks_[index].empty()) return chunks_[index] == b;
    if (total_+b.size() > stored_limit) return false;
    total_ += b.size(); chunks_[index] = std::move(b); ++received_; return true;
  }
  bool complete() const { return received_ && received_ == chunks_.size(); }
  std::shared_ptr<const Buffer> finish() const {
    if (!complete()) return {};
    auto b = std::make_shared<Buffer>(); b->reserve(total_);
    for (const auto& c : chunks_) b->insert(b->end(),c.begin(),c.end());
    return card_sha(*b) == sha_ ? b : nullptr;
  }
 private:
  std::vector<Buffer> chunks_;
  size_t total_ = 0, received_ = 0;
  std::string sha_;
};
// Rooms closes a socket (1008) that sends over 256 KiB/s with a 2 s burst (rooms/src/index.ts
// MAX_BYTES_PER_SECOND, BURST_SECONDS_BYTES). Chunk uploads keep to 3/4 of the rate and half the
// burst, leaving the rest for inputs and pings. Also at most one chunk per 12 ms (the 120/s cap).
struct RoomPacer {
  static constexpr uint64_t kBytesPerSecond = 192 * 1024, kBurstBytes = 256 * 1024, kGapMs = 12;
  uint64_t start = 0, sent = 0;
  static size_t wire(size_t raw) { return (raw+2)/3*4 + 256; } // base64 plus the JSON envelope
  // After a chunk of `raw` bytes went out at `now`: the clock time the next one may go.
  uint64_t after(uint64_t now, size_t raw) {
    if (!sent) start = now;
    sent += wire(raw);
    const uint64_t due = sent > kBurstBytes ? start + (sent-kBurstBytes)*1000/kBytesPerSecond : 0;
    return std::max(now+kGapMs,due);
  }
};

// Budget the actual base64/envelope bytes and per-message gap, plus capture/load/ACK margin.
// The stored limit bounds the arithmetic. Even a maximum-sized transfer stays under the cap.
inline uint64_t transfer_budget(size_t bytes) {
  const uint64_t n = (std::min(bytes,stored_limit)+kCardChunk-1)/kCardChunk;
  const uint64_t wire = n*RoomPacer::wire(kCardChunk);
  const uint64_t paced = wire > RoomPacer::kBurstBytes ?
      ((wire-RoomPacer::kBurstBytes)*1000+RoomPacer::kBytesPerSecond-1)/RoomPacer::kBytesPerSecond : 0;
  return std::min<uint64_t>(200000,10000+std::max(paced,n*RoomPacer::kGapMs));
}

// The game remains paused until transfer completion/cancellation, exit, or its size-based guard.
// Inject only the host services so offline tests execute the same loop with a virtual clock.
template<class Clock, class Exit, class Pump>
void wait_paused(Operation operation, const Result& result, const Channel& channel,
                 uint64_t began, Clock clock, Exit exiting, Pump pump) {
  while (result.reason.empty() && !channel.cancelled.load(std::memory_order_acquire) &&
         !exiting() && clock()-began <
             (operation == Operation::PauseDelta && result.bytes ? transfer_budget(result.bytes->size()) : 20000)) {
    pump();
  }
}

// Called after the SAME rendezvous/link/delay code used by ordinary console startup.
// This milestone deliberately never starts the Engine. Restore B before resuming its native stack.
template<class IO>
void transfer(IO& io, bool host, const std::string& peer, const SessionConfig& config,
              const std::shared_ptr<Channel>& channel) {
  struct Release { std::shared_ptr<Channel> c; ~Release() { c->cancelled.store(true,std::memory_order_release); } } release{channel};
  std::string failure;
  auto emit = [&](Json d) {
    d["v"] = 1; d["match"] = config.session_id;
    try { if (!io.send_room({{"t","msg"},{"to",peer},{"d",d}})) failure = "send-failed"; }
    catch (...) { failure = "send-failed"; }
    return failure.empty();
  };
  auto refuse = [&](const std::string& why) {
    io.log("handoff-join: refuse " + why);
    emit({{"kind","mu-hj-refuse"},{"reason",why}});
    try { if (io.flush_room && !io.room_closed()) io.flush_room(1000); } catch (...) {}
  };
  auto next = [&](Json& d,int wait) {
    if (io.cancelled()) { failure = "cancelled"; return false; }
    if (io.room_closed()) { failure = "room-closed"; return false; }
    Json m;
    if (!io.next_room(m,wait) || str(m,"t") != "msg" || str(m,"from") != peer ||
        !m.count("d") || !m["d"].is_object()) return false;
    d = m["d"];
    if (d.value("v",Json()) != 1 || str(d,"match") != config.session_id) return false;
    if (str(d,"kind") == "mu-hj-refuse") {
      failure = "peer-refused";
      for (const char* reason : {"start-at-N-unavailable","not-quiescent","at-css-timeout",
          "load-validation-failed","hash-mismatch","base-timeout","transfer-timeout","cancelled",
          "room-closed","link-setup-failed","send-failed","capture-failed","delta-capture-failed",
          "base-capture-failed","transfer-oversized","invalid-chunks","transfer-checksum","base-checksum"})
        if (d.value("reason",Json()) == reason) failure = reason;
      return false;
    }
    return true;
  };
  std::deque<Json> inbox;
  auto preserve = [&](Json d) {
    if (inbox.size() >= 128) failure = "protocol-error";
    else inbox.push_back(std::move(d));
  };
  auto take = [&](const char* kind,Json& d,int wait) {
    auto found = std::find_if(inbox.begin(),inbox.end(),[&](const Json& q) { return str(q,"kind") == kind; });
    if (found != inbox.end()) { d = std::move(*found); inbox.erase(found); return true; }
    if (!next(d,wait)) return false;
    if (str(d,"kind") == kind) return true;
    preserve(std::move(d)); return false;
  };
  unsigned serial = host ? 0 : 1; // guest CSS request is published immediately after acceptance
  auto request = [&](Operation op,std::shared_ptr<const Buffer> base = {},std::shared_ptr<const Buffer> delta = {}) {
    auto q = std::make_shared<Request>(); q->serial = ++serial; q->operation = op;
    q->seed = config.seed; q->base = std::move(base); q->delta = std::move(delta);
    if (op == Operation::Base) {
      const auto info = std::atomic_load_explicit(&published_info,std::memory_order_acquire);
      if (info && info->capture_bytes && info->capture_bytes <= 128*1024*1024)
        q->storage = std::make_shared<Buffer>(info->capture_bytes);
    }
    std::shared_ptr<const Request> immutable = std::move(q);
    std::atomic_store_explicit(&channel->request,std::move(immutable),std::memory_order_release);
  };
  auto result = [&](uint64_t until,const char* timeout) -> std::shared_ptr<const Result> {
    while (failure.empty() && io.clock() < until) {
      auto r = std::atomic_load_explicit(&channel->result,std::memory_order_acquire);
      if (r && r->serial == serial) { if (!r->reason.empty()) failure = r->reason; return failure.empty() ? r : nullptr; }
      // Quiescence has its own five-second budget. Once main pauses, capture/load belongs
      // to the ten-second preparation margin. The send budget is set once its size is known.
      const auto paused = channel->paused_at.load(std::memory_order_acquire);
      if (paused) { until = paused+10000; timeout = "transfer-timeout"; }
      if (io.clock() >= until) break;
      Json d; if (next(d,int(std::min<uint64_t>(10,until-io.clock())))) preserve(std::move(d));
    }
    if (failure.empty()) failure = timeout;
    return {};
  };
  auto send_bytes = [&](const char* kind,const Buffer& b,uint64_t until,uint64_t* work_ms = nullptr) {
    if (b.empty() || b.size() > stored_limit) { failure = "transfer-oversized"; return false; }
    const auto hashing_at = transport::now_ms();
    const auto sha = card_sha(b); const size_t n = (b.size()+kCardChunk-1)/kCardChunk;
    if (work_ms) *work_ms += transport::now_ms()-hashing_at;
    io.log(std::string("handoff-join: send ") + kind + " " + std::to_string(b.size()) + " bytes in " + std::to_string(n) + " chunks");
    io.log(std::string("handoff-join: deadline ")+kind+" "+std::to_string(until > io.clock() ? until-io.clock() : 0)+" ms for "+std::to_string(b.size())+" bytes");
    RoomPacer pacer;
    for (size_t k = 0; k < n && failure.empty(); ++k) {
      if (io.clock() >= until) { failure = "transfer-timeout"; break; }
      const size_t size = std::min(kCardChunk,b.size()-k*kCardChunk);
      const auto chunk_at = transport::now_ms();
      Json chunk{{"kind",kind},{"i",k},{"n",n},{"sha256",sha},{"data",card_encode(b.data()+k*kCardChunk,size)}};
      if (work_ms) *work_ms += transport::now_ms()-chunk_at;
      if (!emit(std::move(chunk))) break;
      const auto paced = std::min(until,pacer.after(io.clock(),size));
      while (failure.empty() && io.clock() < paced) { Json d; if (next(d,int(paced-io.clock()))) preserve(std::move(d)); }
    }
    if (failure.empty() && io.flush_room && (io.clock() >= until || !io.flush_room(int(until-io.clock())))) failure = "transfer-timeout";
    return failure.empty();
  };
  auto receive_bytes = [&](const char* kind,uint64_t until) -> std::shared_ptr<const Buffer> {
    Chunks chunks; bool budgeted = false;
    while (failure.empty() && io.clock() < until) {
      Json d; if (!take(kind,d,int(std::min<uint64_t>(10,until-io.clock())))) continue;
      if (!chunks.receive(d)) { failure = "invalid-chunks"; break; }
      if (!budgeted) {
        const size_t bound = std::min(stored_limit,d["n"].get<size_t>()*kCardChunk);
        const auto budget = transfer_budget(bound); until = io.clock()+budget; budgeted = true;
        io.log(std::string("handoff-join: deadline ")+kind+" "+std::to_string(budget)+" ms for <= "+std::to_string(bound)+" bytes");
      }
      if (chunks.complete()) {
        auto b = chunks.finish(); if (!b) failure = "transfer-checksum"; return b;
      }
    }
    if (failure.empty()) failure = "transfer-timeout";
    return {};
  };
  auto wait_kind = [&](const char* kind,uint64_t until,Json& got) {
    while (failure.empty() && io.clock() < until) {
      if (take(kind,got,int(std::min<uint64_t>(10,until-io.clock())))) return true;
    }
    if (failure.empty()) failure = "transfer-timeout";
    return false;
  };
  try {
    std::shared_ptr<const Buffer> base;
    const auto base_at = io.clock(); auto base_until = base_at+30000;
    if (host) {
      request(Operation::Base);
      const auto r = result(base_until,"base-timeout");
      if (r) {
        base = r->bytes; uint64_t work_ms = 0;
        if (r->finish_base) {
          Result encoded = *r; encoded.finish_base(encoded); base = encoded.bytes;
          if (!encoded.reason.empty()) failure = encoded.reason;
          work_ms = encoded.worker_ms;
        }
        base_until = io.clock()+transfer_budget(base ? base->size() : 0);
        if (!base) { if (failure.empty()) failure = "capture-failed"; }
        else if (send_bytes("mu-hj-base",*base,base_until,&work_ms)) {
          Json ack;
          if (wait_kind("mu-hj-base-ok",base_until,ack) && ack.value("sha256",Json()) != card_sha(*base)) failure = "base-checksum";
          io.log("handoff-join: base raw " + std::to_string(r->raw) + " stored " + std::to_string(base->size()) +
              " bytes ms " + std::to_string(io.clock()-base_at));
        }
        if (r->finish_base) io.log("handoff-join: base capture "+std::to_string(r->ms)+" ms on game thread, "+
            std::to_string(work_ms)+" ms on worker");
      }
      Json css;
      if (failure.empty() && wait_kind("mu-hj-at-css",io.clock()+15000,css)) {
        io.log("handoff-join: at-css");
        request(Operation::PauseDelta,base);
        const auto r = result(io.clock()+5000,"not-quiescent");
        if (r && r->bytes) {
          const auto paused = channel->paused_at.load(std::memory_order_acquire);
          const auto until = (paused ? paused : io.clock())+transfer_budget(r->bytes->size());
          if (send_bytes("mu-hj-delta",*r->bytes,until)) {
            Json loaded;
            if (wait_kind("mu-hj-loaded",until,loaded)) {
              if (loaded.value("frame",Json()) != r->frame || loaded.value("hash",Json()) != r->hash) failure = "hash-mismatch";
              else failure = "start-at-N-unavailable";
            }
          }
        } else if (failure.empty()) failure = "capture-failed";
      }
    } else {
      // Do not load here: only main-thread Load below can validate and commit spans.
      base = receive_bytes("mu-hj-base",base_until);
      if (base && failure.empty()) {
        emit({{"kind","mu-hj-base-ok"},{"sha256",card_sha(*base)}});
        uint64_t raw = 0;
        if (base->size() >= 84 && !std::memcmp(base->data(),"MUHS",4))
          for (unsigned j = 0; j < 8; ++j) raw |= uint64_t((*base)[76+j]) << (8*j);
        io.log("handoff-join: base raw " + std::to_string(raw) + " stored " + std::to_string(base->size()) +
            " bytes ms " + std::to_string(io.clock()-base_at));
        const auto css = result(io.clock()+15000,"at-css-timeout");
        if (css) {
          emit({{"kind","mu-hj-at-css"}}); io.log("handoff-join: at-css");
          // Allow quiescence/capture before the first chunk; its existing n then budgets the paced receive.
          const auto delta_at = io.clock();
          const auto delta = receive_bytes("mu-hj-delta",delta_at+15000);
          if (delta) {
            request(Operation::Load,base,delta);
            const auto r = result(io.clock()+5000,"not-quiescent");
            if (r) {
              io.log("handoff-join: delta pages " + std::to_string(r->pages) + " stored " + std::to_string(delta->size()) +
                  " bytes ms " + std::to_string(io.clock()-delta_at));
              emit({{"kind","mu-hj-loaded"},{"frame",r->frame},{"hash",r->hash}});
              Json stop; wait_kind("mu-hj-refuse",io.clock()+10000,stop);
            }
          }
        }
      }
    }
    refuse(failure.empty() ? "start-at-N-unavailable" : failure);
  } catch (...) { refuse("protocol-error"); }
}
} // namespace yougame::console::handoff_join
