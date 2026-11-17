// Metal-owned temporary cache files; never remove a final archive/recipe/MSL.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace gx::metal {
class TemporaryFiles {
 public:
  TemporaryFiles(std::initializer_list<std::filesystem::path> paths) : paths_(paths) {}
  TemporaryFiles(const TemporaryFiles&) = delete;
  TemporaryFiles& operator=(const TemporaryFiles&) = delete;
  ~TemporaryFiles() noexcept {
    // Runs on the owning worker, including exception/cancellation unwind.
    // Never wait for this cleanup on the renderer's quit path.
    for (const auto& path : paths_) {
      try { std::error_code ec; std::filesystem::remove(path, ec); } catch (...) {}
    }
  }
 private:
  std::vector<std::filesystem::path> paths_;
};

inline bool owned_temporary_name(std::string_view name) {
  auto take = [&](std::string_view& part) {
    const auto dot = name.find('.');
    if (dot == name.npos) return false;
    part = name.substr(0, dot); name.remove_prefix(dot + 1); return true;
  };
  auto digits = [](std::string_view part, bool hex = false) {
    if (part.empty()) return false;
    for (char c : part)
      if (!(c >= '0' && c <= '9') && !(hex && c >= 'a' && c <= 'f')) return false;
    return true;
  };
  std::string_view base, pid, context;
  if (!take(base) || !take(pid) || !take(context) || !digits(pid) || pid == "0") return false;
  if (base == "recipes") return context.size() == 16 && digits(context, true) && name == "pending.bin";
  if (base.size() == 11 && base.substr(0, 10) == "pipelines-" && base[10] >= '0' && base[10] <= '3')
    return context.size() == 16 && digits(context, true) && name == "pending.metalar";
  if (base.size() != 19 || (base.substr(0, 3) != "vs-" && base.substr(0, 3) != "ps-") ||
      !digits(base.substr(3), true) || !digits(context)) return false;
  return name == "hlsl" || name == "spv" || name == "metal" || name == "log";
}

// Call only with our writable msl-3 directory, never the seed/bundle or its
// parent. No recursion and no symlink traversal. Errors are best effort.
inline size_t prune_temporary_files(const std::filesystem::path& cache) {
  size_t removed = 0;
  const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
  std::error_code ec;
  for (std::filesystem::directory_iterator it(cache, ec), end; !ec && it != end; it.increment(ec)) {
    const auto& entry = *it;
    if (!owned_temporary_name(entry.path().filename().string())) continue;
    const auto status = entry.symlink_status(ec);
    if (ec) { ec.clear(); continue; }
    if (!std::filesystem::is_regular_file(status)) continue;
    const auto modified = entry.last_write_time(ec);
    if (ec) { ec.clear(); continue; }
    if (modified >= cutoff) continue;
    if (std::filesystem::remove(entry.path(), ec)) ++removed;
    ec.clear();
  }
  return removed;
}
} // namespace gx::metal
