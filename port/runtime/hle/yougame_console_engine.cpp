// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_console_engine.h"
#include "yougame_console_notice.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace yougame::console {
namespace {
Engine global_engine;
std::atomic<bool> global_active{false};
uint64_t monotonic_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
MuPadStatus disconnected() { MuPadStatus p{}; p.err = -1; return p; }
MuPadStatus first(const Pads& hardware) {
  for (const auto& p : hardware) if (p.err == 0) return p;
  return disconnected();
}
std::string hex(uint64_t v) {
  std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(16) << v; return s.str();
}
}
Engine& console_engine() { return global_engine; }
bool console_active() { return global_active.load(std::memory_order_acquire); }
bool prepare_card(const std::filesystem::path& data, const SessionConfig& config,
                  std::filesystem::path& card) {
  if (data.empty() || !Engine::valid(config)) return false;
  std::error_code ec;
  if (!std::filesystem::is_directory(data, ec) || ec) return false;
  auto path = data;
  for (const auto& component : {std::string("console"), config.session_id, std::string("card")}) {
    path /= component;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return false;
    ec.clear();
    if (std::filesystem::is_symlink(status)) return false;
    if (!std::filesystem::exists(status)) std::filesystem::create_directory(path, ec);
    if (ec || !std::filesystem::is_directory(path, ec) || ec) return false;
  }
  // Never erase/reuse a save, including an earlier launch with the same start id.
  if (!std::filesystem::is_empty(path, ec) || ec) return false;
  card = path; return true;
}
bool Engine::valid(const SessionConfig& c) {
  if (c.session_id.empty() || c.session_id.size() > 128 || c.session_id == "." || c.session_id == ".." ||
      !std::all_of(c.session_id.begin(), c.session_id.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
      }) || c.input_delay < 0 || c.input_delay > 120 || c.epoch_ms < 946684800000LL ||
      c.epoch_ms > 4102444800000LL || c.ports.size() < 2 || c.ports.size() > 4) return false;
  int previous = 0;
  for (int p : c.ports) { if (p <= previous || p > 4) return false; previous = p; }
  return std::find(c.ports.begin(), c.ports.end(), c.my_port) != c.ports.end();
}
void Engine::log(const std::string& text) const { if (hooks_.log) hooks_.log(text); }
bool Engine::begin(std::unique_ptr<SessionLink> link, const SessionConfig& c, EngineHooks hooks, bool rollback) {
  if (active_ || started_ || !link || !valid(c)) return false;
  config_ = c; hooks_ = std::move(hooks); link_ = std::move(link);
  if (!hooks_.now_ms) hooks_.now_ms = monotonic_ms;
  active_ = true; ended_ = false; frame_ = 0;
  ticks_ = 0; next_retrace_ = frame_ticks; retraces_ = 0;
  if (this == &global_engine) global_active.store(true, std::memory_order_release);
  enable_rollback(rollback);
  log(rollback_enabled_ ? "console: rollback on" : "console: rollback off (lockstep)");
  pads_.fill(disconnected());
  for (int p : c.ports) pads_[p - 1] = MuPadStatus{};
  // Everybody sends the initial neutral window. It is confirmed like every later frame.
  for (int f = 0; f < c.input_delay; ++f) {
    local_[f] = MuPadStatus{}; link_->send_pad(f, MuPadStatus{});
  }
  log("console: session " + c.session_id + " port " + std::to_string(c.my_port) + " of " +
      std::to_string(c.ports.size()) + " delay " + std::to_string(c.input_delay));
  return true;
}
void Engine::end(const std::string& reason, bool desync) {
  if (!active_) return;
  active_ = false; ended_ = true;
  if (this == &global_engine) global_active.store(false, std::memory_order_release);
  match_ = body_clock_ = false;
  clocks_.clear(); candidates_.clear();
  pads_.fill(disconnected()); local_.clear(); checks_.clear();
  for (auto& history : remote_) history.clear();
  log("console: session ended: " + reason);
  const auto message = session_notice(reason);
  if (hooks_.notice) hooks_.notice(message ? message : desync ? "Your game and your friend's game went out of sync." :
                                         "Lost connection to your friend.");
  link_.reset();
}
bool Engine::compare_digests() {
  for (auto it = checks_.begin(); it != checks_.end();) {
    for (int p : config_.ports) {
      const unsigned bit = 1u << (p - 1);
      if (!(it->second.pending & bit)) continue;
      const auto other = link_->digest(p, it->first);
      if (!other) continue;
      if (*other != it->second.value) {
        if (it->first == 0) log("console: rollback setting differs between seats");
        log("console: desync at frame " + std::to_string(it->first) + " with port " +
            std::to_string(p) + " (mine " + hex(it->second.value) + ", theirs " + hex(*other) + ")");
        end("desync", true); return false;
      }
      it->second.pending &= ~bit;
    }
    if (!it->second.pending) it = checks_.erase(it);
    else if (hooks_.now_ms() - it->second.since >= 8000) {
      end("state digest timed out"); return false;
    } else ++it;
  }
  return true;
}
bool Engine::advance(const Pads& hardware, const MuStatePod& state) {
  if (!active_) return false;
  if (started_) ++frame_;
  started_ = true;
  if (!frame_) log("console: frame 0");
  const auto sample = first(hardware);
  const uint32_t target = frame_ + config_.input_delay;
  // Issuance survives loads and corrected scene boundaries. A sampled target is immutable.
  if (local_.emplace(target, sample).second) link_->send_pad(target, sample);
  if (frame_ % 60 == 0 && !match_) {
    candidate(frame_, state);
    // Lockstep state precedes this body's input wait, just as before rollback was enabled.
    if (!publish(frame_)) return false;
  }
  if (!frame_ && link_->frame_zero_barrier) {
    const auto barrier_at = hooks_.now_ms();
    log("console: waiting for every peer at frame 0 (60 s)");
    for (;;) {
      link_->pump(0);
      if (auto lost = link_->lost()) { end(*lost); return false; }
      bool ready = true;
      for (int p : config_.ports) if (p != config_.my_port && !link_->digest(p,0)) ready = false;
      if (ready) break;
      const auto elapsed = hooks_.now_ms() - barrier_at;
      if (elapsed >= 60000) { end(kBootTimeout); return false; }
      if (hooks_.responsive) hooks_.responsive();
      if (!active_) return false;
      link_->pump(static_cast<int>(std::min<uint64_t>(5,60000-elapsed)));
    }
    if (auto check = checks_.find(0); check != checks_.end()) check->second.since = hooks_.now_ms();
    log("console: frame 0 barrier passed");
  }
  const uint64_t began = hooks_.now_ms();
  if (!window_began_) window_began_ = began;
  int missing = 0;
  for (;;) {
    link_->pump(0);
    if (auto lost = link_->lost()) { end(*lost); return false; }
    if (!compare_digests()) return false;
    receive(frame_);
    bool ready = true;
    for (int p : config_.ports) if (p != config_.my_port && confirmed_[p - 1] < frame_) {
      missing = p;
      // b=0 is confirmed. At most seven bodies can depend on an unresolved input;
      // the next admission waits before capture could evict the oldest required checkpoint.
      if (!match_ || frame_ == match_origin_ || int64_t(frame_) - confirmed_[p - 1] > 7)
        ready = false;
    }
    const auto waited = hooks_.now_ms() - began;
    if (ready) {
      if (!compose(frame_, match_)) { end("required input history missing"); return false; }
      admitted_through_ = std::max(admitted_through_, frame_);
      retain_history();
      waited_ += waited; max_wait_ = std::max(max_wait_, waited);
      if (waited > 17) { ++long_waits_; long_port_ = missing; }
      if (frame_ && frame_ % 600 == 0) {
        const auto now = hooks_.now_ms(), wall = now > window_began_ ? now - window_began_ : 0;
        // Waiting is also where a frame idles until the peer's pad is due; only waits longer than a
        // frame, or a wall time well over 10 s per 600 frames, mean the session is running slow.
        log("console: frame " + std::to_string(frame_) + " waited " + std::to_string(waited_) +
            " ms max " + std::to_string(max_wait_) + " ms; 600 frames in " + std::to_string(wall) + " ms (" +
            std::to_string(wall ? 600000 / wall : 0) + " fps); " + std::to_string(long_waits_) +
            " waits over a frame" + (long_waits_ ? ", last on port " + std::to_string(long_port_) : std::string()));
        waited_ = max_wait_ = long_waits_ = 0; long_port_ = 0; window_began_ = now;
      }
      return true;
    }
    if (waited >= 8000) { end("pad wait timed out at frame " + std::to_string(frame_)); return false; }
    if (hooks_.responsive) hooks_.responsive();
    if (!active_) return false;
    const auto elapsed = hooks_.now_ms() - began;
    if (elapsed >= 8000) { end("pad wait timed out at frame " + std::to_string(frame_)); return false; }
    link_->pump(static_cast<int>(std::min<uint64_t>(5, 8000 - elapsed)));
  }
}
void Engine::receive(uint32_t through) {
  for (int p : config_.ports) {
    if (p == config_.my_port) continue;
    auto& confirmed = confirmed_[p - 1];
    auto& history = remote_[p - 1];
    while (confirmed < through) {
      const auto next = uint32_t(confirmed + 1);
      auto pad = link_->pad(p, next);
      if (!pad) break; // A future packet cannot confirm a hole, even if the transport ACKed it.
      history.emplace(next, *pad);
      ++confirmed;
    }
  }
}
bool Engine::compose(uint32_t frame, bool predict) {
  Pads composed; composed.fill(disconnected());
  auto local = local_.find(frame);
  if (local == local_.end()) return false;
  composed[config_.my_port - 1] = local->second;
  for (int p : config_.ports) if (p != config_.my_port) {
    const auto& history = remote_[p - 1];
    auto it = history.upper_bound(frame);
    if (it == history.begin()) return false;
    --it;
    if (!predict && it->first != frame) return false;
    composed[p - 1] = it->second;
  }
  pads_ = composed;
  return true;
}
void Engine::retain_history() {
  // More than the rollback horizon and the largest delay, while remaining within link retention.
  const auto oldest = frame_ > 1024 ? frame_ - 1024 : 0;
  local_.erase(local_.begin(), local_.lower_bound(oldest));
  for (auto& history : remote_) history.erase(history.begin(), history.lower_bound(oldest));
}
bool Engine::begin_match() {
  if (!rollback_enabled() || match_) return false;
  match_ = true;
  match_origin_ = started_ ? frame_ + 1 : 0;
  validated_boundary_ = match_origin_;
  ++match_epoch_;
  clocks_.clear(); body_clock_ = false;
  log("console: rollback match epoch " + std::to_string(match_epoch_) + " at session frame " +
      std::to_string(match_origin_));
  return true;
}
void Engine::end_match() {
  match_ = body_clock_ = false;
  clocks_.clear(); candidates_.clear();
}
int64_t Engine::confirmed_through() const {
  int64_t result = admitted_through_;
  for (int p : config_.ports) if (p != config_.my_port) result = std::min(result, confirmed_[p - 1]);
  return result;
}
void Engine::candidate(uint32_t boundary, const MuStatePod& state) {
  if (int64_t(boundary) <= published_through_) return;
  auto digest = state_digest(state);
  // Session compatibility tag: frame zero only; OFF keeps every legacy byte.
  if (!boundary && rollback_enabled_) {
    constexpr uint32_t marker = 0x52424b31; // rollback compatibility version 1, canonical FNV word
    for (unsigned shift = 0; shift < 32; shift += 8) {
      digest ^= (marker >> shift) & 255; digest *= 1099511628211ull;
    }
  }
  candidates_[boundary] = digest;
}
bool Engine::publish(uint32_t corrected_boundary) {
  for (auto it = candidates_.begin(); it != candidates_.end() && it->first <= corrected_boundary;) {
    // Boundary S contains bodies through S-1. Receiving alone never finalizes a candidate:
    // the caller must have compared predictions and rebuilt the corrected timeline first.
    if (match_ && it->first > validated_boundary_) break;
    unsigned pending = 0;
    for (int p : config_.ports) if (p != config_.my_port) pending |= 1u << (p - 1);
    // Testers compare these lines across seats for a short desync check without a full match.
    log("console: hash " + std::to_string(it->first) + " " + hex(it->second));
    checks_[it->first] = {it->second, pending, hooks_.now_ms()};
    link_->send_digest(it->first, it->second);
    published_through_ = it->first;
    it = candidates_.erase(it);
  }
  return compare_digests();
}
bool Engine::validated(uint32_t boundary) {
  if (!match_active() || boundary > UINT32_MAX - match_origin_ || body_clock_) return false;
  const auto session = match_origin_ + boundary;
  // At most the completed current body, and never beyond received causal inputs.
  if (uint64_t(session) > uint64_t(frame_) + 1 || int64_t(session) > confirmed_through() + 1)
    return false;
  validated_boundary_ = std::max(validated_boundary_, session);
  return true;
}
bool Engine::boundary(uint32_t body, const MuStatePod& state) {
  if (!match_active() || frame_ != match_origin_ + body || body_clock_) return false;
  if (!body || frame_ % 60 == 0) candidate(frame_, state);
  if (!publish(frame_)) return false;
  body_start_ = {ticks_, next_retrace_, retraces_};
  body_clock_ = true;
  return true;
}
bool Engine::finish_body(uint32_t body) {
  if (!body_active() || frame_ != match_origin_ + body) return false;
  // One logical VI period per completed body, regardless of presentation or catch-up waits.
  ticks_ += frame_ticks;
  ++retraces_;
  next_retrace_ = ticks_ + frame_ticks;
  body_clock_ = false;
  return true;
}
void Engine::capture_clock(uint32_t body) {
  clocks_[body] = body_start_; // the checkpoint resumes at historical pad injection
  while (clocks_.size() > 7) clocks_.erase(clocks_.begin());
}
bool Engine::restore_clock(uint32_t body) {
  const auto it = clocks_.find(body);
  if (!match_active() || it == clocks_.end()) return false;
  ticks_ = it->second.ticks; next_retrace_ = it->second.next_retrace; retraces_ = it->second.retraces;
  body_clock_ = false;
  clocks_.clear(); // native load frees the timeline; replay rebuilds it
  validated_boundary_ = std::min(validated_boundary_, match_origin_ + body);
  candidates_.erase(candidates_.lower_bound(match_origin_ + body), candidates_.end());
  ++rollback_stats_.loads;
  return true;
}
bool Engine::confirm(uint32_t body) {
  if (!match_active() || frame_ != match_origin_ + body || body_clock_) return false;
  const auto began = hooks_.now_ms();
  for (;;) {
    link_->pump(0);
    if (auto lost = link_->lost()) { end(*lost); return false; }
    if (!compare_digests()) return false;
    receive(frame_);
    if (confirmed_through() >= frame_) return true;
    if (hooks_.now_ms() - began >= 8000) { end("terminal input wait timed out"); return false; }
    if (hooks_.responsive) hooks_.responsive();
    if (!active_) return false;
    const auto elapsed = hooks_.now_ms() - began;
    if (elapsed >= 8000) { end("terminal input wait timed out"); return false; }
    link_->pump(static_cast<int>(std::min<uint64_t>(5, 8000 - elapsed)));
  }
}
bool Engine::commit_match(uint32_t body, const MuStatePod& state) {
  if (!match_active() || body_clock_ || frame_ != match_origin_ + body || confirmed_through() < frame_)
    return false;
  const auto exit = frame_ + 1;
  if (validated_boundary_ < exit) return false;
  candidates_.erase(candidates_.upper_bound(exit), candidates_.end());
  candidate(exit, state);
  if (!publish(exit)) return false;
  ++rollback_stats_.commits;
  end_match();
  return true;
}
bool Engine::inputs(uint32_t body, bool previous, MuConsoleInputs& out) {
  if (!match_active() || body > UINT32_MAX - match_origin_) return false;
  const auto session = match_origin_ + body;
  receive(session);
  auto local = local_.find(session);
  if (local == local_.end()) { end("required local input history missing"); return false; }
  out = {};
  out.epoch = match_epoch_; out.local_port = config_.my_port - 1; out.local = local->second;
  const int64_t cap = int64_t(session) - (previous && body != 0 ? 1 : 0);
  int r = 0;
  for (int p = 1; p <= kMaxPorts; ++p) {
    if (p == config_.my_port) continue;
    const bool owned = std::find(config_.ports.begin(), config_.ports.end(), p) != config_.ports.end();
    const auto latest = owned ? std::min(confirmed_[p - 1], cap) : cap;
    out.latest[r] = int32_t(latest - match_origin_);
    for (int k = 0; k < 7; ++k) {
      auto& pad = out.remote[r][k];
      pad = disconnected();
      if (owned && latest - k >= int64_t(match_origin_)) {
        const auto it = remote_[p - 1].find(uint32_t(latest - k));
        if (it == remote_[p - 1].end()) { end("required remote input history missing"); return false; }
        pad = it->second;
      }
    }
    ++r;
  }
  return true;
}
bool Engine::replay(uint32_t body) {
  if (!match_active() || !started_ || body > UINT32_MAX - match_origin_) return false;
  const auto session = match_origin_ + body;
  // Native rollback never replays beyond the issued frontier. Keep the cursor rewindable;
  // immutable samples above it remain in local_ for the next forward/lockstep bodies.
  if (session > admitted_through_ || !local_.count(session) || !compose(session, true)) {
    end("required replay input history missing"); return false;
  }
  frame_ = session;
  ++rollback_stats_.replayed;
  return true;
}
void Engine::read(const Pads& hardware, Pads& out) const {
  if (active_) out = pads_;
  else if (ended_) {
    out.fill(disconnected()); out[config_.my_port - 1] = first(hardware);
  } else std::memcpy(out.data(), hardware.data(), sizeof(Pads));
}
uint64_t Engine::ticks(uint64_t ordinary) {
  if (!active_) return ordinary;
  if (!rollback_enabled_) poll();
  else if (servicing()) service_ticks_ += poll_ticks;
  else if (!match_ || body_clock_) ticks_ += poll_ticks;
  return servicing() ? service_ticks_ : ticks_;
}
void Engine::poll() {
  if (!active_) return;
  link_->pump(0); // Loading/OS waits service links without advancing a body.
  if (auto lost = link_->lost()) { end(*lost); return; }
  if (!rollback_enabled_) ticks_ += poll_ticks;
  else {
    if (!servicing() && (!match_ || body_clock_)) ticks_ += poll_ticks;
    if (servicing() || !body_clock_) service_ticks_ += poll_ticks;
  }
}
void Engine::retrace() {
  if (!active_) return;
  logical_retrace();
  if (!body_clock_) service_retrace();
}
void Engine::logical_retrace() {
  if (!active_) return;
  if (!rollback_enabled_ || (!servicing() && (!match_ || body_clock_))) {
    ticks_ += frame_ticks;
    ++retraces_;
    next_retrace_ = ticks_ + frame_ticks;
  }
}
void Engine::service_retrace() {
  service_ticks_ += frame_ticks;
  ++service_retraces_;
  service_next_retrace_ = service_ticks_ + frame_ticks;
}
uint64_t Engine::boot_ticks(uint64_t ordinary) const {
  return active_ ? uint64_t(config_.epoch_ms - 946684800000LL) * (MU_TB_HZ / 1000) : ordinary;
}
uint64_t Engine::state_digest(const MuStatePod& state) {
  static_assert(offsetof(MuStatePod, console_valid) == 80 * sizeof(uint32_t), "legacy digest prefix");
  // Canonical little endian words; never hash struct padding, pointers, or native byte order.
  uint64_t hash = 14695981039346656037ull;
  const auto* bytes = reinterpret_cast<const unsigned char*>(&state);
  const size_t size = state.console_valid ? sizeof(state) : offsetof(MuStatePod, console_valid);
  for (size_t i = 0; i < size; i += 4) {
    uint32_t word; std::memcpy(&word, bytes + i, 4);
    for (unsigned shift = 0; shift < 32; shift += 8) { hash ^= (word >> shift) & 255; hash *= 1099511628211ull; }
  }
  return hash;
}
} // namespace yougame::console
