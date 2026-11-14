// Window-free time sync decisions shared by the online flow (slippi_online.cpp) and its test.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace slippi::time_sync {

constexpr int32_t kFrameUs = 16683;   // one console frame at 59.94 Hz
constexpr int32_t kSlackUs = 10000;   // offset left alone
constexpr int32_t kOpeningFrames = 120;
constexpr int kMaxAdvanceFrames = 3;

// Frames this side runs extra when it is behind (offset < 0), one every five frames. Unchanged from
// Slippi Dolphin: more than one frame plus the slack behind, up to three frames once the match has
// started, none before. The count rounds up, so on its own it already covers the whole gap.
inline int AdvanceFrames(int32_t offset_us, int32_t frame) {
  if (offset_us >= -(kFrameUs + kSlackUs)) return 0;
  const int max_frames = frame > kOpeningFrames ? kMaxAdvanceFrames : 0;
  return std::min((-offset_us - kSlackUs) / kFrameUs + 1, max_frames);
}

// Frames this side holds when it is ahead (offset > 0). While the match starts, any offset over the
// slack, up to five frames, so both games begin in step.
//
// After that the side that is behind closes the gap itself (AdvanceFrames), and the offsets the two
// sides measure mirror each other. Holding a frame here as well corrected one gap twice: in a real
// match over a Wi-Fi link (2026-11-06), each time the laptop stalled about three frames on a late
// packet, the wired side held one frame while the laptop ran three extra, which left the laptop a
// frame ahead and closer to its next stall. So after the opening a frame is held only for a gap
// the other side's capped advance cannot cover (more than three frames plus one), as a backstop for
// a peer that cannot catch up. Slippi Dolphin holds nothing after frame 120 (since 2023-08).
inline int HaltFrames(int32_t offset_us, int32_t frame) {
  if (frame <= kOpeningFrames) {
    if (offset_us <= kSlackUs) return 0;
    return std::min((offset_us - kSlackUs) / kFrameUs + 1, 5);
  }
  return offset_us > kSlackUs + (kMaxAdvanceFrames + 1) * kFrameUs ? 1 : 0;
}

}  // namespace slippi::time_sync
