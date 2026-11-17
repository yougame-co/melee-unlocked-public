// Gecko codes in a network match: one connection's offer, answer and lock (gecko_online.h holds the
// rules and the messages). Game thread only.
//
// Direct or an arena between two Melee Unlocked players: when they connect, each offers the codes it
// has on (user_gecko::offerable). The other player sees them on the character select with what each
// does (the known codes table, never the sender's own words alone) and ticks the ones it accepts.
// The match runs every code one side offered and the other accepted, on both games. The decision is
// taken at the match's first frame from the two lock messages (gecko_online.h), so both games always
// decide the same. Anything else (Unranked, Ranked, a Slippi Dolphin opponent): no exchange.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gecko_online.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gecko_session {

// A connection to the opponent started: consent is true for Direct or an arena against a Melee
// Unlocked game that takes Gecko messages. Queues this side's offer.
void connected(bool consent);
void disconnected();
bool consenting();
// Messages to send now (and forgets them), and one received from the opponent.
std::vector<std::vector<uint8_t>> take_outgoing();
void receive(const uint8_t* data, size_t size);
// Just before each selection message (NetplayClient::SetMatchSelections): the lock to send, or
// empty when this connection exchanges no codes.
std::vector<uint8_t> lock_message();
// True once after the codes were agreed: this side's selection goes again with the new lock.
bool take_resend();

// The character select panel: the opponent's codes, waiting for this player's answer.
struct Incoming {
  std::string id, name, what, kind;
  bool known = false;      // named and described by the known codes table
  bool runnable = true;    // this game can run it online
  std::string why_not;
};
bool asking();
std::vector<Incoming> incoming();
void answer(const std::vector<std::string>& accepted_ids);
// One line for the character select ("Gecko codes this match: 2", "Waiting for your opponent's answer").
std::string status();

// A network match's first frame: decide from the two locks. Then the codes both games run (empty: none).
void match_started();
void match_ended();
const std::vector<gecko_online::Lines>& match_codes();

}  // namespace gecko_session
