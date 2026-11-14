// Exercise Doubles over the console link's injectable broker and real Stream.
#define main console_existing_main
#include "yougame_console_link_test.cpp"
#undef main
#include "yougame_doubles.h"
#include <regex>
using namespace yougame::doubles;
static json home_roster(int n) {
  json out=json::array();
  for (int i=0;i<n;++i) out.push_back({{"id","p"+std::to_string(i)},{"connectionId","c"+std::to_string(i)},{"slot",i},{"localIndex",0}});
  return out;
}
#ifndef DOUBLES_FIXTURE_ONLY
static void invite_profiles_and_reports() {
  // A plain {type:join,room} stays Friends until ticket/room authority says Doubles.
  yougame::friends::Events invite;
  CHECK(invite.push({{"type","join"},{"room","homeaaaa"}},0));
  CHECK(!invite.doubles_join_pending());
  CHECK(invite.take_join(true)=="homeaaaa");
  // The page marks a Doubles home's join push md:1, and the launch hello's nativeTeamsInvite.
  CHECK(invite.push({{"type","join"},{"room","homebbbb"},{"md",1}},0));
  CHECK(invite.doubles_join_pending()); CHECK(invite.take_join(true)=="homebbbb"); CHECK(!invite.doubles_join_pending());
  CHECK(invite.push({{"type","join"},{"room","homecccc"},{"md",1}},0));
  CHECK(invite.push({{"type","join"},{"room","homedddd"}},0));
  CHECK(!invite.doubles_join_pending()); CHECK(invite.take_join(true)=="homedddd");
  yougame::friends::Events launch;
  launch.hello({{"room","homeeeee"},{"nativeTeamsInvite",{{"v",1},{"room","homeeeee"},{"mode","teams"},{"md",1}}}});
  CHECK(launch.doubles_join_pending()); CHECK(launch.take_join(true)=="homeeeee");
  yougame::friends::Events mismatch;
  mismatch.hello({{"room","homeffff"},{"nativeTeamsInvite",{{"v",1},{"room","homegggg"},{"mode","teams"},{"md",1}}}});
  CHECK(!mismatch.doubles_join_pending()); CHECK(mismatch.take_join(true)=="homeffff");
  for (bool ticket_reply : {true,false}) {
    Fixture f(2,1); auto io=f.io(); io.friends_lobby=true;
    std::string routed;
    io.doubles_home=[&](const std::string& room) { routed=room; };
    if (ticket_reply) {
      auto request=io.request;
      io.request=[request](const json& q,int ms) { auto answer=request(q,ms); if(q["type"]=="mpTicket") answer["md"]=1; return answer; };
    } else {
      auto next=io.next_room;
      io.next_room=[next](json& m,int ms) { if(!next(m,ms)) return false;
        if(m["t"]=="welcome") m["room"]={{"size",4},{"mode","teams"},{"queue","private"}};
        return true;
      };
    }
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,io)); CHECK(routed==f.input()["room"]);
    for(const auto& m:f.messages) CHECK(m["t"]!="ready" && m["t"]!="begin");
  }
  CHECK(!home_profile({{"room",{{"size",2},{"mode","console"},{"queue","private"}}}}));
  Home h; h.code="homeaaaa"; h.participants=home_roster(1); h.me=h.host="c0";
  RoomReports reports;
  auto home=reports.room(h,h.code,h.participants,h.me,3);
  CHECK(home["mode"]=="teams" && home["queue"]=="private" && home["size"]==4);
  CHECK(home["nativeTeams"]==json({{"v",1},{"kind","party"},{"home",h.code},{"phase","home"}}));
  CHECK(home["players"].size()==1 && home["party"]["full"]==false);
  CHECK(reports.room(h,h.code,h.participants,h.me,4)["roomEpoch"]==home["roomEpoch"]);
  h.phase=Phase::Admission;
  auto match=reports.room(h,"matchaaa",home_roster(4),h.me,5);
  CHECK(match["roomEpoch"]>home["roomEpoch"] && match["nativeTeams"]["phase"]=="assembling");
  h.returned(); auto again=reports.room(h,h.code,h.participants,h.me,6);
  CHECK(again["roomEpoch"]>match["roomEpoch"]);
  auto clear=reports.clear(); CHECK(clear["roomEpoch"]>again["roomEpoch"]);
  CHECK(clear["code"].is_null() && clear["players"].empty() && !clear.count("nativeTeams"));
  h.players=json::array({{{"id","p0"},{"away",true}}});
  CHECK(reports.room(h,h.code,h.participants,h.me,7)["players"][0]["away"]==true);
  RoomReports next_run; CHECK(next_run.room(h,h.code,h.participants,h.me,0)["roomEpoch"]>clear["roomEpoch"]);
  for(const auto* t:{"kicked","ended","roomClosed"}) CHECK(home_terminal({{"t",t}}));
  CHECK(!home_terminal({{"t","teams-dissolved"},{"reason","reset"}}));
}
static void protocol() {
  const auto host_key=yougame::agent_test::map_keys(json{{"doublesHost",true}},yougame::agent_test::Engine::Source,{},false);
  CHECK(host_key.doubles_room=="new"); CHECK(host_key.doubles_search);
  const auto duo_key=yougame::agent_test::map_keys(json{{"doublesDuoHost",true}},yougame::agent_test::Engine::Source,{},false);
  CHECK(duo_key.doubles_room=="new"); CHECK(duo_key.doubles_humans==2); CHECK(duo_key.doubles_search);
  const auto join_key=yougame::agent_test::map_keys(json{{"doublesJoin","homeaaaa"}},yougame::agent_test::Engine::Source,{},false);
  CHECK(join_key.doubles_room=="homeaaaa"); CHECK(!join_key.doubles_search);
  CHECK(yougame::agent_test::map_keys(json{{"doublesJoin","bad room"}},yougame::agent_test::Engine::Source,{},false).doubles_room.empty());
  const auto h=ticket("mu:fingerprint","homeaaaa",2);
  CHECK(h==json({{"type","mpTicket"},{"teamsLobby",1},{"mu_doubles",1},{"mode","teams"},{"queue","private"},
    {"room","homeaaaa"},{"players",4},{"lobby",{{"version",1},{"minPlayers",2},{"maxLocalPlayers",2},{"compatibility","mu:fingerprint"}}}}));
  // Keys the site's Doubles check refuses on any of these tickets (native-teams.ts).
  const auto refused_keys=[](const json& q) {
    for (const char* k : {"ranked","teams_invite","dev","doubles","party_size","party_local","match_room","world","direct",
                          "spectate","tourney","server","local_players","teams_lobby","party_search"}) if (q.count(k)) return true;
    return false;
  };
  CHECK(!refused_keys(h));
  for (int n : {1,2}) {
    auto q=search_ticket("mu:fingerprint","homeaaaa","abcdefghijklmnopqrst",n);
    CHECK(q["local_partner"]==yougame::friends::kLocalPartnerCapability);
    CHECK(q["party_room"]=="homeaaaa"); CHECK(q["partySearch"]=="abcdefghijklmnopqrst"); CHECK(!q.count("room"));
    CHECK(q["queue"]=="casual"); CHECK(q["players"]==4);
    CHECK(q["lobby"]["minPlayers"]==4); CHECK(q["lobby"]["maxLocalPlayers"]==1); CHECK(!refused_keys(q));
    CHECK(q.count("party")==size_t(n==2)); if (n==2) CHECK(q["party"]==json({{"id",q["partySearch"]},{"n",2}}));
  }
  auto admission=ticket("mu:fingerprint","matchaaa",4);
  CHECK(admission["queue"]=="private"); CHECK(admission["lobby"]["maxLocalPlayers"]==2); CHECK(admission["lobby"]["minPlayers"]==4);
  CHECK(admission["players"]==4); CHECK(admission["room"]=="matchaaa"); CHECK(!refused_keys(admission));
  for (const char* k : {"party","party_room","partySearch"}) { CHECK(!h.count(k)); CHECK(!admission.count(k)); }
  // The generations a search accepts are the ones the site's party_search accepts.
  const std::regex party_search("^[a-z0-9]{12,32}$");
  for (const char* g : {"abcdefghijkl","abcdefghijklmnopqrstuvwxyz012345","abcdefghijk","abcdefghijklmnopqrstuvwxyz0123456",
                        "ABCDEFGHIJKL","abcdef-hijkl",""})
    CHECK(generation_valid(g)==std::regex_match(std::string(g),party_search));
  Home state; state.code="homeaaaa"; state.compatibility="mu:fingerprint";
  json welcome={{"t","welcome"},{"me","c1"},{"host","c0"},{"participants",home_roster(2)}};
  state.observe(welcome); CHECK(!state.search()); CHECK(state.search(true)->at("t")=="ready");
  welcome["me"]="c0"; state.observe(welcome); CHECK(state.search()->at("t")=="doubles-search");
  welcome["participants"]=home_roster(3); state.observe(welcome); CHECK(!state.search()); CHECK(state.status.find("Friends")!=std::string::npos);
  welcome["participants"]=home_roster(2); state.observe(welcome);
  auto generation=json{{"t","lobby"},{"party",{{"queueing",{{"id","abcdefghijklmnopqrst"},{"queue","casual"},{"n",2}}}}}};
  CHECK(state.observe(generation)); CHECK(!state.observe(generation)); CHECK(state.phase==Phase::Searching);
  state.progress({{"t","partner"},{"partner",{{"n","Friend"},{"u","user"}}}}); CHECK(state.partner=="Friend");
  state.progress({{"t","partner"},{"partner",nullptr}}); CHECK(state.partner.empty());
  state.progress({{"t","searching"},{"waiting",3}}); CHECK(state.status.find("3 waiting")!=std::string::npos);
  // A duo home with no queue teammate yet needs two more; a found teammate is named.
  CHECK(state.status.find("Looking for 2 players...")==0); CHECK(state.status.find("B: stop searching")!=std::string::npos);
  state.participants=home_roster(1); state.progress({{"t","partner"},{"partner",{{"n","Ann"},{"u","user"}}}});
  CHECK(state.status.find("Ann is with you - looking for 2 more...")==0); CHECK(state.status.find("3 waiting")!=std::string::npos);
  state.progress({{"t","partner"},{"partner",nullptr}}); state.participants=home_roster(2);
  state.observe({{"t","lobby"},{"party",{{"queueing",nullptr}}}}); CHECK(state.phase==Phase::Home); CHECK(state.generation.empty());
  state.observe(generation); CHECK(state.matched({{"t","matched"},{"room","matchaaa"}})); CHECK(state.phase==Phase::Admission);
  CHECK(state.status=="Match found! Connecting the four players...");
  // After a failed match the host's line says which try this is.
  {
    Home again=state; again.phase=Phase::Searching; again.partner.clear(); again.waiting=-1; again.retry=1;
    CHECK(again.search_line().find("Try 2 of 4: looking for 2 players...")==0);
    again.partner="Ann"; CHECK(again.search_line().find("Try 2 of 4: Ann is with you - looking for")==0);
    again.retry=3; again.partner.clear(); again.generation="bcdefghijklmnopqrstu";
    CHECK(again.matched({{"t","matched"},{"room","matchbbb"}})); CHECK(again.status=="Match found! Connecting the four players (try 4 of 4)...");
  }
  state.observe({{"t","lobby"},{"party",{{"queueing",nullptr}}}}); CHECK(state.phase==Phase::Admission);
  Consent consent; consent.observe({{"revision",9},{"nativeTeams",{{"selectionEpoch",3}}}});
  CHECK(*consent.ready(true)==json({{"t","ready"},{"ready",true},{"revision",9},{"selectionEpoch",3}}));
  CHECK(!consent.ready(true)); consent.observe({{"revision",10},{"nativeTeams",{{"selectionEpoch",4}}}});
  CHECK(consent.ready(true)->at("selectionEpoch")==4); CHECK(consent.begin(3)["revision"]==10);
  Rules r; CHECK(digest(r)==digest());
  r.stocks++; CHECK(digest(r)!=digest()); r={}; r.minutes++; CHECK(digest(r)!=digest());
  r={}; r.items++; CHECK(digest(r)!=digest()); r={}; r.friendly_fire=false; CHECK(digest(r)!=digest());
  r={}; r.team_battle=false; CHECK(digest(r)!=digest()); r={}; std::swap(r.stages[0],r.stages[1]); CHECK(digest(r)!=digest());
  r={}; r.stages.pop_back(); CHECK(digest(r)!=digest());
  for (uint64_t seed=0;seed<1000;++seed) for(int machine=0;machine<4;++machine) {
    CHECK(stage(seed)==stage(seed)); CHECK(std::find(r.stages.begin(),r.stages.end(),stage(seed))!=r.stages.end() || stage(seed)==3);
  }
  // Rooms' console begin: settingsDigest /^[A-Za-z0-9:_-]{0,128}$/, inputDelay an integer 1-8.
  CHECK(std::regex_match(digest(),std::regex("^[A-Za-z0-9:_-]{0,128}$")));
  { Consent c; c.observe(json{{"revision",7},{"nativeTeams",{{"selectionEpoch",6}}}});
    const auto b=c.begin(3);
    CHECK(b==json({{"t","begin"},{"revision",7},{"selectionEpoch",6},{"console",true},{"inputDelay",3},{"settingsDigest",digest()}}));
    CHECK(b["inputDelay"].is_number_integer() && b["inputDelay"]>=1 && b["inputDelay"]<=8);
    CHECK(c.ready(true)==json({{"t","ready"},{"ready",true},{"revision",7},{"selectionEpoch",6}})); CHECK(!c.ready(true)); }
  std::printf("default digest: %s\n",digest().c_str());
}
static void link_case(int seat, bool bad_digest=false, bool bad_team=false, bool bad_battle=false) {
  Fixture f(4,seat); f.host_seat=2;
  auto io=f.io(); io.doubles=true;
  // Change transport arrival order; preserve authoritative frozen port/slot assignments.
  auto roster=f.roster(); std::reverse(roster.begin(),roster.end()); f.lobby_roster=roster;
  const auto next=io.next_room;
  io.next_room=[&,next](json& m,int ms) {
    if (!next(m,ms)) return false;
    if (m["t"]=="welcome") m["nativeTeams"]={{"selectionEpoch",6}};
    if (m.count("console")) {
      m["console"]["settingsDigest"]=bad_digest?"mu:teams:v1:00000000":digest();
      if (!bad_battle) m["console"]["teamBattle"]=true;
      for (auto& p:m["console"]["participants"]) if (!bad_team) p["team"]=p["slot"].get<int>()/2;
    }
    return true;
  };
  SessionConfig c; std::string error;
  auto link=start_session(f.input(),c,error,io);
  CHECK(bool(link)==!(bad_digest||bad_team||bad_battle));
  CHECK(f.requests[0]["mu_doubles"]==1); CHECK(f.requests[0]["lobby"]["minPlayers"]==4);
  bool ready=false,begin=false;
  for (const auto& m:f.messages) {
    if (m["t"]=="ready") { ready=true; CHECK(m["revision"]==7); CHECK(m["selectionEpoch"]==6); }
    if (m["t"]=="begin") { begin=true; CHECK(m["selectionEpoch"]==6); CHECK(m["settingsDigest"]==digest()); }
  }
  CHECK(ready); CHECK(begin==(seat==2));
  if (!link) { CHECK(error.find("doubles:")==0); return; }
  CHECK(c.my_port==seat+1); CHECK(c.doubles); CHECK(c.doubles_stage==stage(c.seed));
  CHECK((c.doubles_teams==std::array<int,4>({0,0,1,1})));
  for (int winner:{0,1}) {
    auto result=finish(c,winner,false,false); CHECK(result["detail"]["tw"]==winner);
    CHECK(result["ranking"]==json(winner==0?std::vector<std::string>{"p0","p1","p2","p3"}:std::vector<std::string>{"p2","p3","p0","p1"}));
  }
  CHECK(finish(c,0,true,false)["draw"]==true); CHECK(finish(c,0,false,true)["void"]==true);
  Home h; h.phase=Phase::Playing;
  std::vector<std::string> sequence{"result"}; h.result_seen=true;
  for (const auto* why:{"left","timed_out","no_show","removed","reset"}) CHECK(terminal({{"t","teams-dissolved"},{"reason",why}}));
  sequence.push_back("teams-dissolved"); link.reset(); sequence.push_back("close-match");
  sequence.push_back(h.returned()["t"].get<std::string>()); sequence.push_back("menu");
  CHECK(sequence==std::vector<std::string>({"result","teams-dissolved","close-match","party-back","menu"})); CHECK(h.phase==Phase::Home);
}
static void epoch_link() {
  Fixture f(4,0); f.reply_start=false;
  auto io=f.io(); io.doubles=true;
  const auto next=io.next_room;
  io.next_room=[&](json& m,int ms) {
    if(!next(m,ms)) return false;
    if(m["t"]=="welcome") m["nativeTeams"]={{"selectionEpoch",6}};
    if(m.count("console")) {
      m["console"]["settingsDigest"]=digest(); m["console"]["teamBattle"]=true;
      for(auto& p:m["console"]["participants"]) p["team"]=p["slot"].get<int>()/2;
    }
    return true;
  };
  const auto send=io.send_room; bool first=true;
  io.send_room=[&](const json& m) {
    if(m["t"]=="begin" && first) {
      CHECK(m["selectionEpoch"]==6); first=false;
      f.push({{"t","command-error"},{"action","begin"}});
      f.push({{"t","lobby"},{"revision",8},{"participants",f.roster()},{"nativeTeams",{{"selectionEpoch",7}}}});
    } else if(m["t"]=="begin") { CHECK(m["selectionEpoch"]==7); CHECK(m["revision"]==8); f.reply_start=true; }
    return send(m);
  };
  SessionConfig c; std::string error; CHECK(start_session(f.input(),c,error,io));
  int ready=0; for(const auto& m:f.messages) if(m["t"]=="ready") ++ready; CHECK(ready==2);
  Fixture ordinary(4,0); auto ordinary_io=ordinary.io();
  CHECK(start_session(ordinary.input(),c,error,ordinary_io));
  for(const auto& q:ordinary.requests) { CHECK(!q.count("mu_doubles")); CHECK(!q.count("teams_lobby")); CHECK(!q.count("teamsLobby")); }
}
static json named_roster(std::initializer_list<const char*> names) {
  json out=json::array(); int i=0;
  for (const char* n : names) { out.push_back({{"id","p"+std::to_string(i)},{"connectionId","c"+std::to_string(i)},{"slot",i},{"localIndex",0},{"name",n}}); ++i; }
  return out;
}
static void home_lines() {
  // The idle home names who joined and who presses START, on both screens.
  Home host; CHECK(!host.observe({{"t","welcome"},{"me","c0"},{"host","c0"},{"participants",named_roster({"Hal"})}}));
  CHECK(host.status==kHomeLine);
  host.observe({{"t","lobby"},{"participants",named_roster({"Hal","Ada L"})}});
  CHECK(host.status=="Ada L joined - press START to search for a match\nB: leave");
  Home member; member.observe({{"t","welcome"},{"me","c1"},{"host","c0"},{"participants",named_roster({"Hal","Ada L"})}});
  CHECK(member.status=="You're on Hal's team - Hal presses START to search\nB: leave");
  // A name the box's font can't draw falls back to plain words.
  Home odd; odd.observe({{"t","welcome"},{"me","c0"},{"host","c0"},{"participants",named_roster({"Hal","Ad\u00e9"})}});
  CHECK(odd.status=="Your partner joined - press START to search for a match\nB: leave");
  Home odd_member; odd_member.observe({{"t","welcome"},{"me","c1"},{"host","c0"},{"participants",named_roster({"Lu\u00efs","Ada"})}});
  CHECK(odd_member.status=="You're on the host's team - the host presses START to search\nB: leave");
  // A stopped search says so to both, and the partner still learns who presses START.
  const json queue={{"id","abcdefghijklmnopqrst"},{"queue","casual"},{"n",2}};
  CHECK(host.observe({{"t","lobby"},{"party",{{"queueing",queue}}}}));
  CHECK(member.observe({{"t","lobby"},{"party",{{"queueing",queue}}}}));
  host.observe({{"t","lobby"},{"party",{{"queueing",nullptr}}}}); CHECK(host.status==kStoppedLine);
  member.observe({{"t","lobby"},{"party",{{"queueing",nullptr}}}});
  CHECK(member.status=="Search stopped - Hal presses START to search again\nB: leave");
  // The host searching again replaces the stopped line on the partner's box too.
  const json again={{"id","bcdefghijklmnopqrstu"},{"queue","casual"},{"n",2}};
  CHECK(member.observe({{"t","lobby"},{"party",{{"queueing",again}}}}));
  CHECK(member.status==member.search_line()); CHECK(member.status.find("stopped")==std::string::npos);
  member.observe({{"t","lobby"},{"participants",named_roster({"Hal","Ada L"})}}); CHECK(member.status==member.search_line());
  // A roster update after the stop keeps the stopped line; the next search clears it.
  host.observe({{"t","lobby"},{"participants",named_roster({"Hal","Ada L"})}}); CHECK(host.status==kStoppedLine);
  // The partner leaving is visible on the host's screen; the next join replaces it.
  host.observe({{"t","lobby"},{"participants",named_roster({"Hal"})}}); CHECK(host.status==kPartnerLeftLine);
  host.observe({{"t","lobby"},{"participants",named_roster({"Hal","Bo"})}});
  CHECK(host.status=="Bo joined - press START to search for a match\nB: leave");
  host.observe({{"t","lobby"},{"participants",named_roster({"Hal","Bo","Cy"})}}); CHECK(host.status=="Three or four players: play Friends instead");
  // Back from a match the join is old news: the line names the duo instead.
  host.observe({{"t","lobby"},{"participants",named_roster({"Hal","Bo"})}});
  CHECK(host.status=="Bo joined - press START to search for a match\nB: leave");
  host.phase=Phase::Playing; host.returned(); CHECK(host.status=="You and Bo - press START to search for a match\nB: leave");
  // A same-size roster refresh doesn't bring the join back.
  host.observe({{"t","lobby"},{"participants",named_roster({"Hal","Bo"})}}); CHECK(host.status=="You and Bo - press START to search for a match\nB: leave");
  // A search that ends on an error keeps its reason through later snapshots, on the host's box only.
  const json third={{"id","cdefghijklmnopqrstuv"},{"queue","casual"},{"n",2}};
  CHECK(host.observe({{"t","lobby"},{"party",{{"queueing",third}}}}));
  host.search_ended("Lost the search connection. Press START to search again\nZ: invite a partner     B: leave");
  CHECK(host.phase==Phase::Home && host.generation.empty());
  host.observe({{"t","lobby"},{"revision",9}}); host.observe({{"t","lobby"},{"participants",named_roster({"Hal","Bo"})}});
  CHECK(host.status.rfind("Lost the search connection",0)==0);
  member.search_ended("Lost the search connection. Press START to search again\nZ: invite a partner     B: leave");
  CHECK(member.status=="Search stopped - Hal presses START to search again\nB: leave");
  // A cancel after that is a plain stop again.
  CHECK(host.observe({{"t","lobby"},{"party",{{"queueing",queue}}}}));
  host.observe({{"t","lobby"},{"party",{{"queueing",nullptr}}}}); CHECK(host.status==kStoppedLine);
  // A second player on the host's own connection is not a partner, and can't search Doubles.
  Home local; json two=named_roster({"Hal","Hal 2"}); two[1]["connectionId"]="c0"; two[1]["localIndex"]=1;
  local.observe({{"t","welcome"},{"me","c0"},{"host","c0"},{"participants",two}});
  CHECK(local.status=="Two players on one connection: play Friends instead\nB: leave"); CHECK(!local.other_seat());
}
int main() {
  protocol(); home_lines(); invite_profiles_and_reports(); epoch_link(); for (int seat=0;seat<4;++seat) link_case(seat);
  link_case(0,true); link_case(0,false,true); link_case(0,false,false,true);
  std::printf("Doubles: %d checks passed\n",checks.load());
}

#endif
