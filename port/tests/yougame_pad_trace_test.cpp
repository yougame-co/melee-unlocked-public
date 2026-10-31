#include "yougame_agent_pad_trace.h"
#include "yougame_agent_test.h"
#include <cstdio>
#ifdef YOUGAME_AGENT_TEST_HOOK
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
using namespace yougame;
namespace fs = std::filesystem;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static std::string read(const fs::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char** argv) {
  CHECK(argc == 2);
  const auto root = fs::u8path(argv[1]);
  fs::remove_all(root);
  fs::create_directories(root / "sub");
  auto parse = [&](nlohmann::json j) { return agent_test::map_keys(j, agent_test::Engine::Source, root); };
  CHECK(parse({{"padTrace", "sub/pads.txt"}}).pad_trace == "sub/pads.txt");
  CHECK(parse({{"padTrace", "sub\\pads.txt"}}).pad_trace == "sub/pads.txt");
  for (const auto* bad : {"", "../pads", "/pads", "C:/pads", "missing/pads", "sub/", ".", "x\ny"}) {
    const auto s = parse({{"padTrace", bad}});
    CHECK(s.pad_trace.empty());
    CHECK(s.log.find("agent-test: padTrace invalid output path; ignored") != s.log.npos);
  }
  CHECK(parse({{"padTrace", true}}).pad_trace.empty());
  agent_paths::Root cap;
  CHECK(cap.load(root));
  agent_test::PadTrace trace;
  CHECK(trace.arm("sub/pads.txt", cap));
  agent_test::TracePad p{0x1100, -80, 80, 0, 0, 0, 0, 0, "relayed slot 2"};
  trace.record(123, 0, p);
  trace.record(124, 0, p);
  CHECK(read(root / "sub/pads.txt") == "123 port 1 src relayed slot 2 buttons 1100 stick -80,80\n");
  p.x = 0; trace.record(125, 0, p);
  p.buttons = 0; trace.record(126, 0, p);
  // Every pad field participates, including fields omitted from the concise output.
  p.cx = 40; trace.record(127, 0, p);
  p.cy = -40; trace.record(128, 0, p);
  p.left = 127; trace.record(129, 0, p);
  p.right = 255; trace.record(130, 0, p);
  p.error = -1; p.source = "none"; trace.record(131, 0, p);
  for (const auto* src : {"sdl", "adapter", "keyboard", "none"}) {
    p.source = src;
    trace.record(132, 3, p);
  }
  const auto contents = read(root / "sub/pads.txt");
  CHECK(std::count(contents.begin(), contents.end(), '\n') == 12);
  std::istringstream lines(contents);
  std::string line;
  const std::regex format(R"(^[0-9]+ port [1-4] src (relayed slot [1-4]|sdl|adapter|keyboard|none) buttons [0-9A-F]{4} stick -?[0-9]+,-?[0-9]+$)");
  while (std::getline(lines, line)) CHECK(std::regex_match(line, format));
  trace.record(133, -1, p); trace.record(133, 4, p);
  CHECK(read(root / "sub/pads.txt") == contents);
  trace.stop();
  trace.record(134, 0, p);
  CHECK(read(root / "sub/pads.txt") == contents);
  CHECK(trace.arm("sub/pads.txt", cap));
  trace.record(135, 0, p);
  trace.stop();
  CHECK(read(root / "sub/pads.txt").substr(0, contents.size()) == contents); // append across sessions
  fs::create_symlink(root / "sub/pads.txt", root / "link");
  CHECK(parse({{"padTrace", "link"}}).pad_trace.empty());
  CHECK(!trace.arm("link", cap));
  fs::create_hard_link(root / "sub/pads.txt", root / "hard");
  CHECK(!trace.arm("hard", cap));
  { std::ofstream out(root / "agent-test.json"); out << R"({"padTrace":"loaded.txt"})"; }
  CHECK(agent_test::load(false, root.u8string().c_str()).pad_trace.empty());
  CHECK(!fs::exists(root / "loaded.txt"));
  CHECK(agent_test::load(true, root.u8string().c_str()).pad_trace == "loaded.txt");
  agent_test::pad_trace.record(1, 1, p);
  CHECK(!read(root / "loaded.txt").empty());
  agent_test::load(false, nullptr);
  const auto loaded = read(root / "loaded.txt");
  agent_test::pad_trace.record(2, 1, {});
  CHECK(read(root / "loaded.txt") == loaded);
  cap.reset();
  fs::remove_all(root);
  std::printf("pad trace: %d/%d checks passed\n", checks, checks);
}
#else
#include <type_traits>
template<class T, class = void> struct HasPadTrace : std::false_type {};
template<class T> struct HasPadTrace<T, std::void_t<decltype(std::declval<T>().pad_trace)>> : std::true_type {};
static_assert(!HasPadTrace<yougame::agent_test::Settings>::value, "production settings exclude tracing");
int main() { return 0; }
#endif
