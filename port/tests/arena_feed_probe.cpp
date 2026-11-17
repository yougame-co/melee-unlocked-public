// Arena feed probe (developer tool). Connects to a running game's Slippi spectator server the way
// the Slippi Launcher does, takes the match as it is played, runs it through the arena feed
// (Sender, then Receiver) and writes the live recording the Source Port's viewer can play while the
// match goes on:
//
//   port_arena_feed_probe --out live.slp [--host 127.0.0.1] [--port 51441] [--full-post] [--seconds 600]
//   melee_source --replay live.slp ...
//
// It proves two things on a real match: the spectator server serves a whole game, and the trimmed
// feed is enough for the viewer. It prints what the feed cost in bytes.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "arena_feed.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <enet/enet.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace feed = slippi::arena_feed;

namespace {

std::string decode64(const std::string& in) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
  };
  std::string out;
  int bits = 0, buffer = 0;
  for (char c : in) {
    const int v = value(c);
    if (v < 0) break;
    buffer = buffer << 6 | v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out += (char)((buffer >> bits) & 0xFF); }
  }
  return out;
}

struct Probe {
  std::string out_path;
  feed::Sender sender;
  feed::Receiver receiver;
  int sizes[256];
  std::vector<uint8_t> codes;                      // the match's code list, as the stream carries it
  bool codes_done = false, opened = false, game_over = false;
  std::deque<std::vector<uint8_t>> held;           // feed messages waiting for the code list
  int first_frame = 0, last_frame = 0;
  bool any_frame = false;
  int refused = 0;

  Probe() {
    for (int& s : sizes) s = -1;
    sender.set_game_tag(1);
    sender.set_out([this](const uint8_t* d, uint32_t n) { held.emplace_back(d, d + n); deliver(); });
  }
  // The receiver is given this build's code list by the game; the probe has no game, so it waits for
  // the list in the stream and opens the receiver with that.
  void deliver() {
    if (!codes_done) return;
    if (!opened) {
      if (!receiver.open(out_path, codes, 1)) { std::printf("probe: %s\n", receiver.error().c_str()); std::exit(2); }
      opened = true;
    }
    while (!held.empty()) {
      if (!receiver.message(0, held.front().data(), (uint32_t)held.front().size())) {
        ++refused;
        std::printf("probe: message refused: %s\n", receiver.error().c_str());
      }
      held.pop_front();
    }
  }
  // One game_event payload: whole recording events, one after another.
  void events(const std::string& bytes) {
    for (size_t at = 0; at < bytes.size();) {
      const uint8_t command = (uint8_t)bytes[at];
      size_t size;
      if (command == 0x35) {
        if (at + 2 > bytes.size()) return;
        size = 1 + (uint8_t)bytes[at + 1];
        for (size_t i = at + 2; i + 2 < at + size && i + 2 < bytes.size(); i += 3)
          sizes[(uint8_t)bytes[i]] = (uint8_t)bytes[i + 1] << 8 | (uint8_t)bytes[i + 2];
      } else {
        if (sizes[command] < 0) { std::printf("probe: event %02X is not in the command table\n", command); return; }
        size = 1 + (size_t)sizes[command];
      }
      if (at + size > bytes.size()) { std::printf("probe: event %02X runs past its message\n", command); return; }
      const uint8_t* event = (const uint8_t*)bytes.data() + at;
      if (command == 0x36) { codes.clear(); codes_done = false; }
      if (command == 0x10 && size == 517 && event[515] == 0x3D) {   // one part of the code list
        const size_t part = (size_t)event[513] << 8 | event[514];
        codes.insert(codes.end(), event + 1, event + 1 + (part > 512 ? 512 : part));
        if (event[516]) codes_done = true;
      }
      if (command == 0x3A && size >= 5) {
        const uint32_t bits = (uint32_t)event[1] << 24 | (uint32_t)event[2] << 16 | (uint32_t)event[3] << 8 | event[4];
        int32_t frame;
        std::memcpy(&frame, &bits, 4);
        if (!any_frame) { first_frame = last_frame = frame; any_frame = true; }
        if (frame > last_frame) last_frame = frame;
      }
      sender.event(event, (uint32_t)size);
      if (command == 0x10) deliver();
      if (command == 0x39) game_over = true;
      at += size;
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1", out;
  int port = 51441, seconds = 600;
  bool full_post = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--host" && i + 1 < argc) host = argv[++i];
    else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
    else if (a == "--full-post") full_post = true;
    else { std::printf("unknown option %s\n", a.c_str()); return 2; }
  }
  if (out.empty()) { std::printf("usage: port_arena_feed_probe --out live.slp [--host ip] [--port n] [--full-post] [--seconds n]\n"); return 2; }
  if (enet_initialize() != 0) { std::printf("probe: ENet did not start\n"); return 2; }

  Probe probe;
  probe.out_path = out;
  probe.sender.set_full_post(full_post);

  const auto begun = std::chrono::steady_clock::now();
  const auto deadline = begun + std::chrono::seconds(seconds);
  ENetHost* client = enet_host_create(nullptr, 1, 3, 0, 0);
  ENetPeer* peer = nullptr;
  bool connected = false;
  // The game may still be starting: keep trying until it serves.
  while (!connected && std::chrono::steady_clock::now() < deadline) {
    ENetAddress address{};
    enet_address_set_host(&address, host.c_str());
    address.port = (enet_uint16)port;
    peer = enet_host_connect(client, &address, 3, 1337);
    ENetEvent event;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (peer && !connected && std::chrono::steady_clock::now() < until)
      if (enet_host_service(client, &event, 50) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) connected = true;
    if (!connected && peer) enet_peer_reset(peer);
  }
  if (!connected) { std::printf("probe: no spectator server at %s:%d\n", host.c_str(), port); return 3; }
  std::printf("probe: connected to %s:%d\n", host.c_str(), port);
  const std::string hello = "{\"type\":\"connect_request\",\"cursor\":0}";
  enet_peer_send(peer, 0, enet_packet_create(hello.data(), hello.size(), ENET_PACKET_FLAG_RELIABLE));
  enet_host_flush(client);

  uint64_t wire_bytes = 0;      // what the spectator server itself sent (JSON and base64)
  uint64_t raw_bytes = 0;       // the recording events inside it
  bool lost = false;
  std::chrono::steady_clock::time_point game_start{}, game_end{};
  while (!probe.game_over && !lost && std::chrono::steady_clock::now() < deadline) {
    ENetEvent event;
    if (enet_host_service(client, &event, 20) <= 0) continue;
    if (event.type == ENET_EVENT_TYPE_DISCONNECT) { lost = true; break; }
    if (event.type != ENET_EVENT_TYPE_RECEIVE) continue;
    wire_bytes += event.packet->dataLength;
    const json message = json::parse((const char*)event.packet->data, (const char*)event.packet->data + event.packet->dataLength, nullptr, false);
    enet_packet_destroy(event.packet);
    if (message.is_discarded() || message.find("type") == message.end() || !message["type"].is_string()) continue;
    const std::string type = message["type"];
    if (type == "start_game") { game_start = std::chrono::steady_clock::now(); std::printf("probe: a game starts\n"); }
    else if (type == "game_event" && message.find("payload") != message.end() && message["payload"].is_string()) {
      const std::string bytes = decode64(message["payload"]);
      raw_bytes += bytes.size();
      probe.events(bytes);
    } else if (type == "end_game") { game_end = std::chrono::steady_clock::now(); if (!probe.game_over) { probe.sender.stop(); break; } }
  }
  if (game_end == std::chrono::steady_clock::time_point{}) game_end = std::chrono::steady_clock::now();
  enet_peer_disconnect_now(peer, 0);
  enet_host_destroy(client);
  enet_deinitialize();

  const feed::Sender::Stats& s = probe.sender.stats();
  const feed::Receiver::Stats& r = probe.receiver.stats();
  const double played = probe.any_frame ? (probe.last_frame - probe.first_frame + 1) / 60.0 : 0.0;
  std::printf("probe: game %s%s; frames %d..%d (%.1f s of match)\n", probe.game_over ? "ended" : "did not end",
              lost ? ", connection lost" : "", probe.first_frame, probe.last_frame, played);
  std::printf("probe: feed sent %u frames in %llu messages, %llu bytes", s.frames, (unsigned long long)s.messages,
              (unsigned long long)s.bytes);
  if (played > 0.0) std::printf(" (%.0f bytes per second of match)", s.bytes / played);
  std::printf("\nprobe: %u waiting frames replaced after a rollback, %u replaced too late, most held back %zu\n",
              s.superseded, s.late, s.max_waiting);
  std::printf("probe: receiver wrote %u frames, %u gaps, %d messages refused, file %s\n", r.frames, r.gaps, probe.refused,
              probe.receiver.ended() ? "complete" : "left open");
  std::printf("probe: the spectator server sent %llu bytes holding %llu bytes of recording\n",
              (unsigned long long)wire_bytes, (unsigned long long)raw_bytes);
  probe.receiver.close();
  const bool ok = probe.game_over && probe.receiver.ended() && s.late == 0 && r.gaps == 0 && probe.refused == 0 && r.frames == s.frames;
  std::printf("probe: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
