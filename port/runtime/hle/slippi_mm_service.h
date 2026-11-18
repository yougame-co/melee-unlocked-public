// Slippi 3.7.0 matchmaking: the WebSocket queue service (wss://matchmaking.slippi.gg/v1/queue) that
// replaced the ENet matchmaking server, and the STUN encoding used to learn the netplay socket's
// public address. A port of Slippi's Rust matchmaking crate (client.rs, protocol.rs, stun.rs): the
// same messages, field names, timeouts, retries and error texts.
//
// The UDP socket that plays stays with the caller (slippi::Matchmaking): it sends the STUN requests
// built here from that socket, keeps its NAT mapping alive while queued, and connects to the
// opponents once a match arrives. A Session only talks to the service, on a worker thread of its
// own, and never blocks the caller on the network.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace slippi::mm_service {

// The public address one STUN server saw for the netplay socket. Leave it zeroed (answered false)
// for a server that did not answer.
struct StunObservation {
  bool answered = false;
  uint32_t ip_be = 0;   // IPv4 address in network byte order, as in in_addr.s_addr and ENetAddress.host
  uint16_t port = 0;    // host byte order
};

// STUN (RFC 5389) binding request and response: XOR-MAPPED-ADDRESS, with MAPPED-ADDRESS as the
// fallback. Send the request from the netplay socket and pass the reply back with that request.
constexpr size_t kStunRequestLen = 20;
void stun_build_request(uint8_t out[kStunRequestLen]);   // a fresh random transaction id each call
// False, leaving *out untouched, unless resp is a binding success reply to request with an IPv4
// mapped address.
bool stun_parse_response(const uint8_t request[kStunRequestLen], const uint8_t* resp, size_t len, StunObservation* out);
// "cone" when both servers answered with the same address, "symmetric" when both answered with
// different ones, else "unknown". These are the strings the service expects.
const char* classify_nat(const StunObservation& a, const StunObservation& b);

// Everything the search needs. Strings are copied.
struct Request {
  uint8_t mode = 0;                       // 0 ranked, 1 unranked, 2 direct, 3 teams, 4 party
  std::vector<uint8_t> connect_code;      // opponent or lobby code as raw Shift-JIS bytes
  uint16_t netplay_port = 0;              // local UDP port the netplay socket is bound to
  std::string lan_addr;                   // "ip:port" on the local network, or empty
  StunObservation stun[2];                // two servers' answers, in the order they were asked
  std::string uid, play_key, connect_code_self, display_name, app_version;   // app_version e.g. "3.7.0"
};

enum class State : uint8_t { Idle = 0, Connecting = 1, Queued = 2, Matched = 3, Failed = 4 };

namespace detail { struct SessionShared; }

// One search. The constructor starts it; destroying the Session (or cancel()) leaves the queue.
class Session {
 public:
  // url empty: SLIPPI_MM_URL from the environment when set (diagnostic sessions only, see
  // host/test_env.h), else the dev service for an app_version containing "dev", else
  // wss://matchmaking.slippi.gg/v1/queue.
  explicit Session(Request request, std::string url = {});
  // Cancels: the worker sends {"type":"cancel"} if it is connected and exits on its own. Never
  // waits on a connect or handshake in progress.
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  State state() const;
  // The whole "match" message as the service sent it, once (true the first time after Matched).
  bool take_result_json(std::string* out);
  std::string error_message() const;    // why, once Failed
  // latestVersion from an "error" message, or empty. When set, the service knows a newer Slippi
  // release than app_version: remember it in the user record as client.rs does.
  std::string latest_version() const;
  // Leaves the queue. Safe at any time from any thread; never blocks. The state turns Idle once
  // the worker notices (within a quarter second while queued).
  void cancel();

 private:
  std::shared_ptr<detail::SessionShared> shared_;
};

// ---- the wire format, public for the unit test (port/tests/slippi_mm_service_test.cpp)
struct Candidate {
  std::string ip;
  uint16_t port = 0;
  std::string type;   // "srflx" (a STUN answer) or "host" (the LAN address)
};
// protocol.rs Join, field for field.
struct Join {
  std::string uid, play_key, connect_code, display_name;   // "user"
  uint8_t mode = 0;                                        // "search"
  std::vector<uint8_t> search_connect_code;
  std::string app_version;
  uint16_t port = 0;           // the STUN-mapped port when known, else the local port
  std::string lan_addr;        // "ipAddressLan", always sent
  std::vector<Candidate> candidates;   // left out when empty
  std::string nat_type;        // left out when empty
};
// client.rs build_join: candidates srflx (first STUN answer) then host (lan_addr), port, natType.
Join build_join(const Request& request);
// Compact JSON with serde's field order and omission rules; search.connectCode is an array of numbers.
std::string join_json(const Join& join);
std::string resume_json(const std::string& ticket_id, const std::string& token);
std::string simple_json(const char* type);   // {"type":"cancel"}, {"type":"ping"}
std::string default_url(const std::string& app_version);

namespace detail {
// RFC 6455 Sec-WebSocket-Accept for a Sec-WebSocket-Key.
std::string websocket_accept_key(const std::string& key);
// Opens the WebSocket at url (IPv4, TLS for wss) and closes it again without sending a message:
// "" when that worked, else why not. A diagnostic for the transport only.
std::string probe(const std::string& url);
}  // namespace detail

}  // namespace slippi::mm_service
