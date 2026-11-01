#include "phob_adapter_input.h"
#include "yougame_agent_test.h"
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Origin { bool set = false; uint8_t sx=128, sy=128, cx=128, cy=128, tl=0, tr=0; };
struct Sample {
  uint32_t time_us=0; int8_t raw_ax=0, raw_ay=0, raw_cx=0, raw_cy=0, ax=0, ay=0, cx=0, cy=0;
  uint8_t raw_l=0, raw_r=0, l=0, r=0, hw_ax=0, hw_ay=0, hw_cx=0, hw_cy=0;
  uint16_t raw_buttons=0, buttons=0;
};
template<class T, class = void> struct HasSocket : std::false_type {};
template<class T> struct HasSocket<T, std::void_t<decltype(std::declval<T>().phob_socket)>> : std::true_type {};
int main(int argc, char** argv) {
  using namespace host::phob;
  uint8_t report[37] = {0x21}; HardwareInput input;
  for (int port=0; port<4; ++port) for (uint8_t type : {0x10,0x20}) {
    Origin origin;
    auto* c = report + 1 + port*9;
    c[0]=type; c[1]=0x21; c[2]=0x0b;
    c[3]=128; c[4]=127; c[5]=129; c[6]=128; c[7]=3; c[8]=4;
    CHECK(mirror_input(report, port, origin, input));
    c[3]=173; c[4]=107; c[5]=99; c[6]=168; c[7]=23; c[8]=34;
    CHECK(mirror_input(report, port, origin, input));
    const auto sample = live_sample<Sample>(input, 123);
    CHECK(sample.ax == 45 && sample.ay == -20);
    CHECK(sample.cx == -30 && sample.cy == 40);
    CHECK(sample.raw_ax == sample.ax && sample.hw_ax == 173);
    CHECK(sample.l == 20 && sample.r == 30 && sample.time_us == 123);
    CHECK(sample.buttons == (0x100 | 0x2 | 0x1000 | 0x10 | 0x40));
    c[0]=0; CHECK(!mirror_input(report, port, origin, input)); CHECK(!origin.set);
  }
  Origin origin;
  CHECK(!mirror_input(report, -1, origin, input)); CHECK(!mirror_input(report, 4, origin, input));
  report[0]=0; CHECK(!mirror_input(report, 0, origin, input));
  MenuDiagnostic diag;
  CHECK(!diag.due(false, 10)); CHECK(diag.due(true, 10));
  CHECK(!diag.due(true, 10.999)); CHECK(diag.due(true, 11)); CHECK(!diag.due(true, 11));
#ifdef YOUGAME_AGENT_TEST_HOOK
  static_assert(HasSocket<yougame::agent_test::Settings>::value);
  for (int socket=1; socket<=4; ++socket) {
    auto s=yougame::agent_test::map_keys({{"openMenu", "phob:" + std::to_string(socket)}});
    CHECK(s.phob_socket == socket); CHECK(s.log.find("openMenu") != std::string::npos);
  }
  for (const auto& v : {nlohmann::json("phob:0"), nlohmann::json("phob:5"), nlohmann::json("phob:01"),
       nlohmann::json("phob:1x"), nlohmann::json("other:1"), nlohmann::json(1), nlohmann::json(nullptr)})
    CHECK(yougame::agent_test::map_keys({{"openMenu",v}}).phob_socket == 0);
  CHECK(argc == 2);
  const auto root = std::filesystem::u8path(argv[1]);
  std::filesystem::create_directories(root);
  for (auto engine : {yougame::agent_test::Engine::Source, yougame::agent_test::Engine::Static}) {
    { std::ofstream file(root / "agent-test.json"); file << R"({"openMenu":"phob:3"})"; }
    CHECK(yougame::agent_test::load(false, root.u8string().c_str(), engine).phob_socket == 0);
    CHECK(yougame::agent_test::phob_menu_socket.load() == 0);
    CHECK(yougame::agent_test::load(true, root.u8string().c_str(), engine).phob_socket == 3);
    CHECK(yougame::agent_test::phob_menu_socket.exchange(0) == 3);
    CHECK(yougame::agent_test::phob_menu_socket.exchange(0) == 0);
    yougame::agent_test::load(true, root.u8string().c_str(), engine);
    yougame::agent_test::load(false, nullptr, engine);
    CHECK(yougame::agent_test::phob_menu_socket.load() == 0);
  }
  std::filesystem::remove(root / "agent-test.json");
  std::filesystem::remove(root);
#else
  static_assert(!HasSocket<yougame::agent_test::Settings>::value);
#endif
  std::printf("phob_mirror: %d checks passed\n", checks);
}
