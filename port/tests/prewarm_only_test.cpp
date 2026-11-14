// Graphics-only launch lifecycle, without a GPU or game services.
#include "prewarm_only.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#define check(x) do { if (!(x)) { std::printf("prewarm_only_test FAILED line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
namespace pw = app::prewarm_only;
namespace cs = gx::cache_store;
using Stats = gx::PrewarmStats;
struct Renderer {
  bool& alive;
  const char* backend;
  Renderer(bool& a, const char* b = "opengl") : alive(a), backend(b) { alive = true; }
  ~Renderer() { alive = false; }
  Stats prewarm_stats() { return {backend, 0, 0, 0}; }
};
// Child modes exercise real process exit statuses, including OS lock contention across processes.
static int child_status(const std::string& executable, const std::string& mode, const std::string& arg = {}) {
  std::string command = '"' + executable + "\" " + mode;
  if (!arg.empty()) command += " \"" + arg + '"';
#ifdef _WIN32
  command = '"' + command + '"';
#endif
  const int status = std::system(command.c_str());
#ifdef _WIN32
  return status;
#else
  check(status != -1 && WIFEXITED(status));
  return WEXITSTATUS(status);
#endif
}
int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--test-die-hook") {
    pw::die_hook<std::_Exit>("renderer failed");
    return 99;
  }
#ifndef _WIN32
  if (argc > 1 && std::string(argv[1]) == "--test-crash") {
    pw::install_crash_handlers();
    std::raise(SIGSEGV);
    return 99;
  }
#endif
  if (argc > 1 && std::string(argv[1]) == "--test-unknown") {
    const char* args[] = {"game", "--prewarm-only", "--unknown"};
    return pw::with_arguments(3, args, [](const pw::Arguments&) { return 99; }, [](const char*) {});
  }
  if (argc == 3 && std::string(argv[1]) == "--test-lock")
    return pw::with_cache_lock(std::filesystem::u8path(argv[2]), [] { return 0; });

  // tools/ci/inputs/yougame-native.json's args (from sibling mu-ci-console; not in this base).
  // Also cover port/mac/README.md launch defaults and every path inserted by full_launch.h.
  const char* args[] = {"game", "--prewarm-only", "--iso", "{disc}", "--threaded-renderer",
      "--settings-path", "{data}/port-settings.ini", "--sys-dir", "{build}/Sys",
      "--user-dir", "{data}/User/Slippi", "--replay-dir", "{data}/Replays", "--card-dir", "{data}/User/GC/CardA",
      "--log-file", "{data}/melee.log", "--shader-cache", "{data}/shadercache", "--vanilla-only",
      "--no-mod-autodetect", "--quiet", "--fullscreen", "--no-music", "--sysgen-data-dir", "sysgen",
      "--music", "50", "--fps", "monitor", "--frame-mode", "authored", "--dlss", "quality",
      "--volume", "100", "--hidden", "--load-settings", "--backend", "vulkan"};
  constexpr int count = sizeof(args) / sizeof(*args);
  check(pw::requested(count, args));
  check(!pw::requested(1, args));
  const auto parsed = pw::parse(count, args);
  check(parsed.settings_path == "{data}/port-settings.ini" && parsed.shader_cache == "{data}/shadercache");
  check(parsed.sys_dir == "{build}/Sys" && parsed.log_file == "{data}/melee.log" && parsed.backend == "vulkan");
  check(pw::log_path({}) == "melee_port_prewarm.log");
  check(pw::log_path("my session.log") == "my session_prewarm.log");
  check(pw::log_path("logs/game.session.txt") == "logs/game.session_prewarm.log");
  const char* missing[] = {"game", "--prewarm-only", "--shader-cache"};
  check(pw::with_arguments(3, missing, [](const pw::Arguments&) { return 99; }, [](const char*) {}) == 64);
  for (const auto* flag : {"--settings-path", "--iso", "--music", "--sysgen-data-dir", "--log-file", "--script", "--dlss-mode"}) {
    const char* value[] = {"game", flag, "--prewarm-only", "--quiet", "--prewarm-only"};
    check(!pw::requested(4, value));
    check(pw::requested(5, value));
  }
  const char* after_end[] = {"game", "--", "--prewarm-only"};
  const char* boolean[] = {"game", "--frame-generation", "--prewarm-only"};
  check(!pw::requested(3, after_end));
  check(pw::requested(3, boolean));

  for (auto saved : {pw::SaveResult::Ok, pw::SaveResult::Failed, pw::SaveResult::Timeout}) {
    bool alive = false; std::vector<int> events; Renderer* raw = nullptr;
    auto rc = pw::run([&] { events.push_back(1); auto r = std::make_unique<Renderer>(alive); raw = r.get(); return r; },
      [&] { check(alive); events.push_back(2); return saved; },
      [&](const Stats& s, long long ms, pw::SaveResult ok) { check(alive && s.recipes == 0 && ms >= 0 && ok == saved); events.push_back(3); },
      [](const char*) { check(false); });
    check(rc == (saved == pw::SaveResult::Ok ? 0 : saved == pw::SaveResult::Failed ? 3 : 5));
    check(events == std::vector<int>({1, 2, 3}));
    if (saved == pw::SaveResult::Timeout) { check(alive && cs::frozen()); delete raw; cs::g_frozen = false; }
    check(!alive);
  }
  int calls = 0;
  auto flush = [&] { ++calls; return pw::SaveResult::Ok; };
  auto report = [&](const Stats&, long long, pw::SaveResult) { ++calls; };
  auto error = [&](const char*) { ++calls; };
  check(pw::run([] { return std::unique_ptr<Renderer>{}; }, flush, report, error) == 2);
  check(calls == 0);
  bool alive = false;
  check(pw::run([&] { return std::make_unique<Renderer>(alive, nullptr); }, flush, report, error) == 2);
  check(!alive && calls == 0);
  check(pw::run([]() -> std::unique_ptr<Renderer> { throw std::runtime_error("no device"); }, flush, report, error) == 2);
  check(calls == 1);
  check(pw::run([]() -> std::unique_ptr<Renderer> { throw gx::UnsupportedPrewarmBackend("d3d11"); }, flush, report, error) == 6);
  check(calls == 2);
  check(pw::run([&] { return std::make_unique<Renderer>(alive); },
      []() -> pw::SaveResult { throw std::runtime_error("write threw"); },
      [&](const Stats&, long long, pw::SaveResult saved) { check(saved == pw::SaveResult::Failed); ++calls; }, error) == 3);
  check(!alive && calls == 4);

  check(child_status(argv[0], "--test-die-hook") == 2);
  check(child_status(argv[0], "--test-unknown") == 64);
#ifndef _WIN32
  check(child_status(argv[0], "--test-crash") == 2);
#endif
  const auto dir = std::filesystem::temp_directory_path() / ("prewarm-lock-test-" + std::to_string(
#ifdef _WIN32
    GetCurrentProcessId()
#else
    getpid()
#endif
  ));
  std::filesystem::remove_all(dir);
  {
    pw::CacheLock first(dir);
    check(first.acquired());
    check(child_status(argv[0], "--test-lock", dir.string()) == 4);
    check(pw::with_cache_lock(dir, [] { return 99; }) == 4);
  }
  check(child_status(argv[0], "--test-lock", dir.string()) == 0);
  check(std::filesystem::is_regular_file(dir / "prewarm.lock"));
  std::filesystem::remove_all(dir);
  std::puts("prewarm_only_test PASS");
}
