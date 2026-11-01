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
  std::printf("yougame_release (%s): %d checks, 40 compatibility combinations, %d version cases passed\n",
              *compat_tag ? compat_tag : "default", checks, versions);
}
