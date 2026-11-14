// Shared graphics-only startup result. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <stdexcept>
namespace gx {
struct PrewarmStats {
  const char* backend = nullptr;
  uint64_t recipes = 0, cache_hits = 0, compiled = 0;
};
struct UnsupportedPrewarmBackend : std::runtime_error {
  using std::runtime_error::runtime_error;
};
}
