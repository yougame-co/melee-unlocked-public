// Window-free tests of the production Slippi-link request lifecycle.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_slippi_link.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace yougame::slippi_linking;
static std::atomic<int> checks{0};
// Path strings only: these tests never open a sign-in file or discover launcher folders.
static const auto first_file = std::filesystem::absolute("slippi-link-fixture/user.json");
static const auto second_file = std::filesystem::absolute("slippi-link-other/user.json");
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
struct Harness {
  Presentation p;
  Flow flow{p};
  std::atomic<int> sends{0};
  std::atomic<bool> release{false}, retired{false};
  json reply = {{"ok", true}, {"linked", false}, {"now", false}};
  const char* error = nullptr;
  bool hold_cancel = false;
  int logins = 0, restarts = 0;
  std::vector<std::string> logs;
  Harness() { p.enabled = true; }
  ~Harness() { release = true; }
  bool press(Screen screen = Screen::Search, uint32_t scene = 0x80001) {
    return flow.press(screen, scene, [this](const json& q, int ms, auto cancelled) {
      CHECK(q.dump() == "{\"type\":\"slippiLink\"}"); CHECK(ms == 600000);
      ++sends;
      while (!release && (hold_cancel || !cancelled())) std::this_thread::yield();
      retired = true;
      if (cancelled()) throw std::runtime_error("Cancelled");
      if (error) throw std::runtime_error(error);
      return reply;
    });
  }
  void tick(uint32_t scene = 0x80001, bool match = false) {
    flow.tick(scene, match, [this] {
      CHECK(slippi::account::app_sign_in() == first_file); ++logins;
    }, [this] { CHECK(logins == 1); ++restarts; },
        [this](const char* text) { logs.emplace_back(text); });
  }
  void wait_send() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sends && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    CHECK(sends == 1);
  }
  void finish(uint32_t scene = 0x80001, bool match = false, size_t expected_logs = 1) {
    release = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (flow.pending() && std::chrono::steady_clock::now() < deadline) { tick(scene, match); std::this_thread::yield(); }
    CHECK(!flow.pending() && !p.open && retired && logs.size() == expected_logs);
  }
};
int main() {
  // Isolate this process from the real launch environment without reading its value or any file.
#ifdef _WIN32
  CHECK(_wputenv_s(L"YOUGAME_SLIPPI_USER_JSON", second_file.c_str()) == 0);
#else
  CHECK(setenv("YOUGAME_SLIPPI_USER_JSON", second_file.c_str(), 1) == 0);
#endif
  CHECK(slippi::account::app_sign_in() == second_file);
#ifdef _WIN32
  CHECK(_wputenv_s(L"YOUGAME_SLIPPI_USER_JSON", L"") == 0);
#else
  CHECK(unsetenv("YOUGAME_SLIPPI_USER_JSON") == 0);
#endif
  CHECK(slippi::account::app_sign_in().empty());
  for (Screen screen : {Screen::Search, Screen::Refusal}) {
    for (const json& file : {json(), json(""), json("user.json"), json("relative/user.json"),
         json((first_file.parent_path() / "wrong.json").u8string()), json(17), json::array(),
         json(first_file.u8string() + std::string(1, '\0') + "user.json")}) {
      Harness h; h.reply = {{"ok", true}, {"linked", true}, {"now", true}, {"file", file}};
      CHECK(h.press(screen)); h.wait_send(); h.finish();
      CHECK(!h.logins && !h.restarts && h.p.later && !h.p.now);
      CHECK(h.p.line(screen) == kLater && !h.press(screen));
      CHECK(h.logs[0] == "slippi link: linked, no file");
      CHECK(slippi::account::app_sign_in().empty());
    }
    Harness h; h.reply = {{"ok", true}, {"linked", true}, {"now", true}};
    CHECK(h.press(screen)); h.wait_send(); h.finish();
    CHECK(h.p.line(screen) == kLater && !h.logins && !h.restarts);
    CHECK(h.logs[0] == "slippi link: linked, no file");
    CHECK(slippi::account::app_sign_in().empty());
  }
  // Even a supplied valid path cannot grant access after a decline, refusal or next-start answer.
  for (const auto& flags : {json{{"ok", true}, {"linked", false}, {"now", true}},
       json{{"ok", false}, {"linked", true}, {"now", true}},
       json{{"ok", true}, {"linked", true}, {"now", false}}}) {
    Harness h; h.reply = flags; h.reply["file"] = first_file.u8string();
    CHECK(h.press()); h.wait_send(); h.finish();
    CHECK(!h.logins && !h.restarts && slippi::account::app_sign_in().empty());
  }
#ifdef _WIN32
  CHECK(answer({{"ok", true}, {"linked", true}, {"now", true},
                {"file", (first_file.parent_path() / "UsEr.JsOn").u8string()}}) == Answer::Now);
  for (const char* file : {"C:user.json", "\\user.json"})
    CHECK(answer({{"ok", true}, {"linked", true}, {"now", true}, {"file", file}}) == Answer::NoFile);
#else
  CHECK(answer({{"ok", true}, {"linked", true}, {"now", true},
                {"file", (first_file.parent_path() / "UsEr.JsOn").u8string()}}) == Answer::NoFile);
#endif
  {
    Harness h; h.hold_cancel = true;
    h.reply = {{"ok", true}, {"linked", true}, {"now", true}, {"file", second_file.u8string()}};
    CHECK(h.press()); h.wait_send(); h.tick(0x10000);
    CHECK(!h.p.open && h.p.line(Screen::Search) == kSearchCta);
    CHECK(!h.press(Screen::Refusal, 0x10000)); CHECK(h.sends == 1);
    h.finish(0x10000); CHECK(!h.logins && !h.restarts && !h.p.later);
    CHECK(slippi::account::app_sign_in().empty());
  }
  for (uint32_t scene : {0x10000u, 0x80101u, 0x80002u, 0x80201u}) {
    Harness h; h.reply = {{"ok", true}, {"linked", true}, {"now", true}, {"file", second_file.u8string()}};
    CHECK(h.press()); h.wait_send(); h.tick(scene); CHECK(!h.p.open); h.finish(scene);
    CHECK(!h.logins && !h.restarts && slippi::account::app_sign_in().empty()); CHECK(h.logs[0] == log_line(Answer::Cancelled));
  }
  {
    Harness h; CHECK(h.press()); h.wait_send(); h.tick(0x80001, true); h.finish(0x80001, true);
    CHECK(!h.logins && !h.restarts && slippi::account::app_sign_in().empty()); CHECK(h.logs[0] == log_line(Answer::Cancelled));
  }
  CHECK(request() == json({{"type", "slippiLink"}})); CHECK(kTimeout == 600000);
  for (const char* text : {kSearching, kSearchCta, kRefusal, kRefusalCta, kOpen, kLater, kLinkedMenu})
    for (unsigned char c : std::string(text)) CHECK(c < 128);
  {
    Harness h; h.p.enabled = false;
    CHECK(h.p.line(Screen::Search) == kSearching); CHECK(h.p.line(Screen::Refusal) == kRefusal);
    CHECK(!h.press()); CHECK(h.sends == 0);
  }
  {
    Harness h; CHECK(h.p.line(Screen::None).empty()); CHECK(!h.press(Screen::None));
    CHECK(h.p.line(Screen::Search) == kSearchCta); CHECK(h.p.line(Screen::Refusal) == kRefusalCta);
    CHECK(h.press()); h.wait_send(); CHECK(h.p.line(Screen::Search) == kOpen);
    CHECK(!h.press()); CHECK(!h.press(Screen::Refusal));
    // Focus, activation, selection and a menu cooldown are not scene transitions.
    for (int i = 0; i < 600; ++i) h.tick();
    CHECK(h.p.open && h.logs.empty() && h.sends == 1);
    h.finish(); CHECK(h.p.line(Screen::Search) == kSearchCta); CHECK(!h.logins && !h.restarts);
    CHECK(h.logs[0] == "slippi link: not linked");
  }
  for (Screen origin : {Screen::Search, Screen::Refusal}) for (bool wrapped : {false, true}) {
    Harness h;
    const json declined = {{"ok", true}, {"linked", false}, {"now", false}, {"declined", true}};
    h.reply = wrapped ? json{{"ok", true}, {"result", declined}} : declined;
    CHECK(h.press(origin)); h.wait_send(); h.finish();
    CHECK(h.p.declined && !h.logins && !h.restarts);
    CHECK(h.logs[0] == "slippi link: declined for this run");
    for (uint32_t scene : {0x80001u, 0x10000u, 0x80201u}) {
      h.tick(scene); h.flow.cancel([&](const char* text) { h.logs.emplace_back(text); });
      CHECK(h.p.line(Screen::Search) == "Searching YouGame players");
      CHECK(h.p.line(Screen::Refusal) == "Sign in to Slippi to play Direct, Teams and Party");
      CHECK(!h.press(Screen::Search, scene)); CHECK(!h.press(Screen::Refusal, scene));
    }
    // Recreating the flow for another screen must not clear this run's choice.
    Flow next_screen(h.p);
    CHECK(!next_screen.press(Screen::Refusal, 0x10000, [&](const json&, int, auto) {
      ++h.sends; return h.reply;
    }));
    CHECK(h.sends == 1 && h.logs.size() == 1);
  }
  for (Screen origin : {Screen::Search, Screen::Refusal}) {
    for (const json& reply : {json{{"linked", false}},
         json{{"ok", true}, {"linked", false}, {"now", false}},
         json{{"ok", true}, {"linked", false}, {"now", false}, {"declined", false}},
         json{{"ok", true}, {"linked", false}, {"now", false}, {"declined", "true"}}}) {
      Harness h; h.reply = reply;
      CHECK(h.press(origin)); h.wait_send(); h.finish();
      CHECK(!h.p.declined && !h.logins && !h.restarts);
      CHECK(h.p.line(Screen::Search) == kSearchCta); CHECK(h.p.line(Screen::Refusal) == kRefusalCta);
      CHECK(h.press(origin)); h.finish(0x80001, false, 2);
      CHECK(h.sends == 2 && !h.p.declined);
      CHECK(h.p.line(Screen::Search) == kSearchCta); CHECK(h.p.line(Screen::Refusal) == kRefusalCta);
    }
  }
  for (bool linked : {false, true}) for (bool now : {false, true}) for (Screen screen : {Screen::Search, Screen::Refusal}) {
    Harness h; h.reply = {{"ok", true}, {"linked", linked}, {"now", now}, {"file", first_file.u8string()}};
    CHECK(h.press(screen)); h.wait_send(); h.finish();
    CHECK(h.logins == (linked && now)); CHECK(h.restarts == (linked && now));
    CHECK(h.p.line(screen) == (linked && !now ? kLater : linked && now ?
        screen == Screen::Search ? yougame::blend::kSearchBoth : kLinkedMenu :
        screen == Screen::Search ? kSearchCta : kRefusalCta));
    if (linked && !now) { CHECK(h.p.later); CHECK(!h.press()); }
  }
  for (bool linked : {false, true}) for (bool now : {false, true}) for (bool inner_ok : {false, true}) {
    Harness h;
    json result = {{"linked", linked}, {"now", now}, {"file", first_file.u8string()}};
    if (inner_ok) result["ok"] = true;
    h.reply = {{"ok", true}, {"result", result}};
    CHECK(h.press()); h.wait_send(); h.finish();
    CHECK(h.logins == (linked && now)); CHECK(h.restarts == (linked && now));
    CHECK(h.p.later == (linked && !now));
  }
  CHECK(answer({{"ok", false}, {"result", {{"ok", true}, {"linked", true}, {"now", true}}}}) == Answer::Error);
  CHECK(answer({{"ok", true}, {"result", {{"ok", false}, {"linked", true}, {"now", true}}}}) == Answer::Error);
  for (const json& reply : {json(), json::array(), json{{"ok", false}}, json{{"ok", true}},
      json{{"ok", true}, {"linked", true}, {"now", 1}}, json{{"ok", true}, {"linked", "true"}, {"now", true}},
      json{{"ok", false}, {"linked", true}, {"now", true}}}) {
    Harness h; h.reply = reply; CHECK(h.press()); h.wait_send(); h.finish();
    CHECK(!h.logins && !h.restarts && !h.p.later); CHECK(h.logs[0] == log_line(Answer::Error));
  }
  for (const char* error : {"The YouGame app did not answer", "private account details must not be logged"}) {
    Harness h; h.error = error; CHECK(h.press()); h.wait_send(); h.finish();
    CHECK(!h.logins && !h.restarts && !h.p.later); CHECK(h.p.line(Screen::Search) == kSearchCta);
    CHECK(h.logs[0] == log_line(std::string(error) == "The YouGame app did not answer" ? Answer::Timeout : Answer::Error));
  }
  {
    // A fresh flow represents a later answer in the same process, with a different app path.
    Harness h; h.reply = {{"ok", true}, {"linked", true}, {"now", true}, {"file", second_file.u8string()}};
    CHECK(h.press()); h.wait_send(); h.finish();
    CHECK(h.logins == 1 && h.restarts == 1 && slippi::account::app_sign_in() == first_file);
    CHECK(h.logs[0] == "slippi link: linked now");
    std::thread reader([] { for (int i = 0; i < 1000; ++i) CHECK(slippi::account::app_sign_in() == first_file); });
    for (int i = 0; i < 1000; ++i) CHECK(slippi::account::app_sign_in() == first_file);
    reader.join();
  }
  std::printf("slippi link: %d checks passed\n", checks.load());
}
