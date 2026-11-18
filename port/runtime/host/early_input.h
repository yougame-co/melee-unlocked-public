// Early inputs: a controller change that matters is sent to the other player the moment it
// happens, stamped with the pad frame it will first appear in, instead of waiting for the next
// frame's pad. The other side predicts with it until the real pad arrives; the real pad always
// decides. The rules (which changes matter, the buffer the receiver predicts from) are in
// early_input_rules.h; this is the sampler thread that watches the local controller.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include "early_input_rules.h"

namespace host { struct PadState; }

namespace host::early_input {

// The transport's send: called on the sampler thread, so it must only queue (every transport here
// hands packets to its own network thread). `target` is the pad frame the change first appears in.
using SendFn = void (*)(int32_t target, const uint8_t pad[kPadBytes]);
// Slippi 3.7.0 input suggestions for Slippi players (hle/slippi_input_suggestions.h): the same
// watching, judged by Slippi's own rule for which changes are worth sending, each with a sequence
// number. Also called on the sampler thread or the game thread: it must only queue.
using SuggestFn = void (*)(int32_t target, uint32_t seq, const uint8_t pad[kPadBytes]);

// The player's setting (Online tab, "Early inputs"), with the MELEE_EARLY_INPUTS=0|1 test override
// applied. Off: nothing is sent, nothing is announced to the other player, and received early pads
// are not used.
bool enabled();
void set_enabled(bool on);

// A match began: watch the controller and send through `send`. Does nothing while disabled or
// already running. stop() ends the thread and must run before the transport goes away.
void start(SendFn send, SuggestFn suggest = nullptr);
void stop();
bool running();

// Every pad the host sends (frame = the pad frame on the wire). It is the baseline the sampler
// judges changes against, and the first non-neutral one identifies which controller the local
// player is holding.
void note_sent_pad(int32_t frame, const uint8_t pad[kPadBytes]);
// The host finished a frame's online command, after note_sent_pad or note_skipped. `skipped`: the
// polled pad was not sent and the same frame is polled again (`pad` is that unsent pad).
// `advancing`: the game runs the next frame on this poll too, so the next poll is a frame later.
// A change seen while the poll was being handled is released here, now that its frame is known.
void note_frame_done(bool skipped, bool advancing, const uint8_t pad[kPadBytes]);

// The pad in wire form (buttons big-endian, sticks, triggers), as hle_pad writes it for the game.
void pad_to_wire(const PadState& pad, uint8_t out[kPadBytes]);

struct Stats {
  uint64_t samples = 0;   // controller reads by the sampler
  uint64_t changes = 0;   // changes judged worth sending (and handed to the transport)
  uint64_t packets = 0;   // early pads that went on the wire (a peer takes them)
  uint64_t guarded = 0;   // changes left to the next poll (inside the guard window)
  uint64_t resent_after_skip = 0;   // pads sent again after a skipped poll
  uint64_t follow_ups = 0;          // byte changes sent after a line crossing, before its poll
  uint64_t received = 0;  // early pads received from the other players (counted by the transports)
  uint64_t suggestions = 0;   // Slippi input suggestions handed to the transport
  int local_port = -1;    // the controller port the local player was found on (-1: not yet)
};
Stats stats();
void reset_stats();
void note_received();
// The transport put an early pad on the wire.
void note_packet();
// The host answered a poll without sending a pad (a stall): the next read is sent again if it
// differs at all from what the other side has.
void note_skipped();
// The game is about to read the controllers for a frame (input_poll), before that frame's pad is
// sent or skipped.
void note_poll();

}  // namespace host::early_input
