// A stand-in for Slippi's matchmaking server, on loopback, for tests only. The game reaches it
// through MELEE_TEST_MM_HOST (hle/slippi_net.cpp) and nowhere else.
//
// It speaks what the game sends and reads: ENet on channel 0, one JSON object per reliable packet.
//   game -> server  {"type":"create-ticket","user":{uid,playKey,connectCode,displayName},
//                    "search":{"mode":2,"connectCode":[Shift-JIS bytes]},"appVersion","ipAddressLan"}
//   server -> game  {"type":"create-ticket-resp"} or with "error"
//   server -> game  {"type":"get-ticket-resp","matchId","isHost","players":[{uid,displayName,
//                    connectCode,port,isLocalPlayer,ipAddress,ipAddressLan}],"stages":[...]}
// Direct (mode 2): two tickets that name each other's codes are a match. Teams (mode 3): four tickets
// naming the same code (one of them its owner) are a match, in the order they came. Other modes are
// refused (tests never queue Unranked or Ranked). A ticket naming a code nobody holds waits until
// its client leaves, as the real server keeps searching.
//
// usage: port_mm_standin <port> [--log file] [--reject-malformed]
// SPDX-License-Identifier: GPL-2.0-or-later
#include <enet/enet.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

FILE* g_log = stdout;
void say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  std::fprintf(g_log, "[%lld] ", (long long)now);
  std::vfprintf(g_log, fmt, ap);
  std::fputc('\n', g_log);
  std::fflush(g_log);
  va_end(ap);
}

// The game sends the searched code as Shift-JIS: full-width letters, digits and '#'.
std::string ascii_code(const json& bytes) {
  std::vector<unsigned char> b;
  if (bytes.is_array()) for (const auto& v : bytes) if (v.is_number_integer()) b.push_back((unsigned char)v.get<int>());
  if (bytes.is_string()) for (char c : bytes.get<std::string>()) b.push_back((unsigned char)c);
  std::string out;
  for (size_t i = 0; i < b.size(); ++i) {
    const unsigned char c = b[i];
    if (c == 0) break;
    if (c == 0x81 && i + 1 < b.size() && b[i + 1] == 0x94) { out += '#'; ++i; continue; }
    if (c == 0x82 && i + 1 < b.size()) {
      const unsigned char d = b[i + 1];
      if (d >= 0x60 && d <= 0x79) { out += (char)('A' + (d - 0x60)); ++i; continue; }
      if (d >= 0x81 && d <= 0x9A) { out += (char)('A' + (d - 0x81)); ++i; continue; }
      if (d >= 0x4F && d <= 0x58) { out += (char)('0' + (d - 0x4F)); ++i; continue; }
    }
    out += (char)std::toupper(c);
  }
  return out;
}

bool well_formed(const std::string& code) {
  const size_t hash = code.find('#');
  if (hash == std::string::npos || hash == 0 || hash > 4 || hash + 1 >= code.size() || code.size() - hash - 1 > 3) return false;
  for (size_t i = 0; i < code.size(); ++i)
    if (i != hash && !std::isalnum((unsigned char)code[i])) return false;
  return true;
}

struct Ticket {
  ENetPeer* peer = nullptr;
  std::string uid, name, code, target, lan;
  int mode = 0;
  bool matched = false;
};

std::map<ENetPeer*, Ticket> g_tickets;
int g_match_count = 0;
bool g_reject_malformed = false;

std::string external(ENetPeer* p) {
  char ip[64];
  enet_address_get_host_ip(&p->address, ip, sizeof ip);
  return std::string(ip) + ":" + std::to_string(p->address.port);
}

void send(ENetPeer* p, const json& j) {
  const std::string s = j.dump();
  enet_peer_send(p, 0, enet_packet_create(s.c_str(), s.size(), ENET_PACKET_FLAG_RELIABLE));
}

void make_match(std::vector<Ticket*> group, const char* kind) {
  ++g_match_count;
  const std::string id = std::string("mode.") + kind + "-standin-" + std::to_string(g_match_count);
  for (size_t me = 0; me < group.size(); ++me) {
    json players = json::array();
    for (size_t i = 0; i < group.size(); ++i) {
      const Ticket& t = *group[i];
      const std::string ext = external(t.peer);
      players.push_back({{"uid", t.uid}, {"displayName", t.name}, {"connectCode", t.code}, {"port", (int)i + 1},
                         {"isLocalPlayer", i == me}, {"ipAddress", ext}, {"ipAddressLan", t.lan.empty() ? ext : t.lan}});
    }
    json resp = {{"type", "get-ticket-resp"}, {"matchId", id}, {"isHost", me == 0}, {"players", players}};
    if (group.size() == 2) resp["stages"] = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
    send(group[me]->peer, resp);
  }
  std::string names;
  for (Ticket* t : group) { t->matched = true; names += " " + t->code + "@" + external(t->peer); }
  say("match %s:%s", id.c_str(), names.c_str());
}

void try_match() {
  for (auto& [pa, a] : g_tickets) {
    if (a.matched || a.mode != 2) continue;
    for (auto& [pb, b] : g_tickets) {
      if (&a == &b || b.matched || b.mode != 2) continue;
      if (a.target == b.code && b.target == a.code) { make_match({&a, &b}, "direct"); break; }
    }
  }
  std::map<std::string, std::vector<Ticket*>> rooms;
  for (auto& [p, t] : g_tickets) if (!t.matched && t.mode == 3) rooms[t.target].push_back(&t);
  for (auto& [code, group] : rooms) {
    if (group.size() < 4) continue;
    bool owner = false;
    for (Ticket* t : group) owner |= t->code == code;
    if (!owner) continue;
    group.resize(4);
    make_match(group, "teams");
  }
}

void on_message(ENetPeer* peer, const std::string& text) {
  const json j = json::parse(text, nullptr, false);
  if (!j.is_object()) { say("bad json from %s", external(peer).c_str()); return; }
  const std::string type = j.value("type", "");
  if (type != "create-ticket") { say("unknown message %s from %s", type.c_str(), external(peer).c_str()); return; }
  Ticket t;
  t.peer = peer;
  const json user = j.value("user", json::object());
  t.uid = user.value("uid", ""); t.name = user.value("displayName", ""); t.code = user.value("connectCode", "");
  const json search = j.value("search", json::object());
  t.mode = search.value("mode", -1);
  t.target = ascii_code(search.find("connectCode") != search.end() ? search["connectCode"] : json());
  t.lan = j.value("ipAddressLan", "");
  say("ticket %s (%s) mode %d searching '%s' from %s lan %s", t.code.c_str(), t.uid.c_str(), t.mode, t.target.c_str(),
      external(peer).c_str(), t.lan.c_str());
  std::string error;
  if (t.uid.empty() || t.code.empty()) error = "Invalid user";
  else if (t.mode != 2 && t.mode != 3) error = "Mode not available on the test server";
  else if (g_reject_malformed && !well_formed(t.target)) error = "Invalid connect code";
  json resp = {{"type", "create-ticket-resp"}};
  if (!error.empty()) resp["error"] = error;
  send(peer, resp);
  if (!error.empty()) { say("refused %s: %s", t.code.c_str(), error.c_str()); return; }
  g_tickets[peer] = t;
  try_match();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: port_mm_standin <port> [--log file] [--reject-malformed]\n"); return 2; }
  const int port = std::atoi(argv[1]);
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--log" && i + 1 < argc) { FILE* f = std::fopen(argv[++i], "w"); if (f) g_log = f; }
    else if (a == "--reject-malformed") g_reject_malformed = true;
  }
  if (enet_initialize() != 0) { std::fprintf(stderr, "enet_initialize failed\n"); return 1; }
  ENetAddress addr;
  enet_address_set_host(&addr, "127.0.0.1");
  addr.port = (enet_uint16)port;
  ENetHost* host = enet_host_create(&addr, 64, 3, 0, 0);
  if (!host) { std::fprintf(stderr, "cannot listen on 127.0.0.1:%d\n", port); return 1; }
  say("matchmaking stand-in listening on 127.0.0.1:%d", port);
  for (;;) {
    ENetEvent ev;
    while (enet_host_service(host, &ev, 50) > 0) {
      if (ev.type == ENET_EVENT_TYPE_CONNECT) say("connect %s", external(ev.peer).c_str());
      else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        on_message(ev.peer, std::string((const char*)ev.packet->data, ev.packet->dataLength));
        enet_packet_destroy(ev.packet);
      } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
        auto it = g_tickets.find(ev.peer);
        if (it != g_tickets.end()) {
          say("disconnect %s (%s)%s", external(ev.peer).c_str(), it->second.code.c_str(), it->second.matched ? "" : ", ticket dropped unmatched");
          g_tickets.erase(it);
        } else {
          say("disconnect %s", external(ev.peer).c_str());
        }
      }
    }
  }
}
