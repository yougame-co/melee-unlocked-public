// Window/GPU-free screenshot protocol and file policy. Apple host only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "embed_mac_pixels.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace mac_embed::shot {
constexpr uint64_t max_id = 9007199254740991ULL; // JavaScript safe integer
constexpr size_t max_png = 16 * 1024 * 1024;
inline std::optional<uint64_t> parse(const std::string& line) {
  constexpr size_t start = 11; // "screenshot " (embed prefix already consumed)
  if (line.compare(0, start, "screenshot ") || line.size() <= start || line[start] == '0') return {};
  uint64_t id = 0;
  for (size_t i = start; i < line.size(); ++i) {
    const unsigned char c = line[i];
    if (c < '0' || c > '9' || id > (max_id - (c - '0')) / 10) return {};
    id = id * 10 + c - '0';
  }
  return id;
}
inline bool size_ok(uint32_t w, uint32_t h) {
  return w && h && w <= 8192 && h <= 8192 && uint64_t(w) * h <= 16000000;
}
inline bool source_ok(int w, int h) {
  return w > 0 && h > 0 && w <= 32768 && h <= 32768 && uint64_t(w) * h <= 64000000;
}
inline std::string quote(const std::string& value) {
  std::string out = "\"";
  const char* hex = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += char(c);
  }
  return out + '"';
}
inline std::string error(uint64_t id, const char* why) {
  return "yg {\"type\":\"screenshot\",\"id\":" + std::to_string(id) + ",\"error\":" + quote(why) + "}";
}
inline std::string path(const std::string& dir, uint64_t id) {
  if (dir.empty() || dir[0] != '/' || dir.size() > 3000 || !id || id > max_id ||
      dir.find('\0') != std::string::npos) return {};
  if (dir != "/") {
    if (dir.back() == '/') return {};
    size_t start = 1;
    while (start < dir.size()) {
      auto end = dir.find('/', start);
      auto part = dir.substr(start, end - start);
      if (part.empty() || part == "." || part == "..") return {};
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  return dir + (dir == "/" ? "" : "/") + "shot-" + std::to_string(id) + ".png";
}
inline std::optional<PixelSize> png_size(const std::vector<uint8_t>& png) {
  static constexpr uint8_t signature[] = {137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R'};
  if (png.size() < 33 || png.size() > max_png || std::memcmp(png.data(), signature, 16)) return {};
  auto u32 = [&](int at) { return uint32_t(png[at]) << 24 | uint32_t(png[at+1]) << 16 |
                                uint32_t(png[at+2]) << 8 | png[at+3]; };
  const auto w = u32(16), h = u32(20);
  if (!size_ok(w, h)) return {};
  return PixelSize{int(w), int(h)};
}
inline std::string success(uint64_t id, const std::string& file, const std::vector<uint8_t>& png) {
  const auto size = png_size(png);
  if (!size) return {};
  auto line = "yg {\"type\":\"screenshot\",\"id\":" + std::to_string(id) + ",\"path\":" + quote(file) +
              ",\"w\":" + std::to_string(size->w) + ",\"h\":" + std::to_string(size->h) + "}";
  return line.size() + 1 <= 4096 ? line : std::string{};
}
// Own only the entry created with O_EXCL. Cancellation can unlink it while the worker
// writes to its open inode; the worker alone closes that fd. No file I/O lock spans encoding
// or a data write, so shutdown need not wait for either to remove a partial file.
class Output {
  std::mutex mutex_;
  bool cancelled_ = false, owned_ = false;
  int root_ = -1;
  std::string name_;
  struct stat original_{};
  void remove_locked() {
    struct stat child{};
    if (owned_ && !fstatat(root_, name_.c_str(), &child, AT_SYMLINK_NOFOLLOW) &&
        child.st_dev == original_.st_dev && child.st_ino == original_.st_ino)
      unlinkat(root_, name_.c_str(), 0);
    owned_ = false;
  }
 public:
  ~Output() { if (root_ >= 0) close(root_); }
  void cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true; remove_locked();
  }
  template<class Write> const char* write(const std::string& dir, uint64_t id,
                                         const std::vector<uint8_t>& png, Write write_bytes) {
    const auto file = path(dir, id);
    if (file.empty() || success(id, file, png).empty()) return "refused";
    char* canonical = realpath(dir.c_str(), nullptr);
    const bool exact = canonical && dir == canonical;
    std::free(canonical);
    if (!exact) return "refused";
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cancelled_) return "failed";
      root_ = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (root_ < 0) return "refused";
      name_ = "shot-" + std::to_string(id) + ".png";
      struct stat parent{}, named{};
      if (fstat(root_, &parent) || lstat(dir.c_str(), &named) ||
          parent.st_dev != named.st_dev || parent.st_ino != named.st_ino) return "refused";
      fd = openat(root_, name_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0) return "refused";
      int status;
      do { status = fstat(fd, &original_); } while (status && errno == EINTR);
      if (status) {
        // Nothing has been written. Remove the entry our exclusive open just made,
        // even if the filesystem cannot supply its identity for later cancellation.
        unlinkat(root_, name_.c_str(), 0); close(fd); return "failed";
      }
      owned_ = true;
      if (!S_ISREG(original_.st_mode) || original_.st_nlink != 1) {
        close(fd); remove_locked(); return "failed";
      }
    }
    bool ok = true;
    size_t at = 0;
    while (ok && at < png.size()) {
      { std::lock_guard<std::mutex> lock(mutex_); if (cancelled_) { ok = false; break; } }
      const ssize_t n = write_bytes(fd, png.data() + at, std::min(size_t(65536), png.size() - at));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) ok = false; else at += size_t(n);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    struct stat finished{}, child{};
    ok = ok && !cancelled_ && !fstat(fd, &finished) && finished.st_nlink == 1 && finished.st_size == off_t(png.size()) &&
         !fstatat(root_, name_.c_str(), &child, AT_SYMLINK_NOFOLLOW) && S_ISREG(child.st_mode) && child.st_nlink == 1 &&
         child.st_dev == original_.st_dev && child.st_ino == original_.st_ino;
    if (close(fd)) ok = false;
    if (!ok) remove_locked();
    return ok ? nullptr : "failed";
  }
  const char* write(const std::string& dir, uint64_t id, const std::vector<uint8_t>& png) {
    return write(dir, id, png, [](int fd, const uint8_t* bytes, size_t count) { return ::write(fd, bytes, count); });
  }
};
inline const char* write(const std::string& dir, uint64_t id, const std::vector<uint8_t>& png) {
  Output output;
  return output.write(dir, id, png);
}
constexpr int64_t deadline_ms = 1500;
struct Request {
  uint64_t id, ticket;
  int64_t deadline;
  bool claimed = false; // Slot mutex only
  std::atomic<bool> cancelled{false};
  Output output;
  Request(uint64_t id, uint64_t ticket, int64_t now) : id(id), ticket(ticket), deadline(now + deadline_ms) {}
  void cancel() { cancelled.store(true); output.cancel(); }
};
class Slot {
  std::atomic<bool> requested_{false}, accepting_{true};
  std::mutex mutex_;
  uint64_t serial_ = 0;
  std::shared_ptr<Request> active_;
  template<class Emit> void fail_locked(Emit emit) {
    active_->cancel();
    emit(error(active_->id, "failed"));
    active_.reset(); requested_.store(false, std::memory_order_relaxed);
  }
 public:
  // This is the ONLY screenshot operation on an idle presentation.
  bool requested() const { return requested_.load(std::memory_order_relaxed); }
  template<class Emit> std::shared_ptr<Request> begin(uint64_t id, int64_t now, Emit emit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_.load() || !id || id > max_id) return {};
    if (active_) { emit(error(id, "busy")); return {}; }
    active_ = std::make_shared<Request>(id, ++serial_, now);
    requested_.store(true, std::memory_order_relaxed);
    return active_;
  }
  uint64_t take() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_.load() || !active_ || active_->claimed) return 0;
    active_->claimed = true; requested_.store(false, std::memory_order_relaxed);
    return active_->ticket;
  }
  // Enqueue under the same lock as stop_accepting, so the shutdown drain covers
  // every accepted submission, including callbacks already copied by the reader.
  template<class Enqueue> void work(uint64_t ticket, Enqueue enqueue) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (accepting_.load() && active_ && active_->ticket == ticket) enqueue(active_);
  }
  template<class Emit> void finish(uint64_t ticket, int64_t now, const std::string& reply, bool ok, Emit emit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || active_->ticket != ticket) return; // expired or superseded
    if (now >= active_->deadline) { fail_locked(emit); return; }
    if (!ok) active_->cancel();
    emit(reply);
    active_.reset(); requested_.store(false, std::memory_order_relaxed);
  }
  template<class Emit> void expire(uint64_t ticket, int64_t now, Emit emit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && active_->ticket == ticket && now >= active_->deadline) fail_locked(emit);
  }
  void stop_accepting() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_.store(false); requested_.store(false, std::memory_order_relaxed);
  }
  template<class Emit> void cancel(Emit emit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) fail_locked(emit);
  }
};
// GL uses this before allocating either the intermediate color image or PBO.
inline PixelSize readback_size(int w, int h) {
  return source_ok(w, h) ? capture_size(w, h, true) : PixelSize{};
}
// GPU bytes -> upright packed RGB, then the existing Retina cap (always enabled for this contract).
inline PixelSize rgb(std::vector<uint8_t>& out, const uint8_t* pixels, int w, int h, size_t pitch, bool bgra, bool bottom_up) {
  if (!pixels || !source_ok(w, h) || pitch < size_t(w) * 4) return {};
  out.resize(size_t(w) * h * 3);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    const auto* in = pixels + size_t(bottom_up ? h - 1 - y : y) * pitch + x * 4;
    auto* dest = out.data() + (size_t(y) * w + x) * 3;
    dest[0] = in[bgra ? 2 : 0]; dest[1] = in[1]; dest[2] = in[bgra ? 0 : 2];
  }
  return limit_capture(out, w, h, true);
}
} // namespace mac_embed::shot
