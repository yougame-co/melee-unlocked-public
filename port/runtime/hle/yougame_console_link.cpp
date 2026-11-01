// Console lockstep transport; no simulation, native sockets, or Slippi protocol.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_console_link.h"
#include <array>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <atomic>
#include <mutex>
#include <thread>

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
struct Peer {
  int port; unsigned link; std::string connection;
  uint64_t heard = 0, sent = 0, resent = 0, late = 0, rtt = 0, opened = 0, retry_at = 0;
  uint32_t ack = kNone, digest_ack = kNone, next = 0, next_digest = 0;
  bool bad_version = false, dirty = true, up_logged = false, sampled = false;
  int reopened = 0;
  std::string path;
  uint64_t worst = 0;
  std::set<uint64_t> pings;
  PadRing pads;
  std::map<uint32_t, uint64_t> digests;
  std::map<uint32_t, Digest> digest_order;
  std::map<uint32_t, Pad> outgoing;
};
class Link final : public SessionLink {
 public:
  SessionIO io;
  SessionConfig config;
  json frozen;
  std::vector<Peer> peers;
  bool claimed = false;
  int host_port = 1;
  uint64_t worst_rtt = 0, choice_until = 0, control_at = 0;
  int chosen_delay = 0;
  bool measuring = false, committed = false;
  std::map<int,uint64_t> reports;
  std::set<int> delay_acks;
  explicit Link(SessionIO i) : io(std::move(i)) { frame_zero_barrier = true; }
  void service() {
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
    if (!peers.empty() && !reason_) end("your friend disconnected");
    for (const auto& p : peers) { try { io.request({{"type","directClose"},{"link",p.link}}, 1000); } catch (...) {} }
    if (io.close) io.close();
    if (claimed) requests.release();
  }
  void end(const std::string& why, bool announce = true) {
    if (reason_) return;
    reason_ = why; io.log("console: session ended: " + why);
    if (announce) for (const auto& p : peers)
      io.send_room({{"t","msg"},{"to",p.connection},{"d",{{"kind","mu-console-end"},{"v",1},{"sessionId",config.session_id},{"reason",why}}}});
  }
  void room_message(const json& m) {
    const auto t = str(m,"t");
    if (t == "sessionEnded") {
      if (str(m,"sessionId") == config.session_id) {
        const auto why = friends::text(m,"reason",160);
        end(why.empty() ? "your friend disconnected" : why);
      }
      return;
    }
    // Lobby presence can change during grace/reconnect or next-session admission.
    // Only sessionEnded interrupts the frozen roster; never replace its identities.
    if (t == "msg" && m.count("d") && m["d"].is_object()) {
      const auto& d = m["d"];
      if (std::none_of(frozen.begin(), frozen.end(), [&](const json& p) { return str(p,"connectionId") == str(m,"from"); })) return;
      if (str(d,"kind") == "mu-console-end" && d.value("v",json()) == 1 && str(d,"sessionId") == config.session_id) {
        const auto why = str(d,"reason");
        if (!friends::text(d,"reason",160).empty()) end(why);
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
      if (io.room_closed()) end("your friend disconnected");
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
      if (!armed_ && ready_since_ && now - *ready_since_ >= 60000) end(kBootTimeout);
      for (const auto& p : peers) if (io.state(p.link) == 3 || (armed_ && now - p.heard >= 5000)) end("your friend disconnected");
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
  std::optional<uint64_t> ready_since_;
  std::optional<std::string> reason_;
  uint32_t newest_ = kNone, digest_serial_ = 0;
  std::map<uint32_t, Digest> out_digests_;
  Bytes header(const Peer& p) const {
    Bytes b; put(b,kWireVersion,1); put(b,config.my_port,1); put(b,0,1); put(b,0,1);
    put(b,tag(config.session_id),8); put(b,p.next ? p.next - 1 : kNone,4); put(b,p.next_digest ? p.next_digest - 1 : kNone,4);
    return b;
  }
  void transmit() {
    if (reason_) return;
    const auto now = io.clock();
    for (auto& p : peers) {
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
      if (!p.pads.find(f)) { if (f < p.next) continue; if (f != p.next) ++p.late; p.pads.insert(f,value); }
      while (p.pads.find(p.next)) ++p.next;
    }
    for (int n = 0; n < b[3]; ++n) {
      const uint32_t serial = uint32_t(get(b,at,4)), frame = uint32_t(get(b,at,4)); const auto value = get(b,at,8);
      if (serial < p.next_digest || uint64_t(serial) >= uint64_t(p.next_digest) + kHistory) continue;
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
  auto fail = [&](const char* why) -> std::unique_ptr<SessionLink> { error = why; i.log("console: start failed: " + error); return nullptr; };
  json request;
  if (!parse_request(input,request)) return fail("invalid console session request");
  if (i.exclusive && !requests.claim()) return fail("a console session is already running");
  link->claimed = i.exclusive;
  try {
    const int players = request["players"].get<int>();
    const bool host_new = request["room"].get<std::string>() == "new";
    const auto code = host_new ? new_room_code(i.random ? i.random() : i.clock()) : request["room"].get<std::string>();
    const json ticket_request = {{"type","mpTicket"},{"room",code},{"players",players},{"mode","console"},{"ranked",false},
      {"lobby",{{"version",1},{"minPlayers",players},{"maxLocalPlayers",1},{"compatibility",i.compatibility}}}};
    const auto ticket = i.request(ticket_request,10000);
    const auto room = friends::room(ticket);
    if (room.empty() || code != room) {
      i.log("console: ticket for room " + code + " came back " + (room.empty() ? std::string("without a room") : "for room " + room));
      return fail("couldn't open your friend's room");
    }
    if (!i.open_room(str(ticket,"ticket"),room,10000)) { i.log("console: room " + room + " socket didn't open"); return fail("couldn't open your friend's room"); }
#ifdef YOUGAME_AGENT_TEST_HOOK
    if (host_new) i.log("agent: console host room " + room);
#endif
    Roster r; r.size = players; json start;
    bool seat_logged = false;
    uint64_t next_begin = 0, ping = i.clock();
    // A friend has to accept the invite, launch and boot (quality's run: the joiner opened the room 114 s after the
    // host, after the old 30 s wait had closed it). A human host can cancel from the waiting panel sooner.
    const uint64_t seats_until = i.clock() + 300000;
    const bool rejoin = request.count("sessionId");
    const auto delay = request.value("inputDelay",3);
    while (i.clock() < seats_until) {
      if (i.cancelled()) return fail("cancelled");
      json m;
      if (i.next_room(m,int(std::min<uint64_t>(50,seats_until-i.clock())))) {
        const auto t = str(m,"t");
        if (t == "welcome") r.me = str(m,"me");
        transport::read_roster(r,m);
        if (t == "command-error" && str(m,"action") == "begin" && str(m,"code") == "console_mixed_builds") {
          i.log("console: command-error console_mixed_builds"); return fail(kMixedBuilds);
        }
        if (t == "error" || t == "ended" || t == "kicked" || t == "command-error") return fail("couldn't start your friend's room");
        if (t == "sessionEnded" && (!rejoin || str(m,"sessionId") == str(request,"sessionId"))) {
          const auto why = friends::text(m,"reason",160);
          i.log("console: session ended: " + why);
          return fail(why.empty() ? "your friend disconnected" : why.c_str());
        }
        if (!m.count("console") && m.count("room") && m["room"].is_object() && m["room"].count("console")) m["console"] = m["room"]["console"];
        if (m.count("console") && m["console"].is_object()) { start = m; break; }
      }
      if (i.room_closed()) { i.log("console: room " + room + " closed while waiting for seats"); return fail("couldn't open your friend's room"); }
      const auto seat = transport::connection_seat(r.participants,r.me);
      if (!seat_logged && seat >= 0) { i.log("console: room " + room + " seat " + std::to_string(seat) + " of " + std::to_string(players)); seat_logged = true; }
      const bool full = valid_roster(r.participants,players,true,true), host = r.me == r.host_connection;
      if (i.waiting) i.waiting(full ? (host ? "Press START to play together" : "Waiting for the host") : friends::kWaiting, full && host);
      if (!rejoin && full && host && i.confirm_start() && i.clock() >= next_begin) {
        if (!i.send_room({{"t","begin"},{"revision",r.revision},{"console",true},{"inputDelay",delay},{"settingsDigest",""}})) return fail("couldn't start your friend's room");
        next_begin = i.clock() + 2000;
      }
      if (i.clock() - ping >= 5000) { i.send_room({{"t","ping"},{"at",i.clock()}}); ping = i.clock(); }
    }
    if (start.is_null()) return fail("your friend didn't join");
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
    for (const auto& p : s["participants"]) c.ports.push_back(p["port"].get<int>());
    std::sort(c.ports.begin(),c.ports.end());
    for (auto key : {"sessionId","seed","epochMs","inputDelay","port"}) {
      if (!request.count(key)) continue;
      const json value = std::string(key) == "port" ? json(c.my_port) : std::string(key) == "seed" ? json(c.seed) :
        std::string(key) == "epochMs" ? json(c.epoch_ms) : s[key];
      if (request[key] != value) return fail("console session start changed");
    }
    link->config = c; link->frozen = seats(s["participants"],true);
    link->host_port = connection_port(link->frozen,r.host_connection);
    if (link->host_port < 1) return fail("invalid console session start");
    if (rejoin) i.log("console: rejoined session " + c.session_id);
    if (i.cancelled()) return fail("cancelled");
    if (i.prepared) { i.prepared(c); return nullptr; }
    i.log("console: start session " + c.session_id + " seed " + hex(c.seed) + " delay " + std::to_string(c.input_delay));
    // A relaunching seat can be absent for ~60 s. Rendezvous through Rooms before
    // starting the shared 20 s mesh budget; never send another begin for this session.
    std::set<std::string> candidates; uint64_t candidate_at = 0;
    auto candidate = [&](const json& m) {
      if (str(m,"t") != "msg" || !m.count("d") || !m["d"].is_object()) return true;
      const auto& d = m["d"];
      if (str(d,"kind") != "mu-cand" || d.value("v",json()) != 2 || str(d,"match") != c.session_id) return true;
      if (str(m,"from") == r.me || std::none_of(link->frozen.begin(),link->frozen.end(),[&](const json& p) { return str(p,"connectionId") == str(m,"from"); })) return true;
      if (str(d,"compat") != i.compatibility) return false;
      candidates.insert(str(m,"from")); return true;
    };
    auto send_candidates = [&] {
      if (i.clock() < candidate_at) return;
      for (const auto& p : link->frozen) if (str(p,"connectionId") != r.me)
        i.send_room({{"t","msg"},{"to",p["connectionId"]},{"d",transport::candidate(true,i.compatibility,"",c.session_id,"","","")}});
      candidate_at = i.clock()+1000;
    };
    if (rejoin) {
      const auto rendezvous_until = i.clock()+65000;
      while (i.clock() < rendezvous_until && candidates.size()+1 < c.ports.size()) {
        if (i.cancelled()) return fail("cancelled");
        send_candidates(); json m;
        if (i.next_room(m,50)) {
          link->room_message(m);
          if (!candidate(m)) return fail(kMixedBuilds);
        }
        if (link->lost()) return fail(link->lost()->c_str());
        if (i.room_closed()) return fail("couldn't connect to your friend");
      }
      if (candidates.size()+1 < c.ports.size()) return fail("couldn't connect to your friend");
    }
    const auto link_at = i.clock(), until = link_at+20000;
    auto budget = [&] { return int(until > i.clock() ? until-i.clock() : 0); };
    // The app refuses an open as not_peer while the peer's seat is briefly absent (mid-relaunch, or its
    // join not seen yet). Keep the room pumping and try again about once a second while time remains.
    auto open_link = [&](const std::string& peer) {
      json reply = i.request({{"type","directOpen"},{"conn",i.connection()},{"peer",peer}},budget());
      while (!reply.value("link",0u) && str(reply,"refused") == "not_peer" && budget() > 1500 && !i.cancelled()) {
        const auto retry_at = i.clock()+1000;
        while (i.clock() < retry_at) {
          json m;
          if (i.next_room(m,int(retry_at-i.clock()))) { link->room_message(m); candidate(m); }
          if (i.room_closed()) return reply;
        }
        reply = i.request({{"type","directOpen"},{"conn",i.connection()},{"peer",peer}},budget());
      }
      return reply;
    };
    // The app may take seconds to map the helper's ring (Friends waits 10 s); every step logs why it stopped.
    if (!i.attach(std::min(10000,budget()))) { i.log("console: link helper attach failed"); return fail("couldn't connect to your friend"); }
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
      Peer peer{}; peer.port = p["port"].get<int>(); peer.link = id; peer.connection = conn; peer.heard = i.clock(); peer.opened = opened; link->peers.push_back(std::move(peer));
      link->startup_work();
    }
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
        if (!link->agree_delay()) {
          if (link->lost()) return fail(link->lost()->c_str());
          return fail(i.cancelled() ? "cancelled" : "couldn't agree on a delay");
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
  } catch (...) { i.log("console: link setup threw"); return fail("couldn't connect to your friend"); } // never print bridge exception text/tickets
}
} // namespace yougame::console
