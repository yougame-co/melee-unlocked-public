// Live Friends pre-copy protocol. The room thread only moves immutable buffers.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "friends_handoff_join.h"
#include "yougame_console_request.h"
#include "yougame_transport.h"
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
  std::shared_ptr<const Buffer> base, delta;
};
struct Result {
  unsigned serial = 0;
  std::string reason, hash;
  uint64_t frame = 0, raw = 0, ms = 0;
  size_t pages = 0;
  std::shared_ptr<const Buffer> bytes;
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
    std::shared_ptr<const Request> immutable = std::move(q);
    std::atomic_store_explicit(&channel->request,std::move(immutable),std::memory_order_release);
  };
  auto result = [&](uint64_t until,const char* timeout) -> std::shared_ptr<const Result> {
    while (failure.empty() && io.clock() < until) {
      auto r = std::atomic_load_explicit(&channel->result,std::memory_order_acquire);
      if (r && r->serial == serial) { if (!r->reason.empty()) failure = r->reason; return failure.empty() ? r : nullptr; }
      // Quiescence has its own five-second budget. Once main pauses, capture/load belongs
      // to the ten-second delta budget, measured from that actual body boundary.
      const auto paused = channel->paused_at.load(std::memory_order_acquire);
      if (paused) { until = paused+10000; timeout = "transfer-timeout"; }
      if (io.clock() >= until) break;
      Json d; if (next(d,int(std::min<uint64_t>(10,until-io.clock())))) preserve(std::move(d));
    }
    if (failure.empty()) failure = timeout;
    return {};
  };
  auto send_bytes = [&](const char* kind,const Buffer& b,uint64_t until) {
    if (b.empty() || b.size() > stored_limit) { failure = "transfer-oversized"; return false; }
    const auto sha = card_sha(b); const size_t n = (b.size()+kCardChunk-1)/kCardChunk;
    for (size_t k = 0; k < n && failure.empty(); ++k) {
      if (io.clock() >= until) { failure = "transfer-timeout"; break; }
      if (!emit({{"kind",kind},{"i",k},{"n",n},{"sha256",sha},
          {"data",card_encode(b.data()+k*kCardChunk,std::min(kCardChunk,b.size()-k*kCardChunk))}})) break;
      const auto paced = std::min(until,io.clock()+12);
      while (failure.empty() && io.clock() < paced) { Json d; if (next(d,int(paced-io.clock()))) preserve(std::move(d)); }
    }
    if (failure.empty() && io.flush_room && (io.clock() >= until || !io.flush_room(int(until-io.clock())))) failure = "transfer-timeout";
    return failure.empty();
  };
  auto receive_bytes = [&](const char* kind,uint64_t until) -> std::shared_ptr<const Buffer> {
    Chunks chunks;
    while (failure.empty() && io.clock() < until) {
      Json d; if (!take(kind,d,int(std::min<uint64_t>(10,until-io.clock())))) continue;
      if (!chunks.receive(d)) { failure = "invalid-chunks"; break; }
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
    const auto base_at = io.clock(), base_until = base_at+30000;
    if (host) {
      request(Operation::Base);
      const auto r = result(base_until,"base-timeout");
      if (r) {
        base = r->bytes;
        if (!base) failure = "capture-failed";
        else if (send_bytes("mu-hj-base",*base,base_until)) {
          Json ack;
          if (wait_kind("mu-hj-base-ok",base_until,ack) && ack.value("sha256",Json()) != card_sha(*base)) failure = "base-checksum";
          io.log("handoff-join: base raw " + std::to_string(r->raw) + " stored " + std::to_string(base->size()) +
              " bytes ms " + std::to_string(io.clock()-base_at));
        }
      }
      Json css;
      if (failure.empty() && wait_kind("mu-hj-at-css",io.clock()+15000,css)) {
        io.log("handoff-join: at-css");
        request(Operation::PauseDelta,base);
        const auto r = result(io.clock()+5000,"not-quiescent");
        if (r && r->bytes) {
          const auto paused = channel->paused_at.load(std::memory_order_acquire);
          const auto until = paused ? paused+10000 : io.clock()+10000;
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
          // The peer may spend five seconds becoming quiescent before its ten-second pause budget.
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
