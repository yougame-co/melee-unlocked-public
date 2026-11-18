// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace host {
// The shared-mode engine period to ask for (IAudioClient3), in device frames. The shortest one a
// driver offers is not taken below kMinSharedPeriodMs: a 2 ms period leaves the device a 4.4 ms
// buffer, so one late wake-up of the output thread is a dropout. Every report on a 96 kHz 2 ms
// endpoint crackled (1,200 to 15,700 gaps a minute, 11-17) while 10 ms endpoints were near zero.
// Auto's queue floor is 10 ms either way (period + 4 ms), so periods up to 6 ms cost no queue.
// The period stays a multiple of the driver's fundamental, inside its range, as Windows requires.
constexpr uint32_t kMinSharedPeriodMs = 6;
inline uint32_t shared_period_frames(uint32_t minimum, uint32_t fundamental, uint32_t maximum, uint32_t rate,
                                     uint32_t min_ms = kMinSharedPeriodMs) {
  if (!minimum || !rate) return minimum;
  const uint64_t wanted = ((uint64_t)rate * min_ms + 999) / 1000;
  if (minimum >= wanted) return minimum;
  const uint32_t step = fundamental ? fundamental : minimum;
  uint64_t period = minimum + (wanted - minimum + step - 1) / step * step;
  if (maximum >= minimum && period > maximum) period = maximum - (maximum - minimum) % step;
  return (uint32_t)std::max<uint64_t>(period, minimum);
}

// Output buffering only. It never changes the game's mixer, alarms or rollback state.
struct AudioBufferPolicy {
  bool adaptive = true;
  uint32_t floor_ms = 40;
  uint32_t target_ms = 40;
  uint64_t clean_frames = 0;

  // Modes: 0 = Auto (adaptive: starts at the given buffer, grows after real gaps, settles back),
  // 1 = Low latency (fixed), 2 = Exclusive (fixed; the device is opened in WASAPI exclusive mode).
  void configure(int mode, int milliseconds) {
    adaptive = mode == 0;
    floor_ms = (uint32_t)std::clamp(milliseconds, adaptive ? 10 : 5, 120);
    target_ms = floor_ms;
    clean_frames = 0;
  }
  // The Exclusive UI has no buffer slider. Do not inherit the hidden default or a saved
  // Low-latency target; use the endpoint floor already chosen from its period and scheduling slack.
  void configure_for_output(int mode, int milliseconds, int device_floor_ms) {
    configure(mode, mode == 2 ? device_floor_ms : std::max(milliseconds, device_floor_ms));
  }
  bool gap(uint64_t frames, uint32_t sample_rate) {
    clean_frames = 0;
    if (!adaptive || !frames || frames > (uint64_t)sample_rate * 60 / 1000) return false;
    const auto before = target_ms;
    target_ms = std::min(120u, target_ms + 4);
    return target_ms != before;
  }
  void clean(uint32_t frames, uint32_t sample_rate) {
    clean_frames += frames;
    if (adaptive && clean_frames >= (uint64_t)sample_rate * 10 && target_ms > floor_ms) {
      --target_ms;
      clean_frames = 0;
    }
  }
};
} // namespace host
