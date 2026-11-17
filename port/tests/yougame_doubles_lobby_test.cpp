// SPDX-License-Identifier: GPL-2.0-or-later
// The Doubles home (G2): the Friends lobby's runtime on its 2-seat profile (yougame_lobby.h
// Profile::Doubles). The codec's 2-seat states, the session's Doubles rules (seat 0 owns the partner
// seat, either player starts, fixed rules, cancel, the cross-team mismatch), the select's view (mode,
// hints, doors 3-4 shut), the bridge calls (yougame_friends.h), the home's search pieces
// (yougame_doubles_lobby.h), and the match from the frozen setup for every team shape: 2+2, 2+1+1,
// 1+1+1+1, CPU 1+2 and 1+1 (yougame_lobby_setup.h plan(..., Doubles)).
#include "yougame_lobby_screen.h"
#include "yougame_lobby_setup.h"
#include "yougame_doubles_lobby.h"
#include "yougame_friends.h"
#include "yougame_compatibility.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

using namespace yougame::lobby;

static json seat_open(int s) { return {{"seat", s}, {"kind", "open"}}; }
static json seat_human(int s, const std::string& id, bool picked = false, bool ready = false) {
  json j = {{"seat", s}, {"kind", "human"}, {"id", id}, {"name", id}};
  if (picked) j["pick"] = {{"char", 2}, {"color", 0}, {"team", 0}};
  if (ready) j["ready"] = true;
  return j;
}
static json seat_cpu(int s, int level, bool picked = false) {
  json j = {{"seat", s}, {"kind", "cpu"}, {"level", level}};
  if (picked) j["pick"] = {{"char", 9}, {"color", 1}, {"team", 0}};
  return j;
}
static json doubles_rules_json() {
  return {{"teams", true}, {"friendlyFire", false}, {"mode", "stock"}, {"stocks", 4}, {"minutes", 8}, {"pause", false}};
}
// A Doubles home's state: two seats, the fixed rules, seat 0 the host.
static json home(int64_t rev, std::vector<json> seats, bool searching = false) {
  return {{"t", "yg.mu.lobby"}, {"v", 1}, {"op", "state"}, {"rev", rev}, {"host", 0}, {"searching", searching},
          {"seats", seats}, {"rules", doubles_rules_json()}, {"setup", nullptr}};
}
static std::vector<json> lobby_frames(const std::vector<json>& out) {
  std::vector<json> v;
  for (const auto& f : out) if (f.value("t", "") == kType) v.push_back(f);
  return v;
}
static Session::Options doubles_opts(const std::string& me) {
  Session::Options o;
  o.me = me;
  o.profile = Profile::Doubles;
  return o;
}

// ---- the match setups (rooms MU-DOUBLES-R2 "Frozen match setup"): team i on seats 2i and 2i+1, humans on
// ports 0..h-1 then CPUs, CPUs "cpu:<seat>" named "CPU <seat+1>" ----
struct S { int seat; const char* kind; const char* id; int ch; int level; };
static json match_setup(const std::vector<S>& seats, uint32_t seed = 1) {
  json out = json::array();
  int port = 0;
  for (int pass = 0; pass < 2; ++pass)
    for (const auto& s : seats) {
      const bool cpu = std::string(s.kind) == "cpu";
      if (cpu != (pass == 1)) continue;
      json j = {{"seat", s.seat}, {"port", port++}, {"kind", s.kind}, {"id", s.id},
                {"name", cpu ? "CPU " + std::to_string(s.seat + 1) : std::string(s.id)},
                {"char", s.ch}, {"color", 0}, {"team", s.seat / 2}};
      if (cpu) j["level"] = s.level;
      else { j["settingsDigest"] = "set1"; j["cardDigest"] = "card1"; }
      out.push_back(j);
    }
  return {{"t", "yg.mu.lobby"}, {"v", 1}, {"op", "setup"}, {"rev", 0}, {"matchId", "doubles-match"}, {"seed", seed},
          {"inputDelay", 2}, {"seats", out}, {"rules", doubles_rules_json()},
          {"stage", kRandomStages[seed % kRandomStages.size()]}};
}
static Setup setup_of(const json& f) {
  Incoming in = decode(f);
  CHECK(in.op == Op::Setup && !in.why);
  return in.setup;
}
static const size_t kBlock = 0x138;
static const uint8_t* rec(const std::vector<uint8_t>& b, int port) { return b.data() + 0x60 + port * 0x24; }

// Every human in the setup builds the same match: the same plan and the same canonical setup hash.
static void same_match_everywhere(const json& f, int humans, int cpus) {
  const Setup s = setup_of(f);
  std::vector<Plan> plans;
  for (const auto& seat : f["seats"])
    if (seat["kind"] == "human") plans.push_back(plan(s, seat["id"].get<std::string>(), "set1", "card1", Profile::Doubles));
  CHECK((int)plans.size() == humans);
  for (size_t i = 0; i < plans.size(); ++i) {
    const Plan& p = plans[i];
    CHECK(p.ok && p.doubles && p.ports == 4 && p.humans == humans && p.local_port == (int)i);
    CHECK(p.input_delay == 2 && p.rules == s.rules && p.stage == s.stage);
    CHECK(setup_digest(p) == setup_digest(plans[0]) && inputs_hash(p) == inputs_hash(plans[0]));
    CHECK(p.ids == plans[0].ids);
  }
  const Plan& p = plans[0];
  for (int port = 0; port < 4; ++port) {
    CHECK((p.port[port].kind == Kind::Cpu) == (port >= humans));                // humans first, then CPUs
    CHECK(p.port[port].pick.team == p.port[port].seat / 2);                      // matchmaking's team
    if (p.port[port].kind == Kind::Cpu) CHECK(p.port[port].name == "CPU " + std::to_string(p.port[port].seat + 1));
  }
  std::vector<uint8_t> block(kBlock, 0);
  CHECK(apply(p, block.data(), block.size()));
  CHECK(block[0x0] == 0x32 && block[0x8] == 1);                                  // stock with a timer, teams on
  CHECK((block[0x1] & 0x01) == (s.rules.friendly_fire ? 1 : 0) && (block[0x2] & 0x08) == 0x08);   // pause off
  CHECK(block[0x12] == 0x01 && block[0x13] == 0xE0);                             // 8 minutes (480 s)
  CHECK(block[0xF] == p.stage);
  int cpu_records = 0;
  for (int port = 0; port < 4; ++port) {
    CHECK(rec(block, port)[2] == 4);                                             // 4 stocks
    CHECK(rec(block, port)[9] == p.port[port].pick.team);
    CHECK(rec(block, port)[1] == (p.port[port].kind == Kind::Cpu ? 1 : 0));
    if (p.port[port].kind == Kind::Cpu) { ++cpu_records; CHECK(rec(block, port)[0xF] == p.port[port].level); }
  }
  CHECK(cpu_records == cpus);
  // The finish ranks all four ids, CPUs too, with the winning team as detail.tw.
  Outcome o; o.end_method = 2; o.has_placements = true;
  for (int port = 0; port < 4; ++port) o.placements[port] = p.port[port].pick.team == 1 ? 0 : 1;
  json r = finish_report(p, o);
  CHECK(r["ranking"].size() == 4 && r["detail"]["tw"] == 1 && r["setup"] == p.frozen && !r["setup"].count("matchId"));
  for (int port = 0; port < 4; ++port) o.placements[port] = p.port[port].pick.team == 0 ? 0 : 1;
  CHECK(finish_report(p, o)["detail"]["tw"] == 0);
}

int main() {
  const uint64_t now = 100000;

  // ---- the codec: a Doubles home has exactly two seats ----
  {
    CHECK(seats_of(Profile::Doubles) == 2 && seats_of(Profile::Friends4) == 4);
    const Rules r = doubles_rules();
    CHECK(r.teams && r.friendly_fire && !r.time && r.stocks == 4 && r.minutes == 8 && !r.pause);
    CHECK(!doubles_rules_old().friendly_fire);
    for (int stage : kDoublesStages) CHECK(stage != 2);                          // no Fountain of Dreams
    Incoming in = decode(home(3, {seat_human(0, "a"), seat_open(1)}, true), Profile::Doubles);
    CHECK(in.op == Op::State && !in.why && in.state.searching && in.state.searching_shown);
    CHECK(in.state.seats[0].kind == Kind::Human && in.state.seats[1].kind == Kind::Open);
    CHECK(in.state.seats[2].seat == 2 && in.state.seats[3].seat == 3 && in.state.seats[3].kind == Kind::Open);
    CHECK(decode(home(3, {seat_human(0, "a"), seat_open(1)}), Profile::Friends4).why);   // 2 seats in a 4-seat room
    CHECK(decode(home(3, {seat_human(0, "a"), seat_open(1), seat_open(2), seat_open(3)}), Profile::Doubles).why);
    json bad = home(3, {seat_human(0, "a"), seat_open(1)}); bad["host"] = 2;
    CHECK(decode(bad, Profile::Doubles).why);
    bad = home(3, {seat_human(0, "a"), seat_open(1)}); bad["searching"] = "yes";
    CHECK(decode(bad, Profile::Doubles).why);
    bad = home(3, {seat_human(0, "a"), seat_open(1)}); bad.erase("searching");
    in = decode(bad, Profile::Doubles);
    CHECK(!in.why && !in.state.searching && !in.state.searching_shown);
    // Start omits stage and inputDelay; cancel carries no rev.
    const json st = doubles_start_frame(7);
    CHECK(st["op"] == "start" && st["rev"] == 7 && !st.count("stage") && !st.count("inputDelay") && !frame_text(st).empty());
    const json c = cancel_frame();
    CHECK(c["op"] == "cancel" && !c.count("rev") && !frame_text(c).empty());
    json extra = c; extra["stage"] = 3;                 // cancel is {t, v, op} (a rev Rooms ignores)
    CHECK(frame_text(extra).empty());
    json withrev = c; withrev["rev"] = 3;
    CHECK(!frame_text(withrev).empty());
  }

  // ---- the session: seat 0 owns the partner seat, either player starts ----
  {
    Session a(doubles_opts("a"));
    CHECK(a.doubles() && a.profile() == Profile::Doubles);
    a.receive(home(1, {seat_human(0, "a"), seat_open(1)}));
    CHECK(a.my_seat() == 0 && a.is_host());
    CHECK(a.can_start() == StartBlock::PickRequired);
    CHECK(!a.set_rules(Rules{}));                    // fixed rules
    CHECK(!a.set_cpu(2, 5) && !a.set_cpu(0, 5));     // only seat 1
    CHECK(a.set_cpu(1, 5));
    CHECK(!a.set_cpu(1, 10));
    CHECK(a.pick(Pick{2, 0, 0}));
    auto out = lobby_frames(a.due(now));
    CHECK(out.size() == 2 && out[0]["op"] == "seat" && out[0]["seat"] == 1 && out[0]["level"] == 5);
    // Alone and picked: Start is offered (Rooms finds the partner too).
    a.receive(home(2, {seat_human(0, "a", true), seat_open(1)}));
    CHECK(a.can_start() == StartBlock::None);
    CHECK(a.start(31));
    out = lobby_frames(a.due(now));
    CHECK(out.size() == 1 && out[0]["op"] == "start" && out[0]["rev"] == 2 && !out[0].count("stage"));
    // A CPU partner needs its pick.
    a.receive(home(3, {seat_human(0, "a", true), seat_cpu(1, 5)}));
    CHECK(a.can_start() == StartBlock::PickRequired);
    CHECK(a.cpu_pick(1, Pick{9, 1, 0}) && a.open_seat(1));
    a.due(now);
    a.receive(home(4, {seat_human(0, "a", true), seat_cpu(1, 5, true)}));
    CHECK(a.can_start() == StartBlock::None);
    // Searching: the select is locked, Start is Searching, cancel goes out without a rev.
    a.receive(home(5, {seat_human(0, "a", true), seat_cpu(1, 5, true)}, true));
    CHECK(a.searching() && !a.lobby_open() && a.can_start() == StartBlock::Searching);
    CHECK(!a.pick(Pick{3, 0, 0}) && !a.start(0));
    CHECK(a.cancel());
    out = lobby_frames(a.due(now));
    CHECK(out.size() == 1 && out[0]["op"] == "cancel" && !out[0].count("rev"));
    // The cross-team mismatch: the home's state with digest_mismatch that ends the search.
    json mm = home(6, {seat_human(0, "a", true), seat_cpu(1, 5, true)}, false);
    mm["error"] = "digest_mismatch"; mm["action"] = "start";
    a.receive(mm);
    CHECK(a.refusal() == kTeamsMismatch && !a.searching() && a.can_start() == StartBlock::None);
    CHECK(!a.cancel());                              // nothing to cancel any more
    // The home's own digest_mismatch (not searching before): stays digest_mismatch.
    a.receive(mm);
    CHECK(a.refusal() == "digest_mismatch");
  }
  {
    // The partner (seat 1): no seat toggles, but Start is theirs too; a pick still missing blocks.
    Session b(doubles_opts("b"));
    b.receive(home(1, {seat_human(0, "a", true), seat_human(1, "b")}));
    CHECK(b.my_seat() == 1 && !b.is_host());
    CHECK(!b.set_cpu(1, 3) && !b.open_seat(1));
    CHECK(b.can_start() == StartBlock::PickRequired);
    b.receive(home(2, {seat_human(0, "a", true, true), seat_human(1, "b", true, true)}));
    CHECK(b.can_start() == StartBlock::None && b.start(0));
    b.receive(home(3, {seat_human(0, "a", true, true), seat_human(1, "b", true)}));
    CHECK(b.can_start() == StartBlock::NotReady);   // ready seen, the partner's hello not in yet
    // A Friends4 session never cancels.
    Session f(Session::Options{});
    CHECK(!f.cancel() && !f.doubles());
  }
  {
    // Control: the profile travels with it; a placed match shows Starting before any setup.
    Control c(Profile::Doubles);
    Session s(doubles_opts("a"));
    s.receive(home(1, {seat_human(0, "a", true), seat_open(1)}, true));
    c.run(s);
    Control::View v = c.view();
    CHECK(c.profile() == Profile::Doubles && v.profile == Profile::Doubles && v.searching && !v.started);
    c.set_matched(true);
    c.run(s);
    CHECK(c.matched() && c.view().started);
    c.set_matched(false);
    c.run(s);
    CHECK(!c.view().started);
    CHECK(Control().profile() == Profile::Friends4);
  }

  // ---- the select's view: mode, two doors, the Doubles hints ----
  {
    const auto view_of = [](const json& st, const std::string& me) {
      Control c(Profile::Doubles);
      Session s(doubles_opts(me));
      s.receive(st);
      c.run(s);
      return c.view();
    };
    ScreenFlags f; f.armed = true;
    Control::View v = view_of(home(2, {seat_human(0, "a", true), seat_open(1)}), "a");
    MuLobbyView out = screen_view(v, f);
    CHECK(out.mode == MU_LOBBY_M_DOUBLES && out.block_hint == MU_LOBBY_HINT_START_PARTNER);
    CHECK(out.seats[2].kind == MU_LOBBY_KIND_OPEN && out.seats[3].kind == MU_LOBBY_KIND_OPEN);
    CHECK(out.seats[2].pick_ch == MU_LOBBY_NONE && out.seats[3].hover_ch == MU_LOBBY_NONE);
    v = view_of(home(2, {seat_human(0, "a", true), seat_cpu(1, 4, true)}), "a");
    CHECK(screen_view(v, f).block_hint == MU_LOBBY_HINT_START_SEARCH);
    CHECK(screen_view(v, f).seats[1].kind == MU_LOBBY_KIND_CPU && screen_view(v, f).seats[1].level == 4);
    v = view_of(home(2, {seat_human(0, "a", true), seat_human(1, "b", true)}), "b");   // the partner starts too
    CHECK(screen_view(v, f).block_hint == MU_LOBBY_HINT_START_SEARCH && !(screen_view(v, f).flags & MU_LOBBY_F_HOST));
    v = view_of(home(2, {seat_human(0, "a", true), seat_human(1, "b")}), "a");
    CHECK(screen_view(v, f).block_hint == MU_LOBBY_HINT_PICK);
    // A guest left alone (the host's seat open) still searches for a partner, not just a team.
    v = view_of(home(2, {seat_open(0), seat_human(1, "b", true)}), "b");
    CHECK(screen_view(v, f).block_hint == MU_LOBBY_HINT_START_PARTNER);
    v = view_of(home(3, {seat_human(0, "a", true), seat_human(1, "b", true)}, true), "b");
    out = screen_view(v, f);
    CHECK(out.mode == (MU_LOBBY_M_DOUBLES | MU_LOBBY_M_SEARCHING) && out.block_hint == MU_LOBBY_HINT_SEARCHING);
    // The partner's link once it is up: route and ping on its door.
    v.links[0] = LinkInfo{"direct", 23};
    out = screen_view(v, f);
    CHECK(out.seats[0].route == MU_LOBBY_ROUTE_DIRECT && out.seats[0].rtt_ms == 23 && out.seats[1].route == MU_LOBBY_ROUTE_NONE);
    // Encode and decode with the mode byte.
    unsigned char bytes[MU_LOBBY_VIEW_SIZE];
    CHECK(mu_lobby_view_encode(&out, bytes, sizeof bytes) == MU_LOBBY_VIEW_SIZE);
    MuLobbyView back;
    CHECK(mu_lobby_view_decode(bytes, sizeof bytes, &back) && back.mode == out.mode && back.my_seat == 1);
    bytes[2] = MU_LOBBY_M_SEARCHING;                                    // searching without Doubles
    CHECK(!mu_lobby_view_decode(bytes, sizeof bytes, &back));
    bytes[2] = MU_LOBBY_M_DOUBLES | MU_LOBBY_M_STAGE_SELECT;            // Doubles has no stage select
    CHECK(!mu_lobby_view_decode(bytes, sizeof bytes, &back));
    bytes[2] = 0x08;                                                    // an unknown mode bit
    CHECK(!mu_lobby_view_decode(bytes, sizeof bytes, &back));
    bytes[2] = MU_LOBBY_M_DOUBLES; bytes[3] = 2;                        // my seat past the home's two
    CHECK(!mu_lobby_view_decode(bytes, sizeof bytes, &back));
    // The Friends lobby's view keeps mode 0.
    Control c4;
    Session s4(Session::Options{});
    c4.run(s4);
    CHECK(screen_view(c4.view(), f).mode == 0);
    // Rooms' refusals of the Doubles frames, as hints.
    CHECK(refusal_hint("pick_required", "start") == MU_LOBBY_HINT_PICK);
    CHECK(refusal_hint("digest_missing", "start") == MU_LOBBY_HINT_NOT_READY);
    CHECK(refusal_hint("digest_mismatch", "start") == MU_LOBBY_HINT_MISMATCH);
    CHECK(refusal_hint("player_away", "start") == MU_LOBBY_HINT_AWAY);
    CHECK(refusal_hint("match_active", "cancel") == MU_LOBBY_HINT_IN_MATCH);
    CHECK(refusal_hint("searching", "pick") == MU_LOBBY_HINT_SEARCHING);
    CHECK(refusal_hint("unranked_only", "start") == MU_LOBBY_HINT_UNRANKED_ONLY);
    CHECK(refusal_hint("fixed_rules", "rules") == MU_LOBBY_HINT_FIXED_RULES);
    CHECK(refusal_hint(kTeamsMismatch, "start") == MU_LOBBY_HINT_TEAM_MISMATCH);
    CHECK(refusal_hint("search_cancelled", "start") == MU_LOBBY_HINT_SEARCH_STOPPED);
    for (const char* e : {"searching", "unranked_only", "fixed_rules", "search_cancelled", kTeamsMismatch})
      CHECK(known_refusal(e));
    // Cancel's request is one byte.
    const unsigned char cancel[] = {MU_LOBBY_OP_CANCEL};
    CHECK(mu_lobby_request_size(MU_LOBBY_OP_CANCEL) == 1 && mu_lobby_request_ok(cancel, 1) && !mu_lobby_request_ok(cancel, 0));
  }

  // ---- the bridge calls (rooms MU-DOUBLES-R2 "G2 bridge calls") ----
  {
    using namespace yougame::friends;
    const std::string key = "doubles-lobby-1:mu-0.9.105-r12345678";
    CHECK(doubles_compatibility(key));
    CHECK(!doubles_compatibility("mu:doubles-lobby-1:x"));          // the tag must lead
    CHECK(!doubles_compatibility("doubles-lobby-1:"));
    CHECK(!doubles_compatibility("friends-lobby-1:mu"));
    CHECK(!lobby_compatibility(key));                               // never the Friends lobby's room
    CHECK(doubles_hold(key) == json({{"type", "mpFriend"}, {"hold", true}, {"players", 2}, {"compatibility", key}}));
    CHECK(doubles_invite("abcdefghjkmn") ==
          json({{"type", "mpFriend"}, {"mode", "teams"}, {"teamsLobby", 1}, {"room", "abcdefghjkmn"}, {"players", 2}}));
    const json h = doubles_home_ticket(key, "abcdefghjkmn");
    CHECK(h["type"] == "mpTicket" && h["mode"] == "teams" && h["teamsLobby"] == 1 && h["players"] == 2 &&
          h["room"] == "abcdefghjkmn" && h["ranked"] == false && h["lobby"]["minPlayers"] == 2 &&
          h["lobby"]["compatibility"] == key && h["lobby"]["maxLocalPlayers"] == 1);
    const json solo = doubles_queue_ticket(key, "abcdefghjkmn", "gen-1", 1);
    CHECK(solo["players"] == 4 && solo["party_room"] == "abcdefghjkmn" && solo["partySearch"] == "gen-1" &&
          solo["ranked"] == false && solo["lobby"]["minPlayers"] == 4 && !solo.count("party") && !solo.count("room"));
    const json pair = doubles_queue_ticket(key, "abcdefghjkmn", "gen-1", 2);
    CHECK(pair["party"] == json({{"id", "gen-1"}, {"n", 2}}));
    CHECK(solo["local_partner"] == kLocalPartnerCapability && pair["local_partner"] == kLocalPartnerCapability);
    bool threw = false;
    try { doubles_hold("friends-lobby-1:mu"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { doubles_queue_ticket(key, "abcdefghjkmn", "", 1); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // The match-admission ticket (the match room's renewals): Teams, teamsLobby 1, 4 players, casual, the
    // same key at min 4 / maxLocal 1, and nothing that names the home or its party.
    const json admit = doubles_match_ticket(key);
    CHECK(admit["type"] == "mpTicket" && admit["mode"] == "teams" && admit["teamsLobby"] == 1 && admit["players"] == 4 &&
          admit["ranked"] == false && admit["lobby"] == json({{"version", 1}, {"minPlayers", 4}, {"maxLocalPlayers", 1},
                                                              {"compatibility", key}}));
    for (const char* field : {"room", "party", "party_room", "partySearch", "hold"}) CHECK(!admit.count(field));
    threw = false;
    try { doubles_match_ticket("friends-lobby-1:mu"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // An invite into a Doubles home, in the shapes the real stack sends (rooms /teams-member, Player.tsx):
    // the launch hello's nativeTeamsInvite {v, room, mode:"teams", md?}, and a running game's Teams push
    // marked teamsLobby:1. Without md those try the 2-seat select first; md:1 is the legacy console Doubles
    // path. A push that names nothing ({type:"join", room}: a tournament bracket push, a 1v1 or Friends invite
    // whose seat lookup missed, an older page's Teams invite) tries 1v1 first, as before G2.
    CHECK(doubles_hint({{"v", 1}, {"room", "abcdefghjkmn"}, {"mode", "teams"}}));
    CHECK(!doubles_hint({{"v", 1}, {"room", "abcdefghjkmn"}, {"mode", "teams"}, {"md", 1}}));
    CHECK(!doubles_hint({{"v", 1}, {"room", "abcdefghjkmn"}}));
    CHECK(!doubles_hint(json()));
    Events e;
    CHECK(e.push({{"slug", "melee-unlocked"}, {"type", "join"}, {"room", "abcdefghjkmn"}, {"teamsLobby", 1}}, 1));
    CHECK(e.take_join(true) == "abcdefghjkmn" && e.join_doubles() && e.join_players() == 0 &&
          e.mismatch_reroutes("abcdefghjkmn"));
    CHECK(e.push({{"slug", "melee-unlocked"}, {"type", "join"}, {"room", "abcdefghjkmp"}, {"md", 1}}, 2));
    CHECK(e.take_join(true) == "abcdefghjkmp" && !e.join_doubles());
    CHECK(e.push({{"slug", "melee-unlocked"}, {"type", "join"}, {"room", "abcdefghjkmq"}, {"players", 4}}, 3));
    CHECK(e.take_join(true) == "abcdefghjkmq" && !e.join_doubles() && e.join_players() == 4);
    CHECK(e.push({{"slug", "melee-unlocked"}, {"type", "join"}, {"room", "abcdefghjkmr"}, {"players", 2}}, 4));
    CHECK(e.take_join(true) == "abcdefghjkmr" && !e.join_doubles());
    CHECK(e.push({{"type", "join"}, {"room", "abcdefghjkms"}, {"teamsLobby", 1}, {"md", 1}}, 4));   // legacy mark
    CHECK(e.take_join(true) == "abcdefghjkms" && !e.join_doubles());
    // md:1 / mu_doubles:1 is a hint: that join never enters the kind retry ("another Doubles profile" in a build
    // without MELEE_DOUBLES shows at once instead of cycling through teams_home and the Friends lobby).
    CHECK(!e.mismatch_reroutes("abcdefghjkms") && e.mismatch_next("abcdefghjkms", kRoom1v1, "another Doubles profile") == -1 &&
          e.first_refusal("abcdefghjkms").empty() &&
          e.refusal_text("abcdefghjkms", "another Doubles profile", true) == "another Doubles profile");
    CHECK(e.push({{"type", "join"}, {"room", "abcdefghjkmp"}, {"md", 1}}, 4));
    CHECK(e.take_join(true) == "abcdefghjkmp" && !e.mismatch_reroutes("abcdefghjkmp") &&
          e.mismatch_next("abcdefghjkmp", kRoom1v1) == -1);
    CHECK(e.push({{"type", "join"}, {"room", "abcdefghjkmt"}, {"mu_doubles", 1}}, 4));
    CHECK(e.take_join(true) == "abcdefghjkmt" && !e.join_doubles() && !e.mismatch_reroutes("abcdefghjkmt") &&
          e.mismatch_next("abcdefghjkmt", kRoom1v1) == -1);
    CHECK(e.push({{"type", "join"}, {"room", "abcdefghjkmu"}, {"md", 0}}, 4));   // md other than 1: no hint
    CHECK(e.take_join(true) == "abcdefghjkmu" && e.mismatch_reroutes("abcdefghjkmu"));
    // A tournament bracket push and a missed-seat 1v1 / Friends invite: {type:"join", room}, 1v1 first.
    CHECK(e.push({{"type", "join"}, {"room", "bracketroomA"}}, 4));
    CHECK(e.take_join(true) == "bracketroomA" && !e.join_doubles() && e.join_players() == 0 &&
          e.mismatch_reroutes("bracketroomA"));
    CHECK(e.push({{"slug", "melee-unlocked"}, {"type", "join"}, {"room", "missedseatAB"}}, 4));
    CHECK(e.take_join(true) == "missedseatAB" && !e.join_doubles());
    Events launch;
    launch.hello({{"room", "abcdefghjkmn"},
                  {"nativeTeamsInvite", {{"v", 1}, {"room", "abcdefghjkmn"}, {"mode", "teams"}}}});
    CHECK(launch.take_join(true) == "abcdefghjkmn" && launch.join_doubles());
    Events legacy;
    legacy.hello({{"room", "abcdefghjkmn"},
                  {"nativeTeamsInvite", {{"v", 1}, {"room", "abcdefghjkmn"}, {"mode", "teams"}, {"md", 1}}}});
    CHECK(legacy.take_join(true) == "abcdefghjkmn" && !legacy.join_doubles());
    Events other;   // a teams invite for another room than the hello's own: no hint
    other.hello({{"room", "abcdefghjkmn"}, {"nativeTeamsInvite", {{"v", 1}, {"room", "abcdefghjkmp"}, {"mode", "teams"}}}});
    CHECK(other.take_join(true) == "abcdefghjkmn" && !other.join_doubles());
    // The page's inviteMode (PR 2280) names the kind an accepted invite opens, in the hello reply and the join
    // push: "singles" 1v1, "default" the 4-player Friends lobby, "teams" the Doubles home. It outranks players
    // and the Teams marks, and counts as a hint: a plain refusal stops (another build or remix), while the room's
    // own kind refusal still reroutes in the mode's order. Unknown or absent changes nothing.
    CHECK(invite_mode_kind({{"inviteMode", "singles"}}) == kRoom1v1 &&
          invite_mode_kind({{"inviteMode", "default"}}) == kRoomFriends4 &&
          invite_mode_kind({{"inviteMode", "teams"}}) == kRoomDoubles);
    for (const json& bad : {json(), json::object(), json::array(), json("teams"), json{{"inviteMode", "ranked"}},
                            json{{"inviteMode", ""}}, json{{"inviteMode", "Teams"}}, json{{"inviteMode", 2}},
                            json{{"inviteMode", json()}}, json{{"invite_mode", "teams"}}})
      CHECK(invite_mode_kind(bad) == -1);
    {
      // What a taken join does: its room, Doubles first, its size, whether a plain / kind refusal reroutes,
      // and the kind after a refused Doubles try (1v1 first and Doubles first: 1v1; Friends lobby first: it).
      struct Taken { std::string room; bool doubles; int players; bool plain, kind; int after_doubles; };
      const auto taken = [](Events& ev, const std::string& code) {
        Taken t{ev.take_join(true), ev.join_doubles(), ev.join_players(), ev.mismatch_reroutes(code),
                ev.mismatch_reroutes(code, true), 0};
        t.after_doubles = ev.mismatch_next(code, kRoomDoubles, "", true);
        return t;
      };
      const auto same = [](const Taken& a, const Taken& b) {
        return a.room == b.room && a.doubles == b.doubles && a.players == b.players && a.plain == b.plain &&
               a.kind == b.kind && a.after_doubles == b.after_doubles;
      };
      const auto pushed = [&](json value) {
        Events ev;
        value["type"] = "join"; value["room"] = "modeinviteAB";
        CHECK(ev.push(value, 1));
        return taken(ev, "modeinviteAB");
      };
      const auto launched = [&](json value) {
        Events ev;
        value["room"] = "modeinviteAB";
        ev.hello(value);
        return taken(ev, "modeinviteAB");
      };
      const json teams_invite = {{"v", 1}, {"room", "modeinviteAB"}, {"mode", "teams"}};
      for (int launch = 0; launch < 2; ++launch) {
        const auto join = [&](const json& value) { return launch ? launched(value) : pushed(value); };
        // singles: 1v1 first, size 2, hinted (a kind refusal tries the Friends lobby next).
        auto t = join({{"inviteMode", "singles"}});
        CHECK(t.room == "modeinviteAB" && !t.doubles && t.players == 2 && !t.plain && t.kind && t.after_doubles == kRoom1v1);
        { Events ev; json v = {{"inviteMode", "singles"}, {"room", "modeinviteAB"}};
          if (launch) ev.hello(v); else { v["type"] = "join"; CHECK(ev.push(v, 1)); }
          CHECK(ev.take_join(true) == "modeinviteAB" && ev.mismatch_next("modeinviteAB", kRoom1v1, "", false) == -1 &&
                ev.mismatch_next("modeinviteAB", kRoom1v1, "", true) == kRoomFriends4); }
        // default: the Friends lobby first, size 4, hinted.
        t = join({{"inviteMode", "default"}});
        CHECK(!t.doubles && t.players == 4 && !t.plain && t.kind && t.after_doubles == kRoomFriends4);
        // teams: the Doubles home first, hinted (a push marked teamsLobby:1 alone stays unhinted, as before).
        t = join(launch ? json{{"inviteMode", "teams"}, {"nativeTeamsInvite", teams_invite}}
                        : json{{"inviteMode", "teams"}, {"teamsLobby", 1}});
        CHECK(t.doubles && t.players == 0 && !t.plain && t.kind && t.after_doubles == kRoom1v1);
        t = join({{"inviteMode", "teams"}});   // the mode alone is enough
        CHECK(t.doubles && t.players == 0 && !t.plain && t.kind);
        // teams with players 4 (a 4-seat Doubles home): still the Doubles home first, the page's size kept.
        t = join(launch ? json{{"inviteMode", "teams"}, {"players", 4}, {"nativeTeamsInvite", teams_invite}}
                        : json{{"inviteMode", "teams"}, {"players", 4}, {"teamsLobby", 1}});
        CHECK(t.doubles && t.players == 4 && !t.plain && t.kind && t.after_doubles == kRoom1v1);
        // ... where the same invite without the mode goes Friends lobby first in a push (players outranks the
        // mark there) and Doubles first in a hello (the Teams invite outranks players), as before.
        t = join(launch ? json{{"players", 4}, {"nativeTeamsInvite", teams_invite}}
                        : json{{"players", 4}, {"teamsLobby", 1}});
        CHECK(t.doubles == bool(launch) && t.players == 4);
        // The mode outranks a contradicting size or mark.
        t = join({{"inviteMode", "singles"}, {"players", 4}});
        CHECK(!t.doubles && t.players == 2 && t.after_doubles == kRoom1v1);
        t = join(launch ? json{{"inviteMode", "default"}, {"nativeTeamsInvite", teams_invite}}
                        : json{{"inviteMode", "default"}, {"teamsLobby", 1}});
        CHECK(!t.doubles && t.players == 4 && t.after_doubles == kRoomFriends4);
        // Unknown and absent: exactly the old behaviour, for every older shape.
        const std::vector<json> shapes = {
            json::object(), json{{"players", 2}}, json{{"players", 4}}, json{{"compatibility", "mu:friends-lobby-1"}},
            json{{"compatibility", "mu-0.9.105"}}, json{{"teamsLobby", 1}}, json{{"teamsLobby", 1}, {"players", 4}},
            json{{"md", 1}}, json{{"mu_doubles", 1}}, json{{"md", 0}}, json{{"nativeTeamsInvite", teams_invite}},
            json{{"nativeTeamsInvite", teams_invite}, {"players", 4}}};
        for (const auto& shape : shapes) {
          const auto old = join(shape);
          for (const json& unknown : {json("ranked"), json(""), json("Teams"), json(3), json()}) {
            json v = shape; v["inviteMode"] = unknown;
            CHECK(same(join(v), old));
          }
        }
        // A legacy md:1 Teams invite keeps its own path whatever the mode says.
        json legacy_invite = teams_invite; legacy_invite["md"] = 1;
        const json legacy_shape = launch ? json{{"nativeTeamsInvite", legacy_invite}} : json{{"teamsLobby", 1}, {"md", 1}};
        json legacy_mode = legacy_shape; legacy_mode["inviteMode"] = "teams";
        CHECK(same(join(legacy_mode), join(legacy_shape)) && !join(legacy_mode).doubles);
      }
      // Absent in the old shapes the tests above already pin: unchanged.
      auto t = pushed({{"teamsLobby", 1}});
      CHECK(t.doubles && t.players == 0 && t.plain);
      t = launched({{"nativeTeamsInvite", teams_invite}});
      CHECK(t.doubles && t.players == 0 && t.plain);
      t = pushed(json::object());
      CHECK(!t.doubles && t.players == 0 && t.plain && t.after_doubles == kRoom1v1);
    }
    // The room decides. A Doubles-first join refused (a socket lobby_mismatch, or the ticket route's
    // teams_lobby_off / bad_teams_lobby / teams_lobby_branch) tries 1v1, then the Friends lobby, each once, still
    // unknown and unhinted; when every kind was tried, the FIRST refusal's words stand. teams_home is never a kind
    // refusal: the route also answers it for a friend's home that is gone, busy, full, blocked or another remix,
    // and a 1v1 try would open a fresh empty room under the dead code.
    CHECK(doubles_kind_refusal("teams_lobby_off") && doubles_kind_refusal("bad_teams_lobby") &&
          doubles_kind_refusal("teams_lobby_branch"));
    CHECK(!doubles_kind_refusal("teams_home"));
    CHECK(!doubles_kind_refusal("host_gone") && !doubles_kind_refusal("teams_home_expired") &&
          !doubles_kind_refusal("teams_invite_expired") && !doubles_kind_refusal("refused") &&
          !doubles_kind_refusal("room_full") && !doubles_kind_refusal(""));
    CHECK(e.push({{"type", "join"}, {"room", "dblroomabcde"}, {"teamsLobby", 1}}, 5));
    CHECK(e.take_join(true) == "dblroomabcde" && e.join_doubles());
    CHECK(e.first_refusal("dblroomabcde").empty());
    CHECK(e.mismatch_next("dblroomabcde", kRoomDoubles, "Your Teams home changed or is unavailable.") == kRoom1v1);
    e.room_retry("dblroomabcde", kRoom1v1);
    CHECK(e.take_join(true) == "dblroomabcde" && !e.join_doubles() && e.join_players() == 2 &&
          e.room_kind("dblroomabcde") == -1 && e.mismatch_reroutes("dblroomabcde"));
    CHECK(e.mismatch_next("dblroomabcde", kRoom1v1, "lobby mismatch (1v1)") == kRoomFriends4);
    e.room_retry("dblroomabcde", kRoomFriends4);
    CHECK(e.take_join(true) == "dblroomabcde" && !e.join_doubles() && e.join_players() == 4);
    CHECK(e.mismatch_next("dblroomabcde", kRoomFriends4, "lobby mismatch (Friends)") == -1);
    CHECK(e.first_refusal("dblroomabcde") == "Your Teams home changed or is unavailable.");
    // The words shown when the last try fails: a kind refusal shows the first refusal's; a real reason that isn't
    // one (host_gone, tournament_seat, ...) shows its own, even after two kinds were tried.
    CHECK(e.refusal_text("dblroomabcde", "lobby mismatch (Friends)", true) == "Your Teams home changed or is unavailable.");
    CHECK(e.refusal_text("dblroomabcde", "Your friend isn't in that game any more", false) ==
          "Your friend isn't in that game any more");
    CHECK(e.refusal_text("dblroomabcde", "You already have a seat in a tournament match", false) ==
          "You already have a seat in a tournament match");
    CHECK(e.refusal_text("neverroomabc", "lobby mismatch", true) == "lobby mismatch");   // no first: its own
    // A 1v1-first join (a bare push, or a launch with no Teams invite or size) refused tries the Friends lobby
    // next, then the Doubles home: a Friends try at a Doubles home is a socket kind refusal (the tries go on),
    // while a Doubles try at a Friends lobby gets teams_home, which ends them (11-08).
    Events plain;
    plain.hello({{"room", "plainroomabc"}});
    CHECK(plain.take_join(true) == "plainroomabc" && !plain.join_doubles() && plain.join_players() == 0);
    CHECK(plain.mismatch_next("plainroomabc", kRoom1v1, "another Teams protocol") == kRoomFriends4);
    plain.room_retry("plainroomabc", kRoomFriends4);
    CHECK(plain.take_join(true) == "plainroomabc" && plain.join_players() == 4 && !plain.join_doubles());
    CHECK(plain.mismatch_next("plainroomabc", kRoomFriends4, "lobby mismatch (Friends)") == kRoomDoubles);
    plain.room_retry("plainroomabc", kRoomDoubles);
    CHECK(plain.take_join(true) == "plainroomabc" && plain.join_doubles());
    CHECK(plain.mismatch_next("plainroomabc", kRoomDoubles) == -1);
    CHECK(plain.first_refusal("plainroomabc") == "another Teams protocol");
    // The page's hello names the room's size when it knows it (nativeRoomSeats): a launch into a Friends lobby
    // goes there first and never reroutes (a refusal then means another build); a size 2 is a 1v1 friend room.
    Events lobby4;
    lobby4.hello({{"room", "lobbyroomabc"}, {"players", 4}});
    CHECK(lobby4.take_join(true) == "lobbyroomabc" && lobby4.join_players() == 4 && !lobby4.join_doubles() &&
          !lobby4.mismatch_reroutes("lobbyroomabc") && lobby4.mismatch_next("lobbyroomabc", kRoomFriends4) == -1);
    Events room2;
    room2.hello({{"room", "pairroomabcd"}, {"players", 2}});
    CHECK(room2.take_join(true) == "pairroomabcd" && room2.join_players() == 2 && !room2.join_doubles() &&
          !room2.mismatch_reroutes("pairroomabcd"));
    // A kind refusal (teams_protocol / doubles_profile) still reroutes a sized join: 2 seats may be a Teams
    // home and 4 a Doubles home. The next try is unhinted (room_retry), so a lobby_mismatch after it goes on.
    Events sized2;
    sized2.hello({{"room", "teamhomeabcd"}, {"players", 2}});
    CHECK(sized2.take_join(true) == "teamhomeabcd" && !sized2.mismatch_reroutes("teamhomeabcd") &&
          sized2.mismatch_reroutes("teamhomeabcd", true));
    CHECK(sized2.mismatch_next("teamhomeabcd", kRoom1v1, "another Teams protocol", true) == kRoomFriends4);
    sized2.room_retry("teamhomeabcd", kRoomFriends4);
    CHECK(sized2.take_join(true) == "teamhomeabcd" && sized2.mismatch_reroutes("teamhomeabcd"));
    CHECK(sized2.mismatch_next("teamhomeabcd", kRoomFriends4, "lobby mismatch (Friends)") == kRoomDoubles);
    CHECK(sized2.first_refusal("teamhomeabcd") == "another Teams protocol");
    Events sized4;
    sized4.hello({{"room", "mdhomeabcdef"}, {"players", 4}});
    CHECK(sized4.take_join(true) == "mdhomeabcdef" &&
          sized4.mismatch_next("mdhomeabcdef", kRoomFriends4, "another Teams protocol", true) == kRoom1v1);
    // An md:1 join never tries another kind, kind refusal or not ("another Doubles profile" = a build without it).
    Events strict;
    CHECK(strict.push({{"type", "join"}, {"room", "strictroomab"}, {"md", 1}, {"players", 4}}, 4));
    CHECK(strict.take_join(true) == "strictroomab" && !strict.mismatch_reroutes("strictroomab", true) &&
          strict.mismatch_next("strictroomab", kRoomFriends4, "another Doubles profile", true) == -1);
    // A hello's Teams invite outranks its seat count (a 4-seat Doubles home reads as players 4).
    Events both;
    both.hello({{"room", "bothroomabcd"}, {"players", 4},
                {"nativeTeamsInvite", {{"v", 1}, {"room", "bothroomabcd"}, {"mode", "teams"}}}});
    CHECK(both.take_join(true) == "bothroomabcd" && both.join_doubles() && both.join_players() == 4);
    Events oddsize;   // any other size is no hint
    oddsize.hello({{"room", "oddsroomabcd"}, {"players", 3}});
    CHECK(oddsize.take_join(true) == "oddsroomabcd" && oddsize.join_players() == 0 &&
          oddsize.mismatch_reroutes("oddsroomabcd"));
    // A menu that joins another way than asked (no app bridge on the static recomp, the console entry, a picker
    // open): each refusal still spends a kind, so the room gets three tries at most, never a loop.
    Events deaf;
    CHECK(deaf.push({{"type", "join"}, {"room", "deafroomabcd"}}, 1));
    CHECK(deaf.take_join(true) == "deafroomabcd");
    int tries = 1, next = deaf.mismatch_next("deafroomabcd", kRoom1v1, "first");
    while (next >= 0 && tries < 10) {
      deaf.room_retry("deafroomabcd", next);
      CHECK(deaf.take_join(true) == "deafroomabcd");
      ++tries;
      next = deaf.mismatch_next("deafroomabcd", kRoom1v1, "again");   // joined as 1v1 every time
    }
    CHECK(tries == 3 && next == -1 && deaf.first_refusal("deafroomabcd") == "first");
    Events deaf2;   // the same without even taking the queued join
    CHECK(deaf2.push({{"type", "join"}, {"room", "deafroomabce"}, {"teamsLobby", 1}}, 1));
    CHECK(deaf2.take_join(true) == "deafroomabce");
    CHECK(deaf2.mismatch_next("deafroomabce", kRoomDoubles) == kRoom1v1);
    CHECK(deaf2.mismatch_next("deafroomabce", kRoomDoubles) == kRoomFriends4);
    CHECK(deaf2.mismatch_next("deafroomabce", kRoomDoubles) == -1);
    CHECK(deaf2.mismatch_next("deafroomabce", kRoomDoubles) == -1);
    // A hinted join (a size) never reroutes on a refusal (a different build); a known kind never does.
    CHECK(e.push({{"type", "join"}, {"room", "sizedroomabc"}, {"players", 4}}, 6));
    CHECK(e.take_join(true) == "sizedroomabc" && e.mismatch_next("sizedroomabc", kRoomFriends4) == -1);
    // Frames decide for good: a Friends lobby joined as Doubles is queued again as the Friends lobby, and
    // a Doubles home joined another way as Doubles.
    e.room_decided("dblroomabcde", kRoomDoubles);
    CHECK(e.take_join(true) == "dblroomabcde" && e.join_doubles() && e.room_kind("dblroomabcde") == kRoomDoubles &&
          !e.mismatch_reroutes("dblroomabcde") && e.mismatch_next("dblroomabcde", kRoomDoubles) == -1 &&
          e.first_refusal("dblroomabcde").empty());
    e.room_decided("plainroomabc", kRoomFriends4);
    CHECK(e.take_join(true) == "plainroomabc" && !e.join_doubles() && e.join_players() == 4 &&
          e.room_kind("plainroomabc") == kRoomFriends4);
    // A new push for a room starts its tries over.
    CHECK(e.push({{"type", "join"}, {"room", "freshroomabc"}, {"teamsLobby", 1}}, 7));
    CHECK(e.take_join(true) == "freshroomabc" && e.mismatch_next("freshroomabc", kRoomDoubles, "one") == kRoom1v1);
    CHECK(e.push({{"type", "join"}, {"room", "freshroomabc"}, {"teamsLobby", 1}}, 8));
    CHECK(e.take_join(true) == "freshroomabc" && e.first_refusal("freshroomabc").empty() &&
          e.mismatch_next("freshroomabc", kRoomDoubles) == kRoom1v1);
    // A friend's home that expired or emptied: the guest's ticket opens a fresh home under its code with only the
    // guest in it. That welcome is a gone room (kRoomGone), never another kind; a live home lists the friend too
    // (away included), and a welcome without a player list never fails the join.
    CHECK(lone_welcome({{"t", "welcome"}, {"me", "p-guest"}, {"players", json::array({{{"id", "p-guest"}, {"seat", 0}}})}}));
    CHECK(lone_welcome({{"t", "welcome"}, {"me", "p-guest"}, {"players", json::array()}}));
    CHECK(!lone_welcome({{"t", "welcome"}, {"me", "p-guest"},
                         {"players", json::array({{{"id", "p-host"}, {"away", true}}, {{"id", "p-guest"}}})}}));
    CHECK(!lone_welcome({{"t", "welcome"}, {"me", "p-guest"}}));
    CHECK(!lone_welcome(json()));
    CHECK(std::string(kRoomGone).find("gone") != std::string::npos);
    // A lobby frame's kind: the Doubles home's muState has 2 seats and `searching`; the Friends lobby 4, none.
    CHECK(doubles_frame({{"t", "yg.mu.lobby"}, {"op", "state"}, {"searching", false},
                         {"seats", json::array({json::object(), json::object()})}}));
    CHECK(doubles_frame({{"seats", json::array({json::object(), json::object()})}}));
    CHECK(doubles_frame({{"searching", true}, {"seats", json::array()}}));
    CHECK(!doubles_frame({{"seats", json::array({json::object(), json::object(), json::object(), json::object()})}}));
    CHECK(!doubles_frame(json()));
    // The compatibility key's tag for each profile, the same length.
    auto make = [](const std::string& v) { return "mu-" + v + "-mod-0123456789abcdef"; };
    CHECK(compatibility("0.9.105", make, Profile::Doubles) == "doubles-lobby-1:mu-0.9.105-mod-0123456789abcdef");
    CHECK(doubles_compatibility(compatibility(std::string(40, 'a'), make, Profile::Doubles)));
    CHECK(compatibility(std::string(40, 'a'), make, Profile::Doubles).size() <= 64);
  }

  // ---- the home's search (yougame_doubles_lobby.h) ----
  {
    DoublesParty party;
    CHECK(!party.observe({{"t", "lobby"}}));
    CHECK(party.observe({{"t", "lobby"}, {"party", {{"queueing", {{"id", "gen-1"}, {"queue", "casual"}, {"n", 2}}}, {"playing", false}}}}));
    CHECK(party.generation == "gen-1" && party.humans == 2 && !party.playing);
    CHECK(party.observe({{"t", "lobby"}, {"party", {{"queueing", {{"id", "gen-2"}, {"n", 3}}}}}}));   // n past 2
    CHECK(party.generation.empty() && party.humans == 0);
    CHECK(party.observe({{"t", "lobby"}, {"party", {{"queueing", nullptr}, {"playing", true}}}}));
    CHECK(party.generation.empty() && party.playing);
    QueueMessage q = queue_message({{"t", "matched"}, {"room", "match-room"}});
    CHECK(q.news == QueueNews::Matched && q.room == "match-room");
    CHECK(queue_message({{"t", "matched"}, {"room", ""}}).news == QueueNews::None);
    q = queue_message({{"t", "search-error"}, {"code", "digest_mismatch"}});
    CHECK(q.news == QueueNews::Refused && q.code == "digest_mismatch");
    CHECK(queue_message({{"t", "error"}}).code == "error");
    q = queue_message({{"t", "searching"}, {"waiting", 3}});
    CHECK(q.news == QueueNews::Searching && q.waiting == 3);
    CHECK(queue_message({{"t", "chat"}}).news == QueueNews::None);
    for (const char* t : {"teams-dissolved", "ended", "roomClosed", "kicked"}) CHECK(doubles_match_ended({{"t", t}}));
    CHECK(!doubles_match_ended({{"t", "welcome"}}));
    const uint64_t e1 = next_room_epoch(), e2 = next_room_epoch();
    CHECK(e2 > e1);
    const json people = json::array({{{"id", "a"}}});
    const json party_report = doubles_room_report("abcdefghjkmn", "abcdefghjkmn", false, e1, people, "a", 4);
    CHECK(party_report["type"] == "mpRoom" && party_report["mode"] == "teams" && party_report["size"] == 2 &&
          party_report["roomEpoch"] == e1 &&
          party_report["nativeTeams"] == json({{"v", 1}, {"kind", "party"}, {"home", "abcdefghjkmn"}, {"phase", "home"}}));
    const json match_report = doubles_room_report("match-room", "abcdefghjkmn", true, e2, people, "a", 1);
    CHECK(match_report["size"] == 4 && match_report["nativeTeams"]["kind"] == "match" &&
          match_report["nativeTeams"]["home"] == "abcdefghjkmn" && match_report["nativeTeams"]["phase"] == "assembling");
    // The page's acceptNativeTeamsRoom (web/src/lib/native-teams.ts): the home is private, the match casual;
    // once the match's setup is planned it reports again as playing (the page blocks invites then).
    CHECK(party_report["queue"] == "private" && match_report["queue"] == "casual");
    const json playing_report = doubles_room_report("match-room", "abcdefghjkmn", true, e2, people, "a", 1, true);
    CHECK(playing_report["queue"] == "casual" && playing_report["size"] == 4 && playing_report["roomEpoch"] == e2 &&
          playing_report["nativeTeams"] == json({{"v", 1}, {"kind", "match"}, {"home", "abcdefghjkmn"}, {"phase", "playing"}}));
    CHECK(doubles_room_report("abcdefghjkmn", "abcdefghjkmn", false, e1, people, "a", 4, true)["nativeTeams"]["phase"] == "home");
    // A placed match waits past Rooms' 60 s arrival and 90 s late-player hold before giving up on its setup.
    CHECK(kDoublesSetupMs > 150000);
  }

  // ---- the match, every team shape ----
  // 2+2: two friend pairs.
  same_match_everywhere(match_setup({{0, "human", "a", 2, 0}, {1, "human", "b", 9, 0}, {2, "human", "c", 20, 0}, {3, "human", "d", 1, 0}}), 4, 0);
  // 2+1+1: a pair against two solos matched together.
  same_match_everywhere(match_setup({{0, "human", "pair-a", 2, 0}, {1, "human", "pair-b", 2, 0}, {2, "human", "solo-c", 9, 0}, {3, "human", "solo-d", 9, 0}}, 5), 4, 0);
  // 1+1+1+1: four solos.
  same_match_everywhere(match_setup({{0, "human", "a", 0, 0}, {1, "human", "b", 1, 0}, {2, "human", "c", 2, 0}, {3, "human", "d", 3, 0}}, 2), 4, 0);
  // CPU 1+2: a human with a CPU partner against a pair; the CPU keeps seat 1 (team 0) on the last port.
  {
    const json f = match_setup({{0, "human", "a", 2, 0}, {1, "cpu", "cpu:1", 9, 7}, {2, "human", "c", 20, 0}, {3, "human", "d", 1, 0}}, 3);
    same_match_everywhere(f, 3, 1);
    const Plan p = plan(setup_of(f), "c", "set1", "card1", Profile::Doubles);
    CHECK(p.port[3].kind == Kind::Cpu && p.port[3].seat == 1 && p.port[3].pick.team == 0 && p.port[3].level == 7);
    CHECK(p.port[1].id == "c" && p.port[1].pick.team == 1);
  }
  // 1+1: two humans, each with a CPU partner (rooms' own example frame).
  {
    const json f = json::parse(R"({"t":"yg.mu.lobby","v":1,"op":"setup","rev":0,"matchId":"example-match","seed":1,"inputDelay":2,"seats":[{"seat":0,"port":0,"kind":"human","id":"a","name":"Player A","char":2,"color":1,"team":0,"settingsDigest":"set1","cardDigest":"card1"},{"seat":2,"port":1,"kind":"human","id":"b","name":"Player B","char":4,"color":0,"team":1,"settingsDigest":"set1","cardDigest":"card1"},{"seat":1,"port":2,"kind":"cpu","id":"cpu:1","name":"CPU 2","char":3,"color":2,"team":0,"level":7},{"seat":3,"port":3,"kind":"cpu","id":"cpu:3","name":"CPU 4","char":3,"color":2,"team":1,"level":7}],"rules":{"teams":true,"friendlyFire":false,"mode":"stock","stocks":4,"minutes":8,"pause":false},"stage":3})");
    same_match_everywhere(f, 2, 2);
    const Plan p = plan(setup_of(f), "b", "set1", "card1", Profile::Doubles);
    CHECK(p.local_port == 1 && p.port[2].seat == 1 && p.port[3].seat == 3 && p.stage == 3);
  }
  // What the Doubles profile refuses (and the Friends profile would take).
  {
    const json good = match_setup({{0, "human", "a", 2, 0}, {1, "human", "b", 9, 0}, {2, "human", "c", 20, 0}, {3, "human", "d", 1, 0}});
    CHECK(plan(setup_of(good), "a", {}, {}, Profile::Doubles).ok);
    json f = good; f["seats"][0]["team"] = 1; f["seats"][2]["team"] = 0;                 // teams off their seats
    CHECK(!plan(setup_of(f), "a", {}, {}, Profile::Doubles).ok && plan(setup_of(f), "a").ok);
    f = good; f["stage"] = 31;                                                           // not seed % 6's stage
    CHECK(!plan(setup_of(f), "a", {}, {}, Profile::Doubles).ok && plan(setup_of(f), "a").ok);
    f = good; f["rules"]["stocks"] = 3;                                                  // not the fixed rules
    CHECK(!plan(setup_of(f), "a", {}, {}, Profile::Doubles).ok && plan(setup_of(f), "a").ok);
    f = good; f["inputDelay"] = 4;
    CHECK(!plan(setup_of(f), "a", {}, {}, Profile::Doubles).ok && plan(setup_of(f), "a").ok);
    f = good; f.erase("inputDelay");                                                     // the default, 2
    CHECK(plan(setup_of(f), "a", {}, {}, Profile::Doubles).ok);
    f = good; f["seats"].erase(3);                                                       // three players
    CHECK(!plan(setup_of(f), "a", {}, {}, Profile::Doubles).ok);
    f = good; f["seats"][1]["settingsDigest"] = "set2";                                  // b's game differs
    CHECK(!plan(setup_of(f), "b", "set1", "card1", Profile::Doubles).ok);
  }
  // The new Doubles shape: friendly fire on with kDoublesStages[seed % 5]. Each shape only as a whole pair.
  {
    const json old_pair = match_setup({{0, "human", "a", 2, 0}, {1, "human", "b", 9, 0}, {2, "human", "c", 20, 0}, {3, "human", "d", 1, 0}});
    CHECK(old_pair["stage"] == 3);                                                       // seed 1
    json f = old_pair; f["rules"]["friendlyFire"] = true; f["stage"] = 8;                // seed 1 in the new list
    CHECK(plan(setup_of(f), "a", {}, {}, Profile::Doubles).ok);
    same_match_everywhere(f, 4, 0);
    json mixed = old_pair; mixed["rules"]["friendlyFire"] = true;                        // new rules, old stage
    CHECK(!plan(setup_of(mixed), "a", {}, {}, Profile::Doubles).ok);
    mixed = old_pair; mixed["stage"] = 8;                                                // old rules, new stage
    CHECK(!plan(setup_of(mixed), "a", {}, {}, Profile::Doubles).ok);
    for (uint32_t seed = 0; seed < 40; ++seed) {
      json g = match_setup({{0, "human", "a", 2, 0}, {1, "human", "b", 9, 0}, {2, "human", "c", 20, 0}, {3, "human", "d", 1, 0}}, seed);
      g["rules"]["friendlyFire"] = true;
      g["stage"] = kDoublesStages[seed % kDoublesStages.size()];
      const Plan q = plan(setup_of(g), "a", {}, {}, Profile::Doubles);
      CHECK(q.ok && q.stage != 2);                                                       // never Fountain of Dreams
    }
  }
  // A local partner is a human port sharing its owner's machine and result identity.
  {
    auto pair = [](json f, int owner, int partner) {
      const auto conn = f["seats"][owner]["id"].get<std::string>();
      f["seats"][owner]["connectionId"] = conn;
      f["seats"][partner]["connectionId"] = conn;
      f["seats"][partner]["localIndex"] = 1;
      f["seats"][partner]["id"] = "local:" + conn + ":1";
      return f;
    };
    auto build = [](const json& f) { return plan(setup_of(f), "a", "set1", "card1", Profile::Doubles); };
    const json four = match_setup({{0,"human","a",2,0},{1,"human","b",9,0},{2,"human","c",20,0},{3,"human","d",1,0}});
    const json two_pairs = pair(pair(four,0,1),2,3);
    Plan p = build(two_pairs);
    CHECK(p.ok && p.humans == 4 && p.machines == 2 && p.local_port == 0 && p.second_local_port == 1);
    CHECK(p.local_ports == std::vector<int>({0,1}));
    const auto other_pair = plan(setup_of(two_pairs), "c", "set1", "card1", Profile::Doubles);
    CHECK(other_pair.ok && other_pair.local_port == 2 && other_pair.second_local_port == 3);
    CHECK(!plan(setup_of(two_pairs), "a").ok);  // Friends never admits local seats
    json roster = json::array({{{"id","local:a:1"},{"connectionId","a"},{"localIndex",1},{"slot",1}},
      {{"id","a"},{"connectionId","a"},{"slot",0}}, {{"id","c"},{"connectionId","c"},{"slot",1}}});
    CHECK(match_roster(roster,p).size() == 2 && match_roster(roster,p)[1]["slot"] == 2);
    json wrong_owner = roster; wrong_owner[1]["connectionId"] = "other";
    CHECK(match_roster(wrong_owner,p).empty());
    Outcome o; o.end_method = 2; o.winner = 0;
    CHECK(finish_report(p,o)["ranking"][1] == "local:a:1");
    json cpu = match_setup({{0,"human","a",2,0},{1,"cpu","cpu:1",9,7},{2,"human","c",20,0},{3,"human","d",1,0}});
    cpu = pair(cpu,1,2);
    p = build(cpu);
    CHECK(p.ok && p.machines == 2 && p.humans == 3 && p.port[2].id == "local:c:1" && p.port[3].id == "cpu:1");
    const auto cpu_pair = plan(setup_of(cpu), "c", "set1", "card1", Profile::Doubles);
    CHECK(cpu_pair.ok && cpu_pair.local_port == 1 && cpu_pair.second_local_port == 2);
    json single_machine = pair(match_setup({{0,"human","a",2,0},{1,"human","b",9,0},
      {2,"cpu","cpu:2",20,7},{3,"cpu","cpu:3",1,7}}),0,1);
    CHECK(!build(single_machine).ok);
    for (const char* key : {"connectionId", "localIndex"}) {
      json f = two_pairs; f["seats"][1].erase(key); CHECK(!build(f).ok);
    }
    json missing_digest = two_pairs; missing_digest["seats"][0].erase("cardDigest"); missing_digest["seats"][1].erase("cardDigest");
    CHECK(!build(missing_digest).ok);
    json no_rev = local_seat_frame(0); no_rev.erase("rev"); CHECK(frame_text(no_rev).empty());
    json f = two_pairs; f["seats"][1]["localIndex"] = 2; CHECK(decode(f).why);
    f = two_pairs; f["seats"][1]["connectionId"] = "c"; CHECK(!build(f).ok);
    f = two_pairs; f["seats"][1]["team"] = 1; CHECK(!build(f).ok);
    f = two_pairs; f["seats"][1]["id"] = "local:wrong:1"; CHECK(!build(f).ok);
    f = two_pairs; f["seats"][1]["settingsDigest"] = "other"; CHECK(!build(f).ok);
    f = two_pairs; f["seats"][2]["connectionId"] = "a"; CHECK(!build(f).ok);
    json distinct = two_pairs;
    distinct["seats"][0]["id"] = "participant-a";
    p = plan(setup_of(distinct), "participant-a", "set1", "card1", Profile::Doubles);
    CHECK(p.ok && p.second_local_port == 1 && p.local_ports == std::vector<int>({0,1}));
    Session a(doubles_opts("a"));
    a.receive(home(1,{seat_human(0,"a",true),seat_open(1)}));
    CHECK(a.local_seat(true));
    CHECK(lobby_frames(a.due(now))[0] == local_seat_frame(1));
    json local = {{"seat",1},{"kind","local"},{"owner","a"},{"localIndex",1},{"name","Local friend"}};
    a.receive(home(2,{seat_human(0,"a",true),local}));
    CHECK(a.owns_local() && !a.set_cpu(1,5) && !a.open_seat(1) && !a.local_seat(true));
    CHECK(a.local_hover({3,1,0}) && a.local_pick({3,1,0}) && a.local_unpick());
    auto out = lobby_frames(a.due(now));
    CHECK(out.size() == 3 && out[0]["seat"] == 1 && out[1]["op"] == "pick" && out[2]["op"] == "unpick");
    Control c(Profile::Doubles); c.run(a); ScreenFlags flags; flags.armed = true;
    auto view = screen_view(c.view(),flags);
    CHECK(view.seats[1].kind == MU_LOBBY_KIND_LOCAL && mu_lobby_local_owned(&view));
    a.receive(home(3,{seat_human(0,"a",true),local},true));
    CHECK(!a.local_hover({3,1,0}) && !a.local_pick({3,1,0}) && !a.local_unpick() && !a.local_seat(false));
    a.receive(home(4,{seat_human(0,"a",true),local})); CHECK(a.local_seat(false));
    auto opt = doubles_opts("participant-a"); opt.connection_id = "a";
    Session distinct_owner(opt);
    distinct_owner.receive(home(4,{seat_human(0,"participant-a",true),local}));
    CHECK(distinct_owner.owns_local() && distinct_owner.local_pick({2,0,0}));
  }
  // ---- team colours (11-11 report): each machine's 0x82 team_id is its player's colour (red 0, blue 1,
  // green 2); a team wears its lowest machine seat's, team 1 moves off a clash; the block is the same on
  // every peer for every colour pair, a local Player 2 and a CPU wear their team's ----
  {
    auto pair = [](json f, int owner, int partner) {
      const auto conn = f["seats"][owner]["id"].get<std::string>();
      f["seats"][owner]["connectionId"] = conn;
      f["seats"][partner]["connectionId"] = conn;
      f["seats"][partner]["localIndex"] = 1;
      f["seats"][partner]["id"] = "local:" + conn + ":1";
      return f;
    };
    const json four = match_setup({{0,"human","a",2,0},{1,"human","b",9,0},{2,"human","c",20,0},{3,"human","d",1,0}});
    const json shapes[] = {
      four,                                                                             // 1+1+1+1
      pair(four, 0, 1),                                                                 // a local P2 with a, two solos
      pair(pair(four, 0, 1), 2, 3),                                                     // two local pairs
      match_setup({{0,"human","a",2,0},{1,"cpu","cpu:1",9,7},{2,"human","c",20,0},{3,"human","d",1,0}}, 3),   // a CPU partner
    };
    auto expected = [](int c0, int c1) {
      if (c1 == c0) c1 = c0 == 0 ? 1 : 0;   // the first of red, blue, green that team 0 isn't
      return std::array<uint8_t, 2>{{(uint8_t)c0, (uint8_t)c1}};
    };
    for (const json& f : shapes) {
      const Setup s = setup_of(f);
      std::vector<Plan> peers;   // every machine's own plan
      for (const auto& seat : f["seats"])
        if (seat["kind"] == "human" && seat.value("localIndex", 0) == 0)
          peers.push_back(plan(s, seat["id"].get<std::string>(), "set1", "card1", Profile::Doubles));
      CHECK(peers.size() >= 2);
      for (int c0 = 0; c0 < kTeamColors; ++c0)
        for (int c1 = 0; c1 < kTeamColors; ++c1)
          for (int other = 0; other < kTeamColors; ++other) {
            // Each machine sends a colour: the lowest seat of each team c0 / c1, its partner machine
            // `other` (outvoted). Every peer holds the same selections, by port.
            const Plan& p0 = peers[0];
            std::array<int, kSeats> sent{{-1, -1, -1, -1}};
            int lowest[2] = {-1, -1};   // each team's machine port with the lowest seat
            for (int port = 0; port < p0.ports; ++port) {
              const PortSpec& x = p0.port[port];
              if (x.kind != Kind::Human || x.local_index != 0) continue;
              int& low = lowest[x.pick.team];
              if (low < 0 || x.seat < p0.port[low].seat) low = port;
              sent[port] = other;
            }
            for (int team = 0; team < 2; ++team) if (lowest[team] >= 0) sent[lowest[team]] = team == 0 ? c0 : c1;
            const auto want = expected(c0, c1);
            std::vector<uint8_t> first;
            for (const Plan& p : peers) {
              CHECK(p.ok && p.doubles && p.ids == p0.ids);
              const auto colors = doubles_colors(p, sent);
              CHECK(colors == want && colors[0] != colors[1]);
              std::vector<uint8_t> block(kBlock, 0);
              CHECK(apply(p, block.data(), block.size(), 0, nullptr, &colors));
              for (int port = 0; port < 4; ++port) {
                CHECK(rec(block, port)[9] == want[p.port[port].pick.team]);   // P2 and CPUs wear their team's
                CHECK(rec(block, port)[1] == (p.port[port].kind == Kind::Cpu ? 1 : 0));
              }
              // Only the team bytes moved: the rest is the room-team block, costume indices included.
              std::vector<uint8_t> plain(kBlock, 0);
              CHECK(apply(p, plain.data(), plain.size()));
              for (int port = 0; port < 4; ++port) plain[0x60 + port * 0x24 + 9] = want[p.port[port].pick.team];
              CHECK(plain == block);
              if (first.empty()) first = block;
              CHECK(block == first);                                         // the same bytes on every peer
              CHECK(inputs_hash(p) == inputs_hash(p0) && setup_digest(p) == setup_digest(p0));
            }
          }
      // Nothing sent (or out of range): red against blue, the room's own numbers.
      std::array<int, kSeats> none{{-1, 7, -1, 255}};
      CHECK(doubles_colors(peers[0], none) == (std::array<uint8_t, 2>{{0, 1}}));
    }
    // A local Player 2 never decides: only its owner's selection exists (the session sends one per machine).
    {
      const Plan p = plan(setup_of(pair(four, 0, 1)), "a", "set1", "card1", Profile::Doubles);
      std::array<int, kSeats> sent{{2, 1, 0, 0}};   // port 1 is a's Player 2: never a machine's own port
      CHECK(p.port[1].local_index == 1 && doubles_colors(p, sent) == (std::array<uint8_t, 2>{{2, 0}}));
    }
    // A Friends (4-player) match keeps its teams as picked: colours are Doubles only.
    {
      json f = four; f["rules"]["friendlyFire"] = true;
      const Plan friends = plan(setup_of(f), "a");
      CHECK(friends.ok && !friends.doubles);
      std::array<int, kSeats> sent{{2, 2, 2, 2}};
      CHECK(doubles_colors(friends, sent) == (std::array<uint8_t, 2>{{0, 1}}));
      const std::array<uint8_t, 2> green{{2, 2}};
      std::vector<uint8_t> a(kBlock, 0), b(kBlock, 0);
      CHECK(apply(friends, a.data(), a.size()) && apply(friends, b.data(), b.size(), 0, nullptr, &green) && a == b);
    }
    // An old-protocol peer (kOnlineProtocol 1) never shares the room: the protocol is in every room key.
    // Were one there, it writes the room's numbers: chosen colours give it another block for the same
    // inputs, a setup hash MISMATCH (refused before frame 1), never a match that drifts; red against
    // blue gives it the same block and the same match.
    {
      CHECK(yougame::online_version("0.9.120") == "0.9.120-p" + std::to_string(yougame::kOnlineProtocol));
      CHECK(yougame::kOnlineProtocol >= 2);
      CHECK(compatibility(yougame::online_version("0.9.120"), [](const std::string& v) { return yougame::compatibility_key(v, false, ""); }, Profile::Doubles) !=
            compatibility("0.9.120-p1", [](const std::string& v) { return yougame::compatibility_key(v, false, ""); }, Profile::Doubles));
      const Plan p = plan(setup_of(four), "a", "set1", "card1", Profile::Doubles);
      auto hash = [&](const std::vector<uint8_t>& block) {
        MuSetupHash h{}; h.inputs = inputs_hash(p); h.setup = mu_fnv64_bytes(setup_digest(p), block.data(), block.size()); return h;
      };
      std::vector<uint8_t> old_block(kBlock, 0), new_block(kBlock, 0);
      CHECK(apply(p, old_block.data(), old_block.size()));                    // protocol 1: the room's numbers
      std::array<int, kSeats> sent{{2, 2, 0, 0}};                             // green against red
      auto colors = doubles_colors(p, sent);
      CHECK(apply(p, new_block.data(), new_block.size(), 0, nullptr, &colors));
      MuSetupHash mine = hash(new_block), theirs = hash(old_block);
      CHECK(mu_setup_hash_decide(1, 1, &mine, &theirs) == MU_SETUP_HASH_MISMATCH);
      std::array<int, kSeats> plain{{0, 0, 1, 1}};                            // red against blue
      colors = doubles_colors(p, plain);
      CHECK(apply(p, new_block.data(), new_block.size(), 0, nullptr, &colors));
      mine = hash(new_block);
      CHECK(mu_setup_hash_decide(1, 1, &mine, &theirs) == MU_SETUP_HASH_AGREE);
    }
  }
  std::printf("ok\n");
  return 0;
}
