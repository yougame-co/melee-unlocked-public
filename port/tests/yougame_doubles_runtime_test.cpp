// The production retained-home coordinator with a fake broker and console SessionIO.
#define DOUBLES_FIXTURE_ONLY
#ifndef MU_DOUBLES_FOUR_PEER
#define MU_DOUBLES_FOUR_PEER 0 // these tests drive the console route; yougame_doubles_four_peer_test.cpp the other
#endif
#include "yougame_doubles_test.cpp"
#include <cstdarg>
namespace {
std::mutex fake_mutex;
std::vector<json> tickets;
std::vector<std::string> order;
std::vector<json> page_reports;
std::deque<json> home_messages;
std::atomic<int> returns{0}, withdrawn{0};
std::atomic<bool> refuse_reports{false}, hold_admission{false}, admission_entered{false};
bool member=false;
std::atomic<bool> member_waits{false}, late_welcome{false}; // the member's home isn't queueing; the welcome waits for push_home
std::atomic<bool> admission_ready{false}, busy_on_roster{false};
int humans=2;
std::string ending="reset";
std::atomic<bool> home_open{false};
std::atomic<bool> silence_home{false}; // the next home closed() check sees Rooms' silent-socket close (4002)
std::atomic<bool> returned{false};
bool granted=true;
std::atomic<bool> fail_attach{false}, helper_gone{false}; // attach refused; helper_gone makes it a no_direct refusal
std::atomic<int> attach_failures{0}; // the next N attaches fail, then they succeed
std::string dissolve_by; // set before entry: each attach sees Rooms dissolve the match, by this connection, then fails
std::atomic<bool> fresh_match{false}; // each admission gets its own match fixture
std::atomic<bool> quiet_search{false}; // search sockets open but never match
std::atomic<bool> lease_mode{false}, lease_held{false}; // like Rooms: a claimed home drops a casual search until released
std::atomic<int> searches{0}, dropped{0}; // doubles-search casual sent / dropped while leased
std::function<void(const json&)> event_sink;
std::unique_ptr<Fixture> match_fixture;
void record(std::string s) { std::lock_guard<std::mutex> lock(fake_mutex); order.push_back(std::move(s)); }
// The released home: what Rooms broadcasts after teams-release or teams-return.
const json released={{"t","lobby"},{"nativeTeams",{{"activeRoom",nullptr}}},{"party",{{"playing",false},{"queueing",nullptr}}}};
json idle_lobby() {
  if(lease_held) return {{"t","lobby"},{"nativeTeams",{{"activeRoom","matchaaa"}}},{"party",{{"playing",true},{"queueing",nullptr}}}};
  return {{"t","lobby"},{"party",{{"queueing",nullptr}}}};
}
}
namespace host {
bool exit_requested() { return false; }
std::mutex log_mutex; std::vector<std::string> log_lines;
void log(const char* format,...) {
  char line[512]; va_list args; va_start(args,format); std::vsnprintf(line,sizeof line,format,args); va_end(args);
  std::lock_guard<std::mutex> lock(log_mutex); log_lines.push_back(line);
}
bool logged(const std::string& want) {
  std::lock_guard<std::mutex> lock(log_mutex);
  return std::find(log_lines.begin(),log_lines.end(),want)!=log_lines.end();
}
}
namespace yougame {
using nlohmann::json;
uint64_t now_ms() { return transport::now_ms(); }
uint64_t random64() { return 12345; }
bool active() { return true; }
struct Identity { bool ok=true; };
Identity wait_identity(int) { return {}; }
bool g_broker=true,g_direct=true;
json bridge(const json& q,int) {
  if(q["type"]=="mpTicket") {
    { std::lock_guard<std::mutex> lock(fake_mutex); tickets.push_back(q); }
    if(q.value("room","")=="matchaaa") {
      admission_entered=true;
      while(hold_admission) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } else if(q["type"]=="mpRoom") {
    { std::lock_guard<std::mutex> lock(fake_mutex); page_reports.push_back(q); }
    record(q["code"].is_null()?"clear":q["code"]=="matchaaa"?"foreground-match":"foreground-home");
    if(busy_on_roster && q["code"]=="matchaaa") doubles::ui.idle=false;
    if(refuse_reports) throw std::runtime_error("page timeout or ok:false");
  }
  json reply={{"ticket","fixture"},{"room",q.value("room","")}};
  if(granted) reply["md"]=1;
  return reply;
}
std::string picked_name; // whom the fake invite list picked
std::string pick_friend_room(const std::function<bool()>&,const std::string&,bool,std::string* invited) {
  if(invited) *invited=picked_name;
  return {};
}
class Socket {
 public:
  std::deque<json> incoming; bool is_home=false; mutable bool closed_=false; mutable int code_=1000;
  bool open_session(const std::string&,const std::string&,const std::string& code,std::string*,int) {
    is_home=!code.empty();
    if(is_home) {
      home_open=true;
      if(!late_welcome) incoming.push_back({{"t","welcome"},{"me",member?"c1":"c0"},{"host","c0"},{"participants",home_roster(humans)}});
      if(member && !member_waits) incoming.push_back({{"t","lobby"},{"party",{{"queueing",{{"id","abcdefghijklmnopqrst"},{"queue","casual"},{"n",humans}}}}}});
    } else if(!quiet_search) {
      incoming.push_back({{"t","partner"},{"partner",{{"n","Partner"},{"u","user"}}}});
      incoming.push_back({{"t","matched"},{"room","matchaaa"}});
    }
    return true;
  }
  bool send(const json& m) {
    const auto t=transport::str(m,"t");
    if(t=="doubles-search" && m["queue"]=="casual") {
      CHECK(is_home); ++searches; record("search");
      if(lease_mode && lease_held) { ++dropped; record("search-dropped"); return true; } // Rooms answers nothing
      incoming.push_back({{"t","lobby"},{"party",{{"queueing",{{"id","abcdefghijklmnopqrst"},{"queue","casual"},{"n",humans}}}}}});
      // Rooms claims the home before delivering matched to the search socket.
      incoming.push_back({{"t","lobby"},{"nativeTeams",{{"activeRoom","matchaaa"}}},
        {"party",{{"playing",true},{"queueing",nullptr}}}});
      if(lease_mode) lease_held=true;
    }
    if(t=="doubles-search" && m["queue"].is_null()) { CHECK(is_home); record("cancel-search"); incoming.push_back(idle_lobby()); }
    if(t=="ready" && m["ready"]==false) {
      CHECK(is_home); ++withdrawn; record("withdraw-home"); hold_admission=false;
      incoming.push_back(idle_lobby());
    }
    if(t=="party-back") { CHECK(is_home); CHECK(home_open); record("party-back"); ++returns; returned=true; }
    return true;
  }
  bool next(json& m,int ms) {
    if(is_home) {
      std::lock_guard<std::mutex> lock(fake_mutex);
      if(!home_messages.empty()) { m=std::move(home_messages.front()); home_messages.pop_front(); return true; }
    }
    if(incoming.empty()) { if(ms)std::this_thread::sleep_for(std::chrono::milliseconds(1)); return false; }
    m=std::move(incoming.front()); incoming.pop_front(); return true;
  }
  void close() { if(!closed_ && is_home) { home_open=false; record("close-home"); } closed_=true; }
  bool closed() const {
    if(is_home && !closed_ && silence_home.exchange(false)) { closed_=true; code_=4002; home_open=false; record("silent-home"); }
    return closed_;
  }
  int close_code() const { return code_; }
  bool busy() const { return false; }
  bool rejoinable() const { return closed_ && (code_==4002 || code_==1006); }
};
struct ConsoleBackend {
  int players=4; bool doubles_room=false;
  std::function<void(const json&)> observe;
  std::function<std::string(int)> renew_ticket;
  bool open(const std::string& t,const std::string& r,int ms) {
    CHECK(home_open); CHECK(renew_ticket); CHECK(renew_ticket(ms)=="fixture");
    event_sink=observe; return match_fixture->stream.open(t,r,ms);
  }
  bool next(json& m,int ms) {
    if(!match_fixture->stream.next(m,ms)) return false;
    if(m["t"]=="welcome") m["nativeTeams"]={{"selectionEpoch",4}};
    if(m.count("console")) {
      m["console"]["settingsDigest"]=doubles::digest(); m["console"]["teamBattle"]=true;
      for(auto& p:m["console"]["participants"]) p["team"]=p["slot"].get<int>()/2;
    }
    return true;
  }
  bool send(const json& m) {
    if(m["t"]=="ready") admission_ready=true;
    if(m["t"]=="leave") record("leave-match");
    if(m["t"]=="finish") {
      CHECK(home_open); record("finish");
      observe({{"t","result"},{"ratings",nullptr}}); record("result");
      // Rooms delivers the return to home as well as the match socket.
      { std::lock_guard<std::mutex> lock(fake_mutex); home_messages.push_back({{"t","teams-dissolved"},{"reason",ending}}); }
      observe({{"t","teams-dissolved"},{"reason",ending}}); record("teams-dissolved");
      return true;
    }
    return match_fixture->stream.send(m);
  }
  bool flush(int) { return true; }
  void close() { CHECK(home_open); record("close-match"); }
};
namespace console {
SessionIO console_io(const std::shared_ptr<ConsoleBackend>& b) {
  if(fresh_match) match_fixture=std::make_unique<Fixture>(4,0); // the last preparation was joined before this one
  auto io=match_fixture->io(); io.compatibility="mu:fingerprint";
  io.attach=[](int) {
    if(!dissolve_by.empty()) { // like Rooms: the match socket hears it first, then the home is released
      event_sink({{"t","teams-dissolved"},{"reason","left"},{"by",dissolve_by}});
      std::lock_guard<std::mutex> lock(fake_mutex); home_messages.push_back(released); return false;
    }
    return !fail_attach && attach_failures.fetch_sub(1)<=0;
  };
  io.helper_missing=[] { return helper_gone.load(); };
  io.request=[](const json& q,int ms) { return q["type"]=="mpTicket"?bridge(q,ms):match_fixture->request(q,ms); };
  io.next_room=[b](json& m,int ms){return b->next(m,ms);};
  io.send_room=[b](const json& m){return b->send(m);};
  return io;
}
} // namespace console
#if MU_DOUBLES_FOUR_PEER
std::string compatibility(int mode) { return "mu:slippi-mode-"+std::to_string(mode); }
#endif
#include "yougame_doubles_runtime.inc"
}
static void reset_fixture() {
  using namespace yougame;
  returned=false; returns=0; silence_home=false; withdrawn=0; admission_entered=false; member=false;
  member_waits=false; late_welcome=false; hold_admission=false; refuse_reports=false; admission_ready=false; busy_on_roster=false; doubles::ui.idle=true;
  fail_attach=false; helper_gone=false; attach_failures=0; fresh_match=false; quiet_search=false;
  lease_mode=false; lease_held=false; searches=0; dropped=0; ending="reset"; dissolve_by.clear();
  { std::lock_guard<std::mutex> lock(fake_mutex); tickets.clear(); order.clear(); page_reports.clear(); home_messages.clear(); }
  match_fixture=std::make_unique<Fixture>(4,0);
}
static std::string line();
#define wait_until(...) wait_until_at(__LINE__, __VA_ARGS__)
static void wait_until_at(int at, const std::function<bool()>& ready) {
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(12);
  while(!ready() && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if(!ready()) {
    std::fprintf(stderr,"wait_until at line %d timed out; box shows: %s\n",at,line().c_str());
    std::lock_guard<std::mutex> lock(host::log_mutex); for (const auto& l : host::log_lines) std::fprintf(stderr,"  log: %s\n",l.c_str());
  }
  CHECK(ready());
}
static void leave_home(bool expect_menu=false) {
  using namespace yougame;
  // B again while a cancelled search winds down (the first B only stops it).
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(12);
  while(doubles::running && std::chrono::steady_clock::now()<until) {
    doubles::ui.command("back");
    for(int n=0;n<300 && doubles::running;++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(!doubles::running);
  CHECK(!home_open);
  std::lock_guard<std::mutex> lock(doubles::ui.mutex); if(expect_menu) CHECK(doubles::ui.menu); doubles::ui.menu=false;
}
static long seen(const char* s);
// After a result the players sit on the results screen: Rooms' dissolve must not pull the game away. Only the
// game's own return to the menu (match-ended) brings the home back.
static void results_then_menu(bool& menu_sent,long results_before=0) {
  using namespace yougame;
  if(menu_sent || seen("result")<=results_before) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(!doubles::ui.stop_match); CHECK(doubles::ui.launch==nullptr); }
  doubles::ui.command("match-ended"); menu_sent=true;
}
static void scenario(int count,const std::string& reason,bool page_failure=false) {
  using namespace yougame;
  reset_fixture(); humans=count; ending=reason; refuse_reports=page_failure;
  // A fresh entry must discard all commands/reports and stop state from an earlier run.
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex);
    doubles::ui.commands.push_back("back"); doubles::ui.reports.push_back({{"t","finish"}}); doubles::ui.stop_match=true;
  }
  doubles::entry("new",true,count);
  for(int round=0;round<2;++round) {
    std::shared_ptr<doubles::Launch> live; bool reported=false, menu_sent=false;
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(12);
    while(std::chrono::steady_clock::now()<until && returns<=round) {
      if(reason=="reset" && reported) results_then_menu(menu_sent,round);
      bool stop=false;
      { std::lock_guard<std::mutex> lock(doubles::ui.mutex);
        if(!live) live=std::exchange(doubles::ui.launch,{});
        stop=std::exchange(doubles::ui.stop_match,false);
      }
      if(live && !reported) {
        CHECK(home_open);
        // The launch leaves "Match found" up a moment; the return below drops it.
        { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.lingering.find("Match found!")==0); }
        if(reason=="reset") doubles::report(live->config,1,false,false);
        else { record("teams-dissolved"); event_sink({{"t","teams-dissolved"},{"reason",reason}}); }
        reported=true;
      }
      if(stop) {
        live.reset(); record("close-links");
        // Race an engine-end notification with runtime back(): this must not leave the home.
        std::lock_guard<std::mutex> lock(doubles::ui.mutex);
        doubles::ui.commands.push_back("match-ended"); doubles::ui.stopped=true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(returned); CHECK(returns==round+1); CHECK(home_open); CHECK(doubles::running);
    { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.foreground); CHECK(doubles::ui.lingering.empty()); }
    if(round==0) { match_fixture=std::make_unique<Fixture>(4,0); doubles::ui.command("search"); }
  }
  { std::lock_guard<std::mutex> lock(fake_mutex);
    CHECK(tickets.size()==7); CHECK(tickets[0]["lobby"]["minPlayers"]==2);
    CHECK(tickets[1].count("party")==size_t(count==2)); CHECK(tickets[2]["room"]=="matchaaa");
    for(const auto& q:tickets) {
      CHECK(q["teamsLobby"]==1); CHECK(!q.count("teams_lobby")); CHECK(!q.count("party_search")); CHECK(!q.count("local_players"));
      // A room ticket (home or match) names no search; a search names the home and its generation.
      if(q.count("room")) { CHECK(q["queue"]=="private"); for(const char* k:{"party","party_room","partySearch"}) CHECK(!q.count(k)); }
      else { CHECK(q["queue"]=="casual"); CHECK(q.count("party_room")); CHECK(q["partySearch"].is_string());
             if(q.count("party")) CHECK(q["party"]==json({{"id",q["partySearch"]},{"n",2}})); }
    }
    for(size_t i=0;i<order.size();++i) if(order[i]=="party-back") {
      CHECK(i>=3); CHECK(order[i-1]=="foreground-home"); CHECK(order[i-2]=="close-match"); CHECK(order[i-3]=="close-links");
    }
    const auto at=[](const char* s){return std::find(order.begin(),order.end(),s)-order.begin();};
    if(reason=="reset") CHECK(at("result")<at("teams-dissolved"));
    else { CHECK(at("result")==order.size()); CHECK(at("finish")==order.size()); }
    CHECK(at("teams-dissolved")<at("close-links"));
    CHECK(at("close-links")<at("close-match")); CHECK(at("close-match")<at("party-back"));
    CHECK(std::find(order.begin(),order.end(),"close-home")==order.end());
    // Roster failures in both home and match reports did not tear down either round.
    CHECK(std::count(order.begin(),order.end(),"foreground-match")>=2);
  }
  leave_home(true);
}
static void busy_member() {
  using namespace yougame;
  reset_fixture(); member=true; humans=2; doubles::ui.idle=false;
  doubles::entry("homeaaaa");
  wait_until([] { return withdrawn.load()>0; });
  CHECK(home_open); CHECK(doubles::running);
  { std::lock_guard<std::mutex> lock(fake_mutex); CHECK(tickets.size()==1); CHECK(!std::count(order.begin(),order.end(),"cancel-search")); }
  // A barrier command makes the runtime process the queued withdrawal snapshot before leave.
  doubles::ui.command("foreground");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  leave_home();
}
static void cancel_admission(bool opened=false) {
  using namespace yougame;
  reset_fixture(); humans=2; hold_admission=!opened;
  if(opened) match_fixture->reply_start=false;
  doubles::entry("new",true,2);
  wait_until([&] { return opened?admission_ready.load():admission_entered.load(); });
  doubles::ui.command("back");
  wait_until([] { return returns.load()==1; });
  CHECK(withdrawn==1); CHECK(home_open); CHECK(doubles::running);
  { std::lock_guard<std::mutex> lock(fake_mutex);
    CHECK(std::count(order.begin(),order.end(),"leave-match")==int(opened));
    // The host also stops the search; a member never sends it.
    CHECK(std::count(order.begin(),order.end(),"cancel-search")==1);
    CHECK(std::find(order.begin(),order.end(),"withdraw-home")<std::find(order.begin(),order.end(),"party-back"));
  }
  leave_home();
}
static void busy_during_admission() {
  using namespace yougame;
  reset_fixture(); humans=2; busy_on_roster=true;
  doubles::entry("new",true,2);
  wait_until([] { return returns.load()==1; });
  CHECK(withdrawn==1); CHECK(home_open); CHECK(doubles::running);
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex);
    CHECK(!doubles::ui.launch); CHECK(!doubles::ui.menu); CHECK(!doubles::ui.foreground);
  }
  leave_home();
}
// An md:1 join into another home leaves this one and opens that one; the same home only comes forward.
static void switch_home() {
  using namespace yougame;
  reset_fixture(); humans=2; // the host: a member here would follow the fixture's queueing party into a search
  doubles::entry("homeaaaa");
  wait_until([] { return home_open.load(); });
  doubles::entry("homebbbb");
  wait_until([] { std::lock_guard<std::mutex> lock(fake_mutex); return tickets.size()==2 && home_open.load(); });
  { std::lock_guard<std::mutex> lock(fake_mutex);
    CHECK(tickets[1]["room"]=="homebbbb"); CHECK(std::count(order.begin(),order.end(),"close-home")==1);
  }
  CHECK(doubles::running);
  doubles::entry("homebbbb");
  doubles::ui.command("foreground");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  { std::lock_guard<std::mutex> lock(fake_mutex); CHECK(tickets.size()==2); }
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.next_room.empty()); }
  leave_home();
}
// Without the route's md:1 grant the home must never open.
static void not_granted() {
  using namespace yougame;
  reset_fixture(); granted=false; home_open=false;
  { std::lock_guard<std::mutex> lock(fake_mutex); tickets.clear(); }
  doubles::entry("new",true,2);
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(doubles::running && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(!doubles::running); CHECK(!home_open);
  { std::lock_guard<std::mutex> lock(fake_mutex); CHECK(tickets.size()==1); }
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.menu); doubles::ui.menu=false; }
  granted=true;
}
// The home goes silent (4002) while the engine winds the match down: rejoin it, then party-back.
static void silent_home_after_match() {
  using namespace yougame;
  reset_fixture(); humans=2; ending="reset";
  doubles::entry("new",true,2);
  std::shared_ptr<doubles::Launch> live; bool reported=false, menu_sent=false;
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(12);
  while(std::chrono::steady_clock::now()<until && returns<1) {
    if(reported) results_then_menu(menu_sent);
    bool stop=false;
    { std::lock_guard<std::mutex> lock(doubles::ui.mutex);
      if(!live) live=std::exchange(doubles::ui.launch,{});
      stop=std::exchange(doubles::ui.stop_match,false);
    }
    if(live && !reported) { doubles::report(live->config,1,false,false); reported=true; }
    if(stop) {
      live.reset(); record("close-links"); silence_home=true;
      std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.stopped=true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(returns==1); CHECK(home_open); CHECK(doubles::running);
  { std::lock_guard<std::mutex> lock(fake_mutex);
    const auto at=[](const char* s){return std::find(order.begin(),order.end(),s)-order.begin();};
    CHECK(at("silent-home")<at("party-back")); CHECK(at("close-match")<at("party-back"));
    // Home, search, match open and its renewal, then one fresh home ticket for the rejoin.
    CHECK(tickets.size()==5); CHECK(tickets[4]["room"]==tickets[0]["room"]);
  }
  leave_home(true);
}
static std::string line() { std::lock_guard<std::mutex> lock(yougame::doubles::ui.mutex); return yougame::doubles::ui.line; }
static bool shows(const char* s) { return line().find(s)!=std::string::npos; }
static long casual_tickets() {
  std::lock_guard<std::mutex> lock(fake_mutex);
  return std::count_if(tickets.begin(),tickets.end(),[](const json& q) { return q.value("queue",json())=="casual"; });
}
static long seen(const char* s) { std::lock_guard<std::mutex> lock(fake_mutex); return std::count(order.begin(),order.end(),s); }
static void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
static void push_home(const json& m) { std::lock_guard<std::mutex> lock(fake_mutex); home_messages.push_back(m); }
// The host's requeue as a member sees it: a new queueing generation.
static const json next_generation={{"t","lobby"},{"party",{{"queueing",{{"id","bcdefghijklmnopqrstu"},{"queue","casual"},{"n",2}}}}}};
static const char* kCapped=yougame::doubles::kCappedLine;
// A failed admission sends the host back to the queue three times per START, then says so.
static void requeue_cap() {
  using namespace yougame;
  reset_fixture(); humans=2; fail_attach=true; fresh_match=true;
  doubles::entry("new",true,2);
  wait_until([] { return returns.load()==4; }); settle();
  CHECK(searches==4); CHECK(returns==4); CHECK(withdrawn==4); CHECK(shows(kCapped));
  doubles::ui.command("search");
  wait_until([] { return returns.load()==8; }); settle();
  CHECK(searches==8); CHECK(shows(kCapped)); CHECK(seen("search-dropped")==0);
  leave_home();
}
// The third automatic search may still play; a match that ends after it started does not search again.
static void played_match_ends_without_search() {
  using namespace yougame;
  reset_fixture(); humans=2; attach_failures=3; fresh_match=true;
  doubles::entry("new",true,2);
  std::shared_ptr<doubles::Launch> live; bool reported=false, menu_sent=false;
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(12);
  while(std::chrono::steady_clock::now()<until && returns<4) {
    if(reported) results_then_menu(menu_sent);
    bool stop=false;
    { std::lock_guard<std::mutex> lock(doubles::ui.mutex);
      if(!live) live=std::exchange(doubles::ui.launch,{});
      stop=std::exchange(doubles::ui.stop_match,false);
    }
    if(live && !reported) { CHECK(searches==4); CHECK(!shows(kCapped)); doubles::report(live->config,1,false,false); reported=true; }
    if(stop && live) { live.reset(); std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.stopped=true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(reported); CHECK(returns==4); settle();
  CHECK(searches==4); CHECK(shows("START to search"));
  { std::lock_guard<std::mutex> lock(fake_mutex);
    CHECK(std::find(order.begin(),order.end(),"result")!=order.end());
    CHECK(std::find(std::find(order.begin(),order.end(),"result"),order.end(),"search")==order.end());
  }
  // Playing reset the count (as START does): a new run of failures gets three more.
  fail_attach=true; doubles::ui.command("search");
  wait_until([] { return returns.load()==8; }); settle();
  CHECK(searches==8); CHECK(shows(kCapped));
  leave_home(true);
}
// A seat whose helper refused attach (no_direct) stays out of the queue and keeps saying why.
static void helper_missing_host() {
  using namespace yougame;
  reset_fixture(); humans=2; fail_attach=true; helper_gone=true; fresh_match=true;
  doubles::entry("new",true,2);
  wait_until([] { return shows(kHelperMissing); }); settle();
  CHECK(searches==1); CHECK(returns==1);
  const auto before=seen("foreground-home");
  push_home({{"t","lobby"},{"participants",home_roster(2)},{"nativeTeams",{{"activeRoom",nullptr}}},
    {"party",{{"playing",false},{"queueing",nullptr}}}});
  wait_until([&] { return seen("foreground-home")>before; });
  CHECK(shows(kHelperMissing)); CHECK(shows("\nB to leave")); CHECK(searches==1);
  leave_home();
}
// A member never searches itself; it follows its host's requeue unless its own helper is missing.
static void member_requeue(bool helper) {
  using namespace yougame;
  reset_fixture(); member=true; humans=2; fail_attach=true; helper_gone=helper; fresh_match=true;
  doubles::entry("homeaaaa");
  wait_until([] { return returns.load()==1; });
  if(helper) wait_until([] { return shows(kHelperMissing); });
  settle();
  CHECK(searches==0); CHECK(casual_tickets()==1); CHECK(withdrawn==1);
  push_home(next_generation);
  if(helper) {
    wait_until([] { return withdrawn.load()==2; }); settle();
    CHECK(casual_tickets()==1); CHECK(shows(kHelperMissing)); CHECK(shows("\nB to leave"));
    // The member's own START clears it: the host's next search pulls it in again.
    doubles::ui.command("search");
    wait_until([] { return !shows(kHelperMissing); });
    push_home({{"t","lobby"},{"party",{{"queueing",{{"id","cdefghijklmnopqrstuv"},{"queue","casual"},{"n",2}}}}}});
    wait_until([] { return casual_tickets()==2; });
  } else {
    wait_until([] { return casual_tickets()==2 && returns.load()==2; }); settle();
  }
  CHECK(searches==0); CHECK(seen("cancel-search")==0);
  leave_home();
}
// Rooms releases the home, then says teams-dissolved on it, while admission is still running: one requeue,
// unless the member (c1) caused it.
static void home_dissolve_before_playing(const char* by) {
  using namespace yougame;
  reset_fixture(); humans=2; hold_admission=true; fresh_match=true;
  doubles::entry("new",true,2);
  wait_until([] { return admission_entered.load(); });
  quiet_search=true;
  push_home(released); push_home({{"t","teams-dissolved"},{"reason","left"},{"by",by}});
  wait_until([] { std::lock_guard<std::mutex> lock(doubles::ui.mutex); return doubles::ui.stop_match; });
  hold_admission=false;
  if(std::string(by)=="c1") { wait_until([] { return returns.load()==1; }); settle(); CHECK(searches==1); CHECK(shows("START to search")); }
  else { wait_until([] { return searches.load()==2; }); settle(); CHECK(searches==2); }
  CHECK(returns==1); CHECK(withdrawn==0);
  leave_home();
}
// The match socket names who ended it: the member's own B, closed game or switch keeps the duo home;
// a stranger (c9) leaving is a failed start like any other.
static void match_dissolve_by(bool ours) {
  using namespace yougame;
  reset_fixture(); humans=2; fresh_match=true; dissolve_by=ours?"c1":"c9";
  doubles::entry("new",true,2);
  if(ours) { wait_until([] { return returns.load()==1; }); settle(); CHECK(searches==1); CHECK(shows("START to search")); }
  else wait_until([] { return searches.load()==2; });
  leave_home();
}
// A requeue is for the duo that pressed START: not once the partner left (a solo search with a stranger).
static void requeue_needs_start_duo() {
  using namespace yougame;
  reset_fixture(); humans=2; fail_attach=true; fresh_match=true; lease_mode=true;
  doubles::entry("new",true,2);
  wait_until([] { return returns.load()==1; }); settle();
  CHECK(searches==1);
  const auto before=seen("foreground-home");
  { std::lock_guard<std::mutex> lock(fake_mutex); lease_held=false;
    home_messages.push_back({{"t","lobby"},{"participants",home_roster(1)},{"nativeTeams",{{"activeRoom",nullptr}}},
      {"party",{{"playing",false},{"queueing",nullptr}}}});
  }
  wait_until([&] { return seen("foreground-home")>before; }); settle();
  CHECK(searches==1); CHECK(dropped==0); CHECK(shows("START to search"));
  leave_home();
}
// Nor from a member Rooms made host while the match was connecting: members never search.
static void promoted_member_never_searches() {
  using namespace yougame;
  reset_fixture(); member=true; humans=2; fail_attach=true; hold_admission=true; fresh_match=true;
  doubles::entry("homeaaaa");
  wait_until([] { return admission_entered.load(); });
  const json solo=json::array({{{"id","p1"},{"connectionId","c1"},{"slot",0},{"localIndex",0}}});
  push_home({{"t","lobby"},{"host","c1"},{"participants",solo},{"nativeTeams",{{"activeRoom",nullptr}}},
    {"party",{{"playing",false},{"queueing",nullptr}}}});
  settle(); hold_admission=false;
  wait_until([] { return returns.load()==1; }); settle();
  CHECK(searches==0); CHECK(casual_tickets()==1);
  leave_home();
}
// A failure while the game is busy keeps the search owed until the home is back in front of an idle game;
// B before then drops it.
static void requeue_waits_for_idle(bool leave) {
  using namespace yougame;
  reset_fixture(); humans=2; fail_attach=true; hold_admission=true; fresh_match=true;
  doubles::entry("new",true,2);
  wait_until([] { return admission_entered.load(); });
  doubles::ui.idle=false; hold_admission=false;
  wait_until([] { return returns.load()==1; }); settle();
  CHECK(searches==1); CHECK(withdrawn==1);
  doubles::ui.idle=true; settle();
  CHECK(searches==1); // still retained behind the game
  if(leave) {
    // One pass sees both: the home comes forward and B leaves it before the owed search could go out.
    { std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.commands.push_back("foreground"); doubles::ui.commands.push_back("back"); }
    wait_until([] { return !doubles::running.load(); });
    CHECK(searches==1);
  } else {
    doubles::ui.command("foreground");
    wait_until([] { return searches.load()==2; });
  }
  leave_home();
}
// START while a match is connecting keeps why that match fails: the helper notice, with no wasted search first.
static void start_during_admission_keeps_reason() {
  using namespace yougame;
  reset_fixture(); humans=2; fail_attach=true; helper_gone=true; hold_admission=true; fresh_match=true;
  doubles::entry("new",true,2);
  wait_until([] { return admission_entered.load(); });
  doubles::ui.command("search"); settle();
  hold_admission=false;
  wait_until([] { return shows(kHelperMissing); }); settle();
  CHECK(searches==1); CHECK(returns==1);
  leave_home();
}
// The search waits while Rooms still holds the home, and goes out once it is released.
static void requeue_waits_for_release() {
  using namespace yougame;
  reset_fixture(); humans=2; fail_attach=true; fresh_match=true; lease_mode=true;
  doubles::entry("new",true,2);
  wait_until([] { return returns.load()==1; }); settle();
  CHECK(searches==1); CHECK(dropped==0);
  { std::lock_guard<std::mutex> lock(fake_mutex); lease_held=false; home_messages.push_back(released); }
  wait_until([] { return searches.load()==2 && returns.load()==2; }); settle();
  CHECK(searches==2); CHECK(dropped==0);
  leave_home();
}
// A START the home can't act on says why on screen and never moves anything; B still leaves.
static void member_start_says_why() {
  using namespace yougame;
  reset_fixture(); member=true; member_waits=true; humans=2;
  doubles::entry("homeaaaa");
  wait_until([] { return shows("START to search"); });
  doubles::ui.command("search");
  wait_until([] { return shows("Only the host can search - ask your partner to press START"); }); settle();
  CHECK(searches==0); CHECK(shows("B: leave"));
  doubles::ui.command("invite");
  wait_until([] { return shows("Only the host can invite - ask your partner to press Z"); });
  // The host's search replaces the notice with the search line.
  quiet_search=true; push_home(next_generation);
  wait_until([] { return shows("Looking for"); }); CHECK(!shows("Only the host"));
  leave_home();
}
// A START before the welcome in a home where this seat turns out to be the member never searches.
static void member_start_before_welcome() {
  using namespace yougame;
  reset_fixture(); member=true; member_waits=true; late_welcome=true; humans=2;
  doubles::entry("homeaaaa");
  wait_until([] { return home_open.load() && !shows("Opening Doubles home"); });
  doubles::ui.command("search");
  wait_until([] { return shows("Joining the Doubles home"); });
  push_home({{"t","welcome"},{"me","c1"},{"host","c0"},{"participants",home_roster(2)}});
  wait_until([] { return shows("Only the host can search"); }); settle();
  CHECK(searches==0); CHECK(casual_tickets()==0);
  // The host leaves: the notice about them goes, and the box says the partner left.
  auto alone=home_roster(2); alone.erase(0);
  push_home({{"t","lobby"},{"host","c1"},{"participants",alone}});
  wait_until([] { return shows("Your partner left"); }); CHECK(!shows("Only the host can search"));
  leave_home();
}
// A START before the home's welcome is kept: the search starts once the home has joined.
static void start_before_welcome() {
  using namespace yougame;
  reset_fixture(); humans=1; late_welcome=true; quiet_search=true;
  doubles::entry("new");
  wait_until([] { return home_open.load() && shows("Opening Doubles home") == false; });
  doubles::ui.command("search");
  wait_until([] { return shows("Joining the Doubles home"); }); settle();
  CHECK(searches==0);
  push_home({{"t","welcome"},{"me","c0"},{"host","c0"},{"participants",home_roster(1)}});
  wait_until([] { return searches.load()==1; });
  leave_home();
}
// Pads reach the home's poll without a focus gate; every button it reads becomes a command.
static void poll_reads_buttons() {
  using namespace yougame;
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.reset_run(); for (auto& b:doubles::ui.buttons) b=0; }
  CHECK(doubles::ui.poll(0,true,0x200)); CHECK(doubles::ui.poll(1,true,0x1000)); CHECK(doubles::ui.poll(2,true,0x10));
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex);
    CHECK(doubles::ui.commands.size()==3); CHECK(doubles::ui.commands[0]=="back"); CHECK(doubles::ui.commands[1]=="search");
    CHECK(doubles::ui.commands[2]=="invite");
    doubles::ui.commands.clear(); for (auto& b:doubles::ui.buttons) b=0;
    doubles::ui.picker=true; }
  // The invite list is open: the page reads the pads, the home only keeps them from the game.
  CHECK(doubles::ui.poll(0,true,0x1200)); CHECK(doubles::ui.poll(0,true,0x10));
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.commands.empty());
    doubles::ui.picker=false; doubles::ui.active=false; for (auto& b:doubles::ui.buttons) b=0; }
}
// The B that leaves the home is still held when the menu gets the pads back; the game sees it only
// after a release, so it never also backs out of the online menu. A new press goes through.
static void held_back_stays_off_after_leaving() {
  using namespace yougame;
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.reset_run(); for (auto& b:doubles::ui.buttons) b=0; }
  CHECK(doubles::ui.poll(0,true,0x200)); CHECK(doubles::ui.poll(1,true,0x100));
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.commands.clear(); doubles::ui.active=false; }
  CHECK(!doubles::ui.poll(0,true,0x200)); CHECK(doubles::ui.carried_over(0)==0x200);
  CHECK(!doubles::ui.poll(1,true,0x110)); CHECK(doubles::ui.carried_over(1)==0x100);
  CHECK(!doubles::ui.poll(0,true,0x200)); CHECK(doubles::ui.carried_over(0)==0x200);
  CHECK(!doubles::ui.poll(0,true,0)); CHECK(doubles::ui.carried_over(0)==0);
  CHECK(!doubles::ui.poll(0,true,0x200)); CHECK(doubles::ui.carried_over(0)==0);
  // An unplugged or unfocused pad reads as released.
  CHECK(!doubles::ui.poll(1,false,0x100)); CHECK(doubles::ui.carried_over(1)==0);
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); CHECK(doubles::ui.commands.empty());
    for (auto& b:doubles::ui.buttons) b=0; }
}
// The invite list names whom it sent to, in the box's ASCII; a copied link names no one.
static void invited_names() {
  using namespace yougame;
  CHECK(doubles::invited({{"to",{{"username","ada"},{"display_name","Ada L"}}}})=="Ada L");
  CHECK(doubles::invited({{"to",{{"username","ada"},{"display_name","Ad\u00e9"}}}})=="ada");
  CHECK(doubles::invited({{"to",nullptr},{"code","homeaaaa"}}).empty());
  CHECK(doubles::invited(json()).empty());
  CHECK(doubles::invited_line("Ada")=="Invited Ada - waiting for them to join\nZ: invite someone else     B: leave");
  CHECK(doubles::invited_line("").rfind("Invite sent - waiting for your partner",0)==0);
}
// "Match found" outlives the launch for a second, then until the online session is up, at most three.
static void match_found_lingers() {
  using namespace yougame;
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.reset_run(); doubles::ui.line="home"; }
  CHECK(doubles::ui.shown(1000,false)=="home"); CHECK(doubles::ui.shown(1000,true).empty());
  doubles::ui.linger("Match found!",1000);
  { std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.foreground=false; doubles::ui.playing=true; }
  CHECK(doubles::ui.shown(1500,true)=="Match found!"); CHECK(doubles::ui.shown(2000,true).empty());
  CHECK(doubles::ui.shown(3999,false)=="Match found!"); CHECK(doubles::ui.shown(4000,false).empty());
  CHECK(doubles::ui.shown(999,false).empty());
  std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.reset_run(); CHECK(doubles::ui.lingering.empty());
  doubles::ui.active=false; doubles::ui.line.clear(); doubles::ui.playing=false;
}
// Z, a pick, then the box says whom the host invited until they join.
static void invite_names_the_partner() {
  using namespace yougame;
  reset_fixture(); humans=1; quiet_search=true; picked_name="Ada";
  doubles::entry("new");
  wait_until([] { return home_open.load() && shows("START to search"); });
  doubles::ui.command("invite");
  wait_until([] { return shows("Invited Ada - waiting for them to join"); }); CHECK(shows("Z: invite someone else"));
  { std::lock_guard<std::mutex> lock(host::log_mutex); host::log_lines.clear(); }
  auto roster=home_roster(2); roster[1]["name"]="Ada";
  push_home({{"t","lobby"},{"participants",roster},{"party",{{"queueing",nullptr}}}});
  // The host's cue to press START: the box names who joined, and the log says so.
  wait_until([] { return shows("Ada joined - press START to search for a match"); }); CHECK(!shows("Invited Ada"));
  wait_until([] { return host::logged("doubles: partner joined (2 players in the home)"); });
  push_home({{"t","lobby"},{"participants",home_roster(1)}});
  wait_until([] { return shows("Your partner left"); });
  wait_until([] { return host::logged("doubles: partner left (1 player in the home)"); });
  picked_name.clear();
  doubles::ui.command("back");
  wait_until([] { return !doubles::running.load(); }); CHECK(!home_open);
  std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.menu=false;
}
// B during a search stops only the search, and says so; the next B leaves the home.
static void back_from_search_then_leave() {
  using namespace yougame;
  reset_fixture(); humans=1; quiet_search=true;
  doubles::entry("new");
  wait_until([] { return home_open.load() && shows("START to search"); });
  doubles::ui.command("search");
  wait_until([] { return shows("Looking for 3 players..."); }); CHECK(shows("B: stop searching"));
  doubles::ui.command("back");
  wait_until([] { return shows("Search stopped. Press START to search again"); }); settle();
  CHECK(doubles::running); CHECK(home_open); CHECK(seen("cancel-search")==1); CHECK(shows("B: leave Doubles"));
  doubles::ui.command("back");
  wait_until([] { return !doubles::running.load(); }); CHECK(!home_open);
  std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.menu=false;
}
// yougame_doubles_four_peer_test.cpp reuses this fixture with its own main.
#ifdef DOUBLES_RUNTIME_FIXTURE_ONLY
int runtime_existing_main() {
#else
int main() {
#endif
  poll_reads_buttons(); held_back_stays_off_after_leaving(); invited_names(); match_found_lingers();
  invite_names_the_partner();
  for(const auto* why:{"reset","left","timed_out","no_show","removed"}) scenario(2,why);
  scenario(1,"reset"); scenario(2,"reset",true);
  busy_member(); cancel_admission(); cancel_admission(true); busy_during_admission();
  switch_home();
  not_granted();
  silent_home_after_match();
  requeue_cap(); played_match_ends_without_search(); helper_missing_host();
  member_requeue(false); member_requeue(true); home_dissolve_before_playing(""); home_dissolve_before_playing("c1");
  requeue_waits_for_release();
  match_dissolve_by(true); match_dissolve_by(false); requeue_needs_start_duo(); promoted_member_never_searches();
  requeue_waits_for_idle(false); requeue_waits_for_idle(true); start_during_admission_keeps_reason();
  member_start_says_why(); start_before_welcome(); member_start_before_welcome();
  back_from_search_then_leave();
  std::printf("Doubles retained-home coordinator: %d checks passed\n",checks.load());
  return 0;
}
