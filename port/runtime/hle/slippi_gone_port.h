// The inputs a remote port hands to the game, and how a port that has left is read.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "../abi/mu_friends_lobby.h"

namespace slippi {

constexpr int32_t kNoFrame = std::numeric_limits<int32_t>::min();

// The read Slippi Dolphin uses for every port: what is kept for the port, from a byte offset, padded with
// zeros to a full window. For a port that has left, the offset comes from another opponent's progress.
inline std::vector<uint8_t> ReplyWindow(const std::vector<uint8_t>& data, int offset, int pad_size, int frames) {
  std::vector<uint8_t> tx;
  if (offset < (int)data.size()) tx.assign(data.begin() + offset, data.end());
  tx.resize((size_t)pad_size * (size_t)frames, 0);
  return tx;
}

// What a peer's build message says about this read. received is false for a plain Slippi peer, which
// never sends one; caps2 is 0 for a build from before the second capability byte.
struct PeerBuildRow {
  bool received = false;
  uint8_t caps2 = 0;
};

// Whether every peer reads a port that has left the same way we would. All of them must have told us so:
// a plain Slippi peer or an older YouGame build keeps Slippi's read, and one player using another rule splits the match.
inline bool EveryPeerReadsGoneOwnFrames(const PeerBuildRow* rows, int count) {
  if (count <= 0) return false;
  for (int i = 0; i < count; ++i)
    if (!rows[i].received || !mu_gone_own_frames_capable(rows[i].caps2)) return false;
  return true;
}

// Settled once, at the match's first leave, so a late build message can't move a match from one read to the
// other halfway through. latch is -1 until then, and unknown reads as no.
inline int LatchGoneRead(int latch, bool every_peer_reads_own_frames) {
  return latch < 0 ? (every_peer_reads_own_frames ? 1 : 0) : latch;
}

// A port's last frame, taken the first time it is read after leaving. A pad that arrives after that, past
// a stall's forced disconnect, must not rewrite frames the game was already told were empty.
inline int32_t LatchLastFrame(int32_t latch, int32_t seen) { return latch == kNoFrame ? seen : latch; }

// The frame a port that has left is timed from when it despawns. A late pad must not move that either, so with
// the gate open it can't pass the latched last frame; otherwise this is Slippi's value, untouched.
inline int32_t GoneLatestFrame(int latch, int32_t last_frame_latch, int32_t seen) {
  return latch == 1 && last_frame_latch != kNoFrame ? std::min(seen, last_frame_latch) : seen;
}

// Whether a port that has left is read by its own frames. With nobody left connected there is no other
// player to agree with and the old read stands too.
inline bool GonePortReadsOwnFrames(bool gone, bool have_connected_opponent, bool every_peer_reads_own_frames) {
  return gone && have_connected_opponent && every_peer_reads_own_frames;
}

// Builds a port's reply window, newest first, where entry k is the input for read_frame - k.
// newest_first holds what we kept for the port, starting at kept_last_frame. Frames after last_frame and
// frames older than what we kept are zeros. The frame the game reads is set by the other opponents' progress,
// which differs between players, and it must not choose which of this port's inputs comes back.
//
// Known limit: the result depends on this player's copy of the port's inputs and the last frame it saw.
// If the leaver's final packet reached only some players, they hold different last frames and can
// still split. Slippi's read has the same hole; closing it needs the players to agree on the last frame.
inline std::vector<uint8_t> GonePortWindow(const std::vector<uint8_t>& newest_first, int32_t kept_last_frame,
    int32_t last_frame, int32_t read_frame, int pad_size, int frames) {
  std::vector<uint8_t> tx((size_t)pad_size * (size_t)frames, 0);
  for (int k = 0; k < frames; ++k) {
    const int64_t wanted = (int64_t)read_frame - k;
    if (wanted > last_frame) continue;
    const int64_t back = (int64_t)kept_last_frame - wanted;
    if (back < 0) continue;
    const uint64_t at = (uint64_t)back * (uint64_t)pad_size;
    if (at + (uint64_t)pad_size > newest_first.size()) break;
    std::copy_n(newest_first.begin() + (std::ptrdiff_t)at, pad_size, tx.begin() + (std::ptrdiff_t)(k * pad_size));
  }
  return tx;
}

struct KeptInputs {
  std::vector<uint8_t> newest_first;
  int32_t last_frame = 0;
};

struct PortReplyWindow {
  std::vector<uint8_t> window;
  bool own_frames = false;
};

// The whole decision for one port's reply, so the tests exercise what the game runs. latch comes from
// LatchGoneRead; -1 or 0 keeps Slippi's read, byte for byte. kept() fetches everything held for the port and
// is only called on the own-frames path. last_frame_latch is this port's LatchLastFrame state.
template <class KeptFn>
inline PortReplyWindow PortReply(bool gone, bool have_connected_opponent, int latch, int32_t& last_frame_latch,
    const std::vector<uint8_t>& slippi_data, int slippi_offset, int32_t read_frame, int pad_size, int frames, KeptFn kept) {
  if (!GonePortReadsOwnFrames(gone, have_connected_opponent, latch == 1))
    return {ReplyWindow(slippi_data, slippi_offset, pad_size, frames), false};
  const KeptInputs held = kept();
  last_frame_latch = LatchLastFrame(last_frame_latch, held.last_frame);
  return {GonePortWindow(held.newest_first, held.last_frame, last_frame_latch, read_frame, pad_size, frames), true};
}

}  // namespace slippi
