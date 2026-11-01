// Friends live-join agreement. Incomplete live continuations return to the relaunch path.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_console_card.h"
#include <functional>
#include <memory>

namespace yougame::console {
#ifdef YOUGAME_AGENT_TEST_HOOK
inline std::atomic<bool> friends_handoff_join_test{false};
#endif
inline bool friends_handoff_join_enabled() {
#if defined(YOUGAME_FRIENDS_HANDOFF_DEFAULT) && YOUGAME_FRIENDS_HANDOFF_DEFAULT
  return true;
#elif defined(YOUGAME_AGENT_TEST_HOOK)
  return friends_handoff_join_test.load(std::memory_order_acquire);
#else
  return false;
#endif
}
namespace handoff_join {
using Json = nlohmann::json;
struct Info {
  uint64_t library_size = 0;
  std::string library_sha256, spans_digest, os;
  bool valid() const {
    return library_size && card_hash(library_sha256) && card_hash(spans_digest) &&
        (os == "windows" || os == "linux" || os == "macos");
  }
};
// Only the main game thread publishes metadata. The entry thread never reads game memory.
inline std::shared_ptr<const Info> published_info;
inline std::atomic<bool> host_css{false};

class Agreement {
 public:
  using Send = std::function<bool(const Json&)>;
  using Log = std::function<void(const std::string&)>;
  Agreement(bool host, bool enabled, Info info, std::string match, std::string peer,
            Send send, Log log)
      : host_(host), enabled_(enabled), info_(std::move(info)), match_(std::move(match)),
        peer_(std::move(peer)), send_(std::move(send)), log_(std::move(log)) {}
  void expect(uint64_t now) {
    if (!done_ && !waiting_) { waiting_ = true; until_ = now + 10000; }
  }
  void begin(uint64_t now, bool css, bool shared_card) {
    if (!host_ || !enabled_ || done_) return;
    expect(now);
    if (!css) { refuse("host-not-vs-css"); return; }
    if (!shared_card) { refuse("shared-card-required"); return; }
    if (!info_.valid()) { refuse("metadata-unavailable"); return; }
    offered_ = identity("mu-hj-offer");
    offered_["sceneMajor"] = 2; offered_["sceneMinor"] = 0;
    if (emit(offered_)) log_("handoff-join: offer sent");
  }
  void receive(const Json& m, uint64_t now) {
    if (done_ || !m.is_object() || text(m,"t") != "msg" || text(m,"from") != peer_ ||
        !m.count("d") || !m["d"].is_object()) return;
    const auto& d = m["d"];
    if (d.value("v",Json()) != 1 || text(d,"match") != match_) return;
    const auto kind = text(d,"kind");
    if (kind != "mu-hj-offer" && kind != "mu-hj-accept" && kind != "mu-hj-refuse") return;
    if (waiting_ && now >= until_) { refuse("timeout"); return; }
    if (kind == "mu-hj-refuse") {
      // Peer text never reaches logs. Use the bounded protocol reason vocabulary.
      const auto reason = text(d,"reason");
      finish(known_reason(reason) ? reason : "peer-refused"); return;
    }
    if (kind == "mu-hj-offer" && !host_) {
      log_("handoff-join: offer received");
      expect(now);
      if (!enabled_) { refuse("switch-off"); return; }
      if (!info_.valid()) { refuse("metadata-unavailable"); return; }
      if (d.value("sceneMajor",Json()) != 2 || d.value("sceneMinor",Json()) != 0) {
        refuse("host-not-vs-css"); return;
      }
      const auto why = mismatch(d);
      if (!why.empty()) { refuse(why); return; }
      if (!offered_.is_null() && offered_ != d) { refuse("conflicting-offer"); return; }
      if (!offered_.is_null()) return;
      offered_ = d;
      if (emit(identity("mu-hj-accept"))) { accepted_ = true; log_("handoff-join: accept sent"); }
    } else if (kind == "mu-hj-accept" && host_ && !offered_.is_null()) {
      const auto why = mismatch(d);
      if (!why.empty()) { refuse(why); return; }
      log_("handoff-join: accept received");
      accepted_ = true;
    }
  }
  bool accepted() const { return accepted_ && !done_; }
  bool waiting() const { return waiting_ && !done_ && !accepted_; }
  bool engaged() const { return waiting_ || done_; }
  uint64_t remaining(uint64_t now) const { return waiting() && now < until_ ? until_-now : 0; }
  void tick(uint64_t now) { if (waiting() && now >= until_) refuse("timeout"); }
  void refuse(const std::string& reason) {
    if (done_) return;
    done_ = true; waiting_ = false;
    emit({{"kind","mu-hj-refuse"},{"v",1},{"match",match_},{"reason",reason}});
    log_("handoff-join: refuse " + reason);
  }
 private:
  static std::string text(const Json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
  }
  static bool known_reason(const std::string& s) {
    for (const char* reason : {"switch-off", "metadata-unavailable", "host-not-vs-css",
        "shared-card-required", "wrong-library", "wrong-spans", "wrong-os", "timeout",
        "send-failed", "cancelled", "room-closed", "conflicting-offer", "protocol-error",
        "start-at-N-unavailable", "requires-one-guest", "peer-not-offering"}) if (s == reason) return true;
    return false;
  }
  Json identity(const char* kind) const {
    return {{"kind",kind},{"v",1},{"match",match_},{"librarySize",info_.library_size},
        {"librarySha256",info_.library_sha256},{"spansDigest",info_.spans_digest},{"os",info_.os}};
  }
  std::string mismatch(const Json& d) const {
    if (d.value("librarySize",Json()) != info_.library_size ||
        text(d,"librarySha256") != info_.library_sha256) return "wrong-library";
    if (text(d,"spansDigest") != info_.spans_digest) return "wrong-spans";
    if (text(d,"os") != info_.os) return "wrong-os";
    return {};
  }
  bool emit(const Json& d) {
    bool sent = false;
    try { sent = send_({{"t","msg"},{"to",peer_},{"d",d}}); } catch (...) {}
    if (!sent && !done_) finish("send-failed");
    return sent;
  }
  void finish(const std::string& reason) {
    done_ = true; waiting_ = false;
    log_("handoff-join: refuse " + reason);
  }
  bool host_, enabled_, waiting_ = false, done_ = false, accepted_ = false;
  Info info_;
  std::string match_, peer_;
  Send send_;
  Log log_;
  uint64_t until_ = 0;
  Json offered_;
};
} // namespace handoff_join
} // namespace yougame::console
