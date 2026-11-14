// Disc-free startup cache run. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../runtime/gx/prewarm.h"
#include "../runtime/gx/pipeline_cache_store.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace app::prewarm_only {
// Exit codes: 0 ok, 2 no renderer, 3 write failed, 4 already running,
//             5 save timeout, 6 unsupported backend, 64 usage.
constexpr int no_renderer = 2, write_failed = 3, already_running = 4,
              save_timeout = 5, unsupported_backend = 6, usage_error = 64;
using SaveResult = gx::cache_store::FlushResult;
struct Arguments {
  std::string settings_path, shader_cache, sys_dir, log_file, backend;
};
// Include the normal parser's value flags so a path/string equal to --prewarm-only cannot
// accidentally select this mode. Acceptance of those flags is separately restricted in parse().
inline bool takes_value(std::string_view flag) {
  constexpr std::string_view flags[] = {
    "--iso", "--mod-base-iso", "--state-trace", "--state-digest", "--frames", "--fps", "--frame-mode",
    "--scale", "--window", "--aspect", "--settings-path", "--import-cosmetic", "--scan-disc-skins",
    "--select-cosmetic", "--deselect-cosmetic", "--backend", "--dlss", "--dlss-mode", "--ssao", "--dlss-jitter-sign", "--frame-times", "--pin-phase", "--capture", "--capture-frame",
    "--capture-every", "--capture-burst", "--capture-sim-frame", "--script", "--online-mode", "--dump",
    "--shader-cache", "--trace-func", "--sys-dir", "--sysgen-data-dir", "--replay-dir", "--lab-dir",
    "--card-dir", "--log-file", "--replay", "--trace", "--lobby-status-file", "--online-delay",
    "--ranked-rules", "--user-dir", "--lobby-direct", "--lobby-character", "--chat", "--netplay-port",
    "--local-peer", "--test-stage", "--language", "--ssaa", "--msaa", "--dump-frame",
    "--time-base", "--rng-seed", "--volume", "--music", "--gecko-codes", "--sharpness", "--crt",
    "--border", "--shader", "--anisotropy", "--effects", "--lightweight", "--look", "--hang-watch",
    "--audio-dump", "--input-log", "--lcancel-log", "--music-volume", "--match", "--card-self-test",
    "--mod-dir", "--mod-iso", "--mod-gci", "--te-options", "--te-options2", "--cpu-training",
    "--mod-profile", "--slippi-menus", "--online-test", "--settings-standby", "--settings-tab"
  };
  for (auto value : flags) if (flag == value) return true;
  return flag.rfind("--p2p-", 0) == 0;
}
inline bool requested(int argc, const char* const* argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--") break;
    if (flag == "--prewarm-only") return true;
    if (takes_value(flag)) ++i;
  }
  return false;
}
inline std::string log_path(const std::string& session_log) {
  if (session_log.empty()) return "melee_port_prewarm.log";
  const auto path = std::filesystem::u8path(session_log);
  return (path.parent_path() / (path.stem().u8string() + "_prewarm.log")).u8string();
}
inline Arguments parse(int argc, const char* const* argv) {
  Arguments out;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + flag);
      return argv[++i];
    };
    if (flag == "--prewarm-only" || flag == "--hidden" || flag == "--threaded-renderer" || flag == "--load-settings" ||
        flag == "--vanilla-only" || flag == "--no-mod-autodetect" || flag == "--quiet" ||
        flag == "--fullscreen" || flag == "--no-music") {}
    else if (flag == "--settings-path") out.settings_path = next();
    else if (flag == "--shader-cache") out.shader_cache = next();
    else if (flag == "--sys-dir") out.sys_dir = next();
    else if (flag == "--log-file") out.log_file = next();
    else if (flag == "--backend") out.backend = next();
    // Desktop launch spec and Mac bundle defaults: no gameplay files are opened here.
    else if (flag == "--iso" || flag == "--user-dir" || flag == "--card-dir" || flag == "--replay-dir" ||
             flag == "--volume" || flag == "--sysgen-data-dir" || flag == "--music" || flag == "--fps" ||
             flag == "--frame-mode" || flag == "--dlss") next();
    else throw std::invalid_argument("unsupported prewarm-only option: " + flag);
  }
  return out;
}
// Parse failures have one exit mapping, shared by the real entry point and process tests.
template<class Work, class Usage> int with_arguments(int argc, const char* const* argv, Work work, Usage usage) {
  Arguments args;
  try { args = parse(argc, argv); }
  catch (const std::invalid_argument& e) { usage(e.what()); return usage_error; }
  return work(args);
}
// Stable lock inode/file: never unlink it, which could let another process lock a new file at the
// same path. OS ownership is released even after a crash or end_process (no CRT unload required).
class CacheLock {
 public:
  explicit CacheLock(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    const auto path = folder / "prewarm.lock";
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open prewarm.lock");
    OVERLAPPED overlapped{};
    locked_ = LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) != 0;
    if (!locked_ && GetLastError() != ERROR_LOCK_VIOLATION) {
      CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE;
      throw std::runtime_error("cannot lock prewarm.lock");
    }
#else
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd_ < 0) throw std::runtime_error("cannot open prewarm.lock");
    locked_ = flock(fd_, LOCK_EX | LOCK_NB) == 0;
    if (!locked_ && errno != EWOULDBLOCK && errno != EAGAIN) {
      close(fd_); fd_ = -1;
      throw std::runtime_error("cannot lock prewarm.lock");
    }
#endif
  }
  ~CacheLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) close(fd_);
#endif
  }
  CacheLock(const CacheLock&) = delete;
  CacheLock& operator=(const CacheLock&) = delete;
  bool acquired() const { return locked_; }
 private:
  bool locked_ = false;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};
template<class Work> int with_cache_lock(const std::filesystem::path& folder, Work work) {
  CacheLock lock(folder);
  return lock.acquired() ? work() : already_running;
}
#ifndef _WIN32
// Avoid OS crash reporters/core dumps as well as the app dialog: this mode starts before the
// interactive host installs its handlers. _Exit is signal-safe and releases the OS cache lock.
inline void install_crash_handlers() {
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
    std::signal(sig, [](int) { std::_Exit(no_renderer); });
}
#endif
// Same fatal-error hook for every host; the injected process exit makes it testable without a GPU.
template<auto Exit> void die_hook(const char*) { Exit(no_renderer); }
// Keep the renderer alive through the bounded flush and report. A timed-out saver may still be
// inside the driver: leave that renderer to the OS and end the process without joining its thread.
template<class Create, class Flush, class Report, class Error>
int run(Create create, Flush flush, Report report, Error error) {
  const auto start = std::chrono::steady_clock::now();
  try {
    auto renderer = create();
    if (!renderer) return no_renderer;
    const auto stats = renderer->prewarm_stats();
    if (!stats.backend) return no_renderer;
    SaveResult saved;
    try { saved = flush(); }
    catch (const std::exception& e) { error(e.what()); saved = SaveResult::Failed; }
    if (saved == SaveResult::Timeout) {
      gx::cache_store::freeze();
      (void)renderer.release();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    report(stats, ms, saved);
    return saved == SaveResult::Ok ? 0 : saved == SaveResult::Timeout ? save_timeout : write_failed;
  } catch (const gx::UnsupportedPrewarmBackend& e) {
    error(e.what());
    return unsupported_backend;
  } catch (const std::exception& e) {
    error(e.what());
    return no_renderer;
  }
}
} // namespace app::prewarm_only
