// Simulation-thread-only pre-save restart; the retained home lives outside game memory.
int doubles_checkpoint() {
  auto& run=g_doubles_origin;
  if(run.boot.ready()) return 1;
  if(!g_css_origin.armed || g_console_boot || g_replaying || g_online_test_mode>=0 || source_p2p::requested()) return 0;
  try {
    MuStateRegion regions[3]{};
    const auto count=g_game.state_regions(regions,3);
    run.regions.assign(regions,regions+count);
    std::vector<MuStateRegion> exclusions(g_game.state_exclusions(nullptr,0));
    g_game.state_exclusions(exclusions.data(),uint32_t(exclusions.size()));
    append_sound_driver_exclusions(exclusions);
    run.spans=native_savestate::subtract(run.regions,exclusions);
    std::vector<MuStateRegion> included;
    for(const auto& span:run.spans) included.push_back({span.address,span.size});
    run.excluded_spans=native_savestate::subtract(run.regions,included);
    run.boot.capture(run.spans); run.excluded.capture(run.excluded_spans);
    run.aram.capture({{host::aram,MU_HOST_ARAM_BYTES}});
    run.boot_allocations=g_css_origin.live_allocations;
    std::vector<native_savestate::Span> allocations;
    for(const auto& block:run.boot_allocations) allocations.push_back({static_cast<uint8_t*>(block.first),block.second});
    run.allocations.capture(std::move(allocations));
    run.audio=g_audio; run.restore_gpu=gx::capture_boot_state();
    run.restore_clock=host::capture_native_boot_clock();
    host::log("doubles: pre-save boot checkpoint captured"); return 1;
  } catch(...) {
    run={}; host::log("doubles: pre-save boot checkpoint unavailable"); return 0;
  }
}
int doubles_tick() {
  using namespace yougame;
  bool stop=false, menu=false;
  std::string teams_room; // the 4-peer route's match room (yougame_doubles_teams.h), empty otherwise
  bool teams_settled=false; // that match has its result (or Rooms reset its room after the start)
  {
    std::lock_guard<std::mutex> lock(doubles::ui.mutex);
    // back() clears playing before this tick serves its stop, so a stop reads the room regardless.
    teams_room=doubles::ui.playing || doubles::ui.stop_match?doubles::ui.teams_room:std::string();
    teams_settled=doubles::ui.teams_settled;
    stop=std::exchange(doubles::ui.stop_match,false);
    menu=std::exchange(doubles::ui.menu,false);
    if(!g_doubles_launch) g_doubles_launch=std::exchange(doubles::ui.launch,{});
  }
  auto& engine=console::console_engine();
  auto idle=[] {
    int32_t scene[2]{};
    return !console::console_active() && slippi::online::session_mode()<0 && !g_replaying &&
      !g_console_boot && g_online_test_mode<0 && !source_p2p::requested() &&
      g_game.practice && g_game.practice(MU_PRACTICE_SCENE,scene,2)==0 && friends::menu_safe(scene[0],scene[1]);
  };
  doubles::ui.idle=idle();
  static bool returning=false;
  // The 4-peer route's home came back while this seat was still on its Teams character select: the
  // menu is where the home is, so the select is left as soon as it is up (never from inside a game).
  static bool leave_teams_css=false;
  const bool own_stop=stop && engine.config().doubles;
  returning |= own_stop;
  auto leave_css=[](const char* why) {
    int32_t scene[2]{};
    if(!g_game.practice || g_game.practice(MU_PRACTICE_SCENE,scene,2)!=0) return false;
    if(scene[0]==1) return true; // already on the menu
    if(scene[0]!=8) return false; // 8: the online character select
    host::log("doubles: leaving the Teams character select (%s)",why);
    int32_t arg=0; g_game.practice(MU_PRACTICE_FRIENDS_RETURN,&arg,1);
    return true;
  };
  if(stop) {
    g_doubles_launch.reset();
    // The 4-peer route: the home is back, so this seat's Teams session ends with it.
    if(!teams_room.empty()) {
      if(slippi::online::session_mode()>=0) {
        host::log("doubles: closing the 4-peer Teams session");
        slippi::online::native_cleanup_match();
      }
      leave_teams_css=true;
    }
  }
  if(!stop && !teams_room.empty()) leave_teams_css=false; // a new match: its select is where the four go
  if(leave_teams_css && leave_css("the home is back")) leave_teams_css=false;
  if(stop) {
    if(engine.config().doubles) {
      engine.end(console::kSelfLeft); engine=console::Engine{};
      // Keep the disposable card isolated, as ordinary console sessions do after End.
      g_console_host_pad=-1; g_console_boot=false;
      origin_end_card_session();
    }
    std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.stopped=true;
  }
  if(menu) {
    if ((returning || idle()) && !console::console_active() && !g_replaying &&
        slippi::online::session_mode()<0 && g_game.practice) {
      int32_t arg=0; g_game.practice(MU_PRACTICE_FRIENDS_RETURN,&arg,1);
    }
    returning=false;
  }
  if(g_doubles_launch) {
    if (!idle()) {
      host::log("doubles: launch withdrawn while game is busy");
      g_doubles_launch.reset(); doubles::ui.command("unavailable");
      std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.stopped=true;
      return 0;
    }
    auto& run=g_doubles_origin;
    if(!run.boot.ready() || !g_css_origin.armed || !gx::supports_game_reset()) {
      host::log("doubles: pre-save console boot unavailable");
      g_doubles_launch.reset(); doubles::ui.command("match-ended");
      std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.stopped=true;
      return 0;
    }
    if(g_handoff_disc_depth || !g_handoff_callbacks_pending || g_handoff_callbacks_pending()) return 0;
    std::filesystem::path card;
    const char* data=std::getenv("YOUGAME_DATA");
    if(!data || !console::prepare_card(std::filesystem::u8path(data),g_doubles_launch->config,card)) {
      host::log("doubles: disposable session card unavailable");
      g_doubles_launch.reset(); doubles::ui.command("match-ended");
      std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.stopped=true;
      return 0;
    }
    auto launch=std::exchange(g_doubles_launch,{});
    console::EngineHooks hooks;
    hooks.log=[](const std::string& line) { host::log("%s",line.c_str()); };
    // Once the match has its result, the dissolve and the links closing behind it are expected: no notice.
    hooks.notice=[](const char* line) { if(!*line || !yougame::doubles::ui.over()) screen_label::show(line,8.0); };
    hooks.host_ticks=[] { return host::cpu ? host::cpu->tb : uint64_t(0); };
    hooks.host_catch_up=console_host_catch_up;
    hooks.responsive=[] { host::window_pump(); };
    bool began=false;
    try { began=engine.begin(std::move(launch->link),launch->config,std::move(hooks),console::kConsoleRollbackDefault); } catch (...) {}
    if(!began) {
      host::log("doubles: console boot failed; returning home");
      // Drop the Link and the half-begun engine before the run thread may close the backend.
      launch.reset(); engine=console::Engine{};
      doubles::ui.command("match-ended");
      std::lock_guard<std::mutex> lock(doubles::ui.mutex); doubles::ui.stopped=true;
      return 0;
    }
    // Return to the C-only pre-main continuation after the existing disc/callback gate.
    run.boot.restore(); run.excluded.restore(); run.aram.restore();
    std::set<void*> allocated;
    for(const auto& block:g_css_origin.live_allocations) allocated.insert(block.first);
    for(const auto& block:g_css_origin.boot_allocations) allocated.insert(block.first);
    for(auto* block:allocated) if(!run.boot_allocations.count(block)) std::free(block);
    run.allocations.restore();
    g_css_origin={}; g_css_origin.armed=true; g_css_origin.live_allocations=run.boot_allocations;
    run.restore_gpu(); audio_core::reset(g_audio); g_audio=run.audio; host::audio_reset_stream();
    for(int port=0;port<4;++port) host::input_rumble(port,false);
    run.restore_clock(); g_console_catch_up.store(0,std::memory_order_release); g_resim=false; g_console_in_retrace=false; g_disc_loads.clear();
    g_savestates.end(); g_mem1_watch={}; origin_mount_card({});
    g_console_card=std::move(card); g_console_boot=true; g_console_host_pad=-1;
    return -51;
  }
  if(!teams_room.empty() && !stop) {
    // The 4-peer route: the home returns when this seat is back on the main menu after its Teams
    // session (B out of the character select, or after a match), never between games. A room plays one
    // match: once it is settled, the character select the results land on is left for the menu.
    static std::string room;
    static bool seen_online=false,left=false,settled_out=false;
    if(room!=teams_room) { room=teams_room; seen_online=left=settled_out=false; }
    int32_t scene[2]{};
    if(g_game.practice && g_game.practice(MU_PRACTICE_SCENE,scene,2)==0) {
      seen_online |= slippi::online::session_mode()>=0 || scene[0]==8; // 8: the online character select
      if(seen_online && scene[0]==1 && !left) { left=true; doubles::ui.command("match-ended"); }
      if(teams_settled && !left && !settled_out && scene[0]==8) settled_out=leave_css("the match is over");
    }
  }
  if(engine.config().doubles) {
    static bool seen_css=false,left_css=false;
    static std::string session;
    if(session!=engine.config().session_id) { session=engine.config().session_id; seen_css=left_css=false; }
    int32_t scene[2]{};
    if(g_game.practice && g_game.practice(MU_PRACTICE_SCENE,scene,2)==0) {
      seen_css |= scene[0]==2;
      // Back on the menu (after the results screen, or B out of character select): the home returns.
      if(seen_css && scene[0]==1 && !left_css) { left_css=true; doubles::ui.command("match-ended"); }
    }
    // A session that ended before this game's own end returns at once (even when the others' result is in, it was
    // still playing); after it, Rooms closes the match while the results screen is up, and the home waits for the
    // game's own return to the menu above.
    if(!engine.active() && !left_css && !doubles::ui.finished()) { left_css=true; doubles::ui.command("match-ended"); }
  }
  return 0;
}
