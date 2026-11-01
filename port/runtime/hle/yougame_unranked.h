// YouGame Unranked keeps the ordinary Slippi CSS/rematch loop, including after disconnect.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace yougame::unranked {
inline bool keep_css(bool active, int mode) { return active && mode == 1; }
// Ranked (including local Ranked practice) owns its CSS result reveal. Never
// let either Training setting replace it. Unranked's existing policy is unchanged.
inline bool allow_training_return(bool active, int mode) { return mode != 0 && !keep_css(active, mode); }
} // namespace yougame::unranked
