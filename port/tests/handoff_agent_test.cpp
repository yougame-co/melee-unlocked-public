// Source-only configuration, confined outputs, and inert non-hook builds.
#include "yougame_agent_test.h"
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char** argv) {
  using namespace yougame::agent_test;
  using json = nlohmann::json;
  CHECK(argc == 2);
#ifdef YOUGAME_AGENT_TEST_HOOK
  const auto root = std::filesystem::path(argv[1]) / "handoff_agent_hook_fixture";
#else
  const auto root = std::filesystem::path(argv[1]) / "handoff_agent_off_fixture";
#endif
  std::filesystem::create_directory(root);
  const json config{{"handoffSave", "a.muhs"}, {"handoffAt", 900}, {"handoffDiffDump", "a.muhd"},
      {"handoffHashLog", "a.log"}, {"handoffKeep", json::array({json::array({"0x80000000", 8})})}};
  auto map = [&](const json& value, Engine engine = Engine::Source) { return map_keys(value, engine, root); };
#ifdef YOUGAME_AGENT_TEST_HOOK
  CHECK(!yougame::console::friends_handoff_join_enabled());
  CHECK(map({{"friendsHandoffJoin",true}}).friends_handoff_join);
  CHECK(!map({{"friendsHandoffJoin",true}}, Engine::Static).friends_handoff_join);
  for (const auto& bad : {json(1),json("true"),json(nullptr)})
    CHECK(!map({{"friendsHandoffJoin",bad}}).friends_handoff_join);
  { std::ofstream out(root / "agent-test.json"); out << json{{"friendsHandoffJoin",true}}; }
  CHECK(load(true, root.string().c_str()).friends_handoff_join);
  CHECK(yougame::console::friends_handoff_join_enabled());
  load(false, root.string().c_str());
  CHECK(!yougame::console::friends_handoff_join_enabled());
  auto settings = map(config);
  CHECK(settings.handoff.enabled() && settings.handoff.error.empty());
  CHECK(settings.handoff.at == 900 && settings.handoff.keep.size() == 1);
  CHECK(settings.handoff.save == "a.muhs" && settings.handoff.diff_dump == "a.muhd");
  CHECK(!map(config, Engine::Static).handoff.enabled());
  for (const auto& bad : {json(-1), json(0), json(299), json(30001), json(900.5), json("900")}) {
    auto c = config; c["handoffAt"] = bad; CHECK(!map(c).handoff.error.empty());
  }
  for (const auto& bad : {json("0x1"), json::array({"0x1", 0}), json::array({"0x1", -1}),
      json::array({"0x1", 1.5}), json::array({"0x10000000000000000", 8}), json::array({"1234", 1})}) {
    auto c = config; c["handoffKeep"] = json::array({bad}); CHECK(!map(c).handoff.error.empty());
  }
  for (const char* path : {"../outside", "/absolute", "a:stream", ""}) {
    auto c = config; c["handoffLoad"] = path; c.erase("handoffSave"); CHECK(!map(c).handoff.error.empty());
  }
  auto c = config; c.erase("handoffAt"); CHECK(!map(c).handoff.error.empty());
  c = config; c["handoffLoad"] = "b.muhs"; CHECK(!map(c).handoff.error.empty());
  c = config; c["handoffDiffDump"] = "a.muhs"; CHECK(!map(c).handoff.error.empty());
  c = config; c["online"] = "direct"; CHECK(!map(c).handoff.error.empty());
  { std::ofstream out(root / "agent-test.json"); out << config; }
  CHECK(load(true, root.string().c_str()).handoff.enabled());
  CHECK(handoff.enabled()); // load publishes only an active, Source hook configuration
  CHECK(!load(false, root.string().c_str()).handoff.enabled() && !handoff.enabled());
#else
  CHECK(!yougame::console::friends_handoff_join_enabled());
  CHECK(map({{"friendsHandoffJoin",true}}).arguments.empty());
  CHECK(map(config).arguments.empty());
  CHECK(map(config).environment.empty());
#endif
  std::filesystem::remove_all(root);
  std::puts("handoff agent parsing/gating passed");
}
