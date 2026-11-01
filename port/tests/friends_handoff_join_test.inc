// Uses the console link test's real Stream + fake room, with no sockets or game process.
static handoff_join::Info hj_info() { return {123456,std::string(64,'a'),std::string(64,'b'),"windows"}; }
// Also linked against HEAD's unmodified console_link.cpp by the differential gate.
static void friends_handoff_off_trace() {
  for (int seat : {0,1}) for (bool shared_card : {false,true}) {
    Fixture f(2,seat); auto io = f.io(); bool prepared = false; int flushes = 0;
    const auto save = save_fixture(); const auto digest = card_sha(save);
    io.friends_lobby = true; io.handoff_join = false;
    io.handoff_css = []() -> bool { CHECK(false); return false; };
    io.host_save = [=] { return shared_card ? save : Bytes{}; };
    io.persist_card = [](const SessionConfig&) { return true; };
    io.flush_room = [&](int) { ++flushes; return true; };
    json config;
    io.prepared = [&](const SessionConfig& c) {
      prepared = true; config = shared(c); config["card"] = c.card_digest;
      config["cardBytes"] = c.card_bytes.size(); config["default"] = c.default_card;
    };
    f.alter_start = [&](json& m) { if (shared_card) m["console"]["cardDigest"] = digest; };
    const auto next = io.next_room;
    io.next_room = [&](json& m,int ms) {
      const bool got = next(m,ms);
      if (got && shared_card && m.count("console")) m["console"]["cardDigest"] = digest;
      return got;
    };
    const auto open = io.open_room;
    io.open_room = [&](const std::string& t,const std::string& r,int ms) {
      const bool ok = open(t,r,ms);
      if (seat && shared_card) {
        const size_t n = (save.size()+kCardChunk-1)/kCardChunk;
        for (size_t i = 0; i < n; ++i) f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-card"},
            {"v",1},{"match","session-fixed"},{"i",i},{"n",n},
            {"data",card_encode(save.data()+i*kCardChunk,std::min(kCardChunk,save.size()-i*kCardChunk))}}}});
      }
      return ok;
    };
    const auto send = io.send_room;
    io.send_room = [&](const json& m) {
      if (m.count("d") && str(m["d"],"kind") == "mu-card" && m["d"]["i"] == 0)
        f.push({{"t","msg"},{"from","c1"},{"d",{{"kind","mu-card-verdict"},{"v",1},{"match","session-fixed"},{"ok",true}}}});
      if (m.count("d") && str(m["d"],"kind") == "mu-card-verdict")
        f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-card-final"},{"v",1},{"match","session-fixed"},{"use","host"}}}});
      return send(m);
    };
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,std::move(io)) && prepared && error.empty());
    const json trace{{"seat",seat},{"sharedCard",shared_card},{"messages",f.messages},{"requests",f.requests},
        {"logs",f.logs},{"time",f.now},{"waits",f.waits},{"config",config},{"flushes",flushes},{"packets",f.packets}};
    std::puts(trace.dump().c_str());
  }
}
static void friends_handoff_agreement() {
  using handoff_join::Agreement;
  auto info = hj_info();
  std::vector<json> host_out,guest_out;
  std::vector<std::string> host_log,guest_log;
  auto room = [](const json& out,const char* from) { return json{{"t","msg"},{"from",from},{"d",out["d"]}}; };
  Agreement host(true,true,info,"s","guest",[&](const json& j) { host_out.push_back(j); return true; },
      [&](const std::string& s) { host_log.push_back(s); });
  Agreement guest(false,true,info,"s","host",[&](const json& j) { guest_out.push_back(j); return true; },
      [&](const std::string& s) { guest_log.push_back(s); });
  host.begin(100,true,true); CHECK(host.waiting() && host_out.size() == 1);
  const auto offer = room(host_out[0],"host");
  guest.receive(offer,101); CHECK(guest.accepted() && !guest.waiting() && guest_out.size() == 1);
  CHECK(guest_out[0]["d"]["kind"] == "mu-hj-accept");
  guest.receive(offer,102); CHECK(guest_out.size() == 1); // duplicate doesn't extend deadline
  host.receive(room(guest_out[0],"guest"),103); CHECK(!host.waiting());
  CHECK(host.accepted() && guest.accepted());
  host.refuse("start-at-N-unavailable");
  CHECK(host_out.back()["d"]["reason"] == "start-at-N-unavailable");
  guest.receive(room(host_out.back(),"host"),104); CHECK(!guest.waiting());
  CHECK(guest_log.back() == "handoff-join: refuse start-at-N-unavailable");
  for (const char* why : {"switch-off","metadata-unavailable","wrong-library","wrong-spans","wrong-os",
                        "host-not-vs-css","conflicting-offer"}) {
    ++cases; guest_out.clear();
    auto local = info;
    if (std::string(why) == "metadata-unavailable") local.library_size = 0;
    Agreement g(false,std::string(why) != "switch-off",local,"s","host",
        [&](const json& j) { guest_out.push_back(j); return true; },[](const std::string&) {});
    auto changed = offer;
    const std::string reason = why;
    if (reason == "wrong-library") changed["d"]["librarySize"] = 9;
    if (reason == "wrong-spans") changed["d"]["spansDigest"] = std::string(64,'c');
    if (reason == "wrong-os") changed["d"]["os"] = "linux";
    if (reason == "host-not-vs-css") changed["d"]["sceneMinor"] = 1;
    if (reason == "conflicting-offer") {
      g.receive(offer,100); changed["d"]["extra"] = true;
    }
    g.receive(changed,101);
    CHECK(!g.waiting() && guest_out.back()["d"]["reason"] == why);
  }
  for (const char* field : {"librarySha256","librarySize","spansDigest","os","sceneMajor","sceneMinor"}) {
    for (const auto& bad : {json(),json::array(),json::object(),json(true)}) {
      ++cases; guest_out.clear();
      Agreement g(false,true,info,"s","host",[&](const json& j) { guest_out.push_back(j); return true; },[](const std::string&) {});
      auto changed = offer; changed["d"][field] = bad;
      g.receive(changed,100); CHECK(!guest_out.empty() && guest_out.back()["d"]["kind"] == "mu-hj-refuse");
    }
  }
  for (const char* field : {"from","match","v","kind"}) {
    ++cases; guest_out.clear();
    Agreement g(false,true,info,"s","host",[&](const json& j) { guest_out.push_back(j); return true; },[](const std::string&) {});
    g.expect(100);
    auto spoof = offer;
    if (std::string(field) == "from") spoof[field] = "unknown";
    else spoof["d"][field] = "unrelated";
    g.receive(spoof,101); CHECK(guest_out.empty() && g.waiting());
    g.tick(10099); CHECK(g.waiting());
    g.tick(10100); CHECK(!g.waiting() && guest_out.back()["d"]["reason"] == "timeout");
  }
  for (bool throws : {false,true}) {
    ++cases; host_log.clear();
    Agreement h(true,true,info,"s","guest",[&](const json&) { if (throws) throw 7; return false; },
        [&](const std::string& s) { host_log.push_back(s); });
    h.begin(100,true,true); CHECK(!h.waiting());
    CHECK(host_log.back() == "handoff-join: refuse send-failed");
  }
  for (const char* why : {"host-not-vs-css","shared-card-required","metadata-unavailable"}) {
    ++cases; host_out.clear(); auto local = info; const std::string reason = why;
    if (reason == "metadata-unavailable") local.library_sha256.clear();
    Agreement h(true,true,local,"s","guest",[&](const json& j) { host_out.push_back(j); return true; },[](const std::string&) {});
    h.begin(100,reason != "host-not-vs-css",reason != "shared-card-required");
    CHECK(!h.waiting() && host_out.size() == 1 && host_out.back()["d"]["reason"] == why);
  }
  // Accept is bound to the same identity as the offer, not just the frozen sender.
  host_out.clear();
  Agreement h(true,true,info,"s","guest",[&](const json& j) { host_out.push_back(j); return true; },[](const std::string&) {});
  h.begin(100,true,true);
  auto bad_accept = room(host_out[0],"guest"); bad_accept["d"]["kind"] = "mu-hj-accept";
  bad_accept["d"]["librarySha256"] = std::string(64,'c'); h.receive(bad_accept,101);
  CHECK(!h.waiting() && host_out.back()["d"]["reason"] == "wrong-library");
}

static void friends_handoff_flow() {
  const auto save = save_fixture(); const auto digest = card_sha(save);
  struct Result { std::vector<json> messages,requests; std::vector<std::string> logs; uint64_t now; SessionConfig config; int flushes; };
  auto run = [&](bool friends, bool enabled, const std::string& reply) {
    Fixture f; auto io = f.io(); Result result{}; bool prepared = false;
    io.friends_lobby = friends; io.handoff_join = enabled; io.handoff_info = hj_info();
    io.handoff_css = [] { return true; };
    io.host_save = [&] { return save; }; io.persist_card = [](const SessionConfig&) { return true; };
    io.flush_room = [&](int) { ++result.flushes; return true; };
    io.prepared = [&](const SessionConfig& c) { result.config = c; prepared = true; };
    const auto send = io.send_room;
    io.send_room = [&](const json& m) {
      if (m.count("d") && str(m["d"],"kind") == "mu-card" && m["d"]["i"] == 0)
        f.push({{"t","msg"},{"from","c1"},{"d",{{"kind","mu-card-verdict"},{"v",1},{"match","session-fixed"},{"ok",true}}}});
      if (m.count("d") && str(m["d"],"kind") == "mu-hj-offer" && reply != "silent") {
        auto d = m["d"]; d["kind"] = reply == "accept" ? "mu-hj-accept" : "mu-hj-refuse";
        if (reply != "accept") d["reason"] = reply;
        f.push({{"t","msg"},{"from","c1"},{"d",d}});
      }
      return send(m);
    };
    f.alter_start = [&](json& m) { m["console"]["cardDigest"] = digest; };
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,std::move(io)) && prepared && error.empty());
    CHECK(result.config.card_bytes == save && result.config.card_digest == digest);
    CHECK(f.packets == 0);
    for (const auto& q : f.requests) CHECK(str(q,"type") != "directOpen");
    result.messages = f.messages; result.requests = f.requests; result.logs = f.logs; result.now = f.now;
    return result;
  };
  // The existing preparation path is the control. Normalize only the pre-existing private
  // ticket flag; all room JSON, logs, elapsed fake time, card results and flush counts must match.
  auto old = run(false,false,"accept"); const auto off = run(true,false,"accept");
  old.requests.front()["queue"] = "private";
  CHECK(old.messages == off.messages && old.requests == off.requests && old.logs == off.logs);
  CHECK(old.now == off.now && old.flushes == off.flushes);
  CHECK(old.config.card_bytes == off.config.card_bytes && old.config.card_digest == off.config.card_digest);
  for (const auto& m : off.messages) if (m.count("d")) {
    CHECK(str(m["d"],"kind").find("mu-hj-") != 0); CHECK(!m["d"].count("handoffJoin"));
  }
  for (const char* reply : {"accept","switch-off","wrong-library","silent"}) {
    ++cases; const auto on = run(true,true,reply);
    const std::string why = std::string(reply) == "accept" ? "start-at-N-unavailable" :
        std::string(reply) == "silent" ? "timeout" : reply;
    CHECK(std::find(on.logs.begin(),on.logs.end(),"handoff-join: refuse " + why) != on.logs.end());
    CHECK(on.config.card_digest == off.config.card_digest && on.config.card_bytes == off.config.card_bytes);
    if (std::string(reply) == "silent") CHECK(on.now == off.now+10000);
  }
  // Real Stream guest: an advertised offer can arrive inside the card-final repeat wait.
  for (bool enabled : {false,true}) for (bool silent : {false,true}) {
    ++cases; Fixture f(2,1); auto io = f.io(); bool prepared = false;
    io.friends_lobby = true; io.handoff_join = enabled; io.handoff_info = hj_info();
    io.host_save = [] { return Bytes{}; }; io.persist_card = [](const SessionConfig&) { return true; };
    io.prepared = [&](const SessionConfig& c) { CHECK(c.card_bytes == save && c.card_digest == digest); prepared = true; };
    const auto next = io.next_room;
    io.next_room = [&](json& m,int ms) {
      const bool got = next(m,ms); if (got && m.count("console")) m["console"]["cardDigest"] = digest; return got;
    };
    const auto open = io.open_room;
    io.open_room = [&](const std::string& ticket,const std::string& code,int ms) {
      const bool ok = open(ticket,code,ms);
      const size_t n = (save.size()+kCardChunk-1)/kCardChunk;
      for (size_t i = 0; i < n; ++i) f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-card"},
          {"v",1},{"match","session-fixed"},{"i",i},{"n",n},
          {"data",card_encode(save.data()+i*kCardChunk,std::min(kCardChunk,save.size()-i*kCardChunk))}}}});
      return ok;
    };
    const auto send = io.send_room;
    io.send_room = [&](const json& m) {
      if (m.count("d") && str(m["d"],"kind") == "mu-card-verdict") {
        f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-card-final"},{"v",1},
            {"match","session-fixed"},{"use","host"},{"handoffJoin",1}}}});
        const auto info = hj_info();
        f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-hj-offer"},{"v",1},
            {"match","session-fixed"},{"librarySize",info.library_size},{"librarySha256",info.library_sha256},
            {"spansDigest",info.spans_digest},{"os",info.os},{"sceneMajor",2},{"sceneMinor",0}}}});
      }
      if (!silent && m.count("d") && str(m["d"],"kind") == "mu-hj-accept")
        f.push({{"t","msg"},{"from","c0"},{"d",{{"kind","mu-hj-refuse"},{"v",1},
            {"match","session-fixed"},{"reason","start-at-N-unavailable"}}}});
      return send(m);
    };
    SessionConfig c; std::string error;
    CHECK(!start_session(f.input(),c,error,std::move(io)) && prepared && error.empty());
    const std::string why = !enabled ? "switch-off" : "start-at-N-unavailable";
    CHECK(std::find(f.logs.begin(),f.logs.end(),"handoff-join: refuse " + why) != f.logs.end());
    CHECK(f.now <= 10001 && f.packets == 0);
  }
  ++cases; Fixture four(4); auto io = four.io(); bool prepared = false;
  io.friends_lobby = io.handoff_join = true;
  io.prepared = [&](const SessionConfig&) { prepared = true; };
  SessionConfig c; std::string error;
  CHECK(!start_session(four.input(),c,error,std::move(io)) && prepared && error.empty());
  CHECK(std::find(four.logs.begin(),four.logs.end(),"handoff-join: refuse requires-one-guest") != four.logs.end());
  ++cases; Fixture old_host(2,1); auto guest = old_host.io(); prepared = false;
  guest.friends_lobby = guest.handoff_join = true;
  guest.prepared = [&](const SessionConfig&) { prepared = true; };
  CHECK(!start_session(old_host.input(),c,error,std::move(guest)) && prepared && error.empty());
  CHECK(old_host.now == 1); // enabled guest never waits for an old or switch-off host
  CHECK(std::find(old_host.logs.begin(),old_host.logs.end(),"handoff-join: refuse peer-not-offering") != old_host.logs.end());
}

static void friends_handoff_chunks() {
  using namespace handoff_join;
  CHECK(quiescent(0,0)); CHECK(!quiescent(1,0)); CHECK(!quiescent(0,1)); CHECK(!quiescent(0,2));
  Buffer bytes(kCardChunk+19,0x5a);
  auto chunk = [&](size_t k) { return json{{"n",2},{"i",k},{"sha256",card_sha(bytes)},
      {"data",card_encode(bytes.data()+k*kCardChunk,std::min(kCardChunk,bytes.size()-k*kCardChunk))}}; };
  Chunks c;
  CHECK(c.receive(chunk(1)) && !c.complete()); CHECK(c.receive(chunk(1)));
  CHECK(c.receive(chunk(0)) && c.complete() && *c.finish() == bytes);
  for (const auto& bad : {json(-1),json(0),json(999999),json(true),json("2"),json()}) {
    ++cases; auto d = chunk(0); d["n"] = bad; Chunks r; CHECK(!r.receive(d));
  }
  for (const char* field : {"i","data","sha256"}) {
    ++cases; auto d = chunk(0); d[field] = json::array(); Chunks r; CHECK(!r.receive(d));
  }
  auto d = chunk(1); d["data"] = card_encode(bytes.data(),18);
  Chunks conflict; CHECK(conflict.receive(chunk(1))); CHECK(!conflict.receive(d));
  d = chunk(0); d["data"] = card_encode(bytes.data(),18); Chunks short_page; CHECK(!short_page.receive(d));
  d = chunk(0); d["sha256"] = std::string(64,'a'); Chunks mismatch;
  CHECK(mismatch.receive(d)); d = chunk(1); d["sha256"] = std::string(64,'a');
  CHECK(mismatch.receive(d) && mismatch.complete() && !mismatch.finish());
}

// Real room/meet/direct-open/delay setup, followed by scripted main-thread publications.
// No Engine starts and no mu-console-end is emitted when falling back to prepared().
static void friends_handoff_live_transfer() {
  using namespace handoff_join;
  const auto save = save_fixture();
  for (const char* mode : {"ok","base-timeout","at-css-timeout","not-quiescent","ack-timeout",
      "hash-mismatch","send-failed","send-throws","link-failed","cancelled","bad-base-ack"}) {
    ++cases; const std::string how = mode;
    Fixture f; f.auto_delay = true; auto io = f.io();
    io.friends_lobby = io.handoff_join = true; io.handoff_info = hj_info();
    io.handoff_css = [] { return true; }; io.flush_room = [](int) { return true; };
    io.host_save = [&] { return save; }; io.persist_card = [](const SessionConfig&) { return true; };
    f.alter_start = [&](json& m) { m["console"]["cardDigest"] = card_sha(save); };
    bool prepared = false, began = false;
    std::shared_ptr<Channel> channel;
    io.handoff_begin = [&] { began = true; return channel = std::make_shared<Channel>(); };
    io.prepared = [&](const SessionConfig& c) {
      CHECK(c.card_bytes == save && c.card_digest == card_sha(save));
      CHECK(channel && channel->cancelled.load() && !channel->paused.load()); prepared = true;
    };
    const Buffer base(kCardChunk+19,0x31), delta(kCardChunk+7,0x42);
    unsigned serviced = 0;
    const auto next = io.next_room;
    io.next_room = [&](json& m,int wait) {
      if (channel) {
        const auto q = std::atomic_load(&channel->request);
        if (q && q->serial != serviced && how != "base-timeout") {
          auto r = std::make_shared<Result>(); r->serial = q->serial;
          if (q->operation == Operation::Base) { r->bytes = std::make_shared<const Buffer>(base); r->raw = 40*1024*1024; }
          if (q->operation == Operation::PauseDelta) {
            if (how == "not-quiescent") r->reason = "not-quiescent";
            else { r->bytes = std::make_shared<const Buffer>(delta); r->frame = 907; r->hash = "0123456789abcdef"; }
          }
          std::shared_ptr<const Result> immutable = r; std::atomic_store(&channel->result,immutable); serviced = q->serial;
        }
      }
      return next(m,wait);
    };
    const auto send = io.send_room;
    io.send_room = [&](const json& m) {
      if (!m.count("d")) return send(m);
      const auto& d = m["d"]; const auto kind = str(d,"kind");
      auto push = [&](json reply) { reply["v"] = 1; reply["match"] = "session-fixed"; f.push({{"t","msg"},{"from","c1"},{"d",reply}}); };
      if (kind == "mu-card" && d["i"] == 0) push({{"kind","mu-card-verdict"},{"ok",true}});
      if (kind == "mu-hj-offer") { auto accept = d; accept["kind"] = "mu-hj-accept"; push(accept); }
      // Ack and at-css arrive during the sender's LAST pacing interval, before it starts waiting.
      if (kind == "mu-hj-base" && d["i"] == d["n"].get<size_t>()-1) {
        push({{"kind","mu-hj-base-ok"},{"sha256",how == "bad-base-ack" ? std::string(64,'a') : card_sha(base)}});
        if (how != "at-css-timeout") push({{"kind","mu-hj-at-css"}});
      }
      if (kind == "mu-hj-delta" && d["i"] == d["n"].get<size_t>()-1 && how != "ack-timeout")
        push({{"kind","mu-hj-loaded"},{"frame",907},{"hash",how == "hash-mismatch" ? "bad" : "0123456789abcdef"}});
      if (kind == "mu-hj-base" && how == "send-failed") return false;
      if (kind == "mu-hj-base" && how == "send-throws") throw std::runtime_error("offline send failure");
      return send(m);
    };
    if (how == "link-failed") f.attach_ok = false;
    io.cancelled = [&] { return began && how == "cancelled"; };
    SessionConfig config; std::string error;
    CHECK(!start_session(f.input(),config,error,std::move(io)) && error.empty() && prepared && began);
    CHECK(std::any_of(f.requests.begin(),f.requests.end(),[](const json& q) { return str(q,"type") == "directOpen"; }) ||
        how == "link-failed" || how == "cancelled");
    CHECK(std::none_of(f.messages.begin(),f.messages.end(),[](const json& m) {
      return m.count("d") && str(m["d"],"kind") == "mu-console-end";
    }));
    std::string reason = how == "ok" ? "start-at-N-unavailable" : how == "base-timeout" ? "base-timeout" :
        how == "at-css-timeout" || how == "ack-timeout" ? "transfer-timeout" : how == "link-failed" ? "link-setup-failed" :
        how == "bad-base-ack" ? "base-checksum" : how == "send-throws" ? "send-failed" : how;
    CHECK(std::find(f.logs.begin(),f.logs.end(),"handoff-join: refuse "+reason) != f.logs.end());
    CHECK(f.now < 70000);
  }
}

static void friends_handoff_guest_transfer() {
  using namespace handoff_join;
  for (const char* test : {"ok","base-timeout","base-checksum","invalid-chunks","css-timeout","load-refused","load-timeout","peer-refused"}) {
    ++cases; const std::string how = test;
    SessionIO io; uint64_t now = 0; std::deque<json> inbox; std::vector<json> sent;
    std::vector<std::string> logs;
    auto channel = std::make_shared<Channel>();
    auto css = std::make_shared<Request>(); css->serial = 1; css->operation = Operation::GuestCSS;
    channel->request = css;
    io.clock = [&] { return now; }; io.cancelled = [] { return false; }; io.room_closed = [] { return false; };
    io.flush_room = [](int) { return true; }; io.log = [&](const std::string& s) { logs.push_back(s); };
    auto push = [&](json d) { d["v"] = 1; d["match"] = "s"; inbox.push_back({{"t","msg"},{"from","host"},{"d",d}}); };
    const Buffer base(kCardChunk+1,0x31), delta(55,0x42);
    auto enqueue = [&](const char* kind,const Buffer& b) {
      const size_t n = (b.size()+kCardChunk-1)/kCardChunk;
      for (size_t k = 0; k < n; ++k) {
        auto d = json{{"kind",kind},{"i",k},{"n",n},{"sha256",card_sha(b)},
            {"data",card_encode(b.data()+k*kCardChunk,std::min(kCardChunk,b.size()-k*kCardChunk))}};
        if (how == "base-checksum") d["sha256"] = std::string(64,'a');
        if (how == "invalid-chunks") d["n"] = UINT64_MAX;
        push(d);
      }
    };
    if (how != "base-timeout") enqueue("mu-hj-base",base);
    io.next_room = [&](json& m,int wait) {
      const auto q = std::atomic_load(&channel->request);
      if (q && !(how == "load-timeout" && q->operation == Operation::Load) && how != "css-timeout") {
        auto r = std::make_shared<Result>(); r->serial = q->serial;
        if (q->operation == Operation::Load) {
          CHECK(q->base && *q->base == base && q->delta && *q->delta == delta);
          r->frame = 907; r->hash = "0123456789abcdef";
          if (how == "load-refused") r->reason = "load-validation-failed";
        }
        std::shared_ptr<const Result> immutable = r; std::atomic_store(&channel->result,immutable);
      }
      if (inbox.empty()) { now += wait; return false; }
      m = std::move(inbox.front()); inbox.pop_front(); return true;
    };
    io.send_room = [&](const json& m) {
      sent.push_back(m); const auto kind = str(m["d"],"kind");
      if (kind == "mu-hj-at-css") {
        if (how == "peer-refused") push({{"kind","mu-hj-refuse"},{"reason","untrusted\ntext"}});
        else enqueue("mu-hj-delta",delta);
      }
      if (kind == "mu-hj-loaded") push({{"kind","mu-hj-refuse"},{"reason","start-at-N-unavailable"}});
      return true;
    };
    SessionConfig config; config.session_id = "s";
    transfer(io,false,"host",config,channel);
    CHECK(channel->cancelled.load());
    const std::string reason = how == "ok" ? "start-at-N-unavailable" : how == "base-timeout" ? "transfer-timeout" :
        how == "base-checksum" ? "transfer-checksum" : how == "css-timeout" ? "at-css-timeout" :
        how == "load-refused" ? "load-validation-failed" : how == "load-timeout" ? "not-quiescent" : how;
    if (std::find(logs.begin(),logs.end(),"handoff-join: refuse "+reason) == logs.end()) {
      std::fprintf(stderr,"guest mode %s expected %s, last log %s\n",test,reason.c_str(),logs.empty() ? "empty" : logs.back().c_str());
    }
    CHECK(std::find(logs.begin(),logs.end(),"handoff-join: refuse "+reason) != logs.end());
    if (how == "ok") CHECK(std::any_of(sent.begin(),sent.end(),[](const json& m) { return str(m["d"],"kind") == "mu-hj-loaded"; }));
    CHECK(now <= 30000);
  }
}
