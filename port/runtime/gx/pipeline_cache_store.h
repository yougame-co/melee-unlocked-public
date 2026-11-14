// Saves the shader and pipeline caches while the game runs, on a thread of their own.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The pipeline caches used to be written only when the renderer was destroyed. A Friends join
// relaunches the game, and the desktop app ends a game that has not exited 5 s after it asked, so
// the save at the end often never happened: the next launch compiled every pipeline again (20 s
// and more on a first start). Each backend now registers what it keeps with a Saver:
//
//   - The backend marks the saver dirty whenever something new is worth keeping (any thread,
//     cheap). The saver thread writes once things have been quiet for a moment, or after a few
//     seconds at most while pipelines keep arriving. No frame waits on the disk. During a match
//     the backend holds timed saves (hold()), so the next menu writes them.
//   - Every file is written to a temporary name and renamed over the old one, so a game that is
//     killed half way through a save keeps the previous complete file.
//   - flush_all() asks every live saver to write now and waits (bounded); request_flush_all() and
//     wait_flush_all() split it, so an exit can send its Friends Leave while the caches are written.
//   - freeze() (crash handlers, die(), a lost device) stops every save and rename from then on.
//
// Header-only on purpose: every build (Windows, Linux, Mac and their test targets) picks it up
// without another source file in its list.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace gx::cache_store {

enum class FlushResult { Ok, Failed, Timeout };
inline const char* flush_result_name(FlushResult result) {
  return result == FlushResult::Ok ? "ok" : result == FlushResult::Timeout ? "timeout" : "failed";
}

// Set when the process is ending badly (a crash, a lost device, die()): no save starts and no file
// is renamed into place from then on, so a cache read from a broken device or heap never replaces a
// good one. A lock-free atomic store and nothing else, so crash and signal handlers may call it.
inline std::atomic<bool> g_frozen{false};
static_assert(std::atomic<bool>::is_always_lock_free, "freeze() runs inside crash handlers");
inline void freeze() noexcept { g_frozen.store(true); }
inline bool frozen() noexcept { return g_frozen.load(); }

// The bytes a producer hands over. It keeps its storage from one save to the next and never clears
// what it grows into, so a 100 MB pipeline library is copied straight into it each time, not into a
// fresh zero-filled allocation.
class Buffer {
 public:
  uint8_t* data() { return data_.get(); }
  const uint8_t* data() const { return data_.get(); }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool empty() const { return size_ == 0; }
  void clear() { size_ = 0; }
  // Bytes past the old size are left as they are. Growing leaves a quarter more room, so a cache
  // that grew a little since the last save fits without another allocation.
  void resize(size_t size) {
    if (size > capacity_) {
      const size_t capacity = size + size / 4;
      std::unique_ptr<uint8_t[]> grown(new uint8_t[capacity]);   // default-initialized: not zeroed
      if (size_) std::memcpy(grown.get(), data_.get(), size_);
      data_ = std::move(grown);
      capacity_ = capacity;
    }
    size_ = size;
  }
  void assign(const void* bytes, size_t size) {
    resize(size);
    if (size) std::memcpy(data_.get(), bytes, size);
  }

 private:
  std::unique_ptr<uint8_t[]> data_;
  size_t size_ = 0, capacity_ = 0;
};

namespace detail {
// A rename Windows refuses for a moment: an antivirus scanning the new file, or the relaunched game
// reading the old one (access denied, sharing violation, lock violation).
inline bool rename_busy(const std::error_code& ec) {
#ifdef _WIN32
  if (ec.category() == std::system_category() && (ec.value() == 5 || ec.value() == 32 || ec.value() == 33)) return true;
  return ec == std::errc::permission_denied || ec == std::errc::device_or_resource_busy;
#else
  return ec == std::errc::device_or_resource_busy;
#endif
}
// Large files (a pipeline library, a Vulkan cache) reach the disk before the rename, as the old
// D3D12 code did with MOVEFILE_WRITE_THROUGH: a power cut right after leaves the old file or the new
// one, never an empty one. Small files (a GL program per link) skip it. Best effort.
constexpr size_t kSyncFrom = size_t(1) << 20;
inline void sync_file(FILE* file) {
#ifdef _WIN32
  (void)_commit(_fileno(file));
#else
  (void)fsync(fileno(file));
#endif
}
// "<target>.<pid>-<n>.tmp", the names write_file_atomic gives its temporaries. With target empty,
// any target.
inline bool temporary_of(const std::string& name, const std::string& target) {
  if (name.size() < 4 || name.compare(name.size() - 4, 4, ".tmp") != 0) return false;
  const std::string stem = name.substr(0, name.size() - 4);
  const size_t dot = stem.rfind('.');
  if (dot == std::string::npos || dot == 0 || (!target.empty() && (dot != target.size() || stem.compare(0, dot, target) != 0)))
    return false;
  const std::string serial = stem.substr(dot + 1);
  const size_t dash = serial.find('-');
  auto digits = [](const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  return dash != std::string::npos && digits(serial.substr(0, dash)) && digits(serial.substr(dash + 1));
}
inline void remove_stale(const std::filesystem::path& folder, const std::string& target, std::chrono::seconds older_than) {
  std::error_code ec;
  const auto now = std::filesystem::file_time_type::clock::now();
  for (std::filesystem::directory_iterator it(folder.empty() ? std::filesystem::path(".") : folder, ec), end; !ec && it != end; it.increment(ec)) {
    if (!temporary_of(it->path().filename().string(), target)) continue;
    std::error_code time_ec;
    const auto written = std::filesystem::last_write_time(it->path(), time_ec);
    if (!time_ec && now - written > older_than) std::filesystem::remove(it->path(), time_ec);
  }
}
}  // namespace detail

// Writes data to path through a temporary file in the same folder and a rename, so a reader (or the
// next launch) sees the old complete file or the new complete one, never a cut-off one.
inline bool write_file_atomic(const std::string& path, const void* data, size_t size) {
  if (frozen()) return false;
  static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
  const long long pid = (long long)_getpid();
#else
  const long long pid = (long long)getpid();
#endif
  const std::string temporary = path + "." + std::to_string(pid) + "-" + std::to_string(serial.fetch_add(1)) + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return false;
  bool ok = size == 0 || std::fwrite(data, 1, size, file) == size;
  ok = std::fflush(file) == 0 && ok;
  if (ok && size >= detail::kSyncFrom) detail::sync_file(file);
  ok = std::fclose(file) == 0 && ok;
  ok = ok && !frozen();   // frozen meanwhile: the process is going down, keep the old file
  std::error_code ec;
  for (int attempt = 0; ok; ++attempt) {
    ec.clear();
    std::filesystem::rename(temporary, path, ec);   // replaces an existing file (MoveFileEx on Windows)
    if (!ec || attempt == 5 || !detail::rename_busy(ec) || frozen()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20 * (attempt + 1)));   // 20..100 ms apart
  }
  ok = ok && !ec;
  if (!ok) std::filesystem::remove(temporary, ec);
  return ok;
}

inline bool remove_file(const std::string& path) {
  std::error_code ec;
  return std::filesystem::remove(path, ec);
}

// Temporaries a killed process left next to path. Only old ones: another copy of the game may be
// writing the same cache this moment.
inline void remove_stale_temporaries(const std::string& path, std::chrono::seconds older_than = std::chrono::seconds(120)) {
  const std::filesystem::path target(path);
  detail::remove_stale(target.parent_path(), target.filename().string(), older_than);
}
// The same for every file in a folder (the GL program binaries, one file each).
inline void remove_stale_temporaries_in(const std::string& folder, std::chrono::seconds older_than = std::chrono::seconds(120)) {
  detail::remove_stale(std::filesystem::path(folder), std::string(), older_than);
}

// What a producer wants done with its file this time. Failed: nothing could be produced now (a
// cache that grew during the copy): counted as a failed write, so it is tried again and a flush
// reports it.
enum class Produce { Nothing, Write, Remove, Failed };
// Called on the saver thread. Fills out (cleared before the call) when it returns Write.
using Producer = std::function<Produce(Buffer& out)>;
// Called on the saver thread when the bytes a producer gave could not be written (a full disk, a
// file another program holds open): the producer forgets that it saved them, so the next round
// writes them again. Producers mark their state saved when they hand the bytes over.
using Failed = std::function<void()>;
// Called on the saver thread after a save that wrote or removed something: bytes written, and why
// the save ran ("save", "flush", "exit").
using SavedCallback = std::function<void(uint64_t bytes, const char* why)>;

class Saver;
struct Registry {
  std::mutex mutex;
  std::vector<Saver*> savers;
};
inline Registry& registry() {
  static Registry* r = new Registry;   // never destroyed: a saver may outlive static destruction
  return *r;
}
inline std::atomic<uint64_t> g_next_saver_id{1};

class Saver {
 public:
  using Clock = std::chrono::steady_clock;
  // quiet: how long nothing new must arrive before a save; longest: how long a save may wait while
  // new things keep arriving.
  explicit Saver(std::chrono::milliseconds quiet = std::chrono::milliseconds(1500),
                 std::chrono::milliseconds longest = std::chrono::milliseconds(8000))
      : quiet_(quiet), longest_(longest), id_(g_next_saver_id.fetch_add(1)) {}
  ~Saver() { stop(); }
  Saver(const Saver&) = delete;
  Saver& operator=(const Saver&) = delete;

  // Before start(): one file this saver keeps, and what to do when writing it fails.
  void add(std::string path, Producer producer, Failed failed = {}) {
    files_.push_back({std::move(path), std::move(producer), std::move(failed), 0, {}});
  }
  // Before start(): a folder enqueue() writes into, whose stale temporaries are removed too.
  void add_folder(std::string folder) { folders_.push_back(std::move(folder)); }
  void on_saved(SavedCallback callback) { on_saved_ = std::move(callback); }

  void start() {
    if (thread_.joinable()) return;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      stopping_ = false;
      running_ = true;
    }
    thread_ = std::thread([this] { run(); });
    auto& r = registry();
    std::lock_guard<std::mutex> lk(r.mutex);
    r.savers.push_back(this);
  }

  // Any thread: something new is worth keeping.
  void mark_dirty() {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      const auto now = Clock::now();
      if (!dirty_) first_dirty_ = now;
      dirty_ = true;
      last_dirty_ = now;
    }
    cv_.notify_all();
  }

  // Any thread: write these bytes to path on the saver thread (a cache entry the backend already
  // has in memory, such as a linked GL program).
  void enqueue(std::string path, std::vector<uint8_t> bytes) {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      queue_.push_back({std::move(path), std::move(bytes), 0});
      const auto now = Clock::now();
      if (!dirty_) first_dirty_ = now;
      dirty_ = true;
      last_dirty_ = now;
    }
    cv_.notify_all();
  }

  // Any thread (the render thread, once a frame): while on, timed saves wait; flushes and the last
  // save do not. Backends hold it during a match, so a large copy and write never land in one; the
  // next menu saves.
  void hold(bool on) {
    if (held_.load(std::memory_order_relaxed) == on) return;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      held_.store(on);
    }
    cv_.notify_all();
  }

  // Any thread: save now, and wait up to timeout for it. True when the save finished in time and
  // wrote everything (or there was no saver thread to wait for).
  bool flush(const char* why, std::chrono::milliseconds timeout) {
    const uint64_t target = request_flush(why);
    return wait_flushed(target, Clock::now() + timeout);
  }

  // Asks for a save without waiting; returns the ticket wait_flushed takes.
  uint64_t request_flush(const char* why) {
    uint64_t target;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (!running_) return 0;
      target = ++flush_requested_;
      flush_why_ = why;
    }
    cv_.notify_all();
    return target;
  }
  // False when the save that answered the ticket did not finish by deadline, or failed to write.
  bool wait_flushed(uint64_t target, Clock::time_point deadline) {
    return wait_flushed_result(target, deadline) == FlushResult::Ok;
  }
  FlushResult wait_flushed_result(uint64_t target, Clock::time_point deadline) {
    if (target == 0 || std::this_thread::get_id() == thread_.get_id()) return FlushResult::Ok;   // never wait on ourselves
    std::unique_lock<std::mutex> lk(mutex_);
    if (!done_cv_.wait_until(lk, deadline, [&] { return flush_done_ >= target || !running_; })) return FlushResult::Timeout;
    return flush_ok_ >= target ? FlushResult::Ok : FlushResult::Failed;
  }

  // Writes whatever is still unsaved (on the saver thread), then ends the thread. The owner calls
  // this before it destroys anything the producers read.
  void stop() {
    {
      auto& r = registry();
      std::lock_guard<std::mutex> lk(r.mutex);
      r.savers.erase(std::remove(r.savers.begin(), r.savers.end(), this), r.savers.end());
    }
    if (!thread_.joinable()) return;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  uint64_t id() const { return id_; }
  uint64_t saved_bytes() const { return saved_bytes_.load(); }
  uint64_t saves() const { return saves_.load(); }

 private:
  // A write that fails is tried again on the next rounds, a few times in a row at most: a disk that
  // stays full should not make the saver serialize a large cache every few seconds.
  static constexpr int kMaxAttempts = 3;
  struct File {
    std::string path;
    Producer producer;
    Failed failed;
    int failures;   // in a row
    Buffer out;     // kept between saves (Buffer)
  };
  struct Pending {
    std::string path;
    std::vector<uint8_t> bytes;
    int failures;
  };

  void run() {
    // Temporaries a killed process left behind: here, not in start(), so a folder of program
    // binaries is never listed on the render thread.
    for (const auto& file : files_) remove_stale_temporaries(file.path);
    for (const auto& folder : folders_) remove_stale_temporaries_in(folder);
    std::unique_lock<std::mutex> lk(mutex_);
    for (;;) {
      if (stopping_) {
        const uint64_t target = flush_requested_;
        lk.unlock();
        const bool ok = save("exit");
        lk.lock();
        running_ = false;
        flush_done_ = target;
        if (ok) flush_ok_ = target;
        done_cv_.notify_all();
        return;
      }
      if (flush_requested_ > flush_done_) {
        const uint64_t target = flush_requested_;
        const char* why = flush_why_;
        lk.unlock();
        const bool ok = save(why);
        lk.lock();
        flush_done_ = target;
        if (ok) flush_ok_ = target;
        done_cv_.notify_all();
        continue;
      }
      if (dirty_) {
        const auto due = (std::min)(last_dirty_ + quiet_, first_dirty_ + longest_);   // parenthesized: windows.h min
        if (Clock::now() >= due) {
          if (held_) { cv_.wait(lk); continue; }   // until the hold lifts, a flush or the stop
          lk.unlock();
          save("save");
          lk.lock();
          continue;
        }
        cv_.wait_until(lk, due);
        continue;
      }
      cv_.wait(lk);
    }
  }

  // Saver thread only. Takes the dirty mark first, so anything marked while the producers run is
  // saved by the next round rather than lost. False when something was not written (or frozen).
  bool save(const char* why) {
    if (frozen()) return false;
    std::deque<Pending> queued;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      dirty_ = false;
      queued.swap(queue_);
    }
    uint64_t bytes = 0;
    bool touched = false, retry = false, all = true;
    std::deque<Pending> again;
    for (auto& item : queued) {
      if (frozen()) return false;
      if (write_file_atomic(item.path, item.bytes.data(), item.bytes.size())) { bytes += item.bytes.size(); touched = true; continue; }
      all = false;
      if (++item.failures < kMaxAttempts) again.push_back(std::move(item));
    }
    for (auto& file : files_) {
      if (frozen()) return false;
      file.out.clear();
      // An exception here (out of memory for a large cache) costs this save, never the game.
      bool wrote = true;
      try {
        switch (file.producer(file.out)) {
          case Produce::Nothing: break;
          case Produce::Write:
            wrote = write_file_atomic(file.path, file.out.data(), file.out.size());
            if (wrote) { bytes += file.out.size(); touched = true; }
            break;
          case Produce::Remove:
            remove_file(file.path);
            touched = true;
            break;
          case Produce::Failed: wrote = false; break;
        }
      } catch (...) {
        wrote = false;
      }
      if (wrote) { file.failures = 0; continue; }
      all = false;
      try { if (file.failed) file.failed(); } catch (...) {}
      if (++file.failures < kMaxAttempts) retry = true;
    }
    if (!again.empty() || retry) {   // the next round tries again, after the usual quiet period
      std::lock_guard<std::mutex> lk(mutex_);
      for (auto& item : again) queue_.push_back(std::move(item));
      const auto now = Clock::now();
      if (!dirty_) first_dirty_ = now;
      dirty_ = true;
      last_dirty_ = now;
    }
    if (touched) {
      saved_bytes_ += bytes;
      ++saves_;
      try { if (on_saved_) on_saved_(bytes, why); } catch (...) {}
    }
    return all && !frozen();
  }

  const std::chrono::milliseconds quiet_, longest_;
  const uint64_t id_;
  std::vector<File> files_;
  std::vector<std::string> folders_;
  SavedCallback on_saved_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_, done_cv_;
  bool stopping_ = false, running_ = false, dirty_ = false;
  std::atomic<bool> held_{false};   // written under mutex_ (hold), read by run() under it
  Clock::time_point first_dirty_{}, last_dirty_{};
  std::deque<Pending> queue_;
  uint64_t flush_requested_ = 0, flush_done_ = 0, flush_ok_ = 0;   // flush_ok_: the last ticket a save wrote in full
  const char* flush_why_ = "flush";
  std::atomic<uint64_t> saved_bytes_{0}, saves_{0};
};

// Tickets from request_flush_all, for wait_flush_all: saver id and ticket.
struct FlushTickets {
  std::vector<std::pair<uint64_t, uint64_t>> tickets;
};

// Any thread: every live saver starts writing what it has now; nothing waits.
inline FlushTickets request_flush_all(const char* why) {
  auto& r = registry();
  std::lock_guard<std::mutex> lk(r.mutex);
  FlushTickets t;
  for (Saver* saver : r.savers) t.tickets.push_back({saver->id(), saver->request_flush(why)});
  return t;
}
// Waits for those saves until deadline. A saver that stopped meanwhile wrote its last save as it
// stopped. False when one did not finish in time or failed to write.
inline FlushResult wait_flush_all_result(const FlushTickets& t, Saver::Clock::time_point deadline) {
  auto& r = registry();
  std::lock_guard<std::mutex> lk(r.mutex);
  auto result = FlushResult::Ok;
  for (const auto& [id, ticket] : t.tickets)
    for (Saver* saver : r.savers)
      if (saver->id() == id) {
        const auto saved = saver->wait_flushed_result(ticket, deadline);
        if (saved == FlushResult::Timeout || (saved == FlushResult::Failed && result == FlushResult::Ok)) result = saved;
      }
  return result;
}
inline bool wait_flush_all(const FlushTickets& t, Saver::Clock::time_point deadline) {
  return wait_flush_all_result(t, deadline) == FlushResult::Ok;
}
inline FlushResult flush_all_result(const char* why, std::chrono::milliseconds timeout) {
  const auto deadline = Saver::Clock::now() + timeout;
  return wait_flush_all_result(request_flush_all(why), deadline);
}

// Any thread: every live saver writes what it has now; waits for them up to timeout in total.
// The exit and relaunch paths call this first, so a process that is killed a moment later still
// leaves complete caches behind. Returns false when a saver did not finish in time.
inline bool flush_all(const char* why, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto deadline = Saver::Clock::now() + timeout;
  return wait_flush_all(request_flush_all(why), deadline);
}

}  // namespace gx::cache_store
