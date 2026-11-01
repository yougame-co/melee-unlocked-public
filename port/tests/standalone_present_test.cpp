// GPU-free Apple standalone startup, capability and shared-clock/format checks.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_agent_present.h"
#if defined(YOUGAME_AGENT_TEST_HOOK) && defined(__APPLE__)
#include "yougame_agent_test.h"
#include <cstdlib>
#include <fstream>
#include <iterator>

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace fs = std::filesystem;
using namespace yougame;
static std::string read(const fs::path& path) {
  std::ifstream file(path);
  CHECK(file.good());
  return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv) {
  CHECK(argc == 2);
  auto pattern = (fs::absolute(argv[1]) / "standalone-present-XXXXXX").string();
  CHECK(::mkdtemp(pattern.data()) != nullptr);
  const fs::path directory(pattern), output = directory / "present.log";
  auto& recorder = agent_present::recorder();
  auto start = [&](bool bridge) {
    agent_test::load(bridge, directory.c_str());
    agent_present::start_standalone(bridge);
  };
  CHECK(::unsetenv("MELEE_PRESENT_LOG") == 0);
  start(false);
  CHECK(!recorder.enabled());
  CHECK(fs::is_empty(directory));
  CHECK(::setenv("MELEE_PRESENT_LOG", "relative.log", 1) == 0);
  start(false);
  CHECK(!recorder.enabled() && fs::is_empty(directory));
  const auto missing = directory / "missing" / "present.log";
  CHECK(::setenv("MELEE_PRESENT_LOG", missing.c_str(), 1) == 0);
  start(false);
  CHECK(!recorder.enabled() && !fs::exists(missing.parent_path()));
  const auto protected_file = directory / "protected.log";
  { std::ofstream file(protected_file); file << "untouched"; }
  const auto linked = directory / "linked.log";
  fs::create_symlink(protected_file, linked);
  CHECK(::setenv("MELEE_PRESENT_LOG", linked.c_str(), 1) == 0);
  start(false);
  CHECK(!recorder.enabled() && read(protected_file) == "untouched");
  const auto linked_parent = directory / "linked-parent";
  fs::create_directory_symlink(directory, linked_parent);
  const auto through_link = linked_parent / "refused.log";
  CHECK(::setenv("MELEE_PRESENT_LOG", through_link.c_str(), 1) == 0);
  start(false);
  CHECK(!recorder.enabled() && !fs::exists(directory / "refused.log"));
  { std::ofstream file(output); file << "old log must be truncated at startup"; }
  CHECK(::setenv("MELEE_PRESENT_LOG", output.c_str(), 1) == 0);
  start(false);
  CHECK(recorder.enabled() && !recorder.frames_enabled());
  CHECK(fs::file_size(output) == 0);
  CHECK(!recorder.wants_frame(1));
  const auto epoch = agent_present::Recorder::Clock::time_point(std::chrono::seconds(10));
  const auto wall = std::chrono::system_clock::time_point(std::chrono::milliseconds(123456789));
  recorder.after_swap(1, 77, 2, 10, 1920, 1080, 960, 540, {}, epoch, wall);
  CHECK(::setenv("MELEE_PRESENT_LOG", protected_file.c_str(), 1) == 0);
  // An app bridge never stops/rearms an already configured recorder.
  host::test_env::yougame_session = true;
  agent_present::start_standalone(true);
  CHECK(recorder.enabled());
  recorder.after_swap(2, 78, 2, 10, 1920, 1080, 960, 540, {},
                      epoch + std::chrono::microseconds(16667), wall);
  recorder.stop();
  CHECK(read(output) ==
      "present-log v1 wall_ms=123456789 fields=present us sim scene backbuffer window scale\n"
      "1 0 77 02:0A 1920x1080 960x540 2.000\n"
      "2 16667 78 02:0A 1920x1080 960x540 2.000\n");
  start(true);
  CHECK(!recorder.enabled() && read(protected_file) == "untouched");
  { std::ofstream file(directory / "agent-test.json"); file << R"({"present_log":"app.log"})"; }
  start(true);
  CHECK(recorder.enabled());
  recorder.after_swap(1, 77, 2, 10, 1920, 1080, 960, 540, {}, epoch, wall);
  recorder.stop();
  CHECK(read(directory / "app.log") == agent_present::header(123456789) +
        agent_present::line(1, 0, 77, 2, 10, 1920, 1080, 960, 540));
  CHECK(read(protected_file) == "untouched");
  CHECK(::unsetenv("MELEE_PRESENT_LOG") == 0);
  std::printf("standalone_present_test: %u/%u passed\n", checks, checks);
}
#else
#include <cstdio>
int main() { std::puts("presentation evidence disabled"); }
#endif
