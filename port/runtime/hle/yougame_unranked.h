// YouGame Unranked keeps the ordinary Slippi CSS/rematch loop, including after disconnect.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace yougame::unranked {
inline bool keep_css(bool active, int mode) { return active && mode == 1; }
// Ranked (including local Ranked practice) owns its CSS result reveal. Never
// let either Training setting replace it. Unranked's existing policy is unchanged.
inline bool allow_training_return(bool active, int mode) { return mode != 0 && !keep_css(active, mode); }

// The Unranked character select's "Opponent left" note: the opponent's match link dropped while this side
// was in a YouGame Unranked session with them (on the character select or in a game), and this side's own
// room connection did not die with it (then it was most likely our own connection). The player's own
// leave (holding Z, clearing an error, backing out) and a normal finished game never come here.
inline bool opponent_left(bool session, int mode, bool friend_room, bool was_connected, bool own_drop) {
  return session && mode == 1 && !friend_room && was_connected && !own_drop;
}

// Watches for "the online session is over, with a match played", once per tick. A Friends or Doubles
// lobby never counts: its select is the place to be after a match, and leaving it is the player's own
// choice. It also keeps a search open between matches, so a session reads as running across them, and
// reads as over for a moment while the next match connects (the search has ended, the match not begun).
class SessionWatch {
public:
  bool tick(bool session, bool match_running, bool lobby) {
    if (session) {
      seen_ = true;
      played_ |= match_running;
      lobby_ |= lobby;
      return false;
    }
    if (!seen_) return false;
    const bool over = played_ && !lobby_ && !lobby;
    seen_ = played_ = lobby_ = false;
    return over;
  }

private:
  bool seen_ = false, played_ = false, lobby_ = false;
};
} // namespace yougame::unranked
