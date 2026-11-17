// Arena room rules: who is in the room, who fights, who is next, and what a finished game or a
// button press changes. Plain data and plain functions: no sockets, no clock, no game state. One
// member (the owner, the lowest id present) applies every change and sends the room to the others,
// so every function here is deterministic and checks what it is given.
//
// The ring holds the fighters: two in singles (slots 0 and 1), four in doubles (team A in slots 0 and
// 1, team B in slots 2 and 3). The line holds members waiting to play, front first. Everyone else is
// watching. The lineup changes only when a game ends.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace slippi::arena {

constexpr int kMaxMembers = 16;
// Players one room takes (the host refuses the next one). The rules and the wire handle kMaxMembers;
// rooms ship with the size that real games proved.
constexpr int kRoomLimit = 8;
constexpr uint8_t kNone = 0xFF;
constexpr size_t kMaxCode = 10, kMaxName = 31;
constexpr int kRingMax = 4;

enum class Mode : uint8_t {
  WinnerStays = 0,   // the losers go to the back of the line, the front of the line plays the winners
  TakeTurns = 1,     // whoever has been in longest steps out (in a fresh ring, the loser)
  Ask = 2,           // nothing changes on its own
};
enum class Answer : uint8_t { No = 0, Yes = 1, YesAndPlayWinner = 2 };
enum class Role : uint8_t { Absent, Fighter, InLine, Watching };

struct Member {
  uint8_t id = kNone;
  std::string code;    // Slippi connect code
  std::string name;
  uint16_t wins = 0, losses = 0;
  uint8_t character = kNone;   // picked on the character select (Slippi's external id), kNone not yet
  uint8_t costume = 0;
};

struct Room {
  uint32_t rev = 0;                    // counts up with every change
  Mode mode = Mode::WinnerStays;
  bool doubles = false;                // four fighters in two teams
  std::vector<Member> members;         // in the order they joined
  uint8_t ring[kRingMax] = {kNone, kNone, kNone, kNone};
  uint16_t tenure[kRingMax] = {0, 0, 0, 0};   // games in a row each has finished in the ring
  std::vector<uint8_t> line;           // waiting to play, front first
  uint32_t games = 0;                  // games finished with a winner
  uint8_t streak_id = kNone;           // the member (singles) or team leader (doubles) on a streak
  uint16_t streak = 0;
  bool in_game = false;
  // A member who is not fighting asks to play next, and which fighters have said no.
  uint8_t asker = kNone;
  uint8_t refused = 0;                 // bit per ring slot
  uint8_t declined = kNone;            // whose ask every fighter said no to (shown until the game ends)
  // Decided for the next game: a fighter gives up their spot. A voluntary change replaces the mode's
  // own rotation for that game.
  uint8_t leaving_slot = kNone;
  uint8_t incoming = kNone;            // who takes it; kNone: the front of the line then
  bool leaver_plays_winner = false;    // the one leaving goes to the front of the line
  bool play_winner_next = false;       // after the next finished game the losers go out, whatever the mode
  // Switching between singles and doubles: everyone who would play must say yes.
  bool switch_pending = false;         // a switch to !doubles has been proposed
  uint16_t switch_votes = 0;           // bit per member id that said yes
};

// ---- reading ----
const Member* find(const Room& room, uint8_t id);
// A member's pick on the character select, for the others' panels. False when nothing changed.
bool set_character(Room& room, uint8_t id, uint8_t character, uint8_t costume);
Role role(const Room& room, uint8_t id);
int ring_size(const Room& room);                   // 2 or 4
int ring_slot(const Room& room, uint8_t id);       // the slot, or -1
int team_of_slot(const Room& room, int slot);      // singles: the slot; doubles: 0 for slots 0-1, 1 for 2-3
int line_place(const Room& room, uint8_t id);      // 0 is next, -1 not in line
uint8_t owner(const Room& room);                   // the lowest id present, kNone in an empty room
uint8_t next_up(const Room& room);                 // who comes in at the next change, kNone if nobody
// The members a proposed switch needs a yes from: in singles to doubles, the two fighters and the
// first two in line; in doubles to singles, the four fighters. Empty when a switch is not possible.
std::vector<uint8_t> switch_voters(const Room& room);
// True when the room obeys its own rules (ids unique and in range, ring and line hold members, nobody
// twice, pending choices name people who are there). decode() refuses a room that is not.
bool valid(const Room& room);

// ---- changes (each returns whether anything changed, and bumps rev when it did) ----
// The new member's id, or kNone when the room is full or the code is already in it. A free ring
// slot is filled; otherwise the new member joins the back of the line.
uint8_t join(Room& room, const std::string& code, const std::string& name);
bool leave(Room& room, uint8_t id);
bool set_mode(Room& room, Mode mode);
// A spectator steps into the line, or out of it to just watch. Fighters are not moved by this.
bool toggle_line(Room& room, uint8_t id);
// A member who is not fighting asks to play the next game.
bool ask(Room& room, uint8_t id);
// A fighter answers the ask. The first yes gives up that fighter's spot: at once between games,
// at the end of the game during one.
bool answer(Room& room, uint8_t fighter, Answer answer);
// A fighter offers their spot to the front of the line without being asked (at once between games,
// at the end of the game during one); again, during the game, takes it back.
bool give_up(Room& room, uint8_t fighter, bool play_winner = false);
// Singles to doubles, or back. Anyone in the room may propose; the switch happens when every voter
// has said yes (immediately between games, otherwise when the game ends). A no ends the proposal.
bool propose_switch(Room& room, uint8_t id);
bool vote_switch(Room& room, uint8_t id, bool yes);
bool game_start(Room& room);
// winner_team 0 or 1 (in singles, the winning slot), or kNone for a game with no winner (quit out,
// lost connection): nothing moves then.
bool game_end(Room& room, uint8_t winner_team);

// ---- wire form ----
std::vector<uint8_t> encode(const Room& room);
bool decode(const uint8_t* data, size_t size, Room& out);

// ---- the live feed and the links ----
// Who sends the live feed to whom: for every member who is not fighting, the member they get it
// from (a fighter, or for bigger rooms another watcher who passes it on). Each sender serves at most
// `fanout` members. Deterministic; fighters are kNone.
std::vector<std::pair<uint8_t, uint8_t>> feed_tree(const Room& room, int fanout = 3);   // (member, from)
int feed_hops(const Room& room, uint8_t id, int fanout = 3);   // 0 for a fighter, -1 absent
// The connections the room needs for the next game: the fighters with each other, and every feed
// edge. Each pair is (lower id, higher id), sorted, no duplicates. Room messages travel the same
// links (each member passes a newer room on), which keeps everyone reachable.
std::vector<std::pair<uint8_t, uint8_t>> wanted_links(const Room& room, int fanout = 3);

}  // namespace slippi::arena
