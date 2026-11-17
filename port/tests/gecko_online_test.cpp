// Gecko codes in network matches: identity, agreed set, policy and the messages (gecko_online.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gecko_online.h"

#include <cstdio>
#include <string>

using namespace gecko_online;
static int g_fail = 0;
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }

int main() {
  const Lines a = {{0x04000000u, 0x00000001u}}, b = {{0x04000004u, 0x00000002u}, {0x04000008u, 0x00000003u}};
  check(code_id(a) == code_id(Lines{{0x04000000u, 0x00000001u}}), "the same lines give the same id");
  check(code_id(a) != code_id(b) && code_id(a).size() == 16, "different lines give different 16-digit ids");

  const std::string A = code_id(a), B = code_id(b), C = code_id({{0x0400000Cu, 4u}});
  // I offer A and B, they accept only A; they offer C and B, I accept C.
  const auto mine = agreed({A, B}, {A}, {C, B}, {C});
  const auto theirs = agreed({C, B}, {C}, {A, B}, {A});
  check(mine == theirs, "both sides compute the same agreed set from the same answers");
  check(mine.size() == 2 && set_fingerprint(mine) == set_fingerprint(theirs), "A and C run, B (accepted by nobody) does not");
  check(set_fingerprint({}) == "none", "no codes: the fingerprint says none");

  check(match_policy(false, false, false, false) == Match::Offline, "offline: the player's codes");
  check(match_policy(true, true, true, true) == Match::None, "ranked: no codes");
  check(match_policy(true, true, true, false) == Match::Consent, "Direct or arena against Melee Unlocked: consent");
  check(match_policy(true, true, false, false) == Match::SafeOnly, "against Slippi Dolphin: the safe list");
  check(match_policy(true, false, true, false) == Match::SafeOnly, "Unranked: the safe list");
  check(!runs_without_consent(Match::SafeOnly, b), "an unknown code never runs on the safe list");
  check(runs_without_consent(Match::Offline, b), "offline every code runs");

  std::vector<Offer> offer = {{A, "Mine", a}, {B, "Other", b}};
  Message m;
  const auto o = encode_offer(offer);
  check(decode(o.data(), o.size(), &m) && m.kind == 0 && m.offer.size() == 2 && m.offer[1].lines == b &&
            m.offer[1].id == B && m.offer[0].sent_name == "Mine", "an offer round-trips, ids from the lines");
  const auto ans = encode_answer({A, "not an id"});
  check(decode(ans.data(), ans.size(), &m) && m.kind == 1 && m.ids.size() == 1 && m.ids[0] == A, "an answer keeps only real ids");
  const auto fin = encode_final(set_fingerprint(mine));
  check(decode(fin.data(), fin.size(), &m) && m.kind == 2 && m.fingerprint == set_fingerprint(mine), "the final fingerprint round-trips");
  const auto none = encode_final("none");
  check(decode(none.data(), none.size(), &m) && m.fingerprint == "none", "an empty set round-trips as none");
  const auto lock = encode_lock(set_fingerprint(mine));
  check(decode(lock.data(), lock.size(), &m) && m.kind == 3 && m.fingerprint == set_fingerprint(mine), "a lock round-trips");
  auto bad = o; bad.pop_back();
  check(!decode(bad.data(), bad.size(), &m), "a cut message is refused");
  bad = o; bad.push_back(0);
  check(!decode(bad.data(), bad.size(), &m), "trailing bytes are refused");
  std::printf(g_fail ? "gecko online: %d failures\n" : "gecko online: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
