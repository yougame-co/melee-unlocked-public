// The retained-home coordinator on the 4-peer route: a match found by the home sends each seat to its own
// Teams character select (no console admission), and the home comes back when the game leaves it.
#define MU_DOUBLES_FOUR_PEER 1
#define DOUBLES_RUNTIME_FIXTURE_ONLY
#include "yougame_doubles_runtime_test.cpp"
namespace {
// What doubles_tick does on the simulation thread when the run thread retires the match.
void serve_stop() {
  using namespace yougame;
  wait_until([] { std::lock_guard<std::mutex> lock(doubles::ui.mutex); return doubles::ui.stop_match; });
  std::lock_guard<std::mutex> lock(doubles::ui.mutex);
  doubles::ui.stop_match=false; doubles::ui.stopped=true;
}
std::string search_room() { return yougame::doubles::ui.teams_search_room(); }
void clear_log() { std::lock_guard<std::mutex> lock(host::log_mutex); host::log_lines.clear(); }
void send_home(const yougame::json& m) { std::lock_guard<std::mutex> lock(fake_mutex); home_messages.push_back(m); }
// The game is back on the main menu (doubles_tick): the home returns, the Teams state goes.
void game_back_on_menu(int returns_before) {
  using namespace yougame;
  doubles::ui.command("match-ended");
  serve_stop();
  wait_until([returns_before] { return returns==returns_before+1; });
  CHECK(search_room().empty());
  std::lock_guard<std::mutex> lock(doubles::ui.mutex);
  CHECK(doubles::ui.teams_room.empty()); CHECK(!doubles::ui.teams_enter);
  CHECK(!doubles::ui.teams_started); CHECK(!doubles::ui.teams_settled); CHECK(doubles::ui.teams_failed==0);
}
void four_peer_match_and_return() {
  using namespace yougame;
  reset_fixture(); humans=2;
  doubles::entry("new",true,2);
  for(int round=0;round<2;++round) {
    wait_until([] { return search_room()=="matchaaa"; });
    {
      std::lock_guard<std::mutex> lock(doubles::ui.mutex);
      CHECK(doubles::ui.playing); CHECK(!doubles::ui.foreground); CHECK(doubles::ui.launch==nullptr);
      CHECK(doubles::ui.teams_team==-1); // this fixture's matched frame names no slot or team
    }
    // The console admission never runs: no match ticket from the coordinator, no ConsoleBackend.
    CHECK(!admission_entered);
    {
      std::lock_guard<std::mutex> lock(fake_mutex);
      for(const auto& q:tickets) {
        CHECK(q.value("room","")!="matchaaa");
        CHECK(q["lobby"]["compatibility"]=="mu:slippi-mode-3"); // the home, its search and the match share the Teams key
      }
    }
    // The online menu opens the Teams character select once, and only when the game is idle.
    CHECK(doubles::ui.take_teams_entry(false)[0]==0);
    const auto entry=doubles::ui.take_teams_entry(true);
    CHECK(entry[0]==1); CHECK(entry[1]==0xFF);
    CHECK(doubles::ui.take_teams_entry(true)[0]==0);
    // Nothing settles before the room's start; another room's start or result is not this match's.
    doubles::ui.settle_teams("matchaaa"); CHECK(!doubles::ui.teams_over());
    doubles::ui.teams_start("matchaab"); doubles::ui.settle_teams("matchaab"); CHECK(!doubles::ui.teams_over());
    doubles::ui.teams_start("matchaaa"); // find_match read the room's start
    // After the result Rooms resets the room: a normal end. The four stay on their results, the select
    // never searches the room again, and the home waits for the game's return to the menu.
    send_home({{"t","teams-dissolved"},{"reason","reset"}});
    wait_until([] { return host::logged("doubles: home reset after the 4-peer match (reset); a normal end"); });
    CHECK(returns==round); CHECK(search_room()=="matchaaa"); CHECK(doubles::ui.teams_over());
    { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(!doubles::ui.stop_match); }
    // A settled room is never searched again, however its socket ends.
    CHECK(!doubles::ui.retry_teams("matchaaa"));
    game_back_on_menu(round);
    CHECK(!doubles::ui.teams_over());
    { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.foreground); }
    clear_log();
    if(round==0) doubles::ui.command("search"); // the rematch: the home queues again
  }
  leave_home(true);
}
// A transient failure of the Teams search retries in the same room, kMaxTries searches in all.
void four_peer_retry_cap() {
  using namespace yougame;
  reset_fixture(); humans=2;
  doubles::entry("new",true,2);
  wait_until([] { return search_room()=="matchaaa"; });
  CHECK(!doubles::ui.retry_teams("matchaab")); // another room's failure is not this one's
  for(int n=1;n<doubles::kMaxTries;++n) CHECK(doubles::ui.retry_teams("matchaaa"));
  CHECK(!doubles::ui.retry_teams("matchaaa"));
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.teams_failed==doubles::kMaxTries); }
  CHECK(std::string(doubles::kTeamsCappedLine).find("after 4 tries")!=std::string::npos);
  game_back_on_menu(0);
  clear_log();
  leave_home(true);
}
// The home's dissolve before this seat reached the match is a failed match: the home comes back and
// searches again (the host, three times per START).
void four_peer_dissolve_before_start(bool entered) {
  using namespace yougame;
  reset_fixture(); humans=2;
  doubles::entry("new",true,2);
  wait_until([] { return search_room()=="matchaaa"; });
  CHECK(searches==1);
  if(entered) CHECK(doubles::ui.take_teams_entry(true)[0]==1);
  send_home({{"t","teams-dissolved"},{"reason","left"},{"by","zz"}});
  serve_stop();
  wait_until([] { return returns==1; });
  CHECK(host::logged(std::string("doubles: home dissolve before the 4-peer match started (left, ")+
                     (entered?"no start":"entry not taken")+")"));
  // Rooms releases the home (as it does after any dissolve); the requeue finds the next match room.
  send_home(idle_lobby());
  wait_until([] { return searches==2 && search_room()=="matchaaa"; });
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.teams_enter); CHECK(!doubles::ui.teams_started); }
  game_back_on_menu(1);
  clear_log();
  leave_home(true);
}
// Someone leaves while the four are still on the character select: the room's start has come, but no
// game has begun. Rooms closes the match room, so the select is left and everyone still here searches
// again, told why. An opponent leaving: the duo searches again. The partner leaving: this seat searches
// alone once Rooms drops them from the home. This seat leaving: no new search.
void four_peer_leave_on_select(const std::string& by) {
  using namespace yougame;
  reset_fixture(); humans=2;
  doubles::entry("new",true,2);
  wait_until([] { return search_room()=="matchaaa"; });
  CHECK(doubles::ui.take_teams_entry(true)[0]==1);
  doubles::ui.teams_start("matchaaa"); // the start frame reaches the select before anyone picks
  quiet_search=true; // the next search waits, so its line can be read
  send_home({{"t","teams-dissolved"},{"reason","left"},{"by",by}});
  serve_stop();
  wait_until([] { return returns==1; });
  CHECK(host::logged("doubles: home dissolve before the 4-peer match started (left, no game yet)"));
  CHECK(!host::logged("doubles: home dissolve during the 4-peer match (left); the match keeps its room"));
  if(by=="c0") { // this seat
    send_home(idle_lobby());
    wait_until([] { return host::logged("doubles: this seat left the match; not searching again"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(searches==1);
    leave_home(true);
    return;
  }
  const bool partner=by=="c1";
  // The partner's leave promises no search until Rooms drops them; an opponent's search is owed now.
  wait_until([partner] { return shows(partner?"Your partner left the match.":
                                              "A player left - searching again in a moment"); });
  if(partner) {
    CHECK(!shows("searching again"));
    // Still listed: no search yet. Then Rooms drops the partner and releases the home.
    send_home(idle_lobby());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(searches==1); CHECK(shows("Your partner left the match.")); CHECK(!shows("searching again"));
    humans=1;
    send_home({{"t","lobby"},{"host","c0"},{"participants",home_roster(1)},{"nativeTeams",{{"activeRoom",nullptr}}},
               {"party",{{"playing",false},{"queueing",nullptr}}}});
  } else send_home(idle_lobby());
  wait_until([] { return searches==2; });
  wait_until([partner] { return shows(partner?"Your partner left - searching again: looking for 3 players...":
                                              "A player left - searching again: looking for 2 players..."); });
  CHECK(shows("B: stop searching"));
  CHECK(host::logged(partner?"doubles: the partner is out of the home; searching again with 1 player":
                             "doubles: searching again after a failed match (1 of 3)"));
  leave_home(true);
}
// The home's match is found and this seat is on its Teams character select, the room's start in.
void on_the_select(bool as_member) {
  using namespace yougame;
  reset_fixture(); humans=2; member=as_member;
  if(as_member) doubles::entry("homeaaaa"); else doubles::entry("new",true,2);
  wait_until([] { return search_room()=="matchaaa"; });
  CHECK(doubles::ui.take_teams_entry(true)[0]==1);
  doubles::ui.teams_start("matchaaa");
}
// The room after a leave on the select: the match socket is gone and the game is back on the menu.
void dissolved(const yougame::json& m) {
  send_home(m);
  serve_stop();
}
const yougame::json released_home={{"t","lobby"},{"nativeTeams",{{"activeRoom",nullptr}}},
                                   {"party",{{"playing",false},{"queueing",nullptr}}}};
// Leaves on the select are failed matches: three automatic searches per START, then the capped line,
// and its reason never reaches a later search. no_show and timed_out say a player didn't connect.
void four_peer_select_cap() {
  using namespace yougame;
  on_the_select(false);
  for(int n=1;n<=4;++n) {
    if(n>1) { wait_until([] { return search_room()=="matchaaa"; }); CHECK(doubles::ui.take_teams_entry(true)[0]==1);
              doubles::ui.teams_start("matchaaa"); }
    if(n==4) quiet_search=true;
    dissolved({{"t","teams-dissolved"},{"reason",n%2?"no_show":"timed_out"},{"by",nullptr}});
    wait_until([n] { return returns==n; });
    if(n<4) wait_until([] { return shows("A player didn't connect - searching again in a moment"); });
    send_home(released_home);
    if(n<4) wait_until([n] { return searches==n+1; });
  }
  wait_until([] { return shows("Couldn't connect all four players after 4 tries"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(searches==4); CHECK(!shows("searching again"));
  CHECK(host::logged("doubles: searching again after a failed match (3 of 3)"));
  // START after the cap: a plain search, with three new tries and no old reason.
  doubles::ui.command("search");
  wait_until([] { return searches==5 && shows("Looking for 2 players..."); });
  CHECK(!shows("didn't connect")); CHECK(!shows("Try "));
  leave_home(true);
}
// A game of the match began: its failures are over, so the next match's select gets three searches again.
void four_peer_cap_resets_after_a_game() {
  using namespace yougame;
  on_the_select(false);
  for(int n=1;n<=2;++n) {
    if(n>1) { wait_until([] { return search_room()=="matchaaa"; }); CHECK(doubles::ui.take_teams_entry(true)[0]==1);
              doubles::ui.teams_start("matchaaa"); }
    dissolved({{"t","teams-dissolved"},{"reason","left"},{"by","zz"}});
    wait_until([n] { return returns==n; });
    send_home(released_home);
    wait_until([n] { return searches==n+1; });
  }
  wait_until([] { return search_room()=="matchaaa"; });
  CHECK(doubles::ui.take_teams_entry(true)[0]==1);
  doubles::ui.teams_start("matchaaa"); doubles::ui.teams_game(); // this one plays
  game_back_on_menu(2);
  send_home(released_home);
  clear_log();
  doubles::ui.command("search"); // the rematch
  for(int n=1;n<=3;++n) {
    wait_until([] { return search_room()=="matchaaa"; }); CHECK(doubles::ui.take_teams_entry(true)[0]==1);
    doubles::ui.teams_start("matchaaa");
    dissolved({{"t","teams-dissolved"},{"reason","left"},{"by","zz"}});
    wait_until([n] { return returns==3+n; });
    send_home(released_home);
    wait_until([n] { return searches==4+n; });
  }
  CHECK(host::logged("doubles: searching again after a failed match (3 of 3)"));
  wait_until([] { return search_room()=="matchaaa"; });
  game_back_on_menu(6);
  leave_home(true);
}
// The partner left the select but stays in the home (B on the select): no search is promised, and after
// the wait the host decides. START in that wait searches as the duo; B leaves the home.
void four_peer_partner_stays(int press) { // 0: wait it out, 1: START, 2: B
  using namespace yougame;
  on_the_select(false);
  quiet_search=true;
  dissolved({{"t","teams-dissolved"},{"reason","left"},{"by","c1"}});
  wait_until([] { return returns==1; });
  send_home(released_home);
  wait_until([] { return shows("Your partner left the match.\nB: leave"); });
  if(press==1) {
    doubles::ui.command("search");
    wait_until([] { return searches==2 && shows("Looking for 2 players..."); });
    CHECK(!shows("Your partner left"));
    leave_home(true);
    return;
  }
  if(press==2) {
    doubles::ui.command("back");
    wait_until([] { return !doubles::running; });
    CHECK(searches==1); CHECK(!home_open);
    std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.menu=false;
    return;
  }
  wait_until([] { return shows(doubles::kPartnerLeftMatchLine); }); // about 10 s
  CHECK(searches==1);
  CHECK(host::logged("doubles: the partner is still in the home; not searching again"));
  leave_home(true);
}
// A member whose partner (the host) left the select: once Rooms drops the host and promotes this seat,
// it searches alone.
void four_peer_member_promoted() {
  using namespace yougame;
  on_the_select(true);
  quiet_search=true;
  dissolved({{"t","teams-dissolved"},{"reason","left"},{"by","c0"}});
  wait_until([] { return returns==1; });
  wait_until([] { return shows("Your partner left the match."); });
  CHECK(searches==0);
  humans=1;
  const json solo=json::array({{{"id","p1"},{"connectionId","c1"},{"slot",0},{"localIndex",0}}});
  send_home({{"t","lobby"},{"host","c1"},{"participants",solo},{"nativeTeams",{{"activeRoom",nullptr}}},
             {"party",{{"playing",false},{"queueing",nullptr}}}});
  wait_until([] { return searches==1; });
  wait_until([] { return shows("Your partner left - searching again: looking for 3 players..."); });
  CHECK(host::logged("doubles: the partner is out of the home; searching again with 1 player"));
  leave_home(true);
}
// A member when an opponent left: no promise of its own (the host searches), then the host's search
// line says why.
void four_peer_member_lines() {
  using namespace yougame;
  on_the_select(true);
  quiet_search=true;
  dissolved({{"t","teams-dissolved"},{"reason","left"},{"by","zz"}});
  wait_until([] { return returns==1; });
  send_home(released_home);
  wait_until([] { return shows("presses START to search"); });
  CHECK(!shows("searching again")); CHECK(searches==0);
  send_home({{"t","lobby"},{"party",{{"queueing",{{"id","abcdefghijklmnopqrst"},{"queue","casual"},{"n",2}}}}}});
  wait_until([] { return shows("A player left - searching again: looking for 2 players..."); });
  CHECK(shows("B: stop searching"));
  leave_home(true);
}
// A game of the match began: a later dissolve leaves the match to the Teams client, as before.
void four_peer_leave_after_a_game() {
  using namespace yougame;
  reset_fixture(); humans=2;
  doubles::entry("new",true,2);
  wait_until([] { return search_room()=="matchaaa"; });
  CHECK(doubles::ui.take_teams_entry(true)[0]==1);
  doubles::ui.teams_start("matchaaa");
  doubles::ui.teams_game();
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.teams_played); }
  send_home({{"t","teams-dissolved"},{"reason","left"},{"by","zz"}});
  wait_until([] { return host::logged("doubles: home dissolve during the 4-peer match (left); the match keeps its room"); });
  CHECK(returns==0); CHECK(searches==1);
  game_back_on_menu(0);
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(!doubles::ui.teams_played); }
  clear_log();
  leave_home(true);
}
// A match found while the home is retained (the game was busy when the last one ended) still plays:
// publish() is skipped for a retained home, so the matched branch sets playing itself.
void four_peer_match_while_retained() {
  using namespace yougame;
  reset_fixture(); humans=2;
  doubles::entry("new",true,2);
  wait_until([] { return search_room()=="matchaaa"; });
  doubles::ui.command("unavailable"); // the match retires while the game is busy: the home stays behind it
  serve_stop();
  wait_until([] { return returns==1; });
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(!doubles::ui.foreground); CHECK(!doubles::ui.playing); }
  CHECK(search_room().empty());
  doubles::ui.command("search");
  wait_until([] { return search_room()=="matchaaa"; });
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex);
    CHECK(doubles::ui.playing); CHECK(!doubles::ui.foreground); CHECK(doubles::ui.teams_enter); }
  game_back_on_menu(1);
  clear_log();
  leave_home();
}
}
int main() {
  four_peer_match_and_return();
  four_peer_retry_cap();
  four_peer_dissolve_before_start(false);
  four_peer_dissolve_before_start(true);
  four_peer_match_while_retained();
  four_peer_leave_on_select("zz");
  four_peer_leave_on_select("c1");
  four_peer_leave_on_select("c0");
  four_peer_leave_after_a_game();
  four_peer_select_cap();
  four_peer_cap_resets_after_a_game();
  four_peer_partner_stays(0);
  four_peer_partner_stays(1);
  four_peer_partner_stays(2);
  four_peer_member_promoted();
  four_peer_member_lines();
  std::printf("Doubles 4-peer route: %d checks passed\n",checks.load());
}
