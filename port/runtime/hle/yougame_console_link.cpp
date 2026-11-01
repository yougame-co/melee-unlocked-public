// Console lockstep transport; no simulation, native sockets, or Slippi protocol.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_console_link.h"
#include "yougame_friends_lobby.h"
#include "yougame_console_card.h"
#include "yougame_console_io.h"
#include "yougame_update.h"
#include <array>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <atomic>
#include <mutex>
#include <thread>
#include <future>

namespace yougame::console {
namespace {
using transport::str;
constexpr uint32_t kNone = UINT32_MAX;
std::string hex(uint64_t n) { std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(16) << n; return s.str(); }
uint64_t tag(const std::string& s) { uint64_t n = 14695981039346656037ull; for (unsigned char c : s) n = (n ^ c) * 1099511628211ull; return n; }
void put(Bytes& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
uint64_t get(const Bytes& b, size_t& at, int n) { uint64_t v = 0; while (n--) v = (v << 8) | b[at++]; return v; }
void put_pad(Bytes& b, const MuPadStatus& p) {
  put(b, p.button, 2);
  for (auto v : {uint8_t(p.stick_x), uint8_t(p.stick_y), uint8_t(p.sub_x), uint8_t(p.sub_y),
                p.trigger_l, p.trigger_r, p.analog_a, p.analog_b, uint8_t(p.err)}) b.push_back(v);
}
MuPadStatus get_pad(const Bytes& b, size_t& at) {
  MuPadStatus p{}; p.button = uint16_t(get(b, at, 2));
  p.stick_x = int8_t(b[at++]); p.stick_y = int8_t(b[at++]); p.sub_x = int8_t(b[at++]); p.sub_y = int8_t(b[at++]);
  p.trigger_l = b[at++]; p.trigger_r = b[at++]; p.analog_a = b[at++]; p.analog_b = b[at++]; p.err = int8_t(b[at++]); return p;
}
using Roster = transport::RoomState<transport::Stream>;
bool valid_roster(const json& roster, int players, bool console = false, bool lobby = false) {
  if (!roster.is_array() || roster.size() != size_t(players)) return false;
  if (console) {
    std::set<uint64_t> slots; std::set<int> ports; std::set<std::string> connections, ids;
    for (const auto& p : roster) {
      if (!p.is_object() || !number(p,"slot",0,UINT64_MAX) ||
          (lobby ? !number(p,"localIndex",0,0) : !number(p,"port",1,players)) ||
          str(p,"connectionId").empty() || str(p,"id").empty()) return false;
      slots.insert(p["slot"].get<uint64_t>()); connections.insert(str(p,"connectionId")); ids.insert(str(p,"id"));
      if (!lobby) ports.insert(p["port"].get<int>());
    }
    return slots.size() == size_t(players) && (lobby || ports.size() == size_t(players)) &&
        connections.size() == size_t(players) && ids.size() == size_t(players);
  }
  std::set<int> slots; std::set<std::string> connections, ids;
  for (const auto& p : roster) {
    if (!p.is_object() || !number(p, "slot", 0, players - 1) || !number(p, "localIndex", 0, 0) ||
        str(p, "connectionId").empty() || str(p, "id").empty()) return false;
    slots.insert(p["slot"].get<int>()); connections.insert(str(p, "connectionId")); ids.insert(str(p, "id"));
  }
  return slots.size() == size_t(players) && connections.size() == size_t(players) && ids.size() == size_t(players);
}
json seats(json roster, bool keep_ports = false) {
  json out = json::array();
  for (const auto& p : roster) {
    out.push_back({{"slot",p["slot"]},{"connectionId",p["connectionId"]},{"id",p["id"]},{"localIndex",p.value("localIndex",0)}});
    if (keep_ports) out.back()["port"] = p["port"];
  }
  std::sort(out.begin(), out.end(), [](const json& a, const json& b) { return a["slot"] < b["slot"]; }); return out;
}
int connection_port(const json& participants, const std::string& connection) {
  for (const auto& p : participants) if (str(p,"connectionId") == connection) return p["port"].get<int>();
  return 0;
}
struct Pad { MuPadStatus value{}; uint64_t first_sent = 0, last_sent = 0; unsigned sends = 0; };
struct Digest { uint32_t frame; uint64_t value; };
struct PadRing {
  std::array<std::optional<std::pair<uint32_t,MuPadStatus>>,kHistory> values;
  const MuPadStatus* find(uint32_t frame) const {
    const auto& slot = values[frame % kHistory];
    return slot && slot->first == frame ? &slot->second : nullptr;
  }
  void insert(uint32_t frame,const MuPadStatus& value) { values[frame % kHistory] = std::make_pair(frame,value); }
};
json open_link(const transport::Request& request, int connection, const std::string& peer, int ms) {
  return request({{"type","directOpen"},{"conn",connection},{"peer",peer}},ms);
}
// A bounded bridge request must not hold the transport mutex or the game thread.
// An abandoned answer owns its cleanup, including when Leave beats the answer.
struct Reopen {
  transport::Request request;
  std::mutex mutex;
  std::optional<json> reply;
  explicit Reopen(transport::Request r) : request(std::move(r)) {}
  ~Reopen() {
    if (reply) try {
      const auto id = reply->value("link",0u);
      if (id && id <= 65534) request({{"type","directClose"},{"link",id}},250);
    } catch (...) {}
  }
};
struct Peer {
  int port; unsigned link; std::string connection;
  uint64_t heard = 0, sent = 0, resent = 0, late = 0, rtt = 0, opened = 0, retry_at = 0;
  uint32_t ack = kNone, digest_ack = kNone, next = 0, next_digest = 0, last_heard_frame = kNone;
  bool bad_version = false, dirty = true, up_logged = false, sampled = false;
  int reopened = 0;
  std::string path;
  uint64_t worst = 0;
  std::set<uint64_t> pings;
  PadRing pads;
  std::map<uint32_t, uint64_t> digests;
  std::map<uint32_t, Digest> digest_order;
  std::map<uint32_t, Pad> outgoing;
  bool away = false, relinking = false, opening = false, held = false;
  uint64_t lost_at = 0, reopen_at = 0;
  int room_connection = 0;
  std::shared_ptr<Reopen> reopen;
  std::string opening_connection;
};
class Link final : public SessionLink {
 public:
  SessionIO io;
  SessionConfig config;
  json frozen;
  json identities; // account/slot authority from the start; never replaced by lobby arrivals
  std::vector<Peer> peers;
  bool claimed = false;
  bool handoff_preparation = false;
  int host_port = 1;
  uint64_t worst_rtt = 0, choice_until = 0, control_at = 0;
  int chosen_delay = 0;
  bool measuring = false, committed = false;
  std::map<int,uint64_t> reports;
  std::set<int> delay_acks;
  explicit Link(SessionIO i) : io(std::move(i)) { frame_zero_barrier = true; }
  void service() {
    running_ = true;
    if (!io.background_service) return;
    worker_ = std::thread([this] {
      while (!stop_) {
        try { pump(0); }
        catch (...) { std::lock_guard<std::mutex> lock(mutex_); end("your friend disconnected"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }
  ~Link() override {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    if (!handoff_preparation && !peers.empty() && !reason_) end("your friend disconnected");
    for (const auto& p : peers) { try { io.request({{"type","directClose"},{"link",p.link}}, 1000); } catch (...) {} }
    if (io.close) io.close();
    if (claimed) requests.release();
  }
  void end(const std::string& why, bool announce = true) {
    if (reason_) return;
    reason_ = why; io.log("console: session ended: " + why);
    if (announce && !handoff_preparation) for (const auto& p : peers)
      io.send_room({{"t","msg"},{"to",p.connection},{"d",{{"kind","mu-console-end"},{"v",1},{"sessionId",config.session_id},{"reason",why}}}});
  }
  void room_message(const json& m) {
    const auto t = str(m,"t");
    const auto closed = friends::closed_line(m);
    if (!closed.empty()) { end(closed,false); return; }
    if (t == "sessionEnded") {
      if (str(m,"sessionId") == config.session_id) {
        const auto why = friends::text(m,"reason",160);
        end(why.empty() ? "your friend disconnected" : why);
      }
      return;
    }
    // Keep the frozen ports/accounts. Only the authenticated room may replace the
    // transport identity for the SAME held seat, including on a rejoin welcome.
    if (running_) presence(m);
    if (t == "msg" && m.count("d") && m["d"].is_object()) {
      const auto& d = m["d"];
      if (std::none_of(frozen.begin(), frozen.end(), [&](const json& p) { return str(p,"connectionId") == str(m,"from"); })) return;
      if (str(d,"kind") == "mu-console-end" && d.value("v",json()) == 1 && str(d,"sessionId") == config.session_id) {
        const auto why = str(d,"reason");
        if (!friends::text(d,"reason",160).empty()) end(why == kSelfLeft ? "left" : why);
      }
    }
  }
  void send_pad(uint32_t frame, const MuPadStatus& pad) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reason_) return;
    if (frame == kNone || (newest_ != kNone && frame <= newest_)) return;
    newest_ = frame;
    for (auto& p : peers) {
      if (p.outgoing.size() >= kHistory) { end("your friend disconnected"); return; }
      p.outgoing.emplace(frame, Pad{pad}); p.dirty = true;
    }
    transmit();
    if ((uint64_t(frame) + 1) % 600 == 0) for (auto& p : peers) {
      io.log("console: port " + std::to_string(p.port) + " rtt " + std::to_string(p.rtt) + " ms, resent " +
          std::to_string(p.resent) + ", late " + std::to_string(p.late)); p.resent = p.late = 0;
    }
  }
  std::optional<MuPadStatus> pad(int port, uint32_t frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : peers) if (p.port == port) { if (const auto* value = p.pads.find(frame)) return *value; }
    return {};
  }
  void send_digest(uint32_t frame, uint64_t value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reason_) return;
    if (out_digests_.size() >= kHistory || digest_serial_ == kNone) { end("your friend disconnected"); return; }
    if (frame == 0 && !ready_since_) ready_since_ = io.clock();
    out_digests_.emplace(digest_serial_++, Digest{frame,value});
    for (auto& p : peers) p.dirty = true;
    transmit();
  }
  std::optional<uint64_t> digest(int port, uint32_t frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : peers) if (p.port == port) { auto i = p.digests.find(frame); if (i != p.digests.end()) return i->second; }
    return {};
  }
  std::optional<std::string> lost() const override { std::lock_guard<std::mutex> lock(mutex_); return reason_; }
  bool reconnecting() const override {
    std::lock_guard<std::mutex> lock(mutex_); return relink_since_.has_value();
  }
  uint64_t liveness_clock(uint64_t) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    // Sample time and recovery under ONE lock, so a concurrent completion cannot
    // make this clock go backwards between the Engine's deadline comparisons.
    return (relink_since_ ? *relink_since_ : io.clock()) - relink_total_;
  }
  json open_peer(const std::string& peer, int ms) { return open_link(io.request,io.connection(),peer,ms); }
  void startup_work() {
    // A peer may finish its other links later than ours. Keep its established links
    // alive, and ACK any frame-zero inputs already queued, throughout the mesh setup.
    unsigned id; Bytes b;
    for (int n = 0; n < 256 && io.receive(id,b); ++n)
      for (auto& p : peers) if (p.link == id) receive(p,b);
    transmit();
    paths();
  }
  void pump(int wait_ms) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (reason_) return;
    const auto until = io.clock() + uint64_t(std::max(0,wait_ms));
    auto work = [&] {
      bool received = false;
      json m;
      for (int n = 0; n < 64 && io.next_room(m,0); ++n) room_message(m);
      // The room's own leave/sessionEnded tells the friend; "you left" over the link would hide the notice.
      if (io.cancelled()) end(kSelfLeft,false);
      if (io.room_closed()) {
        for (const auto& p : peers) if (p.relinking) { gave_up(p,"room closed"); break; }
        end("your friend disconnected");
      }
      if (reason_) return false;
      unsigned id; Bytes b;
      for (int n = 0; n < 256 && io.receive(id,b); ++n) {
        received = true;
        for (auto& p : peers) if (p.link == id) receive(p,b);
        if (io.clock() >= until && wait_ms > 0) break;
      }
      const auto now = io.clock();
      if (!armed_ && ready_since_ && std::all_of(peers.begin(),peers.end(),[](const Peer& p) { return p.digests.count(0); })) {
        armed_ = true;
        for (auto& p : peers) p.heard = now;
        io.log("console: all peers at frame 0; liveness armed");
      }
      if (!armed_ && ready_since_ && !relink_since_ && now - *ready_since_ >= 60000 + relink_total_) end(kBootTimeout);
      for (auto& p : peers) {
        const bool closed = io.state(p.link) == 3;
        const auto silent = now >= p.heard ? now - p.heard : 0;
        if (running_ && !p.relinking && p.room_connection != io.connection()) lose(p,now);
        if (p.relinking) { relink(p,now); continue; }
        if (closed || (armed_ && silent >= 5000)) {
          if (running_) { lose(p,now); relink(p,now); continue; }
          io.log("console: port " + std::to_string(p.port) + " silent " + std::to_string(silent) +
              " ms (last heard frame " + (p.last_heard_frame != kNone ? std::to_string(p.last_heard_frame) : std::string("-1")) +
              "): " + (closed ? "transport closed" : "no receive for 5 s"));
          end("your friend disconnected"); break;
        }
      }
      if (relink_since_ && std::none_of(peers.begin(),peers.end(),[](const Peer& p) { return p.relinking; })) {
        relink_total_ += now-*relink_since_; relink_since_.reset();
      }
      transmit(); paths();
      if (committed && config.my_port == host_port && io.clock() < choice_until && io.clock() >= control_at) {
        broadcast(6, worst_rtt, chosen_delay); control_at = io.clock()+50;
      }
      return received;
    };
    const bool received = work();
    const auto now = io.clock();
    if (!reason_ && !received && now < until) {
      // The ring doorbell wakes for datagrams. Recheck room events and keepalives every 10 ms.
      // Sleeping must not block a sampled pad or the loading service worker.
      lock.unlock();
      io.wait(int(std::min<uint64_t>(10,until - now)));
      lock.lock();
      if (!reason_) work();
    }
  }
  void paths() {
    for (auto& p : peers) {
      auto path = io.route(p.link);
      if (path != "lan" && path != "direct" && path != "relay") path = "unknown";
      if (p.up_logged && path != p.path && (path == "lan" || path == "direct"))
        io.log("console: link to port " + std::to_string(p.port) + " now " + path);
      p.path = path;
      if (io.rtt) for (auto sample : io.rtt(p.link)) if (sample <= 60000) {
        p.rtt = sample; p.worst = std::max(p.worst,sample); p.sampled = true;
      }
    }
  }
  void control(Peer& p, uint8_t kind, uint64_t value, int delay = 0) {
    Bytes b{0xc1,uint8_t(config.my_port),kind,uint8_t(delay)};
    put(b,tag(config.session_id),8); put(b,value,8); io.send(p.link,b);
  }
  void broadcast(uint8_t kind, uint64_t value, int delay = 0) {
    for (auto& p : peers) control(p,kind,value,delay);
  }
  bool agree_delay() {
    measuring = true;
    const auto measure_until = io.clock()+500, until = measure_until+3000;
    uint64_t ping_at = 0, report_at = 0;
    while (io.clock() < until && !lost() && !io.cancelled()) {
      pump(5);
      const auto now = io.clock();
      if (now < measure_until && now >= ping_at) {
        for (auto& p : peers) { p.pings.insert(now); control(p,1,now); }
        ping_at = now+50;
      }
      if (now < measure_until) continue;
      const bool sampled = std::all_of(peers.begin(),peers.end(),[](const Peer& p) { return p.sampled; });
      if (sampled && now >= report_at) {
        uint64_t mine = 0; for (const auto& p : peers) mine = std::max(mine,p.worst);
        reports[config.my_port] = mine;
        for (auto& p : peers) if (p.port == host_port) control(p,3,mine);
        report_at = now+50;
      }
      if (config.my_port == host_port && reports.size() == config.ports.size()) {
        if (!chosen_delay) {
          for (const auto& r : reports) worst_rtt = std::max(worst_rtt,r.second);
          chosen_delay = measured_delay(worst_rtt,config.input_delay);
        }
        broadcast(4,worst_rtt,chosen_delay);
        if (delay_acks.size() == peers.size()) {
          committed = true; choice_until = now+3000;
          broadcast(6,worst_rtt,chosen_delay); break;
        }
      }
      if (config.my_port != host_port && committed) break;
    }
    measuring = false;
    if (!committed) return false;
    config.input_delay = chosen_delay;
    io.log("console: input delay " + std::to_string(chosen_delay) + " (worst rtt " + std::to_string(worst_rtt) + " ms)");
    return true;
  }
 private:
  mutable std::mutex mutex_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
  bool armed_ = false;
  bool running_ = false;
  uint64_t relink_total_ = 0;
  std::optional<uint64_t> relink_since_;
  std::optional<uint64_t> ready_since_;
  std::optional<std::string> reason_;
  uint32_t newest_ = kNone, digest_serial_ = 0;
  std::map<uint32_t, Digest> out_digests_;
  void gave_up(const Peer& p, const char* why) {
    if (reason_) return;
    io.log("console: relink gave up after " + std::to_string(io.clock()-p.lost_at) + " ms (" + why + ")");
    end("your friend disconnected");
  }
  void lose(Peer& p, uint64_t now) {
    if (p.relinking || reason_) return;
    p.relinking = true; p.held = false; p.lost_at = now; p.reopen_at = now;
    if (!relink_since_) relink_since_ = now;
    io.log("console: link to port " + std::to_string(p.port) + " lost; waiting for the room to come back");
  }
  void presence(const json& m) {
    if (str(m,"t") == "welcome") {
      const auto room = m.find("room");
      const json* c = m.count("console") && m["console"].is_object() ? &m["console"] :
        room != m.end() && room->is_object() && room->count("console") && (*room)["console"].is_object() ? &(*room)["console"] : nullptr;
      // Rejoining after expiry can miss sessionEnded. A complete welcome must
      // still hold THIS session; lobby membership alone cannot revive its game.
      if (!c || str(*c,"sessionId") != config.session_id) { end("your friend disconnected"); return; }
    }
    const json* roster = nullptr;
    if (m.count("matchParticipants") && m["matchParticipants"].is_array()) roster = &m["matchParticipants"];
    else if (m.count("participants") && m["participants"].is_array()) roster = &m["participants"];
    else if (m.count("players") && m["players"].is_array()) roster = &m["players"];
    else if (str(m,"t") == "welcome") {
      const auto room = m.find("room");
      const json* c = m.count("console") && m["console"].is_object() ? &m["console"] :
        room != m.end() && room->is_object() && room->count("console") && (*room)["console"].is_object() ? &(*room)["console"] : nullptr;
      if (c && str(*c,"sessionId") == config.session_id && c->count("participants") && (*c)["participants"].is_array()) roster = &(*c)["participants"];
    }
    if (!roster) return; // partial controls are not evidence that a seat was released
    for (auto& p : peers) {
      auto frozen_seat = std::find_if(frozen.begin(),frozen.end(),[&](const json& s) { return s["port"] == p.port; });
      if (frozen_seat == frozen.end()) continue;
      const auto identity = std::find_if(identities.begin(),identities.end(),[&](const json& s) { return s["port"] == p.port; });
      if (identity == identities.end()) continue;
      auto seat = std::find_if(roster->begin(),roster->end(),[&](const json& s) {
        const auto conn = str(s,"connectionId").empty() ? str(s,"id") : str(s,"connectionId");
        const auto slot = s.count("slot") ? s["slot"] : s.value("seat",json());
        if (!slot.is_null() && slot != (*identity)["slot"]) return false;
        if (s.count("user") && identity->count("user") && s["user"] != (*identity)["user"]) return false;
        if (conn == p.connection) return true;
        return slot == (*identity)["slot"] && !str(*identity,"user").empty() &&
            str(s,"user") == str(*identity,"user");
      });
      if (seat == roster->end()) { end("your friend disconnected"); return; }
      const auto conn = str(*seat,"connectionId").empty() ? str(*seat,"id") : str(*seat,"connectionId");
      if (conn.empty()) continue;
      const json* player = &*seat;
      if (m.count("players") && m["players"].is_array()) {
        const auto& players = m["players"];
        auto found = std::find_if(players.begin(),players.end(),[&](const json& s) { return str(s,"id") == conn; });
        if (found == players.end()) { end("your friend disconnected"); return; }
        player = &*found;
      }
      p.away = player->value("away",json()) == true;
      if (p.away) p.held = true;
      if (conn != p.connection) {
        if (io.rebound) io.rebound(p.connection,conn);
        lose(p,io.clock()); p.connection = conn; (*frozen_seat)["connectionId"] = conn;
        p.held = true; // the room re-seated the same account
      }
    }
  }
  void relink(Peer& p, uint64_t now) {
    if (reason_) return;
    // The loss timestamp is never extended by rosters, refused opens or retries.
    if (now-p.lost_at >= 75000) { gave_up(p,p.away || io.room_rejoining() ? "room did not return" : "link did not come up"); return; }
    if (p.away || io.room_rejoining()) p.held = true;
    // Without an away/rejoin/re-seat sign the friend's seat is not being held for them.
    if (!p.held && !p.opening && now-p.lost_at >= 20000) { gave_up(p,"seat not held"); return; }
    if (p.reopen) {
      std::optional<json> reply;
      { std::lock_guard<std::mutex> lock(p.reopen->mutex); reply.swap(p.reopen->reply); }
      if (!reply) return;
      p.reopen.reset();
      const auto id = reply->value("link",0u);
      if (id && id <= 65534) { p.link = id; p.opening = true; }
      else {
        const auto code = str(*reply,"refused");
        if (code == "no_direct" || code == "ended") { gave_up(p,"direct links unavailable"); return; }
        p.reopen_at = now+1000; return;
      }
    }
    if (p.opening) {
      if (p.opening_connection != p.connection || p.room_connection != io.connection() || p.away || io.room_rejoining()) {
        p.opening = false; p.reopen_at = now;
      } else if (io.state(p.link) == 2) {
        p.opening = p.relinking = false; p.heard = now; p.dirty = true; p.retry_at = now;
        for (auto& kv : p.outgoing) kv.second.sends = 0; // retry every unacknowledged frame immediately
        auto route = io.route(p.link);
        if (route != "lan" && route != "direct" && route != "relay") route = "unknown";
        p.path = route;
        io.log("console: link to port " + std::to_string(p.port) + " reopened via " + route + " after " + std::to_string(now-p.lost_at) + " ms");
        return;
      } else if (io.state(p.link) == 3) { p.opening = false; p.reopen_at = now+1000; }
      else return;
    }
    if (p.away || io.room_rejoining() || now < p.reopen_at) return;
    p.room_connection = io.connection(); p.opening_connection = p.connection;
    p.reopen = std::make_shared<Reopen>(io.request);
    auto work = [attempt = p.reopen, conn = p.room_connection, peer = p.connection, old = p.link] {
      json reply = json::object();
      try {
        attempt->request({{"type","directClose"},{"link",old}},250);
        reply = open_link(attempt->request,conn,peer,1000);
      } catch (...) {} // bounded retry; never log bridge exception text
      std::lock_guard<std::mutex> lock(attempt->mutex); attempt->reply = std::move(reply);
    };
    if (io.background_service) std::thread(std::move(work)).detach(); else work();
  }
  Bytes header(const Peer& p) const {
    Bytes b; put(b,kWireVersion,1); put(b,config.my_port,1); put(b,0,1); put(b,0,1);
    put(b,tag(config.session_id),8); put(b,p.next ? p.next - 1 : kNone,4); put(b,p.next_digest ? p.next_digest - 1 : kNone,4);
    return b;
  }
  void transmit() {
    if (reason_) return;
    const auto now = io.clock();
    for (auto& p : peers) {
      if (p.relinking) continue;
      // New samples and ACKs must not reset another frame's retry deadline.
      // Batch retries independently of new samples/ACKs. This also coalesces
      // staggered holes instead of perpetually retrying the same small groups.
      auto due = [&](const Pad& v) { return !v.sends || (now >= p.retry_at && now - v.last_sent >= 50); };
      const bool pad_due = std::any_of(p.outgoing.begin(),p.outgoing.end(),[&](const auto& kv) { return due(kv.second); });
      if (!p.dirty && !pad_due && now - p.sent < (out_digests_.empty() ? 100u : 50u)) continue;
      Bytes b = header(p);
      std::vector<uint32_t> frames;
      if (!p.outgoing.empty()) {
        const auto newest = p.outgoing.rbegin()->first;
        if (due(p.outgoing.rbegin()->second)) frames.push_back(newest);
        // Keep newest/oldest-hole priority, but retry each record at most every 50 ms.
        for (const auto& kv : p.outgoing) if (due(kv.second) && (frames.empty() || kv.first != frames.front()) && frames.size() < 16) frames.push_back(kv.first);
      }
      b[2] = uint8_t(frames.size());
      for (auto f : frames) {
        auto& v = p.outgoing.at(f);
        if (v.sends++) { ++p.resent; p.retry_at = now+50; } else v.first_sent = now;
        v.last_sent = now;
        put(b,f,4); put_pad(b,v.value);
      }
      for (const auto& kv : out_digests_) {
        if (p.digest_ack != kNone && kv.first <= p.digest_ack) continue;
        if (b.size() + 16 > 1200 || b[3] == 48) break;
        put(b,kv.first,4); put(b,kv.second.frame,4); put(b,kv.second.value,8); ++b[3];
      }
      io.send(p.link,b); p.sent = now; p.dirty = false;
    }
  }
  void receive(Peer& p, const Bytes& b) {
    if (b.empty()) return;
    if (b[0] == 0xc1) {
      if (b.size() != 20 || b[1] != p.port) return;
      size_t at = 4; if (get(b,at,8) != tag(config.session_id)) return;
      const auto value = get(b,at,8); const auto now = io.clock();
      p.heard = now;
      if (b[2] == 1) control(p,2,value);
      if (b[2] == 2 && p.pings.erase(value) && now >= value) {
        const auto sample = now-value; p.rtt = sample; p.worst = std::max(p.worst,sample); p.sampled = true;
      }
      if (b[2] == 3 && config.my_port == host_port && value <= 60000 && !chosen_delay) reports[p.port] = value;
      if (b[2] == 4 && p.port == host_port && value <= 60000 &&
          b[3] == measured_delay(value,config.input_delay) && (!chosen_delay || (chosen_delay == b[3] && worst_rtt == value))) {
        chosen_delay = b[3]; worst_rtt = value; control(p,5,value,chosen_delay);
      }
      if (b[2] == 5 && config.my_port == host_port && chosen_delay && b[3] == chosen_delay && value == worst_rtt) delay_acks.insert(p.port);
      if (b[2] == 6 && p.port == host_port && chosen_delay && b[3] == chosen_delay && value == worst_rtt) committed = true;
      return;
    }
    if (b[0] != kWireVersion) {
      if (!p.bad_version) { io.log("console: dropped packet version " + std::to_string(b[0]) + " from port " + std::to_string(p.port)); p.bad_version = true; }
      return;
    }
    if (b.size() < 20 || b.size() > 1200 || b[1] != p.port || b[2] > 16 || b[3] > 48 || b.size() != 20u + b[2]*15u + b[3]*16u) return;
    size_t at = 4;
    if (get(b,at,8) != tag(config.session_id)) return;
    const uint32_t ack = uint32_t(get(b,at,4)), da = uint32_t(get(b,at,4));
    if (ack != kNone && (newest_ == kNone || ack > newest_)) return;
    if (da != kNone && da >= digest_serial_) return;
    // Receive clock, independent of simulation frames: production service() pumps
    // this transport on its worker throughout loading, stalls and slow gameplay.
    p.heard = io.clock();
    if (ack != kNone && (p.ack == kNone || ack > p.ack)) {
      auto i = p.outgoing.find(ack); if (i != p.outgoing.end()) p.rtt = io.clock() - i->second.first_sent;
      p.ack = ack; p.outgoing.erase(p.outgoing.begin(),p.outgoing.upper_bound(ack));
    }
    if (da != kNone && (p.digest_ack == kNone || da > p.digest_ack)) p.digest_ack = da;
    while (!out_digests_.empty() && std::all_of(peers.begin(),peers.end(),[&](const Peer& q) { return q.digest_ack != kNone && q.digest_ack >= out_digests_.begin()->first; })) out_digests_.erase(out_digests_.begin());
    for (int n = 0; n < b[2]; ++n) {
      const uint32_t f = uint32_t(get(b,at,4)); const auto value = get_pad(b,at);
      if (f == kNone || uint64_t(f) + kHistory < p.next || uint64_t(f) >= uint64_t(p.next) + kHistory) continue;
      if (p.last_heard_frame == kNone || f > p.last_heard_frame) p.last_heard_frame = f;
      if (!p.pads.find(f)) { if (f < p.next) continue; if (f != p.next) ++p.late; p.pads.insert(f,value); }
      while (p.pads.find(p.next)) ++p.next;
    }
    for (int n = 0; n < b[3]; ++n) {
      const uint32_t serial = uint32_t(get(b,at,4)), frame = uint32_t(get(b,at,4)); const auto value = get(b,at,8);
      if (serial < p.next_digest || uint64_t(serial) >= uint64_t(p.next_digest) + kHistory) continue;
      if (p.last_heard_frame == kNone || frame > p.last_heard_frame) p.last_heard_frame = frame;
      p.digest_order.emplace(serial,Digest{frame,value});
      while (p.digest_order.count(p.next_digest)) {
        const auto d = p.digest_order.at(p.next_digest); p.digests.emplace(d.frame,d.value);
        p.digest_order.erase(p.next_digest++);
      }
      while (p.digests.size() > kHistory) p.digests.erase(p.digests.begin());
    }
    // ACK immediately, including duplicates after a lost ACK. Replaying our pending
    // payload here makes two peers amplify each other's retries at the pump rate.
    // An ACK-only reply never requests another ACK or delays a data retry.
    if (b[2] || b[3]) io.send(p.link,header(p));
  }
};
} // namespace

int measured_delay(uint64_t worst_rtt_ms, int minimum) {
  const auto frames = std::ceil((double(worst_rtt_ms)/2.0)/16.67)+1;
  return std::max(minimum,int(std::clamp(frames,2.0,8.0)));
}

// The ticket route opens a private room only for a code it accepts (web lib/multiplayer.ts ROOM_CODE_RE,
// 6-12 of [a-z0-9]); "new" is not one, so a host names its own room.
std::string new_room_code(uint64_t r) {
  static const char digits[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string code;
  for (int n = 0; n < 12; ++n) { code += digits[r % 36]; r /= 36; }
  return code;
}

std::unique_ptr<SessionLink> start_session(const json& input, SessionConfig& config, std::string& error, SessionIO io) {
  error.clear();
  auto link = std::make_unique<Link>(std::move(io));
  auto& i = link->io;
  // Which stage a throw came from, for the log below (never the exception's text: it can carry a ticket).
  const char* step = "request";
  std::shared_ptr<handoff_join::Channel> live;
  SessionConfig fallback_config;
  auto relaunch = [&]() -> std::unique_ptr<SessionLink> {
    if (live) {
      live->cancelled.store(true,std::memory_order_release);
      // Main restores B before publishing paused=false. Never race prepared() with imported memory.
      while (live->working.load(std::memory_order_acquire)) i.wait(1);
    }
    for (const auto& p : link->peers) { try { i.request({{"type","directClose"},{"link",p.link}},1000); } catch (...) {} }
    link->peers.clear();
    error.clear(); i.prepared(fallback_config); return nullptr;
  };
  auto fail = [&](const char* why) -> std::unique_ptr<SessionLink> {
    if (live) {
      i.log(std::string("handoff-join: refuse ") + (i.cancelled() ? "cancelled" : "link-setup-failed"));
      for (const auto& p : link->frozen) if (p["port"] != fallback_config.my_port) {
        try { i.send_room({{"t","msg"},{"to",p["connectionId"]},{"d",{{"kind","mu-hj-refuse"},{"v",1},
            {"match",fallback_config.session_id},{"reason","link-setup-failed"}}}}); } catch (...) {}
      }
      try { if (i.flush_room && !i.room_closed()) i.flush_room(1000); } catch (...) {}
      return relaunch();
    }
    error = why; i.log("console: start failed: " + error + " (at " + step + ")"); return nullptr;
  };
  json request;
  if (!parse_request(input,request)) return fail("invalid console session request");
  if (i.exclusive && !requests.claim()) return fail("a console session is already running");
  link->claimed = i.exclusive;
  try {
    const int players = request["players"].get<int>();
    const bool host_new = request["room"].get<std::string>() == "new";
    const auto code = host_new ? new_room_code(i.random ? i.random() : i.clock()) : request["room"].get<std::string>();
    json ticket_request = {{"type","mpTicket"},{"room",code},{"players",players},{"mode","console"},{"ranked",false},
      {"lobby",{{"version",1},{"minPlayers",players},{"maxLocalPlayers",1},{"compatibility",i.compatibility}}}};
    if (i.friends_lobby) ticket_request["queue"] = "private";
    step = "ticket";
    const auto ticket = i.request(ticket_request,10000);
    const auto room = friends::room(ticket);
    if (room.empty() || code != room) {
      i.log("console: ticket for room " + code + " came back " + (room.empty() ? std::string("without a room") : "for room " + room));
      return fail("couldn't open your friend's room");
    }
    step = "room";
    if (!i.open_room(str(ticket,"ticket"),room,10000)) {
      i.log("console: room " + room + " socket didn't open");
      return fail(i.room_mismatch && i.room_mismatch() ? kMixedBuilds : "couldn't open your friend's room");
    }
#ifdef YOUGAME_AGENT_TEST_HOOK
    if (host_new) i.log("agent: console host room " + room);
#endif
    if (i.opened) i.opened(room);
    Roster r; r.size = players; json start;
    bool seat_logged = false;
    uint64_t next_begin = 0, ping = i.clock();
    // A friend has to accept the invite, launch and boot (quality's run: the joiner opened the room 114 s after the
    // host, after the old 30 s wait had closed it). A human host can cancel from the waiting panel sooner.
    const uint64_t seats_until = i.clock() + 300000;
    const bool rejoin = request.count("sessionId");
    const auto delay = request.value("inputDelay",3);
    Bytes host_card; std::string host_digest; bool card_read = false;
    const bool sync_card = players == 2 && bool(i.host_save);
    step = "seats";
    while (i.friends_lobby || i.clock() < seats_until) {
      if (i.cancelled()) return fail("cancelled");
      json m;
      if (i.next_room(m,i.friends_lobby ? 50 : int(std::min<uint64_t>(50,seats_until-i.clock())))) {
        const auto t = str(m,"t");
        if (t == "welcome") r.me = str(m,"me");
        transport::read_roster(r,m);
        const auto closed = friends::closed_line(m);
        if (!closed.empty()) return fail(closed.c_str());
        if (i.roster && !r.me.empty()) i.roster(room,r.participants,r.me,r.revision);
        if (t == "command-error" && str(m,"action") == "begin" && str(m,"code") == "console_mixed_builds") {
          i.log("console: command-error console_mixed_builds"); return fail(kMixedBuilds);
        }
        if (t == "error" || t == "ended" || t == "kicked" || t == "command-error") return fail("couldn't start your friend's room");
        if (t == "sessionEnded" && !i.held_reenter && (!rejoin || str(m,"sessionId") == str(request,"sessionId"))) {
          const auto why = friends::text(m,"reason",160);
          i.log("console: session ended: " + why);
          return fail(why.empty() ? "your friend disconnected" : why.c_str());
        }
        if (!m.count("console") && m.count("room") && m["room"].is_object() && m["room"].count("console")) m["console"] = m["room"]["console"];
        if (!i.previous_session_active() && m.count("console") && m["console"].is_object()) { start = m; break; }
      }
      if (i.room_closed()) { i.log("console: room " + room + " closed while waiting for seats"); return fail("couldn't open your friend's room"); }
      const auto seat = transport::connection_seat(r.participants,r.me);
      if (!seat_logged && seat >= 0) { i.log("console: room " + room + " seat " + std::to_string(seat) + " of " + std::to_string(players)); seat_logged = true; }
      const bool full = valid_roster(r.participants,players,true,true), host = r.me == r.host_connection;
      if (!i.friends_lobby && i.waiting) i.waiting(full ? (host ? "Press START to play together" : "Waiting for the host") : friends::kWaiting, full && host);
      if (!rejoin && !i.previous_session_active() && full && host && (i.friends_lobby ? i.css_ready() : i.confirm_start()) && i.clock() >= next_begin) {
        json begin = {{"t","begin"},{"revision",r.revision},{"console",true},{"inputDelay",delay},{"settingsDigest",""}};
        if (i.freeze_pick) {
          std::string name;
          for (const auto& p : r.participants) if (str(p,"connectionId") != r.me) { name = friends::text(p,"name",40); break; }
          begin["settingsDigest"] = i.freeze_pick(name);
        }
        if (sync_card) {
          step = "card read";
          if (!card_read) {
            card_read = true;
            host_card = card_io_until(i.clock,i.cancelled,i.friends_lobby ? i.clock()+20000 : std::min(seats_until,i.clock()+20000),Bytes{},i.host_save);
            // Never refuse START over the save: without a shareable one, the begin carries no
            // cardDigest and every seat boots the same fresh card (default unlocks).
            if (melee_gci(host_card)) host_digest = card_sha(host_card);
            else { i.log("console: couldn't share the host's save (" + std::to_string(host_card.size()) + " bytes); every seat starts with default unlocks"); host_card.clear(); }
          }
          if (!host_digest.empty()) begin["cardDigest"] = host_digest;
          step = "seats";
        }
        if (!i.send_room(begin)) return fail("couldn't start your friend's room");
        next_begin = i.clock() + 2000;
      }
      if (i.clock() - ping >= 5000) { i.send_room({{"t","ping"},{"at",i.clock()}}); ping = i.clock(); }
    }
    if (start.is_null()) return fail("your friend didn't join");
    const auto frozen_at = i.clock();
    step = "start";
    if (!start.count("console") || !start["console"].is_object()) return fail("invalid console session start");
    const auto& s = start["console"];
    uint64_t start_seed = 0;
    if (!s.count("participants") || !valid_roster(s["participants"],players,true)) return fail("invalid console session start");
    if (str(start,"t") == "start" && start.count("participants") && (!valid_roster(start["participants"],players,true,true) || seats(start["participants"]) != seats(s["participants"]))) return fail("invalid console session start");
    const int my_port = connection_port(s["participants"],r.me);
    const auto seed_text = str(s,"seed");
    if ((start.count("matchId") && start["matchId"] != s.value("sessionId",json())) || my_port < 1 || session_id(s).empty() || seed_text.size() != 16 ||
        seed_text.find_first_not_of("0123456789abcdef") != std::string::npos || !read_seed(s,start_seed) ||
        !number(s,"startedAt",0,INT64_MAX) || !number(s,"inputDelay",1,8) ||
        !s.count("settingsDigest") || !s["settingsDigest"].is_string() ||
        str(s,"settingsDigest").size() > 128 || str(s,"settingsDigest").find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:_-") != std::string::npos) return fail("invalid console session start");
    SessionConfig c; c.room = room; c.my_port = my_port; c.session_id = s["sessionId"]; c.seed = start_seed;
    c.epoch_ms = s["startedAt"].get<int64_t>(); c.input_delay = s["inputDelay"].get<int>();
    c.settings_digest = s["settingsDigest"].get<std::string>();
    c.card_digest = str(s,"cardDigest");
    if (!c.card_digest.empty() && !card_hash(c.card_digest)) return fail(kCardFailure);
    if (sync_card && c.card_digest.empty()) i.log("console: no shared save in the start; this seat starts with default unlocks");
    if (request.count("cardDigest") && request["cardDigest"] != c.card_digest) return fail(kCardFailure);
    for (const auto& p : s["participants"]) c.ports.push_back(p["port"].get<int>());
    std::sort(c.ports.begin(),c.ports.end());
    for (auto key : {"sessionId","seed","epochMs","inputDelay","port"}) {
      if (!request.count(key)) continue;
      const json value = std::string(key) == "port" ? json(c.my_port) : std::string(key) == "seed" ? json(c.seed) :
        std::string(key) == "epochMs" ? json(c.epoch_ms) : s[key];
      if (request[key] != value) return fail("console session start changed");
    }
    link->config = c; link->frozen = seats(s["participants"],true); link->identities = s["participants"];
    link->host_port = connection_port(link->frozen,r.host_connection);
    if (link->host_port < 1) return fail("invalid console session start");
    if (i.committed) i.committed(c);
    if (rejoin) i.log("console: rejoined session " + c.session_id);
    if (i.cancelled()) return fail("cancelled");
    if (rejoin) {
      bool restored = false;
      try { restored = i.restore_card && i.restore_card(c) && card_bytes_ok(c); }
      catch (const std::exception&) { /* unreadable local state fails open */ }
      if (!restored) {
        c.default_card = true; c.card_digest.clear(); c.card_bytes.clear();
        i.log("console: save choice missing/default/unreadable; proposing default unlocks");
      }
    }
    // Agreement is scoped to the live two-seat Friends preparation, never boot/rejoin or queues.
    std::unique_ptr<handoff_join::Agreement> hj;
    const bool hj_host = c.my_port == link->host_port;
    if (i.friends_lobby && i.prepared && !rejoin && players == 2) {
      for (const auto& peer : link->frozen) if (str(peer,"connectionId") != r.me)
        hj = std::make_unique<handoff_join::Agreement>(hj_host,i.handoff_join,i.handoff_info,
            c.session_id,str(peer,"connectionId"),i.send_room,i.log);
    }
    if (!c.card_digest.empty()) {
      step = "card transfer";
      const auto until_card = frozen_at + 20000;
      const auto until_verdict = frozen_at + 25000;
      const auto sha8 = c.card_digest.substr(0,8);
      auto card_log = [&](size_t n,const char* result) {
        i.log("console: card received " + std::to_string(n) + " chunks sha " + sha8 + " " + result);
      };
      if (rejoin) {
        // Already restored from the game-local choice; rendezvous agrees all seats below.
      } else {
        const auto host = str(*std::find_if(link->frozen.begin(),link->frozen.end(),[&](const json& p) {
          return p["port"] == link->host_port;
        }),"connectionId");
        const bool am_host = c.my_port == link->host_port;
        const auto until_final = frozen_at + (am_host ? 30000 : 40000);
        std::map<std::string,bool> verdicts;
        std::string final;
        uint64_t final_at = 0;
        bool final_conflict = false;
        // Control messages may arrive during chunk pacing or before a guest has all chunks.
        // Only the frozen host/guest identities and this exact session can decide the save.
        auto control = [&](const json& m) {
          link->room_message(m);
          if (hj) hj->receive(m,i.clock());
          if (str(m,"t") != "msg" || !m.count("d") || !m["d"].is_object()) return;
          const auto& d = m["d"];
          if (d.value("v",json()) != 1 || str(d,"match") != c.session_id) return;
          const auto from = str(m,"from");
          if (am_host && i.clock() < until_verdict && from != r.me && connection_port(link->frozen,from) &&
              str(d,"kind") == "mu-card-verdict" && d.count("ok") && d["ok"].is_boolean())
            verdicts.emplace(from,d["ok"].get<bool>());
          if (!am_host && (!final.empty() || i.clock() < until_final) && from == host && str(d,"kind") == "mu-card-final" &&
              (str(d,"use") == "host" || str(d,"use") == "default")) {
            // Only an opted-in host adds this field. Both switches off keep the old wire/time trace.
            if (hj && d.value("handoffJoin",json()) == 1) hj->expect(i.clock());
            if (final.empty()) { final = str(d,"use"); final_at = i.clock(); }
            else if (final != str(d,"use")) final_conflict = true;
          }
        };
        auto persist = [&] {
          if (!i.persist_card) return false;
          return card_io_until(i.clock,i.cancelled,until_card,false,[write = i.persist_card, card = c] { return write(card); });
        };
        bool ok = true;
        if (am_host) {
          c.card_bytes = std::move(host_card);
          ok = card_bytes_ok(c);
          const size_t n = (c.card_bytes.size()+kCardChunk-1)/kCardChunk;
          if (!ok) card_log(0,"mismatch");
          for (const auto& peer : link->frozen) if (str(peer,"connectionId") != r.me) {
            for (size_t chunk = 0; ok && chunk < n; ++chunk) {
              if (i.cancelled()) return fail("cancelled");
              if (i.clock() >= until_card) { card_log(n,"timeout"); ok = false; break; }
              const size_t offset = chunk*kCardChunk, size = std::min(kCardChunk,c.card_bytes.size()-offset);
              if (!i.send_room({{"t","msg"},{"to",peer["connectionId"]},{"d",{{"kind","mu-card"},{"v",1},
                  {"match",c.session_id},{"i",chunk},{"n",n},{"data",card_encode(c.card_bytes.data()+offset,size)}}}})) { ok = false; break; }
              // <=84 chunks/s, below Rooms' 120/s relay budget; no datagram helper yet.
              const auto paced_until = std::min(until_card,i.clock()+12);
              while (i.clock() < paced_until) {
                if (i.cancelled()) return fail("cancelled");
                json m;
                if (i.next_room(m,int(paced_until-i.clock()))) control(m);
                if (i.room_closed() || link->lost()) { ok = false; break; }
              }
            }
          }
          if (ok && (i.clock() >= until_card || (i.flush_room && !i.flush_room(int(until_card-i.clock()))))) {
            card_log(n,i.clock() >= until_card ? "timeout" : "mismatch"); ok = false;
          }
          if (ok) i.log("console: card sent " + std::to_string(n) + " chunks sha " + sha8);
          if (ok) ok = persist();
          const size_t guests = link->frozen.size()-1;
          auto failed_verdict = [&] {
            return std::any_of(verdicts.begin(),verdicts.end(),[](const auto& v) { return !v.second; });
          };
          while (verdicts.size() < guests && !failed_verdict() && i.clock() < until_verdict) {
            if (i.cancelled()) return fail("cancelled");
            if (i.room_closed() || link->lost()) { ok = false; break; }
            json m;
            if (i.next_room(m,int(std::min<uint64_t>(50,until_verdict-i.clock())))) control(m);
          }
          if (i.cancelled()) return fail("cancelled");
          if (i.room_closed() || link->lost()) ok = false;
          const auto good = std::count_if(verdicts.begin(),verdicts.end(),[](const auto& v) { return v.second; });
          final = ok && size_t(good) == guests ? "host" : "default";
          i.log("console: card verdicts " + std::to_string(good) + "/" + std::to_string(guests) + " ok; using " + final);
          for (int copy = 0; copy < 2; ++copy) {
            for (const auto& peer : link->frozen) if (str(peer,"connectionId") != r.me) {
              json d = {{"kind","mu-card-final"},{"v",1},{"match",c.session_id},{"use",final}};
              if (hj && i.handoff_join) d["handoffJoin"] = 1;
              if (!i.send_room({{"t","msg"},{"to",peer["connectionId"]},{"d",d}})) final = "default";
            }
            if (i.clock() >= until_final || (i.flush_room && !i.flush_room(int(until_final-i.clock()))) ||
                i.clock() >= until_final || i.room_closed() || link->lost()) { final = "default"; break; }
            if (copy == 0) {
              const auto repeat_at = i.clock()+1000;
              while (i.clock() < repeat_at) {
                if (i.cancelled()) return fail("cancelled");
                if (i.clock() >= until_final || i.room_closed() || link->lost()) { final = "default"; break; }
                json m;
                if (i.next_room(m,int(std::min<uint64_t>(50,repeat_at-i.clock())))) control(m);
              }
            }
          }
          // Best effort downgrade; if delivery failed, the relaunch digest rendezvous below
          // compares this default choice with the peer's staged host choice and vetoes both.
          if (final == "default") for (const auto& peer : link->frozen) if (str(peer,"connectionId") != r.me)
            i.send_room({{"t","msg"},{"to",peer["connectionId"]},{"d",{{"kind","mu-card-final"},
                {"v",1},{"match",c.session_id},{"use","default"}}}});
        } else {
          std::vector<Bytes> chunks; size_t total = 0, received = 0;
          while (chunks.empty() || received < chunks.size()) {
            if (i.cancelled()) return fail("cancelled");
            if (i.room_closed() || link->lost()) { ok = false; break; }
            if (i.clock() >= until_card) { card_log(received,"timeout"); ok = false; break; }
            json m;
            if (!i.next_room(m,int(std::min<uint64_t>(50,until_card-i.clock())))) continue;
            control(m);
            if (final == "default") { ok = false; break; }
            if (str(m,"t") != "msg" || str(m,"from") != host || !m.count("d") || !m["d"].is_object()) continue;
            const auto& d = m["d"];
            if (str(d,"kind") != "mu-card" || str(d,"match") != c.session_id) continue;
            if (d.value("v",json()) != 1 || !number(d,"n",1,(kCardLimit+kCardChunk-1)/kCardChunk) ||
                !number(d,"i",0,d["n"].get<size_t>()-1)) { ok = false; break; }
            const size_t n = d["n"].get<size_t>(), index = d["i"].get<size_t>();
            if (chunks.empty()) chunks.resize(n);
            if (chunks.size() != n || !d.count("data") || !d["data"].is_string()) { ok = false; break; }
            Bytes bytes;
            if (!card_decode(d["data"].get<std::string>(),bytes) || bytes.empty()) { ok = false; break; }
            if (!chunks[index].empty()) { if (chunks[index] != bytes) { ok = false; break; } continue; }
            total += bytes.size();
            if (total > kCardLimit) { ok = false; break; }
            chunks[index] = std::move(bytes); ++received;
          }
          if (ok) {
            for (const auto& bytes : chunks) c.card_bytes.insert(c.card_bytes.end(),bytes.begin(),bytes.end());
            ok = card_bytes_ok(c);
            card_log(chunks.size(),ok ? "ok" : "mismatch");
            if (ok) ok = persist();
          }
          if (i.cancelled()) return fail("cancelled");
          if (i.room_closed() || link->lost()) ok = false;
          if (!i.send_room({{"t","msg"},{"to",host},{"d",{{"kind","mu-card-verdict"},
              {"v",1},{"match",c.session_id},{"ok",ok}}}})) ok = false;
          i.log(std::string("console: card verdict ") + (ok ? "ok" : "fail") + " sent");
          // Keep reading through the host's one-second repeat before handing off. The first
          // final decides; an identical repeat is harmless and a conflicting one selects defaults.
          while (!final_conflict && i.clock() < until_final && (final.empty() || i.clock() < final_at+1500)) {
            if (i.cancelled()) return fail("cancelled");
            if (i.room_closed() || link->lost()) { ok = false; break; }
            json m;
            if (i.next_room(m,int(std::min<uint64_t>(50,until_final-i.clock())))) control(m);
          }
          // Persistence can outlast the repeat window when a final arrived during chunks.
          // Drain already queued controls before handoff so that cannot hide a conflicting repeat.
          json queued;
          // A busy/malicious room must not keep START in a zero-wait drain forever.
          int drained = 0;
          while (drained < 256 && i.clock() < until_final && i.next_room(queued,0)) { control(queued); ++drained; }
          if (drained == 256 || i.clock() >= until_final) ok = false;
          if (i.cancelled()) return fail("cancelled");
          if (final_conflict || final.empty() || !ok || i.room_closed() || link->lost()) final = "default";
          // A missing/conflicting final or failed verdict is a local default vote. Even when
          // the host already handed off, both launches exchange their original chosen digest
          // before boot: unequal digests mean default on BOTH, including a seat with the final.
        }
        i.log("console: card final " + final);
        if (final == "default") {
          // Cleanup is best effort, within the final deadline. Its worker owns the card
          // and callback: a stalled filesystem cannot hold START or access this session later.
          if (i.discard_card && !card_io_until(i.clock,i.cancelled,
              std::min(until_final,i.clock()+1000),false,
              [discard = i.discard_card, card = c] { discard(card); return true; }))
            i.log("console: couldn't remove staged save; default launch ignores it");
          c.default_card = true; c.card_bytes.clear(); c.card_digest.clear();
          i.log(std::string("console: ") + kDefaultCard);
        }
      }
    }
    link->config = c;
    if (i.friends_lobby && i.prepared && i.handoff_join && players != 2)
      i.log("handoff-join: refuse requires-one-guest");
    // Finish card choice first: negotiation must never consume its deadlines or change its result.
    // No game body is paused here. Only accepted live preparations proceed through the existing mesh setup.
    if (hj && (i.handoff_join || hj->engaged())) {
      try {
        if (hj_host && i.handoff_join) hj->begin(i.clock(),i.handoff_css(),!c.card_digest.empty());
        if (!hj_host && i.handoff_join && !hj->engaged()) hj->refuse("peer-not-offering");
        while (hj->waiting()) {
          hj->tick(i.clock());
          if (!hj->waiting()) break;
          if (i.cancelled()) { hj->refuse("cancelled"); break; }
          if (i.room_closed() || link->lost()) { hj->refuse("room-closed"); break; }
          json m;
          if (i.next_room(m,int(std::min<uint64_t>(50,hj->remaining(i.clock()))))) {
            link->room_message(m); hj->receive(m,i.clock());
          }
        }
        if (hj->engaged() && i.flush_room && !i.room_closed()) i.flush_room(1000);
      } catch (...) { hj->refuse("protocol-error"); }
    }
    if (hj && hj->accepted() && (c.default_card || !card_bytes_ok(c))) hj->refuse("shared-card-required");
    if (hj && hj->accepted() && i.handoff_begin) {
      live = i.handoff_begin();
      if (live) {
        fallback_config = c; link->handoff_preparation = true;
        if (!hj_host) {
          auto q = std::make_shared<handoff_join::Request>(); q->serial = 1;
          q->operation = handoff_join::Operation::GuestCSS; q->seed = c.seed;
          std::shared_ptr<const handoff_join::Request> immutable = std::move(q);
          std::atomic_store_explicit(&live->request,std::move(immutable),std::memory_order_release);
        }
      }
    }
    if (i.prepared && !live) {
      if (hj && hj->accepted()) hj->refuse("start-at-N-unavailable");
      i.prepared(c); return nullptr;
    }
    i.log("console: start session " + c.session_id + " seed " + hex(c.seed) + " delay " + std::to_string(c.input_delay));
    // A relaunching seat can be absent for ~60 s. Rendezvous through Rooms before
    // starting the shared 20 s mesh budget; never send another begin for this session.
    std::set<std::string> candidates; uint64_t candidate_at = 0;
    const auto restored_digest = c.card_digest;
    bool default_vote = c.default_card;
    // Rendezvous counters for the failure log: room ids only (8 chars), never tokens.
    int cand_sent = 0, room_in = 0, cand_unknown = 0; std::string unknown_from;
    auto id8 = [](const std::string& id) { return id.substr(0,8); };
    auto rendezvous_log = [&](const char* how) {
      std::string expects;
      for (const auto& p : link->frozen) if (str(p,"connectionId") != r.me) expects += (expects.empty() ? "" : ",") + id8(str(p,"connectionId"));
      i.log(std::string("console: rendezvous ") + how + ": me " + id8(r.me) + " expects " + expects + ", sent " +
          std::to_string(cand_sent) + ", room messages in " + std::to_string(room_in) + ", candidates " +
          std::to_string(candidates.size()) + ", from unknown seats " + std::to_string(cand_unknown) +
          (unknown_from.empty() ? "" : " (" + unknown_from + ")"));
    };
    auto candidate = [&](const json& m) {
      if (str(m,"t") != "msg" || !m.count("d") || !m["d"].is_object()) return true;
      const auto& d = m["d"];
      if (str(d,"kind") != "mu-cand" || d.value("v",json()) != 2 || str(d,"match") != c.session_id) return true;
      if (str(m,"from") == r.me) return true;
      if (std::none_of(link->frozen.begin(),link->frozen.end(),[&](const json& p) { return str(p,"connectionId") == str(m,"from"); })) {
        ++cand_unknown; unknown_from = id8(str(m,"from")); return true;
      }
      if (str(d,"compat") != i.compatibility) return false;
      if ((sync_card || rejoin) && str(d,"cardDigest") != restored_digest) default_vote = true;
      candidates.insert(str(m,"from")); return true;
    };
    auto send_candidates = [&] {
      if (i.clock() < candidate_at) return;
      for (const auto& p : link->frozen) if (str(p,"connectionId") != r.me) {
        auto d = transport::candidate(true,i.compatibility,"",c.session_id,"","","");
        if (sync_card || rejoin) d["cardDigest"] = restored_digest;
        i.send_room({{"t","msg"},{"to",p["connectionId"]},{"d",d}}); ++cand_sent;
      }
      candidate_at = i.clock()+1000;
    };
    step = "rendezvous";
    if (rejoin || sync_card) {
      const auto rendezvous_until = i.clock()+65000;
      while (i.clock() < rendezvous_until && candidates.size()+1 < c.ports.size()) {
        if (i.cancelled()) return fail("cancelled");
        send_candidates(); json m;
        if (i.next_room(m,50)) {
          ++room_in;
          link->room_message(m);
          if (!candidate(m)) { rendezvous_log("mixed builds"); return fail(kMixedBuilds); }
        }
        if (link->lost()) { rendezvous_log("lost"); return fail(link->lost()->c_str()); }
        if (i.room_closed()) { rendezvous_log("room closed"); return fail("couldn't connect to your friend"); }
      }
      if (candidates.size()+1 < c.ports.size()) { rendezvous_log("timed out"); return fail("couldn't connect to your friend"); }
      rendezvous_log("done");
      if (default_vote) {
        c.default_card = true; c.card_digest.clear(); c.card_bytes.clear();
        i.log(std::string("console: ") + kDefaultCard);
      }
      link->config = c;
    }
    const auto link_at = i.clock(), until = link_at+20000;
    auto budget = [&] { return int(until > i.clock() ? until-i.clock() : 0); };
    // The app refuses an open as not_peer while the peer's seat is briefly absent (mid-relaunch, or its
    // join not seen yet). Keep the room pumping and try again about once a second while time remains.
    auto open_link = [&](const std::string& peer) {
      json reply = link->open_peer(peer,budget());
      while (!reply.value("link",0u) && str(reply,"refused") == "not_peer" && budget() > 1500 && !i.cancelled()) {
        const auto retry_at = i.clock()+1000;
        while (i.clock() < retry_at) {
          json m;
          if (i.next_room(m,int(retry_at-i.clock()))) { link->room_message(m); candidate(m); }
          if (i.room_closed()) return reply;
        }
        reply = link->open_peer(peer,budget());
      }
      return reply;
    };
    // The app may take seconds to map the helper's ring (Friends waits 10 s); every step logs why it stopped.
    step = "attach";
    if (!i.attach(std::min(10000,budget()))) { i.log("console: link helper attach failed"); return fail("couldn't connect to your friend"); }
    step = "open";
    for (const auto& p : link->frozen) {
      const auto conn = str(p,"connectionId"); if (conn == r.me) continue;
      if (!budget()) { i.log("console: no time left to open a link"); return fail("couldn't connect to your friend"); }
      const auto opened = i.clock();
      const auto reply = open_link(conn);
      const unsigned id = reply.value("link",0u);
      if (!id || id > 65534 || std::any_of(link->peers.begin(),link->peers.end(),[&](const Peer& q) { return q.link == id; })) {
        const auto code = str(reply,"refused");
        i.log("console: link open to port " + std::to_string(p["port"].get<int>()) + " refused (" + (code.empty() ? "link " + std::to_string(id) : code) + ")");
        return fail("couldn't connect to your friend");
      }
      Peer peer{}; peer.port = p["port"].get<int>(); peer.link = id; peer.connection = conn; peer.heard = i.clock(); peer.opened = opened;
      peer.room_connection = i.connection(); link->peers.push_back(std::move(peer));
      link->startup_work();
    }
    step = "links";
    while (budget()) {
      if (i.cancelled()) return fail("cancelled");
      link->startup_work();
      send_candidates();
      json m;
      if (i.next_room(m,std::min(10,budget()))) {
        link->room_message(m);
        if (!candidate(m)) return fail(kMixedBuilds);
      }
      if (link->lost()) return fail(link->lost()->c_str());
      if (i.room_closed()) { i.log("console: room closed while links were opening"); return fail("couldn't connect to your friend"); }
      bool up = candidates.size() == link->peers.size();
      for (auto& p : link->peers) {
        if (i.state(p.link) == 3) {
          i.log("console: link to port " + std::to_string(p.port) + " failed after " + std::to_string(i.clock()-p.opened) + " ms");
          // The app closes an open whose peer never offered within ~10 s (peer_silent): a seat that attached
          // slowly can miss a fast seat's window. Open again (five times at most) while time remains.
          if (p.reopened < 5 && budget() > 3000) {
            const auto reply = open_link(p.connection);
            const unsigned id = reply.value("link",0u);
            if (id && id <= 65534) {
              ++p.reopened; p.link = id; p.opened = i.clock(); p.heard = i.clock();
              i.log("console: link to port " + std::to_string(p.port) + " opened again (" + std::to_string(p.reopened) + ")");
              up = false; continue;
            }
            const auto code = str(reply,"refused");
            i.log("console: link reopen to port " + std::to_string(p.port) + " refused (" + (code.empty() ? "link " + std::to_string(id) : code) + ")");
          }
          return fail("couldn't connect to your friend");
        }
        const auto route = i.route(p.link);
        if (i.state(p.link) != 2) up = false;
        else if (!p.up_logged) {
          i.log("console: link to port " + std::to_string(p.port) + " up via " + (route.empty() ? "unknown" : route) + " in " + std::to_string(i.clock()-p.opened) + " ms");
          p.up_logged = true; p.path = route;
        }
      }
      if (up) {
        for (auto& p : link->peers) p.heard = i.clock();
        step = "delay";
        if (!link->agree_delay()) {
          if (link->lost()) return fail(link->lost()->c_str());
          return fail(i.cancelled() ? "cancelled" : "couldn't agree on a delay");
        }
        if (live) {
          handoff_join::transfer(i,hj_host,link->peers.front().connection,link->config,live);
          return relaunch();
        }
        config = link->config; link->service(); return link;
      }
    }
    {
      std::string states;
      for (const auto& p : link->peers) states += " port " + std::to_string(p.port) + " state " + std::to_string(i.state(p.link));
      i.log("console: links not up in 20 s (candidates " + std::to_string(candidates.size()) + " of " + std::to_string(link->peers.size()) + ";" + states + ")");
    }
    return fail("couldn't connect to your friend");
  } catch (const update::Required&) {   // this build is no longer the listing's current one: say so, as Unranked does
    i.log(std::string("console: setup threw at ") + step + " (update_required)");
    i.log("console: update required"); return fail(update::kMessage);
  } catch (const json::exception&) {
    i.log(std::string("console: setup threw at ") + step + " (json)"); return fail(std::string(step).find("card") == 0 ? kCardFailure : "couldn't connect to your friend");
  } catch (const std::runtime_error&) {
    i.log(std::string("console: setup threw at ") + step + " (runtime_error)"); return fail(std::string(step).find("card") == 0 ? kCardFailure : "couldn't connect to your friend");
  } catch (const std::exception&) {
    i.log(std::string("console: setup threw at ") + step + " (exception)"); return fail(std::string(step).find("card") == 0 ? kCardFailure : "couldn't connect to your friend");
  } catch (...) {   // never print bridge exception text or tickets: the stage and category only
    i.log(std::string("console: setup threw at ") + step + " (other)"); return fail(std::string(step).find("card") == 0 ? kCardFailure : "couldn't connect to your friend");
  }
}
} // namespace yougame::console
