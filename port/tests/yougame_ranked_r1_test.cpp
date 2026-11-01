// Offline R1 wire/presentation contract; no disc, bridge or sockets.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_room.h"
#include "yougame_mode_policy.h"
#include <cstdlib>
#include <deque>
#include <iostream>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using namespace yougame::ranked;
using namespace yougame::transport;
static json standing(int division = 4, bool placed = true, int matches = 5) {
  return {{"tier", placed ? "gold" : "unranked"}, {"division", division}, {"placed", placed},
          {"matches", matches}, {"placementTotal", 5}, {"rating", 1200}, {"order", 9 + 4 - division}};
}
static void rank_read() {
  RankCache cache;
  json answer = {{"queue", "ranked"}, {"mode", "singles"}, {"player", "a"}, {"rank", standing()}};
  CHECK(apply_rank_answer(cache, answer, cache.version()));
  CHECK(cache.header() == "Ranked - Gold IV - 1200");
  auto invalid = answer; invalid["mode"] = "teams";
  CHECK(!apply_rank_answer(cache, invalid, cache.version()));
  invalid = answer; invalid["player"] = nullptr;
  CHECK(!apply_rank_answer(cache, invalid, cache.version()));
  CHECK(!apply_rank_answer(cache, answer, cache.version() - 1));
  RankRead read;
  CHECK(!read.begin(0)); read.enter();
  CHECK(read.begin(0) && !read.begin(1)); read.finish(2, false);
  CHECK(!read.begin(600000)); // the same screen's cached read never polls again
  read.enter(true); CHECK(!read.begin(600001)); // CSS between games of this set
  read.enter(); CHECK(read.begin(600001));
  auto throttled = answer; throttled["status"] = 429; throttled["rank"]["rating"] = 9000;
  CHECK(!read.answer(cache, throttled, cache.version(), 600002));
  CHECK(cache.header() == "Ranked - Gold IV - 1200");
  read.enter(); CHECK(!read.begin(660001) && read.begin(660002));
  read.finish(660003, false);
  read.recover(700000);
  CHECK(!read.begin(719999) && read.begin(720000)); read.finish(720001,true);
  CHECK(!read.begin(740000) && read.begin(780001)); read.finish(780002,false);
  CHECK(!read.begin(1000000) && read.recovery_tries == 2);
  CHECK(apply_rank_answer(cache, {{"queue", "ranked"}, {"mode", "singles"}, {"player", nullptr},
        {"rank", nullptr}, {"reason", "signed_out"}}, cache.version()));
  CHECK(!cache.known());
  for (int division = 1; division <= 4; ++division) {
    CHECK(cache.standing(standing(division), 1250));
    const char* labels[] = {"Gold I", "Gold II", "Gold III", "Gold IV"};
    CHECK(cache.header() == std::string("Ranked - ") + labels[division - 1] + " - 1250");
  }
  answer["rank"] = standing(4, false, 0);
  CHECK(apply_rank_answer(cache, answer, cache.version()) && cache.header() == "Ranked - Placements 0/5");
  answer["rank"]["matches"] = 3;
  CHECK(apply_rank_answer(cache, answer, cache.version()) && cache.header() == "Ranked - Placements 3/5");
}
static void roster() {
  json players = {{{"slot", 1}, {"id", "b"}, {"rating", 1300}, {"rank", standing(3)}},
                  {{"slot", 0}, {"id", "a"}, {"rating", 1100}, {"rank", standing(4, false, 3)}}};
  CHECK(roster_rank(players, 1).header() == "Ranked - Gold III - 1300");
  CHECK(roster_rank_byte(players, 1) == rank_enum(standing(3)));
  CHECK(roster_rank(players, 0).header() == "Ranked - Placements 3/5");
  CHECK(roster_rank_byte(players, 0) == 0);
  players[0].erase("rank"); CHECK(roster_rank_byte(players, 1) == 0xFF);
  players[0]["rank"] = {{"placed", "bad"}}; CHECK(roster_rank_byte(players, 1) == 0xFF);
  CHECK(roster_rank_byte(json::object(), 1) == 0xFF);
  players.insert(players.begin(), {{"slot", "bad"}});
  players.insert(players.begin(), {{"slot", 0}}); // duplicate local entry with no rank
  const auto local = roster_rank(players,0); CHECK(local.known());
  RankCache cache; cache.standing(standing(),1200); const auto version=cache.version();
  cache.update(local); CHECK(cache.version()==version+1 && cache.header()==local.header());
}
struct Socket {
  std::vector<json> sent;
  std::deque<json> input;
  bool closed() const { return input.empty(); }
  bool brokered() const { return false; }
  bool recoverable() const { return false; }
  bool open_session(const std::string&, const std::string&, const std::string&, std::string*, int) { return false; }
  std::function<void(const json&)> on_send;
  bool send(const json& message) { sent.push_back(message); if (on_send) on_send(message); return true; }
  bool next(json& message, int) { if (input.empty()) return false; message = input.front(); input.pop_front(); return true; }
#ifdef YOUGAME_AGENT_TEST_HOOK
  uint64_t agent_test_first_open_at() const { return 0; }
  void agent_test_drop_room() {}
#endif
};
// The fake enforces MU's current-game admission and one immutable report per connection.
struct DuelRoom {
  RoomState<Socket> clients[2];
  struct Ledger { bool starts[2] = {}; json reports[2]; bool completed = false; };
  std::map<std::string, Ledger> games;
  std::string current, last_code;
  bool online[2] = {true, true}, completed = false, decided = false, legacy = false;
  int delivered_starts[2] = {}, delivered_reports[2] = {}, unique_reports[2] = {}, refusals = 0;
  int wins[2] = {};
  json finals[2];
  uint64_t now = 1, left_at[2] = {};
  std::mutex mutex;
  DuelRoom() {
    for (int i = 0; i < 2; ++i) {
      auto& r = clients[i]; r.ranked = true; r.me = i ? "b" : "a"; r.host_connection = "a";
      r.socket = std::make_unique<Socket>(); r.set.configure(i, {"a", "b"});
      r.socket->on_send = [this, i](const json& m) { command(i, m); };
      r.set.receive({{"t", "start"}, {"matchId", "set"}});
    }
  }
  void start(int i, const std::string& id = game_key(0x1234, 31)) { clients[i].set.start_game(id); }
  void end(int i) { Game g; g.winner = 0; clients[i].set.report_game(g); }
  static json outcome(const json& m) {
    const bool draw=m.value("draw",false), neutral=m.value("void",false);
    const auto ranking=m.count("ranking") ? m["ranking"] : str(m,"winner")=="b" ? json{"b","a"} : json{"a","b"};
    json out={{"ranking",draw || neutral ? json{"a","b"} : ranking},{"draw",draw},{"void",neutral}};
    if (m.count("detail")) out["detail"]=m["detail"];
    return out;
  }
  void refuse(int i, const json& m, const std::string& code) {
    ++refusals; last_code = code;
    json answer = {{"t", "command-error"}, {"action", m["t"]}, {"id", m.value("id", "")}, {"error", "arbitrary room text"}};
    if (!legacy) answer["code"] = code;
    clients[i].set.refused(answer, now);
  }
  void command(int i, const json& m) {
    CHECK(m["matchId"] == "set");
    const auto t = str(m, "t"), id = str(m, "id");
    if (t == "finish") { finals[i] = m; return; }
    auto& g = games[id];
    if (t == "game-start") {
      ++delivered_starts[i];
      if (decided) return refuse(i, m, "set_decided");
      if (g.completed) return refuse(i, m, "already_recorded");
      if (current != id && !current.empty() && !games.at(current).completed) return refuse(i, m, "game_active");
      current = id; g.starts[i] = true; return;
    }
    CHECK(t == "game-report"); ++delivered_reports[i];
    if (decided) return refuse(i, m, "set_decided");
    if (g.completed) return refuse(i, m, "already_recorded");
    if (current != id || !g.starts[0] || !g.starts[1]) return refuse(i, m, "not_started");
    const auto reported=outcome(m);
    if (!g.reports[i].is_null() && g.reports[i] != reported) return refuse(i, m, "result_changed");
    if (g.reports[i].is_null()) ++unique_reports[i];
    g.reports[i] = reported;
    if (!g.reports[0].is_null() && !g.reports[1].is_null()) {
      g.completed = completed = true;
      const bool agree = !g.reports[0]["void"].get<bool>() &&
          g.reports[0]["ranking"]==g.reports[1]["ranking"] && g.reports[0]["draw"]==g.reports[1]["draw"] &&
          g.reports[0]["void"]==g.reports[1]["void"];
      const auto ranking=agree ? g.reports[0]["ranking"] : json{"a","b"};
      if (agree && !g.reports[0]["draw"].get<bool>()) decided = ++wins[ranking[0]=="b"] >= kSetWins;
      for (int j = 0; j < 2; ++j) if (online[j])
        clients[j].set.receive({{"t", "game-result"}, {"matchId", "set"}, {"id", id},
                               {"ranking", ranking}, {"draw", agree && g.reports[0]["draw"].get<bool>()}, {"void", !agree}});
    }
  }
  std::vector<json> pump(int i, uint64_t at, bool success = true) {
    now = at;
    auto out = due_messages(clients[i], now);
    for (const auto& m : out) {
      if (!success) break;
      clients[i].set.sent(m); // socket acceptance precedes the queued answer
      command(i, m);
    }
    return out;
  }
  void quit(int i, uint64_t at) {
    now = at;
    clients[i].set.quit();
    CHECK(clients[i].set.leaves_at_once());
    stop_and_flush(clients[i], mutex);
    online[i] = false; left_at[i] = now;
  }
  void grace(uint64_t at) {
    now = at;
    for (int i = 0; i < 2; ++i) if (!online[i] && left_at[i] && now >= left_at[i] + kGameGraceMs) {
      const bool reached = std::any_of(games.begin(), games.end(), [i](const auto& entry) { return entry.second.starts[i]; });
      if (reached && online[1-i])
        clients[1-i].set.receive({{"t", "result"}, {"matchId", "set"}, {"ranking", i == 0 ? json{"b", "a"} : json{"a", "b"}}});
    }
  }
};
static void wire_and_score() {
  json ticket = {{"type", "mpTicket"}, {"mode", "singles"}, {"ranked", true}};
  CHECK(!set_contract_matches(ticket)); set_contract(ticket); CHECK(set_contract_matches(ticket));
  auto mismatch = ticket; mismatch["rulesVersion"] = "other"; CHECK(!set_contract_matches(mismatch));
  mismatch = ticket; mismatch["winsRequired"] = 1; CHECK(!set_contract_matches(mismatch));
  // Both report orders, including a delayed first-frame start well before the game ends.
  for (int first : {0, 1}) for (bool delayed : {false, true}) {
    DuelRoom room;
    room.start(0); CHECK(room.clients[0].set.score_text() == "Game 1 - 0:0");
    auto start = room.pump(0, 1); CHECK(start.size() == 1 && start[0]["t"] == "game-start");
    CHECK(room.delivered_reports[0] == 0 && !room.clients[0].set.awaiting_receipt());
    room.start(0); CHECK(room.pump(0, 20000).empty()); // repeated frame 1 is one start
    room.start(1); room.pump(1, delayed ? 21000 : 1);
    room.end(0); room.end(1);
    CHECK(room.clients[0].set.score_text() == "Game 2 - 0:0");
    auto report = room.pump(first, 30000); CHECK(report[0]["id"] == start[0]["id"]);
    room.pump(1-first, 30000);
    CHECK(room.completed && room.refusals == 0);
    CHECK(room.clients[0].set.games_received() == 1 && room.clients[1].set.games_received() == 1);
  }
  // Backpressure of either command leaves it unsent; retry spacing bounds pump attempts.
  for (bool fail_start : {false, true}) {
    DuelRoom room; room.start(0); room.start(1); room.pump(1, 1);
    if (!fail_start) room.pump(0, 1);
    if (!fail_start) room.end(0);
    auto failed = room.pump(0, 2, false); CHECK(failed.size() == 1);
    CHECK(room.pump(0, 2001).empty());
    auto retry = room.pump(0, 2002); CHECK(retry == failed);
    if (fail_start) { room.end(0); room.pump(0, 2100); }
    room.end(1); room.pump(1, 2200);
    CHECK(room.completed && room.refusals == 0 && room.delivered_starts[0] == 1 && room.delivered_reports[0] == 1);
  }
  // The room records both reports while A is away; its replayed start acknowledges completion.
  {
    DuelRoom room; for (int i : {0,1}) { room.start(i); room.pump(i,1); room.end(i); }
    room.pump(0,2); room.online[0]=false; room.pump(1,3);
    CHECK(room.clients[0].set.awaiting_receipt() && room.completed);
    room.online[0]=true; room.clients[0].set.rejoined();
    CHECK(room.pump(0,4)[0]["t"] == "game-start");
    CHECK(room.pump(0,5).empty());
    CHECK(room.delivered_reports[0]==1 && room.clients[0].set.awaiting_receipt());
    CHECK(room.clients[0].set.games_received()==0 && room.last_code=="already_recorded");
    room.clients[0].set.rejoined(); CHECK(room.pump(0,10000).empty());
    CHECK(room.delivered_starts[0]==2 && room.refusals==1);
    // A refusal never invents the missing winner. Only the actual room outcome
    // can resume play (today's room has no API to request this lost receipt).
    room.clients[0].set.receive({{"t","game-result"},{"matchId","set"},
      {"id",game_key(0x1234,31)},{"ranking",{"a","b"}}});
    CHECK(room.clients[0].set.score_text()=="Game 2 - 1:0");
    for (int i : {0,1}) { room.start(i); room.pump(i,10001); room.end(i); }
    room.pump(0,10002); room.pump(1,10002);
    room.pump(0,10003); room.pump(1,10003);
    CHECK(room.wins[0]==2 && room.finals[0]["winner"]=="a" && room.finals[1]["winner"]=="a");
    for (int i : {0,1}) {
      room.clients[i].set.receive({{"t","result"},{"matchId","set"},{"ranking",{"a","b"}}});
      CHECK(room.clients[i].set.settled() && room.clients[i].set.release_pair());
      CHECK(room.clients[i].set.score_text()==(i==0 ? "Final score - 2:0" : "Final score - 0:2"));
    }
  }
  // An unanswered current start is also replayable exactly once per successful rejoin.
  {
    DuelRoom room; room.start(0); room.pump(0,1);
    room.clients[0].set.rejoined(); CHECK(room.pump(0,2)[0]["t"] == "game-start");
    CHECK(room.pump(0,10000).empty() && room.delivered_starts[0] == 2);
  }
  // A late peer start allows the one delayed not_started retry to succeed.
  {
    DuelRoom room; room.start(0); room.pump(0,1); room.end(0); room.pump(0,2);
    CHECK(room.refusals==1 && room.clients[0].set.awaiting_receipt());
    room.start(1); room.pump(1,3); room.clients[0].set.rejoined();
    CHECK(room.pump(0,2001).empty());
    CHECK(room.pump(0,2002)[0]["t"]=="game-report");
    // Repeated recovery after a successfully sent retry must not spend it again.
    for (uint64_t at : {2002u, 2003u, 4002u, 8000u}) {
      room.clients[0].set.rejoined(); CHECK(room.pump(0,at).empty());
    }
    CHECK(room.delivered_reports[0]==2);
    room.end(1); room.pump(1,8001);
    CHECK(room.completed && room.delivered_reports[0]==2 && room.clients[0].set.games_received()==1);
  }
  // Killing game 1 before any report is a loss once the leaver's first-frame start arrived.
  for (int leaver : {0,1}) {
    DuelRoom room; room.start(leaver); room.pump(leaver,1); room.quit(leaver,2);
    CHECK(room.finals[leaver]["winner"]==(leaver==0?"b":"a"));
    CHECK(!room.clients[1-leaver].set.settled());
    room.grace(2+kGameGraceMs);
    CHECK(!room.completed && room.clients[1-leaver].set.settled() && room.delivered_reports[leaver]==0);
  }
  // Explicit start refusals prevent both commands, even with a later end and reconnect.
  {
    DuelRoom room; room.start(0); room.pump(0,1);
    room.clients[0].set.refused({{"action","game-start"},{"id",game_key(0x1234,31)},{"error","arbitrary wording"}});
    room.end(0); room.clients[0].set.rejoined(); CHECK(room.pump(0,10000).empty());
  }
  // A room receipt decides the score; the decided set shows the final score with no next game.
  Set score; score.configure(0,{"a","b"}); score.receive({{"t","start"},{"matchId","set"}});
  for (int i=1; i<=3; ++i) {
    score.start_game(); Game g; score.report_game(g);
    score.receive({{"t","game-result"},{"matchId","set"},{"id","g"+std::to_string(i)+"-t0"},{"ranking",i==2?json{"b","a"}:json{"a","b"}}});
  }
  CHECK(score.score_text()=="Final score - 2:1");
  score.room_closed(); CHECK(score.score_text()=="Set ended");
  Set orphan; orphan.start_game(); orphan.due(1,false,7); orphan.due(1+kStartWaitMs,false,7);
  CHECK(orphan.score_text()=="Set ended");
  Set no_games; no_games.configure(0,{"a","b"}); no_games.start_game();
  no_games.receive({{"t","start"},{"matchId","set"}}); no_games.quit();
  CHECK(no_games.score_text()=="Set ended");
  // At-once flush uses the same queue and send-success boundary; it cannot invent another start.
  RoomState<Socket> flush; flush.socket=std::make_unique<Socket>();
  flush.set.configure(0,{"a","b"}); flush.set.start_game("flush"); flush.set.receive({{"t","start"},{"matchId","set"}});
  Game g; flush.set.report_game(g); std::mutex mutex; stop_and_flush(flush,mutex);
  CHECK(flush.socket->sent.size()==2 && flush.socket->sent[0]["t"]=="game-start" && flush.socket->sent[1]["t"]=="game-report");
  stop_and_flush(flush,mutex); CHECK(flush.socket->sent.size()==2);
}
static void room_admission() {
  DuelRoom room;
  const std::string id = game_key(0x1234,31);
  room.start(0); room.start(1); room.pump(0,1); room.pump(1,1);
  room.command(0, {{"t","game-start"},{"matchId","set"},{"id","other"}});
  CHECK(room.last_code=="game_active" && !room.games.at("other").starts[0]);
  room.end(0); const auto report = room.pump(0,2)[0];
  room.command(0, report); CHECK(room.unique_reports[0]==1); // identical overwrites, never a second report
  auto same=report; same.erase("winner"); same["ranking"]={"a","b"}; room.command(0,same);
  CHECK(room.unique_reports[0]==1); // the same normalized result, even in another wire form
  auto changed = report; changed["winner"]="b"; room.command(0,changed);
  CHECK(room.last_code=="result_changed" && room.games.at(id).reports[0]==DuelRoom::outcome(report));
  room.end(1); room.pump(1,3); CHECK(room.completed && room.wins[0]==1);
  room.command(0, report); CHECK(room.last_code=="already_recorded" && room.wins[0]==1);
  room.command(0, {{"t","game-start"},{"matchId","set"},{"id",id}});
  CHECK(room.last_code=="already_recorded");
  // The same negotiated RNG/stage in game 2 must still name a distinct room game.
  for (int i : {0,1}) { room.start(i); room.pump(i,4); room.end(i); }
  room.pump(0,5); room.pump(1,5);
  CHECK(room.decided && room.wins[0]==2 && room.games.at(id+"-g2").completed);
  room.command(0, {{"t","game-start"},{"matchId","set"},{"id","third"}});
  CHECK(room.last_code=="set_decided" && !room.games.at("third").starts[0]);
  DuelRoom neutral;
  for (int i : {0,1}) { neutral.start(i); neutral.pump(i,1); Game g; neutral.clients[i].set.report_game(g); }
  neutral.pump(0,2); neutral.pump(1,2);
  CHECK(neutral.completed && !neutral.decided && neutral.wins[0]==0 && neutral.wins[1]==0);
  CHECK(neutral.clients[0].set.score_text()=="Game 2 - 0:0");
}
// A's second refusal is at 2 s; B's report is accepted at 4 s, so the
// room's real report deadline is 19 s, NOT A's old 17 s guess.
static void delayed_room_deadline() {
  for (bool room_answers : {false, true}) {
    DuelRoom room;
    room.start(0); room.pump(0,1); room.end(0); room.pump(0,2);
    room.pump(0,2002); CHECK(room.refusals==2);
    room.start(1); room.pump(1,4000); room.end(1); room.pump(1,4001);
    CHECK(room.games.at(game_key(0x1234,31)).reports[0].is_null());
    CHECK(!room.games.at(game_key(0x1234,31)).reports[1].is_null());
    room.start(0, "next"); // queued gameplay must never get ahead of the room
    CHECK(room.pump(0,17002).empty() && room.clients[0].set.awaiting_receipt());
    CHECK(room.delivered_starts[0]==1 && !room.clients[0].set.release_pair());
    if (room_answers) {
      // The room voids the lone report at its acceptance-based deadline.
      room.now=19001;
      room.games.at(game_key(0x1234,31)).completed=true;
      for (int i : {0,1}) room.clients[i].set.receive({{"t","game-result"},
        {"matchId","set"},{"id",game_key(0x1234,31)},{"ranking",{"a","b"}},{"void",true}});
      CHECK(room.pump(0,19001)[0]["id"]=="next");
      CHECK(room.refusals==2 && room.delivered_starts[0]==2);
    } else {
      CHECK(room.pump(0,2002+kRecoveryWaitMs-1).empty());
      auto final=room.pump(0,2002+kRecoveryWaitMs);
      CHECK(final.size()==1 && final[0]["t"]=="finish" && final[0]["void"]==true);
      CHECK(room.delivered_starts[0]==1 && room.clients[0].set.release_pair());
      CHECK(room.clients[0].set.games_received()==0 && !room.clients[0].set.settled());
      CHECK(room.clients[0].set.score_text()=="Set ended");
    }
  }
}
static void results() {
  ResultsPanel panel; CHECK(panel.state() == ResultsPanel::State::Empty);
  panel.pending("set"); CHECK(panel.rating_text() == "Result being recorded");
  json event = {{"t", "result"}, {"matchId", "set"}, {"queue", "ranked"}, {"ratingDisposition", {{"rated", true}}},
    {"ratings", {{"a", {{"before", 1200}, {"after", 1216}, {"matches", 5}, {"rankBefore", standing(4)}, {"rank", standing(3)}}}}}};
  auto wrong = event; wrong["matchId"] = "other"; CHECK(!panel.receive(wrong, "a"));
  wrong = event; wrong["t"] = "game-result"; CHECK(!panel.receive(wrong, "a"));
  wrong = event; wrong["ratings"] = nullptr; CHECK(!panel.receive(wrong, "a"));
  CHECK(panel.rating_text() == "Result being recorded");
  CHECK(panel.receive(event, "a") && panel.rating_text() == "1200 -> 1216 (+16)" && panel.change_text() == "Promotion: Gold III");
  panel.pending("set"); CHECK(panel.state() == ResultsPanel::State::Rated);
  CHECK(!panel.receive(event, "a"));
  panel = {}; panel.pending("set"); event["ratings"]["a"]["after"] = 1184; event["ratings"]["a"]["rankBefore"] = standing(2);
  CHECK(panel.receive(event, "a") && panel.change_text() == "Demotion: Gold III" && panel.rating_text() == "1200 -> 1184 (-16)");
  panel = {}; panel.pending("set"); event["ratings"]["a"]["rankBefore"] = standing(4, false, 4);
  CHECK(panel.receive(event, "a") && panel.change_text() == "Placed: Gold III");
  panel = {}; panel.pending("set"); event["ratings"]["a"]["rank"] = standing(4, false, 3);
  CHECK(panel.receive(event, "a") && panel.change_text() == "Placements 3/5");
  panel = {}; panel.pending("set"); event["ratingDisposition"] = {{"rated", false}, {"reason", "pack"}}; event["ratings"] = nullptr;
  CHECK(panel.receive(event, "a") && panel.state() == ResultsPanel::State::Unrated && panel.rating_text() == "Unrated: custom content");
  panel = {}; panel.pending("set"); event["ratingDisposition"]["reason"] = "malicious raw server text\n";
  CHECK(panel.receive(event,"a") && panel.rating_text()=="Unrated");
  panel = {}; panel.pending("set"); RankCache cache; cache.standing(standing(),1200);
  panel.recovered_standing("other",cache,false); CHECK(panel.state()==ResultsPanel::State::Pending);
  panel.recovered_standing("set",cache,false); CHECK(panel.rating_text()=="Current standing");
  CHECK(panel.change_text()=="Gold IV - 1200");
  event["ratingDisposition"]={{"rated",false},{"reason","pack"}};
  CHECK(panel.receive(event,"a") && panel.rating_text()=="Unrated: custom content"); // a late receipt still wins
  panel={}; panel.pending("set"); panel.recovered_standing("set",RankCache(),false);
  CHECK(panel.state()==ResultsPanel::State::Pending);
  panel.recovered_standing("set",RankCache(),true); CHECK(panel.rating_text()=="Result unavailable");
  CHECK(log_text("bad\n\x1b\xff",160)=="bad???" && log_text(std::string(500,'x'),160).size()==160);
  CHECK(!yougame::mode_policy::yougame_ranked_offered);
}
int main() { rank_read(); roster(); wire_and_score(); room_admission(); delayed_room_deadline(); results(); std::cout << checks << " checks; R1: standing/cache, roster, score, ordered reports/refusal, authoritative results, hidden row passed\n"; }
