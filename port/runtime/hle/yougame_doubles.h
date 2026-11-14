// Console Doubles protocol and frozen rules. No Slippi matchmaking state.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef MELEE_DOUBLES
#include "yougame_console.h"
#include "yougame_console_request.h"
#include "yougame_doubles_teams.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <climits>
#include <set>
#include <sstream>
#include <iomanip>

namespace yougame::doubles {
using console::json;
struct Rules {
  bool team_battle = true;
  int stocks = 4, minutes = 8, items = 0;
  bool friendly_fire = true;
  // Internal stage IDs: Battlefield, Final Destination, Dream Land, Yoshi's Story, Pokemon Stadium.
  std::vector<int> stages{31,32,28,8,3};
};
inline std::string digest(const Rules& r = {}) {
  // FNV-1a over a versioned canonical decimal record, including stage order.
  std::string record = "1:" + std::to_string(r.team_battle) + ":" + std::to_string(r.stocks) + ":" +
      std::to_string(r.minutes) + ":" + std::to_string(r.items) + ":" + std::to_string(r.friendly_fire);
  for (int stage : r.stages) record += ":" + std::to_string(stage);
  uint32_t hash = 2166136261u;
  for (unsigned char c : record) hash = (hash ^ c)*16777619u;
  std::ostringstream out; out << "mu:teams:v1:" << std::hex << std::setw(8) << std::setfill('0') << hash;
  return out.str();
}
inline int stage(uint64_t seed, const Rules& r = {}) { return r.stages.at(seed % r.stages.size()); }
// The page (Player.tsx) and the launcher turn teamsLobby and partySearch into the route's teams_lobby and
// party_search, and add slug and native_build. A room ticket (home: minimum 2, match: 4) names no search.
inline json ticket(const std::string& compatibility, const std::string& room, int minimum) {
  return {{"type","mpTicket"},{"teamsLobby",1},{"mu_doubles",1},{"mode","teams"},
    {"queue","private"},{"room",room},{"players",4},
    {"lobby",{{"version",1},{"minPlayers",minimum},{"maxLocalPlayers",2},{"compatibility",compatibility}}}};
}
inline json search_ticket(const std::string& compatibility, const std::string& home,
                          const std::string& generation, int humans) {
  auto q = ticket(compatibility,home,4); q.erase("room"); q["queue"] = "casual";
  q["local_partner"] = friends::kLocalPartnerCapability;
  q["lobby"]["maxLocalPlayers"] = 1; q["party_room"] = home; q["partySearch"] = generation;
  // Two humans are two computers (a party of the home's search); one human searches alone. Never local_players.
  if (humans == 2) q["party"] = {{"id",generation},{"n",2}};
  return q;
}
inline std::string text(const json& j, const char* key) { return friends::text(j,key,128); }
inline bool generation_valid(const std::string& s) {
  return s.size() >= 12 && s.size() <= 32 && s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos;
}
inline bool freeze(const json& s, console::SessionConfig& c, std::string& error, const Rules& rules = {}) {
  auto refuse = [&](const char* why) { error = why; return false; };
  if (s.value("settingsDigest",json()) != digest(rules)) return refuse("doubles: settings digest mismatch");
  if (s.value("teamBattle",json()) != true) return refuse("doubles: start missing Team Battle");
  if (!s.count("participants") || !s["participants"].is_array() || s["participants"].size()!=4)
    return refuse("doubles: start needs four participants");
  std::set<int> ports; std::set<std::string> seats, connections;
  for (const auto& p : s["participants"]) {
    if (!console::number(p,"port",1,4) || !console::number(p,"slot",0,3) || !console::number(p,"team",0,1))
      return refuse("doubles: start missing port, slot or team");
    const int port = p["port"].get<int>();
    if (p["slot"] != port-1 || p["team"] != (port-1)/2 || text(p,"id").empty() || text(p,"connectionId").empty())
      return refuse("doubles: invalid frozen teams/ports");
    ports.insert(port); seats.insert(text(p,"id")); connections.insert(text(p,"connectionId"));
    c.doubles_teams[port-1] = p["team"].get<int>(); c.doubles_seats[port-1] = text(p,"id");
  }
  if (ports.size()!=4 || seats.size()!=4 || connections.size()!=4) return refuse("doubles: four computers required");
  c.doubles = true; c.doubles_stage = stage(c.seed,rules); return true;
}
inline json finish(const console::SessionConfig& c, int winner, bool draw, bool invalid) {
  json ranking = json::array();
  for (int team : {winner == 1 ? 1 : 0,winner == 1 ? 0 : 1})
    for (int port=0;port<4;++port) if (c.doubles_teams[port] == team) ranking.push_back(c.doubles_seats[port]);
  return {{"t","finish"},{"matchId",c.session_id},{"ranking",ranking},{"draw",draw},{"void",invalid},
    {"detail",{{"tw",winner == 1 ? 1 : 0}}}};
}
inline bool home_terminal(const json& m) {
  const auto t = text(m,"t");
  return t=="kicked" || t=="ended" || t=="roomClosed";
}
// An invite push has no type marker. Only an authoritative ticket or room profile routes it.
inline bool home_profile(const json& m) {
  if (m.value("md",json())==1) return true;
  const auto r=m.value("room",json());
  return r.is_object() && r.value("size",json())==4 && r.value("mode",json())=="teams" &&
    r.value("queue",json())=="private";
}
inline bool terminal(const json& m) {
  const auto t = text(m,"t"), reason = text(m,"reason");
  const bool known = reason=="left" || reason=="timed_out" || reason=="no_show" || reason=="removed" || reason=="reset";
  return (t=="teams-dissolved" && known) || t=="kicked" || t=="ended" || t=="roomClosed";
}
// Room snapshots carry the epoch inside nativeTeams, including welcome after arrivals.
inline std::optional<uint64_t> selection_epoch(const json& m) {
  const json* nt = m.count("nativeTeams") ? &m["nativeTeams"] : nullptr;
  if (!nt && m.count("room") && m["room"].is_object() && m["room"].count("nativeTeams")) nt = &m["room"]["nativeTeams"];
  if (nt && console::number(*nt,"selectionEpoch",0,INT64_MAX)) return (*nt)["selectionEpoch"].get<uint64_t>();
  if (console::number(m,"selectionEpoch",0,INT64_MAX)) return m["selectionEpoch"].get<uint64_t>();
  return {};
}
struct Consent {
  uint64_t epoch = 0; bool have_epoch = false;
  int revision = -1, sent_revision = -1; uint64_t sent_epoch = UINT64_MAX;
  void observe(const json& m) {
    if (auto e = selection_epoch(m)) { epoch = *e; have_epoch = true; }
    if (console::number(m,"revision",0,INT_MAX)) revision = m["revision"].get<int>();
  }
  std::optional<json> ready(bool full) {
    if (!full || !have_epoch || revision<0 || (revision==sent_revision && epoch==sent_epoch)) return {};
    sent_revision = revision; sent_epoch = epoch;
    return json{{"t","ready"},{"ready",true},{"revision",revision},{"selectionEpoch",epoch}};
  }
  json begin(int delay) const {
    return {{"t","begin"},{"revision",revision},{"selectionEpoch",epoch},{"console",true},
      {"inputDelay",delay},{"settingsDigest",digest()}};
  }
};
// Shown in the Doubles box over the picture: what is happening, then what each button does.
inline constexpr const char* kHomeLine = "Doubles: press START to search for a match\nZ: invite a partner     B: leave";
inline constexpr const char* kSearchKeys = "\nB: stop searching";
inline constexpr int kMaxTries = 4; // START's search, then three automatic searches after a failed match
inline constexpr const char* kCappedLine = "Couldn't connect all four players after 4 tries\nSTART: try again     B: leave";
// The same cap on the 4-peer route, shown on the Teams character select (B there takes the four home).
inline constexpr const char* kTeamsCappedLine = "Couldn't connect all four players after 4 tries. Press B to go back";
// Who the invite list sent to, for the host's box: the display name when the box's ASCII font can draw it,
// else the username; empty when the host copied a link instead.
inline std::string invited(const json& picked) {
  const auto to=picked.is_object()?picked.value("to",json()):json();
  auto name=friends::text(to,"display_name",24);
  if (name.empty() || std::any_of(name.begin(),name.end(),[](char c) { return c<0x20 || c>0x7e; })) name=friends::text(to,"username",24);
  return std::any_of(name.begin(),name.end(),[](char c) { return c<0x20 || c>0x7e; }) ? std::string() : name;
}
inline std::string invited_line(const std::string& name) {
  return (name.empty() ? std::string("Invite sent - waiting for your partner to join") : "Invited "+name+" - waiting for them to join")+
         "\nZ: invite someone else     B: leave";
}
inline constexpr const char* kStoppedLine = "Search stopped. Press START to search again\nZ: invite a partner     B: leave Doubles";
inline constexpr const char* kPartnerLeftLine = "Your partner left. Press START to search, or Z to invite someone\nB: leave";
// The partner left the match but is still in the home: the duo is back, and the host decides what's next.
inline constexpr const char* kPartnerLeftMatchLine = "Your partner left the match. Press START to search again\nB: leave";
// The partner left the match before it began: shown until Rooms drops them from the home (then the search).
inline constexpr const char* kPartnerLeftNotice = "Your partner left the match.\nB: leave";
// Shown for the moment between a player leaving before the match started and the search going out again.
inline std::string requeue_wait_line(const std::string& why) { return why+" - searching again in a moment\nB: leave"; }
// A roster name the box's ASCII font can draw, else empty.
inline std::string drawable_name(const json& p) {
  const auto name=friends::text(p,"name",24);
  return std::any_of(name.begin(),name.end(),[](char c) { return c<0x20 || c>0x7e; }) ? std::string() : name;
}
enum class Phase { Home, Searching, Admission, Playing, Returning };
struct Home {
  std::string code, compatibility, me, host, generation, partner;
  int waiting=-1; // the queue's last count while searching
  json participants = json::array(), players;
  Phase phase = Phase::Home;
  int revision=0;
  bool result_seen = false;
  // Rooms holds the home for a match (party.playing or nativeTeams.activeRoom) and drops a casual search meanwhile.
  bool leased = false;
  bool stopped = false; // the host's last search was cancelled; the home line says so until the next search
  bool partner_left = false; // the home went from two humans to one; cleared when someone joins
  bool just_joined = false; // the second human arrived since the last search or match: the host's line says so
  std::string stop_line; // why the host's last search ended, when it wasn't a plain cancel (kStoppedLine then)
  std::string status = kHomeLine;
  std::string notice; // shown over status until START, B or a match; observe() never touches it
  // The host's automatic search after a match that failed to start (1-3 of the three after START). A member
  // can't see the host's count, so it keeps the plain lines rather than guess.
  int retry = 0;
  // Why the search after a failed match is running, when a player left before it started ("Your partner
  // left"): the search line leads with it. Cleared when that search ends, finds a match, or the home returns.
  std::string requeue_why;
  bool is_host() const { return !me.empty() && me==host; }
  // The other human's roster entry: a second player on this seat's own connection isn't one.
  const json* other_seat() const {
    if (me.empty()) return nullptr;
    for (const auto& p : participants) if (!text(p,"connectionId").empty() && text(p,"connectionId")!=me) return &p;
    return nullptr;
  }
  // The idle home's line: who is here and who presses START, so a join is visible on both screens.
  std::string home_line() const {
    if (participants.size()==2 && !me.empty() && !host.empty()) {
      const json* seat=other_seat();
      if (!seat) return "Two players on one connection: play Friends instead\nB: leave";
      const std::string other=drawable_name(*seat);
      if (is_host()) {
        if (stopped) return stop_line.empty() ? std::string(kStoppedLine) : stop_line;
        if (!just_joined) return (other.empty() ? std::string("You and your partner") : "You and "+other)+
                                 " - press START to search for a match\nB: leave";
        return (other.empty() ? std::string("Your partner joined") : other+" joined")+
               " - press START to search for a match\nB: leave";
      }
      if (other.empty()) return std::string(stopped ? "Search stopped - " : "You're on the host's team - ")+
                                "the host presses START to search\nB: leave";
      return (stopped ? "Search stopped - "+other+" presses START to search again" :
                        "You're on "+other+"'s team - "+other+" presses START to search")+"\nB: leave";
    }
    if (partner_left) return kPartnerLeftLine;
    if (stopped && !stop_line.empty()) return stop_line;
    return stopped ? kStoppedLine : kHomeLine;
  }
  // The search ended without a cancel from the host (an error, a lost socket): say why, on the idle home's terms.
  void search_ended(const char* why) {
    generation.clear(); partner.clear(); requeue_why.clear(); phase=Phase::Home; stopped=true; stop_line=why; status=home_line();
  }
  std::optional<json> search(bool cancel = false) const {
    if (cancel) return is_host() ? json{{"t","doubles-search"},{"queue",nullptr}} : json{{"t","ready"},{"ready",false}};
    if (!is_host() || phase!=Phase::Home || participants.empty() || participants.size()>2) return {};
    return json{{"t","doubles-search"},{"queue","casual"}};
  }
  std::optional<json> observe(const json& m) {
    if (text(m,"t")=="welcome") me = text(m,"me");
    if (m.count("host")) host = text(m,"host");
    if (console::number(m,"revision",0,INT_MAX)) revision=m["revision"].get<int>();
    if (m.count("participants") && m["participants"].is_array()) {
      const auto before=participants.size(); participants = m["participants"];
      if (before!=participants.size()) stopped=false; // a join or leave is newer news than the stopped search
      just_joined = before!=participants.size() ? participants.size()==2 : just_joined;
      if (before==2 && participants.size()==1) partner_left=true;
      else if (participants.size()>=2) partner_left=false;
    }
    if (m.count("players") && m["players"].is_array()) players = m["players"];
    const bool has_party = m.count("party") && m["party"].is_object(), has_teams = m.count("nativeTeams") && m["nativeTeams"].is_object();
    if (has_party || has_teams)
      leased = (has_party && m["party"].value("playing",json())==true) || (has_teams && !text(m["nativeTeams"],"activeRoom").empty());
    if (phase==Phase::Admission || phase==Phase::Playing || phase==Phase::Returning) return {};
    if (participants.size()>2) status = "Three or four players: play Friends instead";
    else if (phase==Phase::Home) status = home_line();
    if (!m.count("party") || !m["party"].is_object()) return {};
    const auto& party = m["party"];
    const auto q = party.value("queueing",json());
    const auto next = q.is_object() ? text(q,"id") : std::string();
    if (next.empty()) {
      // Placement claims the home before matched reaches its search socket.
      if (party.value("playing",json())==true) return {};
      if (m.count("nativeTeams") && m["nativeTeams"].is_object() &&
          !text(m["nativeTeams"],"activeRoom").empty()) return {};
      if (!generation.empty()) { stopped=true; stop_line.clear(); requeue_why.clear(); }
      generation.clear(); partner.clear(); phase = Phase::Home;
      if (participants.size()<=2) status = home_line();
      return {};
    }
    if (next==generation || !generation_valid(next) || participants.empty() || participants.size()>2 ||
        q.value("queue",json())!="casual" || q.value("n",json())!=participants.size()) return {};
    std::set<std::string> connections;
    for (const auto& p : participants) {
      if (p.value("localIndex",json(0))!=0 || text(p,"connectionId").empty()) return {};
      connections.insert(text(p,"connectionId"));
    }
    if (connections.size()!=participants.size()) return {};
    generation = next; phase = Phase::Searching; partner.clear(); waiting=-1; stopped=partner_left=just_joined=false; status=search_line();
    return search_ticket(compatibility,code,generation,int(participants.size()));
  }
  void progress(const json& m) {
    if (text(m,"t")=="partner") {
      const auto p=m.value("partner",json()); partner=p.is_object()?friends::text(p,"n",40):std::string();
      if (phase==Phase::Searching) status=search_line();
    }
    if (text(m,"t")=="searching") {
      waiting=console::number(m,"waiting",0,100000)?m["waiting"].get<int>():-1;
      status=search_line();
    }
  }
  // The seats still open, from the home's roster and the teammate the queue found (ASCII: the box's font).
  std::string search_line() const {
    const int open=std::clamp(4-int(participants.size())-(partner.empty()?0:1),1,3);
    std::string line=partner.empty()?"Looking for ":partner+" is with you - looking for ";
    if (!requeue_why.empty()) line=requeue_why+" - searching again: "+(partner.empty()?"looking for ":line);
    else if (retry>0) line="Try "+std::to_string(retry+1)+" of "+std::to_string(kMaxTries)+": "+(partner.empty()?"looking for ":line);
    line+=std::to_string(open)+(partner.empty()?(open==1?" player...":" players..."):" more...");
    if (waiting>=0) line+=" ("+std::to_string(waiting)+" waiting)";
    return line+kSearchKeys;
  }
  std::optional<json> matched(const json& m) {
    const auto room = friends::room(m);
    if (phase!=Phase::Searching || text(m,"t")!="matched" || room.empty()) return {};
    phase=Phase::Admission; leased=true; requeue_why.clear(); // matched follows the claim
    status=retry>0 ? "Match found! Connecting the four players (try "+std::to_string(retry+1)+" of "+
                     std::to_string(kMaxTries)+")..." : "Match found! Connecting the four players...";
    return ticket(compatibility,room,4);
  }
  // The caller closes match links before invoking this; home is deliberately untouched.
  json returned() {
    phase=Phase::Home; generation.clear(); partner.clear(); requeue_why.clear(); result_seen=false; stopped=just_joined=false;
    status=home_line();
    return {{"t","party-back"}};
  }
};
// Epochs are process-wide: a later home must supersede reports from any older run.
inline std::atomic<uint64_t> room_epoch{0};
struct RoomReports {
  std::mutex mutex;
  uint64_t epoch=0;
  std::string foreground;
  json room(const Home& h,const std::string& code,const json& participants,const std::string& me,int revision) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!epoch || foreground!=code) { epoch=++room_epoch; foreground=code; }
    const bool home=code==h.code;
    return {{"type","mpRoom"},{"code",code},{"roomEpoch",epoch},{"mode","teams"},{"queue","private"},{"size",4},
      {"nativeTeams",{{"v",1},{"kind",home?"party":"match"},{"home",h.code},
        {"phase",home?(h.phase==Phase::Searching?"searching":"home"):(h.phase==Phase::Playing?"playing":"assembling")}}},
      {"players",home && h.players.is_array()?h.players:participants},{"participants",participants},{"me",me},{"host",home?json(h.host):json()},
      {"ready",!home || h.phase!=Phase::Home},{"revision",revision},
      {"party",{{"full",h.participants.size()>=2},{"playing",h.phase==Phase::Playing || h.phase==Phase::Admission},
        {"queueing",h.phase==Phase::Searching}}},{"canSearch",home && bool(h.search())}};
  }
  json clear() {
    std::lock_guard<std::mutex> lock(mutex); epoch=++room_epoch; foreground.clear();
    return {{"type","mpRoom"},{"code",nullptr},{"players",json::array()},{"roomEpoch",epoch}};
  }
};
// Simulation/UI and I/O threads exchange owned messages only.
struct Launch { console::SessionConfig config; std::unique_ptr<console::SessionLink> link; };
struct UI {
  std::mutex mutex;
  std::string pending_room, next_room, line; bool auto_search=false, active=false, menu=false; int auto_humans=1;
  std::deque<std::string> commands;
  std::deque<json> reports;
  std::atomic<bool> idle{false}; // simulation publishes the menu/session gate
  bool foreground=true, playing=false, stop_match=false, stopped=true, finish_reported=false;
  bool result_in=false; // Rooms sent the match result: the dissolve and link loss that follow end it quietly
  bool picker=false; // the invite list is open: the page reads the pads, the home ignores them
  uint16_t buttons[4]{};
  uint16_t carried[4]{}; // held when the home stopped reading the pads; the game sees them after a release
  std::shared_ptr<Launch> launch;
  // The 4-peer route (doubles_teams::kFourPeer): the match room the four play in on the Slippi Teams
  // client, this seat's team from the queue's matched frame, and whether the online menu still owes
  // the trip to the Teams character select (E6 value 14).
  // teams_started: Rooms sent the match room's start (the console session the host's begin froze).
  // teams_settled: the match has its result, or Rooms reset the room after it started: a normal end.
  // teams_failed: the Teams search's transient failures in this room (each retries, up to kMaxTries).
  // teams_played: a game of this match began on this seat. The room's start comes on the character
  // select, before anyone has picked; until a game begins, the match has not started for the players.
  std::string teams_room; int teams_team=-1; bool teams_enter=false, teams_started=false, teams_settled=false;
  bool teams_played=false;
  int teams_failed=0;
  void clear_teams() { // caller holds mutex
    teams_room.clear(); teams_team=-1; teams_enter=false; teams_started=teams_settled=teams_played=false; teams_failed=0;
  }
  void reset_run() { // caller holds mutex
    commands.clear(); reports.clear(); stop_match=false; stopped=true; finish_reported=result_in=false;
    clear_teams();
    active=true; foreground=true; playing=false; menu=false; picker=false; launch.reset(); lingering.clear();
  }
  void open(std::string room, bool search = false, int humans = 1) { std::lock_guard<std::mutex> l(mutex); pending_room=std::move(room); auto_search=search; auto_humans=humans; }
  void back_if_active() { std::lock_guard<std::mutex> l(mutex); if(active) commands.push_back("back"); }
  // The match has its result (this game reported one, or Rooms sent it): the results screen stays up.
  bool over() { std::lock_guard<std::mutex> l(mutex); return finish_reported || result_in; }
  // This seat's own game reached its end (only then is it on the results screen).
  bool finished() { std::lock_guard<std::mutex> l(mutex); return finish_reported; }
  void command(std::string cmd) { std::lock_guard<std::mutex> l(mutex); commands.push_back(std::move(cmd)); }
  std::string status() { std::lock_guard<std::mutex> l(mutex); return active && foreground?line:std::string(); }
  // "Match found" stays up after the launch hands the pads back: at least a second, then until the
  // online session is up (the CSS), and never past three seconds.
  std::string lingering; uint64_t lingering_from=0;
  void linger(std::string text, uint64_t now) { std::lock_guard<std::mutex> l(mutex); lingering=std::move(text); lingering_from=now; }
  std::string shown(uint64_t now, bool online_match) {
    std::lock_guard<std::mutex> l(mutex);
    if (active && foreground) return online_match?std::string():line;
    if (lingering.empty() || now<lingering_from || now-lingering_from>=(online_match?1000u:3000u)) return {};
    return lingering;
  }
  bool poll(int port, bool connected, uint16_t held) {
    std::lock_guard<std::mutex> l(mutex);
    const bool owned = active && foreground && !playing;
    // The B that leaves the home is still down when the menu gets the pads back, and the menu would
    // read it as a new press and leave the online menu too. Keep each held button off until released.
    carried[port] = owned ? held : carried[port] & (connected ? held : 0);
    connected = connected && !picker;
    const auto edge = connected ? held & ~buttons[port] : 0; buttons[port]=connected?held:0;
    if (!owned) return false;
    if (edge & 0x1000) commands.push_back("search");
    if (edge & 0x10) commands.push_back("invite");
    if (edge & 0x200) commands.push_back("back");
    return true;
  }
  // Buttons the caller clears from a pad that poll() left to the game.
  uint16_t carried_over(int port) { std::lock_guard<std::mutex> l(mutex); return carried[port]; }
  // The Teams search's room while this seat plays the home's match on the 4-peer client, else empty.
  std::string teams_search_room() { std::lock_guard<std::mutex> l(mutex); return active && playing ? teams_room : std::string(); }
  // The Teams search in `room` got the room's start (find_match), or the match settled (the room pump).
  void teams_start(const std::string& room) { std::lock_guard<std::mutex> l(mutex); if (!room.empty() && room==teams_room) teams_started=true; }
  // A game of the home's match began on this seat's Teams client (handle_online_inputs, frame 1).
  void teams_game() { std::lock_guard<std::mutex> l(mutex); if (active && playing && !teams_room.empty()) teams_played=true; }
  void settle_teams(const std::string& room) {
    std::lock_guard<std::mutex> l(mutex);
    if (!room.empty() && room==teams_room && teams_started) { teams_settled=true; result_in=true; }
  }
  // The home's match is over on the 4-peer route: the Teams character select never searches its room again.
  bool teams_over() { std::lock_guard<std::mutex> l(mutex); return active && !teams_room.empty() && teams_settled; }
  // A transient failure of the Teams search in `room`: true while it may search again (kMaxTries in all).
  bool retry_teams(const std::string& room) {
    std::lock_guard<std::mutex> l(mutex);
    if (!active || !playing || room.empty() || room!=teams_room || teams_settled) return false;
    return ++teams_failed<kMaxTries;
  }
  // E6 value 14, from the online menu each frame: 1 once (this call consumes it) when the menu should
  // open the Teams character select for the home's match; byte 1 is this seat's team (0xFF unknown).
  std::array<uint8_t,2> take_teams_entry(bool idle) {
    std::lock_guard<std::mutex> l(mutex);
    std::array<uint8_t,2> out{{0,uint8_t(teams_team==0||teams_team==1?teams_team:0xFF)}};
    if (active && playing && teams_enter && !teams_room.empty() && idle) { teams_enter=false; out[0]=1; }
    return out;
  }
};
inline UI ui;
void entry(const std::string& room, bool search = false, int humans = 1);
void report(const console::SessionConfig&, int winner, bool draw, bool invalid);
} // namespace yougame::doubles
#endif
