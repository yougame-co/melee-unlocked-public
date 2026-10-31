// Keep the saved backend choice visible when this host uses another graphics API.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace app {
inline std::string settings_backend_label(const std::string& saved, const std::string& actual) {
  return saved == actual ? saved : "saved " + saved + ", using " + actual;
}
inline const char* settings_backend_in_use(const char* saved) {
#ifdef __APPLE__
  (void)saved;
  return "OpenGL";   // the Mac host uses port/linux/full/gx_backend_linux.cpp
#else
  return saved;
#endif
}
}  // namespace app
