// Included by the native bridge adapter, inside namespace yougame.
namespace doubles {
namespace {
std::atomic<bool> running{false};
struct MatchEvents {
  std::mutex mutex;
  std::deque<json> messages;
  std::atomic<bool> cancelled{false}, opened{false};
};
void publish(const Home& h, bool foreground = true) {
  std::lock_guard<std::mutex> lock(ui.mutex);
  ui.active=true; ui.foreground=foreground; ui.playing=h.phase==Phase::Playing;
  ui.line=h.notice.empty()?h.status:h.notice;
  if (!h.partner.empty() && h.phase!=Phase::Searching) ui.line += "\nPartner: " + h.partner; // the search line names them
}
// A cold Rooms isolate can refuse an open while it fetches the build list; the ticket is still good.
bool open_retrying_busy(Socket& socket, const std::string& ticket, const std::string& room, std::string* error) {
  if (socket.open_session("",ticket,room,error,10000)) return true;
  if (!socket.busy()) return false;
  host::log("doubles: room open refused (busy); retrying once");
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  return socket.open_session("",ticket,room,error,10000);
}
void run(std::string code, bool auto_search, int auto_humans) {
  Home h; std::unique_ptr<Socket> home, search;
  auto page=std::make_shared<RoomReports>();
  auto report_room=[](const json& q) {
    try { bridge(q,1000); } catch (...) { host::log("doubles: page room report unavailable"); }
  };
  std::shared_ptr<ConsoleBackend> match;
  std::shared_ptr<MatchEvents> events;
  auto phase=std::make_shared<std::atomic<Phase>>(Phase::Home); // match-room reports read it off the run thread
  std::future<std::shared_ptr<Launch>> preparation;
  // The four play the home's match on the 4-peer Slippi Teams client (doubles_teams::kFourPeer): no
  // console admission and no ConsoleBackend; the game's own Teams session owns the match room.
  bool teams_route=false;
  std::shared_ptr<std::string> failure; // why the last admission failed, written by the preparation thread before it returns
  std::future<std::string> picker; // who it invited
  std::string invite_line; // h.notice while the invited partner hasn't joined
  auto picker_stop=std::make_shared<std::atomic<bool>>(false);
  uint64_t ping=now_ms(), search_ping=ping;
  bool done=false, retained=false, owe_back=false;
  bool press_notice=false; // h.notice explains a press the home couldn't act on; the host's search replaces it
  bool wait_notice=false; // h.notice is the moment before the search after a player left (teams_failure); it replaces it
  bool deferred_start=false; // START before the welcome: search once joined, if this seat turns out to be the host
  int requeues=0; // automatic searches since the last START
  bool pending_requeue=false; // sent once Rooms releases the home (a casual search is dropped while it is leased)
  bool helper_out=false; // this seat's link helper refused attach (no_direct): it stays out of the queue until START
  std::set<std::string> start_seats; bool start_host=false; // the duo and role that pressed START; a requeue needs both
  std::set<std::string> match_seats; // the home's seats when the 4-peer match was found: who "your partner" is
  // The partner who left the 4-peer match before it began: once Rooms drops them from the roster and this
  // seat leads the home, the seats left search again (requeue_after_partner); requeue_left_until bounds the wait.
  std::string requeue_left; uint64_t requeue_left_until=0;
  uint64_t requeue_why_until=0; // a member's h.requeue_why lapses if the host's search doesn't follow by then
  std::deque<json> held; // home messages that arrived while the results screen was up
  size_t humans=0; // the home's roster size at the last message, for the joined/left log lines
  uint64_t rejoin_until=0, rejoin_backoff=250; // one window and backoff across drops, so a flapping home cannot storm
  // The home is silent-closed (4002) after 30 s without traffic, so every wait on this thread keeps it alive.
  auto keepalive=[&] {
    if (home && !home->closed() && now_ms()-ping>=5000) { home->send({{"t","ping"},{"at",now_ms()}}); ping=now_ms(); }
  };
  auto stop_search=[&] { if (search) { search->send({{"t","cancel"}}); search->close(); search.reset(); } };
  auto withdraw=[&] {
    // The home owns the claim even before a match socket has opened. Any member may cancel;
    // the host also stops the search (Rooms refuses that while someone is away, Ready false still dissolves).
    if (!home || home->closed()) return;
    if (h.is_host()) home->send({{"t","doubles-search"},{"queue",nullptr}});
    home->send({{"t","ready"},{"ready",false}});
  };
  auto back=[&](bool foreground=true) {
    foreground=foreground && (h.phase==Phase::Playing || ui.idle.load());
    const auto entered=now_ms();
    host::log("doubles: returning to the home (%s)",foreground?"foreground":"retained");
    // Main owns the engine and its datagram worker. Retire it before closing its backend.
    { std::lock_guard<std::mutex> lock(ui.mutex); ui.stop_match=true; ui.lingering.clear(); }
    if (events) events->cancelled=true;
    if (preparation.valid()) {
      // Preparation owns the provisional Link until it returns. Never close underneath it.
      auto discarded=preparation.get(); discarded.reset();
    }
    // Only now is a match socket opened by preparation known; leave it so Rooms frees the seat.
    if (match && events && events->opened) { match->send({{"t","leave"}}); match->flush(1000); }
    std::shared_ptr<Launch> abandoned;
    { std::lock_guard<std::mutex> lock(ui.mutex); abandoned=std::exchange(ui.launch,{}); ui.playing=false; ui.foreground=false; }
    abandoned.reset();
    for (;;) {
      bool stopped;
      { std::lock_guard<std::mutex> lock(ui.mutex); stopped=ui.stopped; }
      if (stopped || host::exit_requested()) break;
      keepalive();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto engine_ms=now_ms()-entered;
    if (match) { match->close(); match.reset(); }
    if (teams_route) {
      teams_route=false;
      std::lock_guard<std::mutex> lock(ui.mutex);
      // A game of the match began: its failures are over, so the next leave on a select has three searches again.
      if (ui.teams_played) { requeues=0; h.retry=0; }
      ui.clear_teams();
    }
    stop_search();
    const auto returned=h.returned();
    friends::events.current_room(h.code);
    retained=!foreground;
    publish(h,foreground);
    if (foreground) report_room(page->room(h,h.code,h.participants,h.me,h.revision));
    else report_room(page->clear());
    { std::lock_guard<std::mutex> lock(ui.mutex); ui.menu=foreground; }
    owe_back=!home || home->closed() || !home->send(returned);
    host::log("doubles: back in the home after %d ms (engine %d ms)%s",int(now_ms()-entered),int(engine_ms),
              owe_back?"; party-back waits for the home to reopen":"");
    events.reset();
  };
  // Each admission replaces failure, so a START during one keeps why it fails.
  auto in_match=[&] { return match || preparation.valid() || teams_route; };
  auto clear_notice=[&] {
    const bool had=!h.notice.empty(); h.notice.clear(); helper_out=false; pending_requeue=false;
    wait_notice=false; requeue_left.clear(); h.requeue_why.clear(); return had;
  };
  auto seats=[&] { std::set<std::string> s; for (const auto& p:h.participants) s.insert(text(p,"connectionId")); return s; };
  // START: remember who pressed it, so a requeue never searches for a changed duo or from a promoted member.
  auto start=[&](const json& q) { start_seats=seats(); start_host=h.is_host(); requeues=0; h.retry=0; home->send(q); };
  // Rooms names who ended a match (by: their connection id); this home's own B, closed game or switch is not a failure.
  auto ours=[&](const std::string& by) { return !by.empty() && (by==h.me || start_seats.count(by) || seats().count(by)); };
  // The terminal a failed admission already received on its match socket, read before back() drops it.
  auto dissolved_by=[&] {
    if (!events) return std::string();
    std::lock_guard<std::mutex> lock(events->mutex);
    for (const auto& value:events->messages) if (terminal(value)) return text(value,"by");
    return std::string();
  };
  // Sends the owed search once the home is open, released and in front of an idle game; counted only when it goes out.
  auto requeue=[&] {
    if (!pending_requeue || !home || home->closed() || h.leased || h.phase!=Phase::Home || retained || !ui.idle.load()) return;
    const auto q=h.search();
    if (!q || !start_host || !h.is_host() || seats()!=start_seats) {
      pending_requeue=false; host::log("doubles: not searching again, the duo changed");
      if (wait_notice) { h.notice.clear(); h.requeue_why.clear(); wait_notice=false; publish(h,!retained); }
      return;
    }
    if (!home->send(*q)) return;
    pending_requeue=false; h.retry=++requeues; host::log("doubles: searching again after a failed match (%d of 3)",requeues);
  };
  // The partner who left before the match began is gone from the roster and this seat leads the home: search
  // again as the party that is left. Past the deadline (the partner stayed, or no promotion came) the host decides.
  auto requeue_after_partner=[&] {
    if (requeue_left.empty() || h.phase!=Phase::Home) return;
    const auto now=seats();
    if (!now.count(requeue_left) && h.is_host()) {
      host::log("doubles: the partner is out of the home; searching again with %d player%s",int(now.size()),now.size()==1?"":"s");
      requeue_left.clear(); start_seats=now; start_host=true; requeues=0; h.retry=0; pending_requeue=true;
      h.notice=requeue_wait_line(h.requeue_why); wait_notice=true; press_notice=false; publish(h,!retained);
      return;
    }
    if (now_ms()<requeue_left_until) return;
    const bool stayed=now.count(requeue_left)>0;
    host::log("doubles: %s; not searching again",stayed?"the partner is still in the home":"this seat does not lead the home");
    requeue_left.clear(); h.requeue_why.clear(); wait_notice=false;
    h.notice=stayed && h.is_host() ? kPartnerLeftMatchLine : std::string();
    publish(h,!retained);
  };
  // After a match fails to start (admission, links, or a dissolve before it played), a seat whose link helper refused
  // attach (no_direct) learns why and stays out. Otherwise only the host searches again (members follow its search),
  // three times per START so a failing seat cannot hold a duo in a loop, and never when this home ended the match.
  // True when a search is owed (or went out).
  auto after_failure=[&](const std::string& why,const std::string& by) {
    if (why==console::kHelperMissing) {
      helper_out=true; pending_requeue=false; h.notice=std::string(console::kHelperMissing)+"\nB to leave"; publish(h,!retained);
      return false;
    }
    if (!h.is_host()) return false;
    if (ours(by)) {
      pending_requeue=false; h.requeue_why.clear(); host::log("doubles: match ended from this home; not searching again");
      return false;
    }
    if (requeues>=3) { h.notice=kCappedLine; h.requeue_why.clear(); wait_notice=false; publish(h,!retained); return false; }
    pending_requeue=true; requeue(); return true;
  };
  // A player left the 4-peer match before it began (on the character select, or before it opened): everyone
  // still here searches again, and the search line says why. The host searches and its members follow; when
  // the partner left, the seats left search as a new party once Rooms drops them from the home.
  auto teams_failure=[&](const std::string& reason,const std::string& by) {
    if (!by.empty() && by==h.me) { pending_requeue=false; host::log("doubles: this seat left the match; not searching again"); return; }
    const bool partner=!by.empty() && match_seats.count(by);
    h.requeue_why=partner ? "Your partner left" :
                  reason=="no_show" || reason=="timed_out" ? "A player didn't connect" : "A player left";
    host::log("doubles: %s before the match began",h.requeue_why.c_str());
    if (partner) {
      // No search is promised until Rooms drops the partner: one who pressed B stays in the home, and one
      // whose game closed keeps the seat for a while.
      h.notice=kPartnerLeftNotice; wait_notice=true; press_notice=false;
      requeue_left=by; requeue_left_until=now_ms()+10000; requeue_after_partner();
    } else {
      requeue_why_until=now_ms()+15000; // a member shows it on the host's search, if that comes
      if (after_failure(std::string(),by) && h.phase==Phase::Home) {
        h.notice=requeue_wait_line(h.requeue_why); wait_notice=true; press_notice=false;
      }
    }
    publish(h,!retained);
  };
  // Rooms holds the seat after a dropped (1006) or silent-closed (4002) home; take it back with a fresh ticket.
  auto reopen_home=[&]()->bool {
    if (!home || !home->rejoinable()) return false;
    host::log("doubles: home %s dropped (%d); rejoining",h.code.c_str(),home->close_code());
    if (now_ms()>=rejoin_until) { rejoin_until=now_ms()+60000; rejoin_backoff=250; }
    const auto left=[&](uint64_t most) { const auto now=now_ms(); return now<rejoin_until?std::min(most,rejoin_until-now):0; };
    while (!host::exit_requested() && left(1)) {
      if (search && now_ms()-search_ping>=5000) { search->send({{"t","ping"},{"at",now_ms()}}); search_ping=now_ms(); }
      try {
        const auto reply=bridge(ticket(h.compatibility,h.code,2),int(left(5000)));
        if (left(1) && friends::room(reply)==h.code && reply.value("md",0)==1) {
          auto next=std::make_unique<Socket>(); std::string error;
          if (next->open_session("",ranked::str(reply,"ticket"),h.code,&error,int(left(10000)))) {
            home=std::move(next); ping=now_ms();
            host::log("doubles: home %s rejoined",h.code.c_str());
            if (owe_back && h.phase==Phase::Home) owe_back=!home->send({{"t","party-back"}});
            return true;
          }
        }
      } catch (...) { /* bounded retry, never log a ticket or bridge exception */ }
      std::this_thread::sleep_for(std::chrono::milliseconds(left(rejoin_backoff)));
      rejoin_backoff=std::min<uint64_t>(rejoin_backoff*2,4000);
    }
    host::log("doubles: home %s rejoin gave up",h.code.c_str());
    return false;
  };
  try {
    auto compatibility_backend=std::make_shared<ConsoleBackend>();
    // The 4-peer route's home, search and match room share the Slippi Teams key, so a console-route
    // build never shares a match with a 4-peer one.
#if MU_DOUBLES_FOUR_PEER
    h.compatibility=compatibility(doubles_teams::kTeamsMode);
#else
    h.compatibility=console::console_io(compatibility_backend).compatibility;
#endif
    h.code=code=="new"?console::new_room_code(random64()):code;
    if (!active() || !wait_identity(10000).ok || !g_broker || !g_direct) throw std::runtime_error("unavailable");
    const auto reply=bridge(ticket(h.compatibility,h.code,2),10000);
    if (friends::room(reply)!=h.code) throw std::runtime_error("room");
    // The route grants Doubles only on a listed branch with the flag on; without md:1 this is a plain Friends home.
    if (reply.value("md",0)!=1) throw std::runtime_error("not granted");
    home=std::make_unique<Socket>(); std::string error;
    if (!open_retrying_busy(*home,ranked::str(reply,"ticket"),h.code,&error)) throw std::runtime_error("home");
    friends::events.current_room(h.code);
    host::log("doubles: home opened %s",h.code.c_str());
    publish(h);
    while (!done && !host::exit_requested()) {
      std::deque<std::string> commands; std::deque<json> reports;
      { std::lock_guard<std::mutex> lock(ui.mutex); commands.swap(ui.commands); reports.swap(ui.reports); }
      // A finish goes out before this pass's commands: a match-ended or B right behind it must not drop it.
      for(const auto& report:reports) if(match) match->send(report);
      for (const auto& command:commands) {
        if (command=="foreground") { retained=false; publish(h); report_room(page->room(h,h.code,h.participants,h.me,h.revision)); }
        else if (command=="search") {
          if (clear_notice()) publish(h,!retained);
          if (auto q=h.search()) { host::log("doubles: START, searching"); *picker_stop=true; start(*q); continue; }
          // A press the home can't act on says why, on screen and in the log; it is never silent.
          if (h.phase!=Phase::Home) { host::log("doubles: START ignored (%s)",h.phase==Phase::Searching?"already searching":"in a match"); continue; }
          if (h.me.empty() || h.participants.empty()) {
            host::log("doubles: START before the home joined; searching once it has");
            auto_search=true; deferred_start=true; press_notice=true;
            h.notice="Joining the Doubles home... the search starts in a moment\nB: leave";
          } else if (!h.is_host()) {
            host::log("doubles: START ignored (not the host)");
            h.notice="Only the host can search - ask your partner to press START\nB: leave"; press_notice=true;
          } else host::log("doubles: START ignored (%d players in the home)",int(h.participants.size()));
          publish(h,!retained);
        }
        else if (command=="back" || command=="match-ended" || command=="unavailable") {
          if (command!="back" && h.phase!=Phase::Admission && h.phase!=Phase::Playing) continue;
          if (command=="back") host::log("doubles: B, %s",h.phase==Phase::Searching?"stopping the search":
            h.phase==Phase::Home?"leaving the home":"leaving the match");
          if (command=="back") clear_notice();
          if (h.phase==Phase::Searching) { if(auto q=h.search(true)) home->send(*q); }
          else if (h.phase==Phase::Admission || h.phase==Phase::Playing) {
            withdraw();
            back(command!="unavailable");
          } else done=true;
        } else if (command=="switch") {
          std::string next; { std::lock_guard<std::mutex> lock(ui.mutex); next=ui.next_room; }
          if (next.empty() || next==h.code) {
            { std::lock_guard<std::mutex> lock(ui.mutex); ui.next_room.clear(); }
            retained=false; publish(h); report_room(page->room(h,h.code,h.participants,h.me,h.revision));
          } else {
            // An accepted invite into another home: leave this one, then the entry thread opens that one.
            host::log("doubles: leaving home %s for an invite",h.code.c_str());
            if (h.phase==Phase::Searching) withdraw();
            else if (h.phase==Phase::Admission || h.phase==Phase::Playing) { withdraw(); back(false); }
            done=true;
          }
        } else if (command=="invite" && !(h.phase==Phase::Home && h.is_host() && !picker.valid())) {
          host::log("doubles: Z ignored (%s)",h.phase!=Phase::Home?"not in the home":picker.valid()?"invite list already open":"not the host");
          if (h.phase==Phase::Home && !h.is_host()) {
            h.notice="Only the host can invite - ask your partner to press Z\nB: leave"; press_notice=true; publish(h,!retained);
          }
        } else if (command=="invite") {
          host::log("doubles: Z, opening the invite list");
          picker_stop=std::make_shared<std::atomic<bool>>(false);
          { std::lock_guard<std::mutex> lock(ui.mutex); ui.picker=true; } // the page's list reads the pads now
          picker=std::async(std::launch::async,[picker_stop,room=h.code] {
            std::string invited;
            pick_friend_room([picker_stop] { return picker_stop->load(); },room,true,&invited);
            return invited;
          });
        }
      }
      if (picker.valid() && picker.wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
        try {
          const auto invited=picker.get();
          if (h.phase==Phase::Home && h.participants.size()<2) {
            host::log("doubles: invite sent, waiting for the partner (%s)",invited.empty()?"link":"named");
            h.notice=invite_line=invited_line(invited); press_notice=true; publish(h,!retained);
          }
        } catch (...) { host::log("doubles: invite picker closed"); }
        std::lock_guard<std::mutex> lock(ui.mutex); ui.picker=false;
      }
      // After the result the players are on the results screen: the home's lobby and its dissolve wait for the
      // game's return to the menu (match-ended), so nothing pulls the game away from the results.
      const bool holding=h.phase==Phase::Playing && ui.finished();
      json m; bool got=false;
      if (!held.empty() && !holding) { m=std::move(held.front()); held.pop_front(); got=true; }
      else if (home->next(m,10)) got=true;
      if (got && holding && !home_terminal(m)) { held.push_back(std::move(m)); got=false; }
      if (got) {
        if (home_terminal(m)) { if (in_match()) back(); done=true; }
        else if (text(m,"t")=="teams-dissolved" && teams_route) {
          // The 4-peer match lives in its own room on the Teams client. A reset after it started (Rooms ends
          // every room once its one match has a result) is the normal end: the game leaves its character
          // select or results and the home comes back (match-ended). A dissolve before a game of it began
          // on this seat (the menu never took the entry, the room never started, or the four were still on
          // the character select, where the room's start arrives) is a failed match: Rooms has closed the
          // match room, so the game leaves the select and everyone still here searches again.
          const auto reason=text(m,"reason");
          bool entered, started, played, settled;
          {
            std::lock_guard<std::mutex> lock(ui.mutex);
            entered=!ui.teams_enter; started=ui.teams_started; played=ui.teams_played;
            if (started && reason=="reset") ui.teams_settled=true;
            settled=ui.teams_settled;
          }
          if (settled) host::log("doubles: home reset after the 4-peer match (%s); a normal end",reason.c_str());
          else if (!entered || !started || !played) {
            host::log("doubles: home dissolve before the 4-peer match started (%s, %s)",reason.c_str(),
                      !entered?"entry not taken":!started?"no start":"no game yet");
            back(); teams_failure(reason,text(m,"by"));
          } else host::log("doubles: home dissolve during the 4-peer match (%s); the match keeps its room",reason.c_str());
        }
        else if (text(m,"t")=="teams-dissolved") {
          const auto by=text(m,"by");
          if (in_match()) {
            const bool started=h.phase==Phase::Playing; back(); if (!started) after_failure(failure ? *failure : std::string(),by);
          } else if (ours(by)) pending_requeue=false; // the home's copy of a dissolve the match socket missed
        }
        else {
          const auto previous=h.generation;
          if (auto q=h.observe(m)) {
            *picker_stop=true; pending_requeue=false;
            if (press_notice || wait_notice) { h.notice.clear(); press_notice=wait_notice=false; }
            stop_search();
            if (helper_out) { withdraw(); host::log("doubles: search declined, link helper missing"); continue; }
            if (!ui.idle.load()) {
              withdraw();
              retained=true; publish(h,false);
              host::log("doubles: search withdrawn while game is busy");
              continue;
            }
            const auto answer=bridge(*q,10000); std::string why;
            search=std::make_unique<Socket>();
            if (!open_retrying_busy(*search,ranked::str(answer,"ticket"),"",&why)) {
              search.reset(); if(auto cancel=h.search(true)) home->send(*cancel);
            }
          } else if (!previous.empty() && h.generation.empty() && h.phase==Phase::Home) stop_search();
          if (deferred_start && !h.me.empty() && !h.participants.empty() && !h.is_host()) {
            host::log("doubles: START before the home joined, but this seat isn't the host; not searching");
            auto_search=deferred_start=false; h.notice="Only the host can search - ask your partner to press START\nB: leave";
          }
          if (auto_search && !h.me.empty() && h.participants.size()>=size_t(auto_humans)) {
            if (auto q=h.search()) { clear_notice(); press_notice=deferred_start=false; start(*q); auto_search=false; }
          }
          if (h.participants.size()!=humans) {
            // A notice about the old roster (ask your partner...) would hide the join or leave line; the first
            // roster (the welcome) has no old one, and the notice set for it stays.
            if (humans>0 && press_notice) { h.notice.clear(); press_notice=false; }
            if (humans==1 && h.participants.size()==2 && h.other_seat()) host::log("doubles: partner joined (2 players in the home)");
            else if (humans==2 && h.participants.size()==1) host::log("doubles: partner left (1 player in the home)");
            humans=h.participants.size();
          }
          if (!invite_line.empty() && (h.participants.size()>=2 || h.notice!=invite_line)) {
            if (h.notice==invite_line) { h.notice.clear(); press_notice=false; }
            invite_line.clear();
          }
          if (!retained) publish(h,h.phase!=Phase::Playing);
          // Reuse Friends' page room/invite bridge, reporting the retained home only.
          if (!retained && h.phase!=Phase::Playing && h.phase!=Phase::Admission && m.count("participants"))
            report_room(page->room(h,h.code,h.participants,h.me,h.revision));
        }
      }
      if (home->closed()) {
        if (in_match() && !holding && !teams_route) back();
        // A home that can't be had back still retires the match, so the game is free for the next one.
        if (!reopen_home()) { if (in_match()) back(); done=true; }
      }
      if (search && search->next(m,0)) {
        h.progress(m);
        auto admission=h.matched(m);
#if MU_DOUBLES_FOUR_PEER
        if (admission) {
          // Each seat goes to its own Teams character select; its search joins this match room
          // (find_match's doubles branch), the room's nativeTeams names the teams and the stage seed.
          const auto room=friends::room(*admission);
          host::log("doubles: match found, the four players pick on their own character select");
          *picker_stop=true; stop_search();
          friends::events.current_room(room);
          teams_route=true; match_seats=seats(); requeue_left.clear();
          // requeues and h.retry stay until a game of this match begins (back()): a select that empties
          // before then is a failed match, three automatic searches per START like the console route's.
          clear_notice(); h.phase=Phase::Playing; phase->store(Phase::Playing);
          // publish() is skipped for a retained home, so the match's playing state is set here.
          std::lock_guard<std::mutex> lock(ui.mutex);
          ui.clear_teams(); ui.teams_room=room; ui.teams_team=doubles_teams::matched_team(m); ui.teams_enter=true;
          ui.playing=true; ui.foreground=false;
          ui.stop_match=false; ui.stopped=false; ui.finish_reported=ui.result_in=false;
        } else
#endif
        if (admission) {
          host::log("doubles: match found, connecting the four players");
          *picker_stop=true; stop_search();
          match=std::make_shared<ConsoleBackend>(); match->players=4; match->doubles_room=true;
          events=std::make_shared<MatchEvents>();
          match->observe=[events=events](const json& value) {
            if (text(value,"t")=="result") { std::lock_guard<std::mutex> lock(ui.mutex); ui.result_in=true; }
            if (text(value,"t")=="result" || terminal(value)) {
              std::lock_guard<std::mutex> lock(events->mutex); events->messages.push_back(value);
            }
          };
          friends::events.current_room(friends::room(*admission));
          auto io=console::console_io(match); io.doubles=true; io.host_save={}; io.close=[] {};
          phase->store(Phase::Admission);
          io.roster=[page,home_state=h,phase,report_room](const std::string& room,const json& participants,const std::string& me,int revision) {
            auto now=home_state; now.phase=phase->load();
            report_room(page->room(now,room,participants,me,revision));
          };
          io.cancelled=[e=events] { return e->cancelled.load() || host::exit_requested(); };
          io.open_room=[b=match,e=events,compat=h.compatibility](const std::string& t,const std::string& room,int ms) {
            b->renew_ticket=[room,compat](int ms) { return ranked::str(bridge(ticket(compat,room,4),ms),"ticket"); };
            if (e->cancelled) return false;
            const bool opened=b->open(t,room,ms); e->opened=opened; return opened;
          };
          const json request={{"room",(*admission)["room"]},{"players",4}};
          failure=std::make_shared<std::string>();
          preparation=std::async(std::launch::async,[request,io=std::move(io),failure=failure]() mutable {
            auto launch=std::make_shared<Launch>(); std::string error;
            launch->link=console::start_session(request,launch->config,error,std::move(io));
            if (!launch->link) {
              host::log("doubles: match admission failed: %s",error.c_str());
              *failure=error; return std::shared_ptr<Launch>{};
            }
            return launch;
          });
        } else if (text(m,"t")=="search-error") {
          if(auto cancel=h.search(true)) home->send(*cancel); stop_search();
          h.search_ended("The search stopped. Press START to search again\nZ: invite a partner     B: leave");
        }
        if (!retained) publish(h,h.phase!=Phase::Playing);
      }
      if (search && search->closed()) {
        if(auto cancel=h.search(true)) home->send(*cancel); stop_search();
        h.search_ended("Lost the search connection. Press START to search again\nZ: invite a partner     B: leave"); publish(h);
      }
      if (preparation.valid() && preparation.wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
        auto launch=preparation.get();
        if (!launch) {
          const auto by=dissolved_by();
          withdraw();
          back();
          after_failure(failure ? *failure : std::string(),by);
        } else {
          if (!ui.idle.load()) { launch.reset(); withdraw(); back(false); continue; }
          ui.linger(h.status,now_ms()); // the box the launch hides stays a moment longer
          clear_notice(); h.phase=Phase::Playing; phase->store(Phase::Playing); publish(h,false); requeues=0; h.retry=0;
          std::lock_guard<std::mutex> lock(ui.mutex); ui.stop_match=false; ui.stopped=false; ui.finish_reported=ui.result_in=false; ui.launch=std::move(launch);
        }
      }
      if(events) {
        std::deque<json> messages;
        { std::lock_guard<std::mutex> lock(events->mutex); messages.swap(events->messages); }
        for (const auto& value:messages) {
          if (text(value,"t")=="result") {
            h.result_seen=true; host::log("doubles: result received (ratings null: %s)",value.value("ratings",json()).is_null()?"yes":"no");
          } else if (terminal(value) && h.result_seen && h.phase==Phase::Playing && ui.finished()) {
            // The players are on the results screen: the game's return to the menu (match-ended) brings the home back.
            host::log("doubles: match closed after its result (%s); home after the results screen",text(value,"reason").c_str());
          } else if (terminal(value)) {
            host::log("doubles: match dissolved (%s)",text(value,"reason").c_str());
            const bool started=h.phase==Phase::Playing; back(); if (!started) after_failure(failure ? *failure : std::string(),text(value,"by")); break;
          }
        }
      }
      requeue_after_partner();
      requeue(); // after every input this pass: a release, START or B, or the game coming back to idle
      // A member's reason for a search that never came (the host capped, or stopped) is not a later search's.
      if (!h.requeue_why.empty() && h.phase==Phase::Home && requeue_left.empty() && !pending_requeue &&
          !h.is_host() && now_ms()>=requeue_why_until) h.requeue_why.clear();
      keepalive();
      if (search && now_ms()-search_ping>=5000) { search->send({{"t","ping"},{"at",now_ms()}}); search_ping=now_ms(); }
    }
  } catch (const std::exception& e) {
    host::log("doubles: room setup or transport failed (%s)",e.what());
    if (in_match()) back();
  } catch (...) {
    host::log("doubles: room setup or transport failed");
    if (in_match()) back();
  }
  *picker_stop=true;
  if (picker.valid()) { try { picker.get(); } catch (...) {} }
  stop_search();
  if(home) { home->send({{"t","leave"}}); home->close(); }
  { std::lock_guard<std::mutex> lock(ui.mutex); ui.menu=ui.menu || ui.foreground || ui.playing; ui.active=false; ui.line.clear(); ui.lingering.clear(); ui.playing=false; }
  report_room(page->clear());
  friends::events.current_room({});
}
} // namespace
void entry(const std::string& room,bool search,int humans) {
  if (running.exchange(true)) {
    // A join names a home; the run switches to it (or just comes forward when it is this home).
    std::lock_guard<std::mutex> lock(ui.mutex);
    if (room!="new") { ui.next_room=room; ui.commands.push_back("switch"); }
    else { ui.commands.push_back("foreground"); if(search) ui.commands.push_back("search"); }
    return;
  }
  { std::lock_guard<std::mutex> lock(ui.mutex); ui.reset_run(); ui.next_room.clear(); ui.line="Opening Doubles home..."; }
  std::thread([room,search,humans] {
    std::string code=room; bool auto_search=search; int auto_humans=humans;
    for (;;) {
      run(code,auto_search,auto_humans);
      std::lock_guard<std::mutex> lock(ui.mutex);
      code=std::exchange(ui.next_room,{});
      if (code.empty() || host::exit_requested()) { running=false; return; }
      ui.reset_run(); ui.line="Opening Doubles home..."; auto_search=false; auto_humans=1;
    }
  }).detach();
}
void report(const console::SessionConfig& c,int winner,bool draw,bool invalid) {
  if (!c.doubles) return;
  std::lock_guard<std::mutex> lock(ui.mutex); ui.finish_reported=true; ui.reports.push_back(finish(c,winner,draw,invalid));
}
} // namespace doubles
