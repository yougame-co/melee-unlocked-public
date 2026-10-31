#include "session_logs.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
namespace fs = std::filesystem;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
static std::string read(const fs::path& path) { std::ifstream f(path); std::string s; f >> s; return s; }
int main() {
  const auto dir = fs::temp_directory_path() / ("mu-log-rotation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(dir);
  for (const char* name : {"melee.log", "custom-output"}) {
    const auto path = (dir / name).string();
    for (int session = 1; session <= 8; ++session) {
      CHECK(host::rotate_session_logs(path));
      { std::ofstream f(path); f << session; }
      CHECK(read(path) == std::to_string(session));
      for (int age = 1; age <= 4; ++age) {
        const auto previous = host::previous_log(path, age);
        CHECK(session > age ? read(previous) == std::to_string(session - age) : !fs::exists(previous));
      }
    }
    CHECK(!fs::exists(host::previous_log(path, 5)));
  }
  CHECK(host::previous_log("melee.log", 1) == "melee.prev.log");
  CHECK(host::previous_log("melee_port.log", 1) == "melee_port.prev.log");
  CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) == 10);
  // Nonempty directories cannot be replaced by rename on either POSIX or Windows.
  // Block the whole chain so current -> prev also fails, as with locked Windows logs.
  const auto blocked = (dir / "blocked.log").string();
  for (int age = 1; age <= 4; ++age) {
    const auto target = fs::path(host::previous_log(blocked, age));
    fs::create_directory(target);
    std::ofstream(target / "keep") << "retained";
  }
  for (int session = 1; session <= 2; ++session) {
    std::ofstream(blocked) << "session" << session;
    CHECK(host::rotate_session_logs(blocked));
    CHECK(!fs::exists(blocked));
    std::ofstream(blocked) << "next";
  }
  int saved = 0;
  bool first = false, second = false;
  for (const auto& file : fs::directory_iterator(dir)) {
    if (file.path().filename().string().find("blocked.log.saved.") != 0) continue;
    ++saved;
    first |= read(file.path()) == "session1";
    second |= read(file.path()) == "session2";
  }
  CHECK(saved == 2 && first && second);
  CHECK(read(blocked) == "next");
  for (int age = 1; age <= 4; ++age)
    CHECK(read(fs::path(host::previous_log(blocked, age)) / "keep") == "retained");
  fs::remove_all(dir);
  std::printf("session logs: %d/%d checks passed (retention and blocked rename fallback)\n", checks, checks);
}
