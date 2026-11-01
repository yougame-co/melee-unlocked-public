// Console room/link adapter. All I/O is injectable; offline tests use the real room Stream.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_console.h"
#include "yougame_console_request.h"
#include "yougame_room.h"
#include "yougame_console_notice.h"

namespace yougame::console {
using Bytes = std::vector<uint8_t>;
constexpr size_t kHistory = 2048; // >34 seconds at 60 Hz, including reordering holes
constexpr uint8_t kWireVersion = 1;
struct DirectNotice {
  unsigned link = 0;
  int state = 0; // path updates don't change link liveness
  std::string path;
  std::optional<uint64_t> rtt;
};
inline DirectNotice direct_notice(const json& push) {
  DirectNotice n;
  if (!number(push,"link",1,65534)) return n;
  const auto event = friends::text(push,"event",16);
  if (event != "up" && event != "path" && event != "close") return n;
  n.link = push["link"].get<unsigned>();
  n.state = event == "up" ? 2 : event == "close" ? 3 : 0;
  if (event != "close") {
    const auto path = friends::text(push,"path",16);
    n.path = path == "direct" || path == "lan" || path == "relay" ? path : "unknown";
    if (number(push,"rttMs",0,60000)) n.rtt = push["rttMs"].get<uint64_t>();
  }
  return n;
}
struct SessionIO {
  transport::Request request;
  std::function<uint64_t()> clock = transport::now_ms;
  std::function<uint64_t()> random;
  std::function<bool(const std::string&, const std::string&, int)> open_room;
  std::function<bool(json&, int)> next_room;
  std::function<bool(const json&)> send_room;
  std::function<bool(int)> flush_room; // finish queued card sends before the app can relaunch
  std::function<bool()> room_closed;
  std::function<int()> connection;
  std::function<bool(int)> attach;
  std::function<int(unsigned)> state;
  std::function<std::string(unsigned)> route;
  std::function<std::vector<uint64_t>(unsigned)> rtt; // consume app path RTT samples
  std::function<bool()> cancelled = [] { return false; };
  std::function<bool()> confirm_start = [] { return true; };
  std::function<void(const std::string&,bool)> waiting;
  // Human entry freezes Rooms config, then relaunches; it never boots this running engine.
  std::function<void(const SessionConfig&)> prepared;
  // Present for Friends; four-seat callers retain their existing startup contract.
  std::function<Bytes()> host_save;
  std::function<bool(const SessionConfig&)> persist_card;
  std::function<bool(SessionConfig&)> restore_card;
  std::function<void(unsigned, const Bytes&)> send;
  std::function<bool(unsigned&, Bytes&)> receive;
  std::function<void(int)> wait;
  std::function<void()> close;
  std::function<void(const std::string&)> log;
  std::string compatibility;
  bool exclusive = false; // Production owns the one run-wide helper ring; fakes can model a mesh.
  bool background_service = false; // Service links through boot/loading, independent of bodies.
};
// Exact delay formula, shared with offline checks.
int measured_delay(uint64_t worst_rtt_ms, int minimum);
// Same implementation as the public entry, with a fake broker/link helper in tests.
// A host's own private room code: 12 of [a-z0-9] from a 64-bit random value.
std::string new_room_code(uint64_t random);
std::unique_ptr<SessionLink> start_session(const json&, SessionConfig&, std::string&, SessionIO);
} // namespace yougame::console
