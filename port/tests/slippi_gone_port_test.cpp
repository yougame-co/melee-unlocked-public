// When a player leaves a 3 or 4 player match, every remaining player must read the same inputs for
// them. Before this, the read was aligned to how far ahead another opponent happened to be.
#include "slippi_gone_port.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace slippi;
constexpr int kPad = 12, kFrames = 7;
constexpr uint8_t kOldBuildCaps2 = MU_BUILD_CAPS2_FRIENDS_LOBBY | MU_BUILD_CAPS2_LOCAL_PARTNER;
constexpr uint8_t kNewBuildCaps2 = kOldBuildCaps2 | MU_BUILD_CAPS2_GONE_OWN_FRAMES;
int failures = 0;
void check(bool ok, const char* name) {
  if (!ok) { std::printf("FAILED: %s\n", name); ++failures; }
}

// A kept window, newest first, whose first byte names the frame it belongs to.
std::vector<uint8_t> kept(int32_t last, int count) {
  std::vector<uint8_t> out;
  for (int i = 0; i < count; ++i) {
    std::vector<uint8_t> pad(kPad, 0);
    pad[0] = (uint8_t)(last - i);
    out.insert(out.end(), pad.begin(), pad.end());
  }
  return out;
}

uint8_t first(const std::vector<uint8_t>& tx, int k) { return tx[(size_t)k * kPad]; }

std::vector<uint8_t> window(const std::vector<uint8_t>& data, int32_t last, int32_t read) {
  return GonePortWindow(data, last, last, read, kPad, kFrames);
}

// Slippi's read of a port that has left: index its window by another opponent's lead.
std::vector<uint8_t> slippi_read(const std::vector<uint8_t>& data, int32_t opponents_latest, int32_t frame) {
  return ReplyWindow(data, std::max(0, (opponents_latest - frame) * kPad), kPad, kFrames);
}

void reads_the_last_frame_whatever_the_other_opponent_did() {
  const auto data = kept(100, 2);
  const auto tx = window(data, 100, 100);
  check(first(tx, 0) == 100, "frame L reads L's input");
  check(first(tx, 1) == 99, "frame L-1 reads L-1's input");
  // The bug this replaces, from the 11-08 logs: one frame of lead returned L-1's input, two ran off
  // the kept window and returned zeros.
  check(first(slippi_read(data, 100, 100), 0) == 100, "Slippi's read is right with no lead");
  check(first(slippi_read(data, 101, 100), 0) == 99, "Slippi's read returns L-1 with a lead of one");
  check(first(slippi_read(data, 102, 100), 0) == 0, "Slippi's read loses the input with a lead of two");
}

void frames_after_the_last_one_are_zero() {
  const auto tx = window(kept(100, 3), 100, 103);
  check(first(tx, 0) == 0 && first(tx, 1) == 0 && first(tx, 2) == 0, "past the last frame is zero");
  check(first(tx, 3) == 100 && first(tx, 4) == 99 && first(tx, 5) == 98, "older entries stay on their frames");
}

void older_than_kept_is_zero() {
  const auto tx = window(kept(100, 2), 100, 100);
  check(tx.size() == (size_t)kPad * kFrames, "full window size");
  for (int k = 2; k < kFrames; ++k) check(first(tx, k) == 0, "older than kept is zero");
}

void empty_queue_is_all_zero() {
  const auto tx = GonePortWindow({}, 0, 0, 50, kPad, kFrames);
  bool zero = true;
  for (uint8_t b : tx) zero = zero && b == 0;
  check(zero, "nothing kept reads as zeros");
}

// Who may use the new read. Every row must have sent a build that says it reads this way.
void the_gate_needs_every_peer_to_have_the_new_read() {
  const PeerBuildRow fresh{true, kNewBuildCaps2}, plain{false, 0}, old_build{true, kOldBuildCaps2}, before_byte{true, 0};
  const PeerBuildRow all[3] = {fresh, fresh, fresh};
  check(EveryPeerReadsGoneOwnFrames(all, 3), "every peer has the bit");
  const PeerBuildRow with_plain[3] = {fresh, plain, fresh};
  check(!EveryPeerReadsGoneOwnFrames(with_plain, 3), "a plain Slippi peer closes the gate");
  const PeerBuildRow with_old[3] = {fresh, fresh, old_build};
  check(!EveryPeerReadsGoneOwnFrames(with_old, 3), "an older YouGame build closes the gate");
  const PeerBuildRow with_no_byte[3] = {before_byte, fresh, fresh};
  check(!EveryPeerReadsGoneOwnFrames(with_no_byte, 3), "a build from before the second byte closes the gate");
  check(!EveryPeerReadsGoneOwnFrames(all, 0), "no peers keeps the gate closed");
  check(LatchGoneRead(-1, true) == 1 && LatchGoneRead(-1, false) == 0, "the first leave settles the read");
  check(LatchGoneRead(0, true) == 0 && LatchGoneRead(1, false) == 1, "a settled read does not move");
}

// Every path of the decision the game runs. Slippi's read must come back byte for byte.
void port_reply_only_changes_the_read_when_it_should() {
  const auto slippi_data = kept(100, 2);
  const int offset = 2 * kPad;
  const auto expected = ReplyWindow(slippi_data, offset, kPad, kFrames);
  int fetched = 0;
  const auto fetch = [&] { ++fetched; return KeptInputs{kept(100, 2), 100}; };
  const auto run = [&](bool gone, bool connected, int latch) {
    int32_t last = kNoFrame;
    return PortReply(gone, connected, latch, last, slippi_data, offset, 100, kPad, kFrames, fetch);
  };
  for (int latch : {-1, 0}) {
    const auto r = run(true, true, latch);
    check(!r.own_frames && r.window == expected, "an unsettled or closed gate keeps Slippi's read exactly");
  }
  check(!run(true, false, 1).own_frames, "nobody left connected keeps Slippi's read");
  check(run(false, true, 1).window == expected, "a connected port is untouched");
  check(fetched == 0, "Slippi's path never reads the extra inputs");
  const auto own = run(true, true, 1);
  check(own.own_frames && first(own.window, 0) == 100 && fetched == 1, "an open gate reads the port's own frames");
}

// A pad that lands after the port was first seen gone cannot rewrite frames served as empty.
void a_late_pad_does_not_rewrite_frames() {
  const std::vector<uint8_t> none;
  int32_t last = kNoFrame;
  const auto before = PortReply(true, true, 1, last, none, 0, 101, kPad, kFrames, [] { return KeptInputs{kept(100, 2), 100}; });
  check(last == 100 && first(before.window, 0) == 0 && first(before.window, 1) == 100, "the first read latches the last frame");
  const auto after = PortReply(true, true, 1, last, none, 0, 101, kPad, kFrames, [] { return KeptInputs{kept(102, 4), 102}; });
  check(last == 100, "a late pad does not move the latch");
  check(after.window == before.window, "a late pad leaves the served frames alone");
}

// The despawn timer is read from the same state, so a late pad can't move it for only some players.
void a_late_pad_does_not_move_the_despawn() {
  check(GoneLatestFrame(1, 100, 102) == 100, "with the gate open a late pad can't push the despawn back");
  check(GoneLatestFrame(1, 100, 97) == 97, "an earlier window value is left alone");
  check(GoneLatestFrame(1, kNoFrame, 102) == 102, "before the latch the value is untouched");
  check(GoneLatestFrame(0, 100, 102) == 102 && GoneLatestFrame(-1, 100, 102) == 102, "Slippi's despawn value is untouched");
}

std::string source(const char* path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The pure functions only help if the game calls them with its own state.
void the_game_calls_them() {
  const std::string online = source(SLIPPI_ONLINE_CPP), net = source(SLIPPI_NET_CPP);
  check(!online.empty() && !net.empty(), "sources readable");
  // Build 122 (Slippi 3.7.0): the player drop protocol replaced the gone-port remap in the reply. A
  // dropped port's inputs are settled with the other players and repeat from a shared despawn frame;
  // the helpers above stay tested for the builds that still read this way.
  check(online.find("slippi::PortReply(") == std::string::npos, "the reply no longer remaps a gone port");
  check(online.find("g_netplay->FillDroppedPlayerInputs(frame);") != std::string::npos, "dropped ports repeat their settled input");
  check(online.find("(uint32_t)results[i]->despawn_frame") != std::string::npos, "the game gets each port's despawn frame");
  check(online.find("g_gone_own_frames = -1;\n    std::fill(std::begin(g_gone_last_frame), std::end(g_gone_last_frame), slippi::kNoFrame);") != std::string::npos,
        "a new match forgets the last match's latches");
  check(net.find("slippi::EveryPeerReadsGoneOwnFrames(") != std::string::npos, "the client uses the tested gate");
  check(net.find("MU_BUILD_CAPS2_LOCAL_PARTNER | MU_BUILD_CAPS2_GONE_OWN_FRAMES") != std::string::npos, "we tell peers we read this way");
  check(net.find("if (machine_pad_format_ && SenderPort(peer) != port)") != std::string::npos, "a build message can only speak for its sender");
}
}  // namespace

int main() {
  reads_the_last_frame_whatever_the_other_opponent_did();
  frames_after_the_last_one_are_zero();
  older_than_kept_is_zero();
  empty_queue_is_all_zero();
  the_gate_needs_every_peer_to_have_the_new_read();
  port_reply_only_changes_the_read_when_it_should();
  a_late_pad_does_not_rewrite_frames();
  a_late_pad_does_not_move_the_despawn();
  the_game_calls_them();
  if (failures) { std::printf("%d failed\n", failures); return 1; }
  std::printf("slippi gone port: ok\n");
  return 0;
}
