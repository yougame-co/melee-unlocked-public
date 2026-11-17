// Gecko codes in network matches (HANDOFF-GECKO-ONLINE.md). The pure parts: what identifies a code,
// what kind it is, which codes a match runs, and the messages two Melee Unlocked games exchange.
//
// Between two Melee Unlocked players in Direct or an arena, each offers the codes it has on and
// answers which of the other's it accepts. The match runs every code one side offered and the other
// accepted, on both games, so both compute the same frames. Each side sends the fingerprint of the
// set it will run; when they differ, or an answer does not come, the match runs no codes. A code is
// named and described from the known codes table by its lines, never by the name the sender gave it.
// Against Slippi Dolphin (no build message) and in Unranked only display codes from that table run.
// Ranked runs none.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gecko_online {

using Lines = std::vector<std::pair<uint32_t, uint32_t>>;

// A code's identity: its lines, hashed (16 hex digits). Names and notes do not count.
std::string code_id(const Lines& lines);

enum class Kind : uint8_t {
  Display,    // changes only what this player's screen shows, nothing any player can use (colours, HUD, music)
  Info,       // shows what the other player cannot see (hitboxes, frame data, inputs): consent only
  Gameplay,   // changes the match itself: both games must run it
  Unknown,    // not in the table: treated as gameplay
};
const char* kind_label(Kind k);   // "display only", "shows extra information", "changes gameplay", "unknown code"

struct Known { const char* id; const char* name; const char* what; Kind kind; };
// The table entry for these lines, or null.
const Known* known(const Lines& lines);

// One code offered to the other player.
struct Offer { std::string id; std::string sent_name; Lines lines; };

// Limits on what one side offers (a message stays small; a long list is not read anyway).
constexpr size_t kMaxCodes = 16, kMaxLines = 64;

// The codes a match runs: every id of `mine` the other side accepted and every id of `theirs` this
// side accepted, sorted and without repeats. Both games compute the same list from the same answers.
std::vector<std::string> agreed(const std::vector<std::string>& mine, const std::vector<std::string>& they_accepted,
                                const std::vector<std::string>& theirs, const std::vector<std::string>& i_accepted);
// The fingerprint both sides send before the match (16 hex digits; "none" for an empty set).
std::string set_fingerprint(const std::vector<std::string>& ids);

// Which codes may run in a match of this kind, before any answer (the safe list).
enum class Match : uint8_t { Offline, Consent, SafeOnly, None };
// direct_or_arena: a Direct or arena connection; peer_is_mu: the opponent sent a Melee Unlocked build
// message; ranked: a Ranked queue.
Match match_policy(bool online, bool direct_or_arena, bool peer_is_mu, bool ranked);
// Whether a code runs on its own (no answer needed) under that policy.
bool runs_without_consent(Match m, const Lines& lines);

// The messages (the payload after the message type byte).
//   offer:  'M' 'U' 'G' 1, kind 0, count, then per code: line count, lines (8 bytes each, big endian), name length, name
//   answer: 'M' 'U' 'G' 1, kind 1, count, then per accepted code id: 16 ASCII hex digits
//   final:  'M' 'U' 'G' 1, kind 2, 16 ASCII hex digits (set_fingerprint)
//   lock:   'M' 'U' 'G' 1, kind 3, 16 ASCII hex digits: sent just before every character select
//           selection message, on the same ordered channel. A match starts only after both sides'
//           last selections arrived, so at its first frame each game holds the fingerprint the
//           other last sent and the one it last sent itself; codes run only when the two are equal
//           and not "none". Both games compare the same two values, so both decide the same.
std::vector<uint8_t> encode_offer(const std::vector<Offer>& codes);
std::vector<uint8_t> encode_answer(const std::vector<std::string>& accepted_ids);
std::vector<uint8_t> encode_final(const std::string& fingerprint);
std::vector<uint8_t> encode_lock(const std::string& fingerprint);
struct Message { int kind = -1; std::vector<Offer> offer; std::vector<std::string> ids; std::string fingerprint; };
// False for anything malformed or over the limits (then nothing from it is used).
bool decode(const uint8_t* data, size_t size, Message* out);

}  // namespace gecko_online
