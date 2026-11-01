// Console-style sessions: 2-4 players in the game's normal local modes, remote players on ports 2-4.
// The engine side (lockstep pads, deterministic boot, desync checks) and the link side (room, peers,
// packets) meet only through this header, so each can be tested with a fake of the other.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../abi/mu_host.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace yougame::console {

constexpr int kMaxPorts = 4;

// What every machine in a session agrees on before frame 0.
struct SessionConfig {
  std::string session_id;       // Rooms' start id; names logs and the session card folder.
  std::string room;             // Room code.
  int my_port = 1;              // 1-4: room slot + 1, stable for the whole session.
  std::vector<int> ports;       // Every port in the session, ascending.
  uint64_t seed = 0;            // Game RNG seed at frame 0.
  int64_t epoch_ms = 0;         // The boot clock every machine starts from (rooms' start time).
  int input_delay = 3;          // Frames between sampling a pad and the frame that uses it.
  bool default_card = false;   // Agreed fresh save; survives relaunch against a frozen host digest.
  std::string card_digest;      // Frozen SHA-256 of the host Melee GCI, unless default was agreed.
  std::vector<uint8_t> card_bytes; // Verified host copy, carried only inside this process.
  std::string settings_digest;  // Frozen settings key; friends1 carries the host CSS fighter/costume/pad.
};

// The link side, as the engine sees it. Frames are the engine's lockstep frame numbers, from 0.
class SessionLink {
public:
  bool frame_zero_barrier = false; // Live links require every peer's frame-0 digest before body 0.
  virtual ~SessionLink() = default;
  // Our port's pad for a frame. Sent once per frame, resent until every peer has it.
  virtual void send_pad(uint32_t frame, const MuPadStatus& pad) = 0;
  // A remote port's pad for a frame, once it has arrived.
  virtual std::optional<MuPadStatus> pad(int port, uint32_t frame) = 0;
  // A state digest for a frame, to compare with every peer's.
  virtual void send_digest(uint32_t frame, uint64_t digest) = 0;
  virtual std::optional<uint64_t> digest(int port, uint32_t frame) = 0;
  // Network work. Waits up to wait_ms for a packet; 0 polls.
  virtual void pump(int wait_ms) = 0;
  // Set once a peer has left or timed out: a short reason for the log and the notice.
  virtual std::optional<std::string> lost() const = 0;
  // Transport recovery pauses only wall-clock liveness deadlines, never frame/input rules.
  virtual bool reconnecting() const { return false; }
  virtual uint64_t liveness_clock(uint64_t ordinary) const { return ordinary; }
};

// Joins the session's room, waits for its start, and connects to every peer over the app's link helper.
// Blocks until then. On failure returns nullptr and sets error to a short reason.
std::unique_ptr<SessionLink> start_session(const nlohmann::json& request, SessionConfig& config, std::string& error);

}  // namespace yougame::console
