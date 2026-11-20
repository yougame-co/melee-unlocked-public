// Shipped helper programs (bin/glslangValidator and bin/spirv-cross on Linux, Resources/bin on Mac)
// need their exec bit. Some unzip tools drop file modes, and the game then stopped with "Cannot run
// glslangValidator: Permission denied". Give the bit back for this user, or say what to do.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifndef _WIN32
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
namespace helper_exec {
// `shown` is how the player sees the tool, e.g. "bin/glslangValidator".
inline std::string not_executable_message(const std::string& shown) {
  return shown + " is not executable: unzip Melee Unlocked again with a tool that keeps file permissions, "
                 "or run chmod +x " + shown;
}
// For an existing helper file: "" when it can run (already, or after adding owner_exec; then `note`
// gets one line to log), else the plain message above. One access() call when nothing is wrong.
inline std::string ensure_executable(const std::filesystem::path& path, const std::string& shown, std::string* note) {
  if (access(path.c_str(), X_OK) == 0) return {};
  std::error_code ec;
  std::filesystem::permissions(path, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add, ec);
  if (!ec && access(path.c_str(), X_OK) == 0) {
    if (note) *note = shown + " had no exec permission (unzipped without file modes); made it executable";
    return {};
  }
  return not_executable_message(shown);
}
}  // namespace helper_exec
#endif
