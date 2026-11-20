// SPDX-License-Identifier: GPL-2.0-or-later
// Unzipped without file modes, a shipped bin/ helper has no +x: ensure_executable adds it and says so, or gives
// the player the message that tells them how to fix it. POSIX only.
// clang++ -std=c++17 -Iport/runtime/host port/tests/helper_exec_test.cpp -o /tmp/helper_exec_test && /tmp/helper_exec_test
#include "helper_exec.h"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

int main() {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / ("helper_exec_test_" + std::to_string(getpid()));
  fs::create_directories(dir);
  fs::path tool = dir / "glslangValidator";
  { std::ofstream(tool) << "#!/bin/sh\nexit 0\n"; }

  chmod(tool.c_str(), 0644);   // what a mode-dropping unzip leaves
  std::string note;
  CHECK(helper_exec::ensure_executable(tool, "bin/glslangValidator", &note).empty());
  CHECK(access(tool.c_str(), X_OK) == 0);
  CHECK(note.find("bin/glslangValidator") == 0 && note.find("made it executable") != std::string::npos);

  note.clear();                // already runnable: nothing to say
  CHECK(helper_exec::ensure_executable(tool, "bin/glslangValidator", &note).empty() && note.empty());

  CHECK(helper_exec::not_executable_message("bin/glslangValidator") ==
        "bin/glslangValidator is not executable: unzip Melee Unlocked again with a tool that keeps file permissions, "
        "or run chmod +x bin/glslangValidator");
  // A file it can't change (missing here) gets the message, never a silent pass.
  CHECK(!helper_exec::ensure_executable(dir / "missing", "bin/missing", &note).empty());

  fs::remove_all(dir);
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("helper_exec_test: ok");
  return 0;
}
