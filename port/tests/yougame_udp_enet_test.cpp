// Real ENet protocol/fragment parser, entirely fake socket I/O. No servers/sockets.
#include "yougame_udp_enet.h"
#include <enet/yg_enet_socket.h>
#include <cstdio>
#include <deque>
#include <vector>
namespace {
int checks = 0, failures = 0, parsed = 0, binds = 0;
uint64_t now = 0;
uint16_t bound = 0;
bool bind_ok = true;
struct Datagram { ENetAddress from; std::vector<uint8_t> bytes; };
std::deque<Datagram> incoming;
std::vector<std::string> logs;
void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL: %s\n", name); } }
size_t decompress(void*, const uint8_t*, size_t, uint8_t*, size_t) { ++parsed; return 0; }
ENetEventType last_event = ENET_EVENT_TYPE_NONE;
int receive(ENetHost* host, ENetAddress from, std::vector<uint8_t> bytes, uint32_t timeout = 0) {
  incoming.push_back({from, std::move(bytes)});
  ENetEvent event{};
  const int result = enet_host_service(host, &event, timeout);
  last_event = event.type;
  check(result >= 0, "fake service succeeds");
  if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
  return result;
}
}
extern "C" {
int enet_initialize() { return 0; }
void enet_deinitialize() {}
enet_uint32 enet_time_get() { return enet_uint32(now); }
void enet_time_set(enet_uint32 t) { now = t; }
enet_uint32 enet_host_random_seed() { return 12345; }
ENetSocket enet_socket_create(ENetSocketType) { return 42; }
void enet_socket_destroy(ENetSocket) {}
int enet_socket_bind(ENetSocket, const ENetAddress* a) { ++binds; bound = a->port; return bind_ok ? 0 : -1; }
int enet_socket_get_address(ENetSocket, ENetAddress* a) { a->host = ENET_HOST_ANY; a->port = bound; return 0; }
int enet_socket_set_option(ENetSocket, ENetSocketOption, int) { return 0; }
int enet_socket_send(ENetSocket, const ENetAddress*, const ENetBuffer* b, size_t n) {
  size_t total = 0; for (size_t i = 0; i < n; ++i) total += b[i].dataLength; return int(total);
}
int enet_socket_receive(ENetSocket, ENetAddress* a, ENetBuffer* b, size_t) {
  if (incoming.empty()) return 0;
  const auto packet = std::move(incoming.front()); incoming.pop_front();
  *a = packet.from;
  if (packet.bytes.size() > b[0].dataLength) return ENET_SOCKET_RECEIVE_TRUNCATED;
  if (!packet.bytes.empty()) std::memcpy(b[0].data, packet.bytes.data(), packet.bytes.size());
  return packet.bytes.empty() ? ENET_SOCKET_RECEIVE_EMPTY : int(packet.bytes.size());
}
int enet_socket_wait(ENetSocket, enet_uint32* condition, enet_uint32 timeout) { now += timeout; *condition = 0; return 0; }
}
int main() {
  using namespace yougame::udp;
  yg_enet_select(0);
  ENetAddress local{ENET_HOST_ANY, 51642};
  ENetHost* host = enet_host_create(&local, 3, 3, 0, 0);
  check(host && binds == 1 && bound == 51642 && host->address.host == ENET_HOST_ANY, "ENet exact bind all interfaces");
  if (!host) return 1;
  EnetIngress ingress;
  ingress.clock = [] { return now; };
  ingress.log = [](const char* s) { logs.emplace_back(s); };
  const ENetAddress peer{0x12345678, 51642}, stranger{0x12345679, 51642};
  ingress.senders.allow({peer.host, peer.port}); ingress.attach(host);
  host->compressor.context = &parsed; host->compressor.decompress = decompress;
  receive(host, stranger, {0x4f,0xff});
  check(parsed == 0 && logs.size() == 1, "unknown rejected before decompressor/parser");
  receive(host, peer, {0x4f,0xff});
  check(parsed == 1, "known reaches real ENet parser");
  receive(host, {peer.host, uint16_t(peer.port+1)}, {0x4f,0xff});
  now = 9999; receive(host, stranger, {0x4f,0xff});
  check(parsed == 1 && logs.size() == 1, "wrong source port and rate limit");
  now = 10000; receive(host, stranger, {0x4f,0xff});
  check(logs.back() == "yougame: dropped 3 datagrams from unknown senders" && logs.size() == 2, "ENet ingress aggregate log");
  receive(host, stranger, {}); receive(host, stranger, std::vector<uint8_t>(5000));
  now = 20000; receive(host, stranger, {0x4f,0xff});
  check(logs.back() == "yougame: dropped 3 datagrams from unknown senders" && logs.size() == 3 && parsed == 1,
      "empty and oversized unknown datagrams counted without parsing");
  check(receive(host, peer, {}) == 0, "empty datagram does not end host service with error");
  check(receive(host, peer, std::vector<uint8_t>(5000)) == 0, "truncated datagram does not end host service with error");
  check(parsed == 1, "known empty and truncated datagrams never parsed");
  host->intercept = nullptr;
  check(receive(host, peer, {}) == 0, "empty datagram without ingress does not cause service error");
  check(receive(host, peer, std::vector<uint8_t>(5000)) == 0, "truncated datagram without ingress does not cause service error");
  check(parsed == 1, "empty/truncated without ingress never parsed");
  {
    EnetIngress wake_ingress;
    std::vector<std::string> wake_logs;
    wake_ingress.clock = [] { return now; };
    // Function pointers cannot capture: use the existing log sink, preserving its contents.
    wake_logs.swap(logs);
    wake_ingress.log = [](const char* s) { logs.emplace_back(s); };
    wake_ingress.attach(host);
    ENetAddress loopback{0, bound};
    const uint8_t ip[4] = {127, 0, 0, 1};
    std::memcpy(&loopback.host, ip, sizeof ip);
    const uint64_t before_wake = now;
    check(receive(host, loopback, {0}, 250) == 1 && last_event == (ENetEventType)42 && now == before_wake,
        "own-port loopback byte wakes idle service immediately");
    check(parsed == 1 && logs.empty() && host->connectedPeers == 0, "wake never parsed, counted, or made a peer");
    check(receive(host, {loopback.host, uint16_t(bound + 1)}, {0}) == 0 && last_event == ENET_EVENT_TYPE_NONE,
        "loopback other port dropped, never a wake");
    check(logs == std::vector<std::string>{"yougame: dropped 1 datagrams from unknown senders"}, "wrong wake source counted as unknown");
    now += 10000;
    check(receive(host, {peer.host, bound}, {0}) == 0 && last_event == ENET_EVENT_TYPE_NONE,
        "non-loopback own port dropped, never a wake");
    check(logs.size() == 2, "non-loopback own port counted as unknown");
    check(receive(host, loopback, {0x4f,0xff}) == 0 && last_event == ENET_EVENT_TYPE_NONE,
        "own-port loopback ENet payload is not a wake or peer");
    check(parsed == 1 && host->connectedPeers == 0, "rejected wakes never parsed or made peers");
    // A bad receive must not stop draining: reach the wake later in this same service call.
    incoming.push_back({loopback, {}});
    incoming.push_back({loopback, std::vector<uint8_t>(5000)});
    check(receive(host, loopback, {0}, 250) == 1 && last_event == (ENetEventType)42 && incoming.empty() && parsed == 1,
        "empty and truncated datagrams continue host service through following wake");
    logs.swap(wake_logs);
  }
  ingress.attach(host);
  // Wire sent-time/checksum flags must not underflow decompressor input length.
  receive(host, peer, {0xcf,0xff}); receive(host, peer, {0xcf,0xff,0});
  check(parsed == 1, "truncated sent-time header stops before parser");
  host->checksum = enet_crc32;
  receive(host, peer, {0x4f,0xff}); receive(host, peer, {0xcf,0xff,0,0});
  check(parsed == 1, "truncated checksum header stops before parser");
  host->checksum = nullptr; host->compressor.context = nullptr;
  ENetPeer* connected = enet_host_connect(host, &peer, 3, 0);
  check(connected != nullptr, "known peer allocated");
  if (connected) {
    connected->state = ENET_PEER_STATE_CONNECTED;
    host->maximumPacketSize = 1200; host->maximumWaitingData = 2400;
    // Every command, every truncation of its fixed header, then hostile payload lengths.
    for (unsigned command = 1; command < ENET_PROTOCOL_COMMAND_COUNT; ++command) {
      for (size_t size = 1; size < sizeof(ENetProtocol) + 8; ++size) {
        std::vector<uint8_t> wire(size + 2, 0xff);
        wire[0] = 0; wire[1] = 0; wire[2] = uint8_t(command);
        if (wire.size() > 3) wire[3] = 0; // valid channel; all wire lengths remain hostile
        receive(host, peer, wire);
      }
    }
    check(connected->totalWaitingData <= host->maximumWaitingData, "hostile lengths cannot allocate unbounded payload");
  }
  enet_host_destroy(host);
  bind_ok = false; const int before = binds;
  host = enet_host_create(&local, 3, 3, 0, 0);
  check(!host && binds == before + 1 && bound == 51642, "ENet occupied port fails with no fallback");
  std::printf("yougame_udp_enet: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
