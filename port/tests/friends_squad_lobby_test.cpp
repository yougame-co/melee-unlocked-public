/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#include "yougame_lobby_setup.h"
#include "yougame_lobby_screen.h"
#include "yougame_compatibility.h"
#if MU_SQUAD
#include "yougame_squad.h"
#endif
#include <cstdio>
#include <cstdlib>
using namespace yougame::lobby;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
const std::string budget(64, 'a');
static std::vector<Pick> line(int first = 0, int count = 4) {
  std::vector<Pick> out;
  for (int i = 0; i < count; ++i) out.push_back({first+i, i%2, 0});
  return out;
}
static json room(bool squad = true) {
  Rules r; r.squad = squad;
  json seats = json::array();
  for (int i = 0; i < 4; ++i) {
    json seat = {{"seat", i}, {"kind", i < 2 ? "human" : "open"}};
    if (i < 2) {
      seat["id"] = i ? "b" : "a"; seat["name"] = seat["id"];
      seat["lineup"] = lineup_json(line(i*4));
      seat["pick"] = {{"char", i*4}, {"color", 0}, {"team", 0}};
    }
    seats.push_back(seat);
  }
  return {{"t", kType}, {"v", 1}, {"op", "state"}, {"rev", 1}, {"host", 0},
          {"squadProtocol", 1}, {"seats", seats}, {"rules", rules_json(r)}, {"setup", nullptr}};
}
static json frozen(bool squad = true) {
  json r = room(squad);
  r["op"] = "setup"; r["matchId"] = "squad-test"; r["seed"] = 7; r["stage"] = 31;
  r.erase("squadProtocol"); r.erase("host"); r.erase("setup");
  r["seats"].erase(3); r["seats"].erase(2);
  if (squad) { r["squadVersion"] = 3; r["squadBudget"] = budget; }
  for (int i = 0; i < 2; ++i) {
    auto& s = r["seats"][i]; s.erase("pick"); s["port"] = i;
    s["char"] = i*4; s["color"] = 0; s["team"] = 0;
    if (!squad) { s.erase("squadVersion"); s.erase("squadBudget"); s.erase("lineup"); }
  }
  return r;
}
static Plan build(json f) {
  const auto in = decode(f);
  CHECK(in.op == Op::Setup && !in.why);
  return plan(in.setup, "b", {}, {}, Profile::Friends4, budget);
}
static Session::Options options(const char* id = "a", Profile p = Profile::Friends4) {
  Session::Options o; o.me = id; o.profile = p; o.settings_digest = "settings"; o.card_digest = "card"; o.input_delay = 3; return o;
}
static std::vector<json> frames(Session& s) {
  std::vector<json> out;
  for (const auto& f : s.due(100000)) if (f.value("t", "") == kType) out.push_back(f);
  return out;
}
int main() {
  CHECK(std::string(mu_lobby_hint_text(refusal_hint("squad_two_players", "start"))) == "Squad Strike is 2 players for now.");
  CHECK(std::string(mu_lobby_hint_text(refusal_hint("squad_capability", "start"))) == "Your friend needs the latest update for Squad Strike.");
  CHECK(std::string(mu_lobby_hint_text(refusal_hint("squad_budget", "start"))) == "These two games can't play Squad Strike together.");
  CHECK(std::string(mu_lobby_hint_text(refusal_hint("squad_unavailable", "rules"))) == "Squad Strike isn't available in this room yet.");
  CHECK(refusal_hint("squad_rules", "rules") == MU_LOBBY_HINT_SQUAD_RULES);
  // One codec accepts partial lineups and rejects malformed members everywhere it is used.
  for (int n = 0; n <= 4; ++n) {
    const auto f = lineup_frame(1, line(0, n), 4);
    CHECK(valid_outgoing(f) && !frame_text(f).empty());
    auto st = room(); st["seats"][1]["lineup"] = f["members"];
    const auto in = decode(st); CHECK(!in.why && in.state.seats[1].picked == (n == 4));
  }
  for (const json& members : {json(), json::object(), json::array({{{"char", 26}, {"color", 0}}}),
       json::array({{{"char", 0}, {"color", 6}}}), json::array({{{"char", -1}, {"color", 0}}}),
       json::array({{{"char", 0.5}, {"color", 0}}}), json::array({{{"char", 0}, {"color", false}}}),
       json::array({{{"char", 0}, {"color", 0}, {"extra", 1}}}),
       lineup_json(line(0, 5)), json::array({{{"char", 0}, {"color", 0}}, {{"char", 0}, {"color", 1}}})}) {
    auto f = lineup_frame(1, {}, 1); f["members"] = members; CHECK(!valid_outgoing(f));
    auto st = room(); st["seats"][1]["lineup"] = members; CHECK(decode(st).why);
    auto setup = frozen(); setup["seats"][1]["lineup"] = members; CHECK(decode(setup).why);
  }
  auto f = lineup_frame(1, line(), 1); f.erase("rev"); CHECK(!valid_outgoing(f));
  f = lineup_frame(1, line(), 1); f["rev"] = -1; CHECK(!valid_outgoing(f));
  f = lineup_frame(1, line(), 1); f["seat"] = 4; CHECK(!valid_outgoing(f));
  f = lineup_frame(1, line(), 1); f["extra"] = true; CHECK(!valid_outgoing(f));
  f = hello_frame("settings", "card"); CHECK(valid_outgoing(f));
  f["squadVersion"] = 3; f["squadBudget"] = budget; CHECK(valid_outgoing(f));
  for (const auto& bad : {std::string(64, 'A'), std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'g')}) {
    auto h = f; h["squadBudget"] = bad; CHECK(!valid_outgoing(h));
    auto st = room(); st["seats"][0]["squadBudget"] = bad; CHECK(decode(st).why);
  }
  for (const char* missing : {"squadVersion", "squadBudget"}) { auto h = f; h.erase(missing); CHECK(!valid_outgoing(h)); }
  f["squadVersion"] = 2; CHECK(!valid_outgoing(f));
  auto st = room(); st["squadProtocol"] = true; CHECK(decode(st).why);
  st["squadProtocol"] = 2; CHECK(decode(st).why);
  for (const char* key : {"teams", "mode", "stocks", "squad"}) {
    auto r = rules_frame(Rules{}, 1); r["rules"]["squad"] = true;
    r["rules"][key] = std::string(key) == "teams" ? json(true) : std::string(key) == "mode" ? json("time") : json(3);
    CHECK(!valid_outgoing(r)); st = room(); st["rules"] = r["rules"]; CHECK(decode(st).why);
  }
  // Local admission AND the room advertisement gate the extended hello, in either arrival order.
  for (bool admit_first : {false, true}) {
    Session s(options()); st = room(false); st.erase("squadProtocol");
    if (admit_first) s.squad_admission(budget);
    CHECK(s.receive(st)); auto out = frames(s); CHECK(out.size() == 1 && !out[0].count("squadVersion"));
    if (!admit_first) s.squad_admission(budget);
    CHECK(frames(s).empty()); st["squadProtocol"] = 1; s.receive(st); out = frames(s);
#if MU_SQUAD
    CHECK(out.size() == 1 && out[0]["squadVersion"] == 3 && out[0]["squadBudget"] == budget);
#else
    CHECK(out.empty() && !s.squad_available());
#endif
  }
#if MU_SQUAD
  Session hello(options()); hello.squad_admission(budget); hello.receive(room()); frames(hello);
  st = room(); st["error"] = "shape"; st["action"] = "hello"; hello.receive(st);
  auto plain = frames(hello);
  CHECK(hello.hello_supported() && plain.size() == 1 && plain[0]["op"] == "hello" && !plain[0].count("squadVersion"));
  st.erase("error"); st.erase("action"); hello.receive(st); CHECK(frames(hello).empty());
  st["error"] = "shape"; st["action"] = "hello"; hello.receive(st);
  CHECK(!hello.hello_supported() && frames(hello).empty());
  Plan p = build(frozen()); CHECK(p.ok && p.local_port == 1 && p.ports == 2);
  MuSquadSetup squad{}; CHECK(squad_setup(p, &squad));
  for (int side = 0; side < 2; ++side) for (int i = 0; i < 4; ++i)
    CHECK(squad.sides[side].members[i].kind == side*4+i && squad.sides[side].members[i].color == i%2);
  CHECK(squad.stocks == 4 && finish_report(p, Outcome{})["setup"] == p.frozen);
  auto moved = frozen(); moved["seats"][0]["seat"] = 1; moved["seats"][1]["seat"] = 3;
  const auto sparse = build(moved); CHECK(sparse.ok && sparse.local_port == 1);
  MuSquadSetup sparse_squad{}; CHECK(squad_setup(sparse, &sparse_squad));
  CHECK(std::memcmp(&squad, &sparse_squad, sizeof squad) == 0);
  auto changed = frozen(); changed["seats"][1]["lineup"][3]["char"] = 12;
  CHECK(setup_digest(p) != setup_digest(build(changed)));
  changed = frozen(); changed["seats"][1]["lineup"][3]["color"] = 2;
  CHECK(setup_digest(p) != setup_digest(build(changed)));
  for (int n : {1, 3, 4}) {
    auto bad = frozen(); if (n == 1) bad["seats"].erase(1);
    while (bad["seats"].size() < size_t(n)) { auto seat = bad["seats"][0]; seat["seat"] = bad["seats"].size(); bad["seats"].push_back(seat); }
    CHECK(build(bad).why == "squad_two_players");
  }
  changed = frozen(); changed["seats"][1]["kind"] = "cpu"; changed["seats"][1]["level"] = 4;
  CHECK(build(changed).why == "squad_two_players");
  changed = frozen(); changed["seats"][1]["localIndex"] = 1; CHECK(build(changed).why == "squad_two_players");
  changed = frozen(); changed.erase("squadBudget"); changed.erase("squadVersion"); CHECK(build(changed).why == "squad_capability");
  changed = frozen(); changed["squadBudget"] = std::string(64, 'b'); CHECK(build(changed).why == "squad_budget");
  changed = frozen(); changed["seats"][1].erase("squadBudget"); changed["seats"][1].erase("squadVersion"); CHECK(build(changed).ok);
  changed = frozen(); changed["seats"][1]["lineup"].erase(3); CHECK(build(changed).why == "pick_required");
  changed = frozen(); changed["seats"][1]["char"] = 3; CHECK(build(changed).why == "squad first fighter");
  auto setup = decode(frozen()).setup;
  setup.rules.teams = true; CHECK(plan(setup, "a", {}, {}, Profile::Friends4, budget).why == "squad_rules");
  setup.rules.teams = false; setup.rules.time = true; CHECK(plan(setup, "a", {}, {}, Profile::Friends4, budget).why == "squad_rules");
  setup.rules.time = false; setup.rules.stocks = 3; CHECK(plan(setup, "a", {}, {}, Profile::Friends4, budget).why == "squad_rules");
  // Host cycles three states; rules return with the host's prior timer, pause and stocks.
  Session host(options()); st = room(false); st["rules"]["stocks"] = 3; st["rules"]["minutes"] = 0; st["rules"]["pause"] = true;
  host.squad_admission(budget); host.receive(st); frames(host);
  CHECK(host.cycle_mode()); auto out = frames(host); CHECK(out.size() == 1 && out[0]["rules"]["teams"] == true);
  st["rules"] = out[0]["rules"]; st["rev"] = st["rev"].get<int>() + 1; host.receive(st); CHECK(host.cycle_mode()); out = frames(host);
  CHECK(out.size() == 1 && out[0]["rules"]["squad"] == true && out[0]["rules"]["stocks"] == 4 && out[0]["rules"]["teams"] == false);
  st["rules"] = out[0]["rules"]; st["rev"] = st["rev"].get<int>() + 1; host.receive(st); CHECK(host.cycle_mode()); out = frames(host);
  CHECK(out.size() == 1 && !out[0]["rules"].count("squad") && out[0]["rules"]["teams"] == false && out[0]["rules"]["stocks"] == 3 && out[0]["rules"]["minutes"] == 0 && out[0]["rules"]["pause"] == true);
  Session guest(options("b")); guest.squad_admission(budget); guest.receive(room()); frames(guest);
  CHECK(!guest.cycle_mode() && !guest.pick({1,0,0}) && !guest.unpick());
  CHECK(guest.lineup(line(8, 2))); out = frames(guest); CHECK(out.size() == 1 && out[0]["op"] == "lineup" && out[0]["seat"] == 1);
  st = room(); st["rev"] = 9; st["error"] = "revision"; st["action"] = "lineup"; guest.receive(st);
  out = frames(guest); CHECK(out.size() == 1 && out[0]["rev"] == 9 && out[0]["members"].size() == 2);
  CHECK(guest.lineup({})); out = frames(guest); CHECK(out.size() == 1 && out[0]["members"].empty());
  st = room(); st["setup"] = frozen(); guest.receive(st); frames(guest);
  CHECK(!guest.lineup(line()) && !guest.pick({1,0,0}) && !guest.unpick());
  // Refused starts are never retried as an ordinary start (including old input-delay fallback).
  for (const char* error : {"shape", "revision", "squad_two_players", "squad_capability", "squad_budget", "pick_required", "squad_rules"}) {
    Session h(options()); h.squad_admission(budget); h.receive(room()); frames(h);
    CHECK(h.start(31)); out = frames(h); CHECK(out.size() == 1 && out[0]["op"] == "start");
    st = room(); st["error"] = error; st["action"] = "start"; h.receive(st);
    CHECK(frames(h).empty() && h.state().rules.squad && !h.has_setup() && h.refusal() == error);
    Control c; c.run(h); ScreenFlags sf; sf.armed = true;
    CHECK(screen_view(c.view(), sf).refusal_hint != MU_LOBBY_HINT_NONE);
  }
  for (int roster : {3, 4, 5}) {
    Session h(options()); h.squad_admission(budget); st = room();
    if (roster == 5) { st["seats"][1] = {{"seat",1},{"kind","cpu"},{"level",5}}; }
    else for (int i = 2; i < roster; ++i) st["seats"][i] = {{"seat",i},{"kind","human"},{"id", std::to_string(i)}};
    h.receive(st); frames(h); CHECK(h.can_start() == StartBlock::SquadTwoPlayers && !h.start(31)); CHECK(frames(h).empty());
    CHECK(std::string(mu_lobby_hint_text(MU_LOBBY_HINT_SQUAD_TWO)) == "Squad Strike is 2 players for now.");
    Control control; ScreenFlags flags; flags.armed = true; control.run(h);
    CHECK(screen_view(control.view(), flags).block_hint == MU_LOBBY_HINT_SQUAD_TWO);
    st["rules"]["squad"] = false; st["rules"]["teams"] = true; h.receive(st); CHECK(h.cycle_mode());
    CHECK(h.refusal() == "squad_two_players" && frames(h).size() == 1);
  }
  Session ready(options()); ready.squad_admission(budget); ready.receive(room());
  CHECK(ready.can_start() == StartBlock::None);
  st = room(); st["seats"][1]["lineup"].erase(3); ready.receive(st);
  CHECK(ready.can_start() == StartBlock::PickRequired);
  auto team_pick = decode(frozen()).setup;
  team_pick.seats[1].lineup.front().team = 2;
  CHECK(plan(team_pick, "b", {}, {}, Profile::Friends4, budget).ok);
  team_pick.seats[1].pick.color = 3;
  CHECK(plan(team_pick, "b", {}, {}, Profile::Friends4, budget).why == "squad first fighter");
#else
  CHECK(!build(frozen()).ok);
#endif
  // Missing own admission, an old room, and OFF builds explain unavailability to a guest.
  for (bool protocol : {false, true}) {
    Session missing(options("b")); st = room();
    if (!protocol) st.erase("squadProtocol");
    missing.receive(st); frames(missing);
    CHECK(!missing.lineup(line()) && missing.refusal() == "squad_unavailable");
    Control c; c.run(missing); ScreenFlags sf; sf.armed = true;
    const auto v = screen_view(c.view(), sf);
    CHECK(v.block_hint == MU_LOBBY_HINT_SQUAD_UNAVAILABLE && v.refusal_hint == MU_LOBBY_HINT_SQUAD_UNAVAILABLE);
  }
  Session old(options()); old.squad_admission(budget); st = room(false); st.erase("squadProtocol"); st["rules"]["teams"] = true; old.receive(st); frames(old);
  CHECK(old.cycle_mode() && old.refusal() == "squad_unavailable");
  CHECK(frames(old)[0]["rules"]["teams"] == false);
  Session doubles(options("a", Profile::Doubles)); doubles.squad_admission(budget);
  CHECK(!doubles.squad_available() && !doubles.cycle_mode());
  CHECK(decode(room(), Profile::Doubles).why);
  CHECK(decode(frozen(), Profile::Doubles).why);
  // Captured pre-Squad ordinary setup hash: no added defaults or changed serialization.
#if MU_SQUAD
  for (int mode = 0; mode < 4; ++mode)
    CHECK(yougame::squad::compatibility("0.9.96", false, "", false, "", "", mode, false, "") == "mu-0.9.96-sq");
#endif
  for (auto profile : {Profile::Friends4, Profile::Doubles})
    CHECK(compatibility("0.9.96", [](const std::string& v) {
      return yougame::compatibility_key(v, false, "", false, "", "");
    }, profile) == (profile == Profile::Friends4 ? "friends-lobby-1:mu-0.9.96" : "doubles-lobby-1:mu-0.9.96"));
  const auto ordinary = build(frozen(false)); CHECK(ordinary.ok && !ordinary.rules.squad);
  CHECK(setup_digest(ordinary) == UINT64_C(0xec0efc0da7c0cce9));
  CHECK(inputs_hash(ordinary) == UINT64_C(0xeb66d68754fc8af4));
  auto teams = frozen(false); teams["rules"]["teams"] = true; teams["seats"][1]["team"] = 1;
  CHECK(setup_digest(build(teams)) == UINT64_C(0xef24182290d9c7e2));
  std::puts("friends squad lobby: PASS");
}
