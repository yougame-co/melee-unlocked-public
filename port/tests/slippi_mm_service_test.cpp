// Slippi's matchmaking service client (hle/slippi_mm_service.cpp): the join message exactly as the
// service's own client writes it (the protocol.rs tests), STUN requests and replies and the NAT
// classification (the stun.rs tests), and whole sessions against a fake service on loopback:
// matched, error with latestVersion, cancel, resume after a drop, ping after silence, join timeout,
// lost before a ticket, nothing listening.
//
//   port_slippi_mm_service_test               the checks
//   port_slippi_mm_service_test --probe URL   opens a WebSocket at URL and closes it, sending no
//                                             message (checks TCP, TLS and the upgrade, e.g. against
//                                             wss://matchmaking.slippi.gg/v1/queue)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_mm_service.h"
#include <nlohmann/json.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// slippi_mm_service.cpp logs through the host; this test has none, so the lines go to stdout.
namespace host {
void log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::putchar('\n');
  std::fflush(stdout);
}
}  // namespace host

namespace {
using namespace slippi::mm_service;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::atomic<int> g_failures{0};
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// ------------------------------------------------------------------ protocol.rs
void test_join_serializes_with_service_field_names() {
  Join join;
  join.uid = "u";
  join.play_key = "k";
  join.connect_code = "ME#1";
  join.display_name = "Me";
  join.mode = 2;
  join.search_connect_code = {0x82, 0x60};
  join.app_version = "3.4.0";
  join.port = 41000;
  join.lan_addr = "192.168.1.5:41000";
  join.candidates = {Candidate{"1.2.3.4", 41000, "srflx"}};
  join.nat_type = "cone";
  const std::string text = join_json(join);
  // serde_json's compact output, fields in declaration order.
  CHECK(text == "{\"type\":\"join\",\"user\":{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"ME#1\",\"displayName\":\"Me\"},"
                "\"search\":{\"mode\":2,\"connectCode\":[130,96]},\"appVersion\":\"3.4.0\",\"port\":41000,"
                "\"ipAddressLan\":\"192.168.1.5:41000\",\"candidates\":[{\"ip\":\"1.2.3.4\",\"port\":41000,\"type\":\"srflx\"}],"
                "\"natType\":\"cone\"}");
  json value = json::parse(text, nullptr, false);
  CHECK(value.is_object());
  CHECK(value["type"] == "join");
  CHECK(value["user"]["playKey"] == "k");
  CHECK(value["search"]["connectCode"] == json::array({130, 96}));
  CHECK(value["ipAddressLan"] == "192.168.1.5:41000");
  CHECK(value["candidates"][0]["type"] == "srflx");
  CHECK(value["natType"] == "cone");
  CHECK(value.find("debug") == value.end());
}

void test_empty_optional_fields_are_omitted() {
  Join join;
  join.uid = "u";
  join.play_key = "k";
  join.connect_code = "ME#1";
  join.display_name = "Me";
  join.mode = 1;
  join.app_version = "3.4.0";
  join.port = 41000;
  const std::string text = join_json(join);
  CHECK(text.find("candidates") == std::string::npos);
  CHECK(text.find("natType") == std::string::npos);
  CHECK(text.find("\"search\":{\"mode\":1,\"connectCode\":[]}") != std::string::npos);
  CHECK(text.find("\"ipAddressLan\":\"\"") != std::string::npos);
}

void test_json_escaping() {
  Join join;
  join.display_name = "A\"B\\C\n\x01";
  join.connect_code = "\xff";   // invalid UTF-8 is replaced, never thrown
  const std::string text = join_json(join);
  CHECK(text.find("\"displayName\":\"A\\\"B\\\\C\\n\\u0001\"") != std::string::npos);
  CHECK(text.find("\"connectCode\":\"\xEF\xBF\xBD\"") != std::string::npos);
  CHECK(!json::parse(text, nullptr, false).is_discarded());
}

void test_other_messages() {
  CHECK(resume_json("t1", "tok") == "{\"type\":\"resume\",\"ticketId\":\"t1\",\"token\":\"tok\"}");
  CHECK(simple_json("cancel") == "{\"type\":\"cancel\"}");
  CHECK(simple_json("ping") == "{\"type\":\"ping\"}");
  if (!std::getenv("SLIPPI_MM_URL")) {
    CHECK(default_url("3.7.0") == "wss://matchmaking.slippi.gg/v1/queue");
    CHECK(default_url("3.7.0-dev") == "wss://matchmaking-dev.slippi.gg/v1/queue");
  }
  // RFC 6455 section 1.3's example.
  CHECK(detail::websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

StunObservation observed(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
  StunObservation o;
  o.answered = true;
  const uint8_t octets[4] = {a, b, c, d};
  std::memcpy(&o.ip_be, octets, 4);
  o.port = port;
  return o;
}

Request sample_request() {
  Request r;
  r.mode = 1;
  r.netplay_port = 41000;
  r.uid = "u";
  r.play_key = "k";
  r.connect_code_self = "ME#1";
  r.display_name = "Me";
  r.app_version = "3.7.0";
  return r;
}

// client.rs build_join
void test_build_join() {
  {
    Request r = sample_request();
    r.lan_addr = "192.168.1.5:41000";
    r.stun[0] = observed(1, 2, 3, 4, 50000);
    r.stun[1] = observed(1, 2, 3, 4, 50000);
    const Join j = build_join(r);
    CHECK(j.nat_type == "cone");
    CHECK(j.port == 50000);
    CHECK(j.candidates.size() == 2);
    if (j.candidates.size() == 2) {
      CHECK(j.candidates[0].ip == "1.2.3.4" && j.candidates[0].port == 50000 && j.candidates[0].type == "srflx");
      CHECK(j.candidates[1].ip == "192.168.1.5" && j.candidates[1].port == 41000 && j.candidates[1].type == "host");
    }
    CHECK(j.lan_addr == "192.168.1.5:41000");
    CHECK(j.connect_code == "ME#1" && j.play_key == "k" && j.app_version == "3.7.0");
  }
  {
    Request r = sample_request();
    r.stun[1] = observed(5, 6, 7, 8, 40001);   // only the second server answered
    r.connect_code = {0x82, 0x60};
    const Join j = build_join(r);
    CHECK(j.nat_type == "unknown");
    CHECK(j.port == 40001);
    CHECK(j.candidates.size() == 1 && j.candidates[0].ip == "5.6.7.8" && j.candidates[0].type == "srflx");
    CHECK(j.search_connect_code == std::vector<uint8_t>({0x82, 0x60}));
  }
  {
    Request r = sample_request();
    r.stun[0] = observed(1, 2, 3, 4, 50000);
    r.stun[1] = observed(1, 2, 3, 4, 50001);
    r.lan_addr = "10.0.0.2:notaport";
    const Join j = build_join(r);
    CHECK(j.nat_type == "symmetric");
    CHECK(j.candidates.size() == 1);
    CHECK(join_json(j).find("\"ipAddressLan\":\"10.0.0.2:notaport\"") != std::string::npos);
  }
  {
    const Join j = build_join(sample_request());   // no STUN answer, no LAN address
    CHECK(j.port == 41000 && j.candidates.empty() && j.nat_type == "unknown");
    const std::string text = join_json(j);
    CHECK(text.find("candidates") == std::string::npos && text.find("\"natType\":\"unknown\"") != std::string::npos);
  }
}

// ------------------------------------------------------------------ stun.rs
std::vector<uint8_t> response_for(const uint8_t request[kStunRequestLen], const std::vector<uint8_t>& attrs) {
  std::vector<uint8_t> out = {0x01, 0x01, (uint8_t)(attrs.size() >> 8), (uint8_t)attrs.size(), 0x21, 0x12, 0xA4, 0x42};
  out.insert(out.end(), request + 8, request + 20);
  out.insert(out.end(), attrs.begin(), attrs.end());
  return out;
}

std::vector<uint8_t> xor_mapped(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
  const uint32_t ip = ((uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d) ^ 0x2112A442u;
  const uint16_t xport = (uint16_t)(port ^ 0x2112);
  return {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, (uint8_t)(xport >> 8), (uint8_t)xport,
          (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip};
}

bool same(const StunObservation& x, const StunObservation& y) { return x.answered == y.answered && x.ip_be == y.ip_be && x.port == y.port; }

void test_stun() {
  uint8_t a[kStunRequestLen], b[kStunRequestLen];
  stun_build_request(a);
  stun_build_request(b);
  const uint8_t header[8] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
  CHECK(std::memcmp(a, header, 8) == 0);
  CHECK(std::memcmp(a + 8, b + 8, 12) != 0);

  {  // parses XOR-MAPPED-ADDRESS; ip_be is in network byte order
    const auto resp = response_for(a, xor_mapped(203, 0, 113, 7, 41234));
    StunObservation out;
    CHECK(stun_parse_response(a, resp.data(), resp.size(), &out));
    CHECK(same(out, observed(203, 0, 113, 7, 41234)));
    const uint8_t octets[4] = {203, 0, 113, 7};
    CHECK(std::memcmp(&out.ip_be, octets, 4) == 0);
  }
  {  // prefers XOR-MAPPED-ADDRESS over MAPPED-ADDRESS and skips unknown attributes
    std::vector<uint8_t> attrs = {0x80, 0x22, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o', 0, 0, 0};   // SOFTWARE, odd length
    const std::vector<uint8_t> mapped = {0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x03, 0xE8, 10, 0, 0, 1};   // 10.0.0.1:1000
    attrs.insert(attrs.end(), mapped.begin(), mapped.end());
    const auto x = xor_mapped(198, 51, 100, 2, 5000);
    attrs.insert(attrs.end(), x.begin(), x.end());
    const auto resp = response_for(a, attrs);
    StunObservation out;
    CHECK(stun_parse_response(a, resp.data(), resp.size(), &out));
    CHECK(same(out, observed(198, 51, 100, 2, 5000)));
  }
  {  // MAPPED-ADDRESS alone is the fallback
    const auto resp = response_for(a, {0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x03, 0xE8, 10, 0, 0, 1});
    StunObservation out;
    CHECK(stun_parse_response(a, resp.data(), resp.size(), &out));
    CHECK(same(out, observed(10, 0, 0, 1, 1000)));
  }
  {  // rejects a foreign transaction, an error reply and a short reply, leaving out untouched
    const StunObservation sentinel = observed(9, 9, 9, 9, 9);
    StunObservation out = sentinel;
    const auto foreign = response_for(b, xor_mapped(203, 0, 113, 7, 1));
    CHECK(!stun_parse_response(a, foreign.data(), foreign.size(), &out));
    auto error = response_for(a, xor_mapped(203, 0, 113, 7, 1));
    error[0] = 0x01;
    error[1] = 0x11;
    CHECK(!stun_parse_response(a, error.data(), error.size(), &out));
    const uint8_t tiny[5] = {};
    CHECK(!stun_parse_response(a, tiny, sizeof tiny, &out));
    const auto ipv6_only = response_for(a, {0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    CHECK(!stun_parse_response(a, ipv6_only.data(), ipv6_only.size(), &out));
    CHECK(same(out, sentinel));
  }
}

void test_classify_nat() {
  const StunObservation a = observed(1, 1, 1, 1, 40000), b = observed(1, 1, 1, 1, 40001), none;
  CHECK(std::strcmp(classify_nat(a, a), "cone") == 0);
  CHECK(std::strcmp(classify_nat(a, b), "symmetric") == 0);
  CHECK(std::strcmp(classify_nat(a, none), "unknown") == 0);
  CHECK(std::strcmp(classify_nat(none, none), "unknown") == 0);
}

// ------------------------------------------------------------------ a fake service on loopback
#ifdef _WIN32
using sock_t = SOCKET;
const sock_t kNoSocket = INVALID_SOCKET;
void close_socket(sock_t s) { closesocket(s); }
const int kSendFlags = 0;
#else
using sock_t = int;
const sock_t kNoSocket = -1;
void close_socket(sock_t s) { ::close(s); }
const int kSendFlags = MSG_NOSIGNAL;
#endif

bool readable(sock_t s, int ms) {
#ifdef _WIN32
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  return select(0, &set, nullptr, nullptr, &tv) > 0;
#else
  pollfd p{s, POLLIN, 0};
  return poll(&p, 1, ms) > 0;
#endif
}

// One accepted connection, server side of RFC 6455.
struct Conn {
  sock_t s = kNoSocket;
  std::string in;
  Conn() = default;
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
  ~Conn() { drop(); }
  void drop() {
    if (s != kNoSocket) close_socket(s);
    s = kNoSocket;
  }
  bool receive(int ms) {
    if (s == kNoSocket || !readable(s, ms)) return false;
    char buffer[4096];
    const int n = (int)recv(s, buffer, sizeof buffer, 0);
    if (n <= 0) return false;
    in.append(buffer, (size_t)n);
    return true;
  }
  bool send_all(const std::string& data) {
    for (size_t off = 0; off < data.size();) {
      const int n = (int)send(s, data.data() + off, (int)(data.size() - off), kSendFlags);
      if (n <= 0) return false;
      off += (size_t)n;
    }
    return true;
  }
  bool upgrade(int ms) {
    while (in.find("\r\n\r\n") == std::string::npos)
      if (!receive(ms)) return false;
    const size_t end = in.find("\r\n\r\n");
    const std::string head = in.substr(0, end);
    in.erase(0, end + 4);
    const size_t k = head.find("Sec-WebSocket-Key: ");
    if (k == std::string::npos || head.compare(0, 4, "GET ") != 0) return false;
    const std::string key = head.substr(k + 19, head.find("\r\n", k) - (k + 19));
    return send_all("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                    detail::websocket_accept_key(key) + "\r\n\r\n");
  }
  // A whole frame already in `in`, unmasked.
  bool take(int* opcode, std::string* payload, bool* masked) {
    if (in.size() < 2) return false;
    const uint8_t b0 = (uint8_t)in[0], b1 = (uint8_t)in[1];
    uint64_t length = b1 & 0x7F;
    size_t header = 2;
    if (length == 126) {
      if (in.size() < 4) return false;
      length = (uint64_t)(uint8_t)in[2] << 8 | (uint8_t)in[3];
      header = 4;
    } else if (length == 127) {
      if (in.size() < 10) return false;
      length = 0;
      for (int i = 2; i < 10; ++i) length = length << 8 | (uint8_t)in[(size_t)i];
      header = 10;
    }
    *masked = (b1 & 0x80) != 0;
    const size_t mask_at = header;
    if (*masked) header += 4;
    if (in.size() < header + length) return false;
    payload->assign(in, header, (size_t)length);
    if (*masked)
      for (size_t i = 0; i < payload->size(); ++i) (*payload)[i] = (char)((uint8_t)(*payload)[i] ^ (uint8_t)in[mask_at + (i & 3)]);
    *opcode = b0 & 0x0F;
    in.erase(0, header + (size_t)length);
    return true;
  }
  bool read_frame(int* opcode, std::string* payload, int ms) {
    bool masked = false;
    while (!take(opcode, payload, &masked))
      if (!receive(ms)) return false;
    if (!masked) client_sent_unmasked = true;
    return true;
  }
  // The next text message (pongs skipped); false on close or silence.
  bool read_text(std::string* text, int ms) {
    int opcode = 0;
    std::string payload;
    while (read_frame(&opcode, &payload, ms)) {
      if (opcode == 1) { *text = payload; return true; }
      if (opcode == 8) return false;
    }
    return false;
  }
  bool send_frame(int opcode, const std::string& payload) {
    std::string frame(1, (char)(0x80 | opcode));
    if (payload.size() < 126) {
      frame.push_back((char)payload.size());
    } else {
      frame.push_back((char)126);
      frame.push_back((char)(payload.size() >> 8));
      frame.push_back((char)payload.size());
    }
    return send_all(frame + payload);
  }
  bool send_text(const std::string& text) { return send_frame(1, text); }
  bool client_sent_unmasked = false;
};

class FakeService {
 public:
  FakeService() {
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    listen(listener_, 4);
    socklen_t length = sizeof address;
    getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
  }
  ~FakeService() { stop(); }
  void stop() {
    if (listener_ != kNoSocket) close_socket(listener_);
    listener_ = kNoSocket;
  }
  std::string url() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/v1/queue"; }
  // Accepts and upgrades one connection.
  bool accept_one(Conn* conn, int ms) {
    if (!readable(listener_, ms)) return false;
    conn->s = accept(listener_, nullptr, nullptr);
    return conn->s != kNoSocket && conn->upgrade(5000);
  }

 private:
  sock_t listener_ = kNoSocket;
  uint16_t port_ = 0;
};

template <class F>
bool wait_for(F done, int ms) {
  const auto end = Clock::now() + std::chrono::milliseconds(ms);
  while (Clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return done();
}

const char* const kMatch = "{\"type\":\"match\",\"matchId\":\"mode.unranked-1\",\"isHost\":true,\"players\":[]}";

void test_session_matched() {
  FakeService service;
  std::string join_text, pong;
  bool unmasked = true;
  std::thread server([&] {
    Conn c;
    if (!service.accept_one(&c, 5000) || !c.read_text(&join_text, 5000)) return;
    c.send_text("not json");   // ignored
    c.send_text("{\"type\":\"joined\",\"ticketId\":\"t1\",\"token\":\"secret\"}");
    c.send_text("{\"type\":\"status\",\"secondsInQueue\":1.5}");
    c.send_frame(9, "hi");
    int opcode = 0;
    if (c.read_frame(&opcode, &pong, 5000) && opcode != 10) pong = "wrong opcode";
    c.send_text(kMatch);
    std::string ignored;
    c.read_frame(&opcode, &ignored, 2000);   // the client's close
    unmasked = c.client_sent_unmasked;
  });
  Request request = sample_request();
  request.stun[0] = observed(1, 2, 3, 4, 50000);
  request.stun[1] = observed(1, 2, 3, 4, 50000);
  {
    Session session(request, service.url());
    CHECK(wait_for([&] { return session.state() == State::Matched; }, 8000));
    std::string result;
    CHECK(session.take_result_json(&result));
    CHECK(result == kMatch);
    CHECK(!session.take_result_json(&result));
  }
  server.join();
  CHECK(pong == "hi");
  CHECK(!unmasked);
  CHECK(join_text == join_json(build_join(request)));
}

void test_session_error_with_latest_version() {
  FakeService service;
  std::thread server([&] {
    Conn c;
    std::string join;
    if (!service.accept_one(&c, 5000) || !c.read_text(&join, 5000)) return;
    c.send_text("{\"type\":\"error\",\"error\":\"Please update Slippi\",\"latestVersion\":\"3.7.1\"}");
    int opcode = 0;
    std::string ignored;
    c.read_frame(&opcode, &ignored, 2000);
  });
  Session session(sample_request(), service.url());
  CHECK(wait_for([&] { return session.state() == State::Failed; }, 8000));
  CHECK(session.error_message() == "Please update Slippi");
  CHECK(session.latest_version() == "3.7.1");
  server.join();
}

void test_session_cancel_and_destroy() {
  FakeService service;
  std::string cancel_text, destroy_text;
  std::thread server([&] {
    {
      Conn c;
      std::string join;
      if (!service.accept_one(&c, 5000) || !c.read_text(&join, 5000)) return;
      c.send_text("{\"type\":\"joined\",\"ticketId\":\"t1\",\"token\":\"secret\"}");
      c.read_text(&cancel_text, 5000);
    }
    Conn c;
    std::string join;
    if (!service.accept_one(&c, 5000) || !c.read_text(&join, 5000)) return;
    c.send_text("{\"type\":\"joined\",\"ticketId\":\"t2\",\"token\":\"secret\"}");
    c.read_text(&destroy_text, 5000);
  });
  {
    Session session(sample_request(), service.url());
    CHECK(wait_for([&] { return session.state() == State::Queued; }, 8000));
    session.cancel();
    CHECK(wait_for([&] { return session.state() == State::Idle; }, 2000));
  }
  {
    Session session(sample_request(), service.url());
    CHECK(wait_for([&] { return session.state() == State::Queued; }, 8000));
  }   // the destructor cancels
  server.join();
  CHECK(cancel_text == "{\"type\":\"cancel\"}");
  CHECK(destroy_text == "{\"type\":\"cancel\"}");
}

void test_session_resumes_after_a_drop() {
  FakeService service;
  std::string resume_text;
  std::thread server([&] {
    {
      Conn c;
      std::string join;
      if (!service.accept_one(&c, 5000) || !c.read_text(&join, 5000)) return;
      c.send_text("{\"type\":\"joined\",\"ticketId\":\"t1\",\"token\":\"secret\"}");
    }   // dropped without a close frame
    Conn c;
    if (!service.accept_one(&c, 8000) || !c.read_text(&resume_text, 5000)) return;
    c.send_text("{\"type\":\"joined\",\"ticketId\":\"t1\",\"token\":\"secret\"}");
    c.send_text(kMatch);
    int opcode = 0;
    std::string ignored;
    c.read_frame(&opcode, &ignored, 2000);
  });
  Session session(sample_request(), service.url());
  CHECK(wait_for([&] { return session.state() == State::Matched; }, 10000));
  server.join();
  CHECK(resume_text == "{\"type\":\"resume\",\"ticketId\":\"t1\",\"token\":\"secret\"}");
}

void test_session_pings_when_quiet() {
  FakeService service;
  std::string ping_text;
  std::thread server([&] {
    Conn c;
    std::string join;
    if (!service.accept_one(&c, 5000) || !c.read_text(&join, 5000)) return;
    c.send_text("{\"type\":\"joined\",\"ticketId\":\"t1\",\"token\":\"secret\"}");
    if (!c.read_text(&ping_text, 8000)) return;   // after five quiet seconds
    c.send_text(kMatch);
    int opcode = 0;
    std::string ignored;
    c.read_frame(&opcode, &ignored, 2000);
  });
  const auto start = Clock::now();
  Session session(sample_request(), service.url());
  CHECK(wait_for([&] { return session.state() == State::Matched; }, 10000));
  CHECK(Clock::now() - start >= std::chrono::milliseconds(4900));
  server.join();
  CHECK(ping_text == "{\"type\":\"ping\"}");
}

void test_session_join_timeout() {
  FakeService service;
  std::thread server([&] {
    Conn c;
    std::string join;
    if (!service.accept_one(&c, 5000) || !c.read_text(&join, 5000)) return;
    int opcode = 0;
    std::string ignored;
    c.read_frame(&opcode, &ignored, 8000);   // silent until the client gives up
  });
  Session session(sample_request(), service.url());
  CHECK(wait_for([&] { return session.state() == State::Failed; }, 9000));
  CHECK(session.error_message() == "Failed to join mm queue");
  server.join();
}

void test_session_lost_before_a_ticket() {
  FakeService service;
  std::thread server([&] {
    Conn c;
    std::string join;
    if (!service.accept_one(&c, 5000)) return;
    c.read_text(&join, 5000);
  });   // closes right after the join
  Session session(sample_request(), service.url());
  CHECK(wait_for([&] { return session.state() == State::Failed; }, 8000));
  CHECK(session.error_message() == "Lost connection to the mm server");
  server.join();
}

void test_session_nothing_listening() {
  std::string url;
  {
    FakeService closed;
    url = closed.url();
  }
  Session session(sample_request(), url);
  CHECK(session.state() == State::Connecting || session.state() == State::Failed);
  CHECK(wait_for([&] { return session.state() == State::Failed; }, 20000));
  CHECK(session.error_message() == "Failed to connect to mm server");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::printf("no Winsock\n"); return 2; }
#endif
  if (argc == 3 && std::strcmp(argv[1], "--probe") == 0) {
    const std::string error = detail::probe(argv[2]);
    std::printf("%s\n", error.empty() ? "probe: connected (TCP, TLS, WebSocket upgrade)" : ("probe: " + error).c_str());
    return error.empty() ? 0 : 1;
  }
  test_join_serializes_with_service_field_names();
  test_empty_optional_fields_are_omitted();
  test_json_escaping();
  test_other_messages();
  test_build_join();
  test_stun();
  test_classify_nat();
  test_session_matched();
  test_session_error_with_latest_version();
  test_session_cancel_and_destroy();
  test_session_resumes_after_a_drop();
  test_session_lost_before_a_ticket();
  test_session_pings_when_quiet();
  test_session_join_timeout();
  test_session_nothing_listening();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures.load());
    return 1;
  }
  std::printf("slippi_mm_service: all checks passed\n");
  return 0;
}
