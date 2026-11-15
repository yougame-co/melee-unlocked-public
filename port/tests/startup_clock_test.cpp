// Startup timing suffix and the log's first line (runtime/host/startup_clock.h).
#include "startup_clock.h"
#include <cstdio>
#include <regex>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
  using host::startup::log_header;
  using host::startup::suffix;
  CHECK(suffix(0) == " (+0 ms)");
  CHECK(suffix(1234) == " (+1234 ms)");
  CHECK(suffix(93000) == " (+93000 ms)");
  // Appended, never inserted: the existing wording still starts the line.
  const std::string line = std::string("disc: image prefetched into the file cache (1392 MB in 4.7 s)") + suffix(21000);
  CHECK(line.rfind("disc: image prefetched into the file cache (1392 MB in 4.7 s)", 0) == 0);
  CHECK(line == "disc: image prefetched into the file cache (1392 MB in 4.7 s) (+21000 ms)");

  CHECK(log_header("0.9.120", "melee_source.exe", "0.9.120-online-test.103") ==
        "Melee Unlocked 0.9.120, melee_source.exe, YouGame build 0.9.120-online-test.103");
  CHECK(log_header("0.9.120", "melee_source.exe", "") == "Melee Unlocked 0.9.120, melee_source.exe");
  CHECK(log_header("0.9.120", "melee_source.exe", nullptr) == "Melee Unlocked 0.9.120, melee_source.exe");
  // tools/crash_relay/src/crash_markdown.js reads the version with this pattern.
  std::smatch m;
  const std::string header = log_header("0.9.120", "melee_source.exe", "0.9.120-online-test.103");
  CHECK(std::regex_search(header, m, std::regex(R"(^Melee Unlocked (\d+(?:\.\d+){1,3}),)")) && m[1] == "0.9.120");
  // Builds that set no label (Linux, Mac, bare targets) compile with an empty one.
  CHECK(std::string(MELEE_YOUGAME_BUILD).empty());
  std::puts("11 checks passed");
}
