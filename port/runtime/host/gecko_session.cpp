// Gecko codes in a network match: one connection's offer, answer and lock (gecko_session.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gecko_session.h"
#include "host.h"
#include "user_gecko.h"

#include <algorithm>
#include <map>

namespace gecko_session {
namespace {

struct State {
  bool consent = false;
  std::vector<gecko_online::Offer> mine, theirs;
  bool theirs_received = false, i_answered = false, they_answered = false;
  std::vector<std::string> i_accepted, they_accepted;
  std::vector<std::vector<uint8_t>> outgoing;
  std::string agreed_fp = "none";
  std::vector<gecko_online::Lines> agreed;
  std::string sent_lock = "none", peer_lock = "none";
  std::vector<gecko_online::Lines> sent_set;
  bool in_match = false;
  std::vector<gecko_online::Lines> running;
  bool resend = false;
};
State g;

void settle() {
  if (!g.theirs_received || !g.i_answered || !g.they_answered) return;
  std::vector<std::string> mine_ids, theirs_ids;
  for (const auto& o : g.mine) mine_ids.push_back(o.id);
  for (const auto& o : g.theirs) theirs_ids.push_back(o.id);
  const std::vector<std::string> ids = gecko_online::agreed(mine_ids, g.they_accepted, theirs_ids, g.i_accepted);
  std::map<std::string, gecko_online::Lines> by_id;
  for (const auto& o : g.mine) by_id[o.id] = o.lines;
  for (const auto& o : g.theirs) by_id[o.id] = o.lines;
  g.agreed.clear();
  for (const auto& id : ids) g.agreed.push_back(by_id[id]);
  g.agreed_fp = gecko_online::set_fingerprint(ids);
  g.resend = true;
  host::log("gecko online: %zu codes agreed (%s)", ids.size(), g.agreed_fp.c_str());
}

}  // namespace

// Off until two games have played agreed codes through matches side by side (rollback included)
// without a desync: until then no offer is sent, no lock either, and matches run no codes.
constexpr bool kOnlineCodes = false;

void connected(bool consent) {
  g = State{};
  g.consent = consent && kOnlineCodes;
  if (!g.consent) return;   // switched off: no offer goes out
  for (const auto& o : user_gecko::offerable()) {
    if (g.mine.size() >= gecko_online::kMaxCodes) break;
    gecko_online::Offer offer;
    offer.sent_name = o.name;
    offer.lines.assign(o.lines.begin(), o.lines.begin() + (std::ptrdiff_t)std::min(o.lines.size(), gecko_online::kMaxLines));
    offer.id = gecko_online::code_id(offer.lines);
    g.mine.push_back(std::move(offer));
  }
  g.outgoing.push_back(gecko_online::encode_offer(g.mine));
  host::log("gecko online: offering %zu codes", g.mine.size());
}

void disconnected() { g = State{}; }
bool consenting() { return g.consent; }

std::vector<std::vector<uint8_t>> take_outgoing() {
  std::vector<std::vector<uint8_t>> out;
  out.swap(g.outgoing);
  return out;
}

void receive(const uint8_t* data, size_t size) {
  if (!g.consent) return;
  gecko_online::Message m;
  if (!gecko_online::decode(data, size, &m)) { host::log("gecko online: a malformed message was ignored"); return; }
  if (m.kind == 0 && !g.theirs_received) {
    g.theirs = std::move(m.offer);
    g.theirs_received = true;
    host::log("gecko online: the opponent offers %zu codes", g.theirs.size());
    if (g.theirs.empty()) answer({});   // nothing to ask
  } else if (m.kind == 1 && !g.they_answered) {
    // Only ids this side offered count.
    for (const auto& id : m.ids)
      if (std::any_of(g.mine.begin(), g.mine.end(), [&](const gecko_online::Offer& o) { return o.id == id; })) g.they_accepted.push_back(id);
    g.they_answered = true;
    host::log("gecko online: the opponent accepted %zu of this side's codes", g.they_accepted.size());
    settle();
  } else if (m.kind == 3) {
    g.peer_lock = m.fingerprint;
  }
}

bool take_resend() { const bool r = g.resend; g.resend = false; return r; }

std::vector<uint8_t> lock_message() {
  if (!g.consent) return {};
  g.sent_lock = g.agreed_fp;
  g.sent_set = g.agreed;
  return gecko_online::encode_lock(g.sent_lock);
}

bool asking() { return g.consent && g.theirs_received && !g.i_answered && !g.theirs.empty(); }

std::vector<Incoming> incoming() {
  std::vector<Incoming> out;
  for (const auto& o : g.theirs) {
    Incoming in;
    in.id = o.id;
    if (const gecko_online::Known* k = gecko_online::known(o.lines)) {
      in.known = true;
      in.name = k->name;
      in.what = k->what;
      in.kind = gecko_online::kind_label(k->kind);
    } else {
      in.name = o.sent_name.empty() ? std::string("Unnamed code") : o.sent_name;
      in.what = std::to_string(o.lines.size()) + " lines; not a code this game knows, so its name is the opponent's own";
      in.kind = gecko_online::kind_label(gecko_online::Kind::Unknown);
    }
    in.runnable = user_gecko::can_run(o.lines, &in.why_not);
    out.push_back(std::move(in));
  }
  return out;
}

void answer(const std::vector<std::string>& accepted_ids) {
  if (!g.consent || g.i_answered) return;
  g.i_accepted.clear();
  for (const auto& id : accepted_ids)
    for (const auto& o : g.theirs)
      if (o.id == id && user_gecko::can_run(o.lines, nullptr)) g.i_accepted.push_back(id);
  g.i_answered = true;
  g.outgoing.push_back(gecko_online::encode_answer(g.i_accepted));
  host::log("gecko online: accepted %zu of the opponent's codes", g.i_accepted.size());
  settle();
}

std::string status() {
  if (!g.consent) return {};
  if (g.in_match) return g.running.empty() ? std::string() : "Gecko codes this match: " + std::to_string(g.running.size());
  if (asking()) return "Your opponent wants to use Gecko codes";
  if (g.theirs_received && g.i_answered && !g.they_answered && !g.mine.empty()) return "Waiting for your opponent to answer about your codes";
  if (g.agreed_fp != "none") return "Gecko codes agreed for this match: " + std::to_string(g.agreed.size());
  return {};
}

void match_started() {
  g.in_match = true;
  const bool same = g.consent && g.sent_lock == g.peer_lock && g.sent_lock != "none";
  g.running = same ? g.sent_set : std::vector<gecko_online::Lines>{};
  if (g.consent)
    host::log("gecko online: match starts with %zu codes (this side %s, the opponent %s)", g.running.size(), g.sent_lock.c_str(), g.peer_lock.c_str());
}

void match_ended() { g.in_match = false; g.running.clear(); }

const std::vector<gecko_online::Lines>& match_codes() { return g.running; }

}  // namespace gecko_session
