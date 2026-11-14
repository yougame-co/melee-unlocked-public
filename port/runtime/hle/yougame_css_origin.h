// Canonical Friends origin: same-process only, independent of match rollback and relaunch.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_savestate.h"
#include <chrono>
#include <functional>

namespace yougame::css_origin {
enum class Reset { restored, retry, unavailable };
struct Services {
  std::function<bool()> disc_quiet;
  std::function<void()> restore_excluded, flush_gpu, reset_audio, mount_card, reset_host;
};
// A refused disc/callback gate is mutation-free. The caller retains its request and retries at
// the next engine body. Resume the C scene stack only AFTER this function has returned.
inline Reset reset_to_boot(const native_savestate::Snapshot& boot, const Services& services,
                           double& reset_ms) {
  if (!boot.ready()) return Reset::unavailable;
  if (!services.disc_quiet()) return Reset::retry;
  const auto began = std::chrono::steady_clock::now();
  boot.restore();
  services.restore_excluded();
  services.flush_gpu();
  services.reset_audio();
  services.mount_card();
  services.reset_host();
  reset_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now()-began).count();
  return Reset::restored;
}
} // namespace yougame::css_origin
