// Offline release policies: compile both the legacy default and branch-isolated keys.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_compatibility.h"
#include "yougame_friends.h"
#include "yougame_update.h"
#include "yougame_search.h"
#include "yougame_mode_policy.h"
#include <cstdio>
#include <cstdlib>

int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
  using namespace yougame;
  using nlohmann::json;
  const std::string fp(64, 'a'), rules(64, 'b');
  const std::string suffix = *compat_tag ? std::string("-") + compat_tag : "";
  CHECK(compatibility_key("0.9.80", false, "") == "mu-0.9.80" + suffix);
  CHECK(compatibility_key("0.9.80", true, fp, true, rules) ==
        "mu-0.9.80-st-mod-aaaaaaaaaaaaaaaa-rbbbbbbbb" + suffix);
  for (const std::string version : {std::string("0.9.80"), std::string("dev build/+"), std::string(100, 'v'),
                                   std::string(58, 'v') + "-st", std::string(37, 'v') + "-st"})
    for (bool mod : {false, true}) for (bool engine : {false, true}) for (bool ranked : {false, true}) {
      const auto digest = ranked ? rules : "";
      const auto legacy = compatibility_key(version, mod, fp, engine, digest, "");
      const auto mixed = compatibility_key(version, mod, fp, engine, digest, "mixed");
      const auto key = compatibility_key(version, mod, fp, engine, digest);
      CHECK(key == (*compat_tag ? mixed : legacy));
      CHECK(mixed != legacy && mixed.size() <= 64);
      CHECK(mixed.substr(mixed.size() - 6) == "-mixed");
      CHECK(mixed != compatibility_key(version, mod, fp, !engine, digest, "mixed"));
      CHECK(mixed != compatibility_key(version, mod, fp, engine, digest, "other"));
      CHECK(!mod || mixed.find("-mod-aaaaaaaaaaaaaaaa") != std::string::npos);
      CHECK(!ranked || mixed.find("-rbbbbbbbb") != std::string::npos);
      CHECK(friends::ticket(key, "room123")["lobby"]["compatibility"] == key);
      CHECK(friends::team_ticket(key, "mutabc")["lobby"]["compatibility"] == key);
    }
  CHECK(compatibility_key("0.9.80", true, "bad").empty());
  // A test timer seat never shares a key with a normal seat, and two seats with the same timer do.
  CHECK(compat_tag_with_test_timer("", nullptr).empty() && compat_tag_with_test_timer("mixed", nullptr) == "mixed");
  CHECK(compat_tag_with_test_timer("", "60") == "t60" && compat_tag_with_test_timer("mixed", "60") == "mixed-t60");
  CHECK(compat_tag_with_test_timer("", "abc") == "t0");   // atoi, as the setup block plays it
  for (const char* tag : {"", "mixed"}) {
    const auto plain = compatibility_key("0.9.86", false, "", false, rules, tag);
    const auto t60 = compatibility_key("0.9.86", false, "", false, rules, compat_tag_with_test_timer(tag, "60"));
    CHECK(plain != t60 && t60 == compatibility_key("0.9.86", false, "", false, rules, compat_tag_with_test_timer(tag, "60")));
    CHECK(t60 != compatibility_key("0.9.86", false, "", false, rules, compat_tag_with_test_timer(tag, "90")));
    CHECK(t60.size() <= 64 && t60.find("-rbbbbbbbb") != std::string::npos);
  }
  CHECK(compatibility_key("0.9.80", false, "", false, "bad").empty());

  const json refusal = {{"code", "update_required"}, {"error", "human text"}, {"current", "0.9.81"}, {"build", nullptr}};
  for (const auto& reply : {refusal, json{{"ok", false}, {"status", 409}, {"code", "update_required"}, {"current", "0.9.81"}},
                           json{{"ok", false}, {"result", refusal}}}) {
    CHECK(update::required(update::refusal(reply)));
    CHECK(update::current(update::refusal(reply)) == "0.9.81");
    search_policy::BusyRetry retry;
    CHECK(retry.delay(reply) == 0 && retry.tries() == 0);
  }
  for (const auto& reply : {json(), json::array(), json{{"status", 409}}, json{{"code", 409}},
                           json{{"code", nullptr}}, json{{"error", "update_required"}},
                           json{{"error", update::kMessage}}, json{{"code", "UPDATE_REQUIRED"}},
                           json{{"code", "update_required_other"}}, json{{"code", "version_mismatch"}},
                           json{{"code", "busy"}, {"status", 409}}, json{{"result", "update_required"}}})
    CHECK(!update::required(update::refusal(reply)));
  CHECK(update::current({{"current", "0.9.81\nforged log"}}) == "unknown");
  CHECK(update::current({{"current", "0.9.81-beta"}}) == "0.9.81-beta");
  CHECK(update::current({{"current", 123}}) == "unknown");
  CHECK(update::current(json()) == "unknown");
  CHECK(std::string(update::Required().what()) == update::kMessage);

  // Keep keyboard folding coverage; Direct pool selection no longer depends on code shape.
  std::string folded;
  CHECK(friends::direct_code_from_game(std::string("NAME") + "\x81\x94" + "1234", &folded));
  CHECK(folded == "NAME#1234");
  CHECK(friends::direct_code_from_game("\x82\x60\x81\x94\x82\x50", &folded));
  CHECK(folded == "A#1");
  CHECK(friends::direct_code_from_game("", &folded));
  CHECK(folded.empty());
  for (int mode : {2, 3}) {
    CHECK(mode_policy::matchmaking_route(mode, true, true, false, false, true) == mode_policy::MatchmakingRoute::Slippi);
    CHECK(mode_policy::matchmaking_route(mode, true, false, false, false, true) == mode_policy::MatchmakingRoute::Locked);
  }
  CHECK(!friends::direct_code_from_game("\x81", &folded));

  int versions = 0;
  for (const char* v : {"0.9.81", "0.10.0", "1.0.0", "0.9.80.1", "000.009.081", "0.9.1000000000000000000000000000000"}) {
    CHECK(update::newer(v, "0.9.80"));
    update::Notice notice;
    update::Notice explicit_notice;
    CHECK(explicit_notice.command(std::string("embed update_available ") + v, "0.9.80"));
    CHECK(explicit_notice.available() == update::newer(v, "0.9.80"));
    CHECK(notice.command(std::string("embed update_available ") + v));
    CHECK(notice.available());
    ++versions;
  }
  for (const char* v : {"", "0.9.80", "0.9.80.0", "0.9.9", "0.9", "0", "0.9.80-beta", "v0.9.81", "0..9.81",
                        "0.9.81.", ".0.9.81", "0.9.81 ", "0.9.81\n", "garbage", "-1.2.3"}) {
    CHECK(!update::newer(v, "0.9.80"));
    update::Notice notice;
    update::Notice explicit_notice;
    CHECK(explicit_notice.command(std::string("embed update_available ") + v, "0.9.80"));
    CHECK(explicit_notice.available() == update::newer(v, "0.9.80"));
    CHECK(notice.command(std::string("embed update_available ") + v));
    CHECK(!notice.available());
    ++versions;
  }
  CHECK(!update::newer("0.9.81", "dev"));
  update::Notice notice;
  CHECK(notice.command("embed update_available"));
  CHECK(!notice.command("embed volume 0.5"));
  CHECK(!notice.command("update_available 0.9.81"));
  CHECK(!notice.available());
  notice.refuse();
  CHECK(notice.available());
  CHECK(notice.command("embed update_available 0.9.79"));
  CHECK(notice.available()); // A stale app line cannot erase the site's refusal.
  CHECK(update::version_line() == "Melee Unlocked 0.9.80");
  const std::string id = "01234567-89ab-cdef-0123-456789abcdef";
  const std::string other = "fedcba98-7654-3210-fedc-ba9876543210";
  update::Notice ready;
  CHECK(ready.pending().empty() && !ready.visible());
  CHECK(ready.command("embed update_ready " + id));
  CHECK(ready.pending() == id && !ready.available());
  CHECK(ready.take_ready_log() == id && ready.take_ready_log().empty());
  for (const auto& bad : {"", " ", "0.9.85-online-test.27", "01234567-89AB-cdef-0123-456789abcdef",
       "0123456789ab-cdef-0123-456789abcdef", "01234567-89ab-cdef-0123-456789abcde",
       "01234567-89ab-cdef-0123-456789abcdef0", "01234567-89ab-cdef-0123-456789abcdeg"}) {
    CHECK(ready.command(std::string("embed update_ready ") + bad));
    CHECK(ready.pending() == id && ready.take_ready_log().empty());
  }
  for (const auto& bad : {std::string("embed update_ready"), "embed update_ready\t" + id,
                          "embed update_ready " + id + "\n", "embed update_ready " + id + " extra",
                          "embed update_ready  " + id, "embed update_ready " + id + "\r"}) {
    CHECK(ready.command(bad)); CHECK(ready.pending() == id);
  }
  CHECK(!ready.command("embed volume 1"));
  CHECK(!ready.command("update_ready " + id));
  CHECK(ready.command("embed update_available 0.9.81")); CHECK(ready.available());
  CHECK(ready.pending() == id);
  update::SafePoint safe{true,false,false,false,false};
  CHECK(update::safe(safe)); CHECK(!update::safe({}));
  for (int condition = 0; condition < 5; ++condition) {
    auto blocked = safe;
    if (condition == 0) blocked.idle_scene = false;
    if (condition == 1) blocked.online_busy = true;
    if (condition == 2) blocked.room_or_link = true;
    if (condition == 3) blocked.ranked_set = true;
    if (condition == 4) blocked.friends_room = true;
    CHECK(!update::safe(blocked)); CHECK(!ready.begin(blocked));
    CHECK(ready.presented(5000).empty() && ready.pending() == id);
  }
  CHECK(ready.command("embed update_ready " + other)); CHECK(ready.pending() == other);
  CHECK(ready.begin(safe)); CHECK(ready.visible()); CHECK(!ready.begin(safe));
  CHECK(ready.presented(0).empty()); CHECK(ready.presented(999).empty());
  // A newer installation during the notice replaces the pending id too.
  CHECK(ready.command("embed update_ready " + id));
  CHECK(ready.presented(1000) == "yougame: update accept " + id);
  CHECK(ready.pending().empty());
  for (uint64_t now : {1000,1001,2000,9999}) CHECK(ready.presented(now).empty());
  CHECK(ready.command("embed update_ready " + other)); CHECK(ready.pending().empty());
  CHECK(!ready.begin(safe));
  update::Notice refused_without_ready;
  refused_without_ready.refuse(); CHECK(!refused_without_ready.begin(safe));
  update::Notice refused_with_ready;
  CHECK(refused_with_ready.command("embed update_ready " + id));
  refused_with_ready.refuse(); CHECK(refused_with_ready.begin(safe));
  CHECK(refused_with_ready.presented(500).empty());
  CHECK(refused_with_ready.presented(1500) == "yougame: update accept " + id);
  // From an Online screen the line ends in " online" (the app passes --resume online); it is not a safety condition.
  update::Notice from_online;
  CHECK(from_online.command("embed update_ready " + id));
  auto online_safe = safe; online_safe.online_screen = true; CHECK(update::safe(online_safe));
  CHECK(from_online.begin(online_safe)); CHECK(from_online.presented(0).empty());
  CHECK(from_online.presented(1000) == "yougame: update accept " + id + " online");
  auto online_blocked = online_safe; online_blocked.ranked_set = true; CHECK(!update::safe(online_blocked));
  CHECK(update::resume(nullptr) == update::Resume::Absent);
  CHECK(update::resume("online") == update::Resume::Online);
  for (const auto* value : {"", "Online", "ranked", "online ", "friends"})
    CHECK(update::resume(value) == update::Resume::Unknown);
  CHECK(update::idle_scene(0,0)); CHECK(update::idle_scene(1,0)); CHECK(update::idle_scene(8,0));
  CHECK(!update::idle_scene(0x19,0));  // GM_DEBUG_CUTSCENE, not the title
  CHECK(!update::idle_scene(8,2)); CHECK(!update::idle_scene(8,5));
  CHECK(!update::idle_scene(0x0e,1)); CHECK(!update::idle_scene(0x2b,1));
  CHECK(!update::idle_scene(0xff,0));
  std::printf("yougame_release (%s): %d checks, 40 compatibility combinations, %d version cases passed\n",
              *compat_tag ? compat_tag : "default", checks, versions);
}
