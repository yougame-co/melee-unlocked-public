// Blended Unranked's coordinator (yougame_blend.h) through every order of events, on a fake clock,
// and why a Slippi login cannot be used. No sockets, threads, servers, accounts or game.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_blend.h"
#include "slippi_account.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define blend_test_pid _getpid
#else
#include <unistd.h>
#define blend_test_pid getpid
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
using yougame::blend::Coordinator;
using yougame::blend::Side;
using Lines = std::vector<std::string>;
int cases = 0;

std::string line(const std::string& text) { return "yougame: unranked blend: " + text; }
// The lines decided since the last look, exactly.
bool said(Coordinator& c, const Lines& expected) {
  Lines want;
  for (const auto& text : expected) want.push_back(line(text));
  return c.take_log() == want;
}
bool winner(const Coordinator& c, Side side) { Side s = side == Side::YouGame ? Side::Slippi : Side::YouGame; return c.committed(&s) && s == side; }

// Someone else is in YouGame's queue: YouGame alone, and it matches inside the 15 s.
void youGame_first_and_youGame_matches() {
  Coordinator c(1000);
  c.tick(1500);
  CHECK(!c.take_start_slippi() && said(c, {}));
  c.searching(2, 1400);
  CHECK(said(c, {"YouGame queue waiting 2 -> YouGame first"}));
  c.tick(4000);                       // a word arrived: the 3 s timer is off
  c.tick(16399);                      // 15 s from the word, not from the start
  CHECK(!c.take_start_slippi() && said(c, {}) && !c.live(Side::Slippi));
  c.searching(3, 5000);               // later words with others there change nothing
  CHECK(said(c, {}));
  CHECK(c.matched(Side::YouGame, 9000));
  CHECK(said(c, {"matched on YouGame after 8000 ms; the other was not searching"}));
  CHECK(winner(c, Side::YouGame) && c.live(Side::YouGame) && !c.live(Side::Slippi));
  c.tick(20000);
  CHECK(!c.take_start_slippi());      // the timers never start Slippi after a commit
  CHECK(!c.matched(Side::Slippi, 20000));
  CHECK(said(c, {"declined a late Slippi match"}));
  CHECK(winner(c, Side::YouGame) && !c.over());
  ++cases;
}

// YouGame first, nobody matched in 15 s: Slippi too, and Slippi is first.
void youGame_first_then_slippi_wins() {
  Coordinator c(0);
  c.searching(2, 2500);
  CHECK(said(c, {"YouGame queue waiting 2 -> YouGame first"}));
  c.tick(17499);
  CHECK(!c.take_start_slippi());
  c.tick(17500);
  CHECK(said(c, {"Slippi search started after 17500 ms"}));
  CHECK(c.take_start_slippi() && !c.take_start_slippi());   // once
  CHECK(c.live(Side::YouGame) && c.live(Side::Slippi));
  CHECK(c.matched(Side::Slippi, 30000));
  CHECK(said(c, {"matched on Slippi after 30000 ms; cancelled the other"}));
  CHECK(winner(c, Side::Slippi) && !c.live(Side::YouGame) && c.live(Side::Slippi));
  CHECK(!c.matched(Side::YouGame, 30001));
  CHECK(said(c, {"declined a late YouGame match"}));
  ++cases;
}

// Alone in YouGame's queue: both at once, and YouGame is first.
void alone_both_at_once() {
  Coordinator c(1000);
  c.searching(1, 1300);
  CHECK(said(c, {"YouGame queue waiting 1 -> both at once", "Slippi search started after 300 ms"}));
  CHECK(c.take_start_slippi());
  c.searching(1, 2300);
  c.tick(30000);
  CHECK(said(c, {}) && !c.take_start_slippi());
  CHECK(c.matched(Side::YouGame, 5000));
  CHECK(said(c, {"matched on YouGame after 4000 ms; cancelled the other"}));
  CHECK(!c.live(Side::Slippi));
  // A count of 0 (malformed) reads as alone.
  Coordinator zero(0);
  zero.searching(0, 50);
  CHECK(said(zero, {"YouGame queue waiting 0 -> both at once", "Slippi search started after 50 ms"}));
  ++cases;
}

// No word from the queue in 3 s: Slippi too. A word after that only reports.
void no_queue_word_in_3s() {
  Coordinator c(0);
  c.tick(2999);
  CHECK(!c.take_start_slippi() && said(c, {}));
  c.tick(3000);
  CHECK(said(c, {"Slippi search started after 3000 ms"}) && c.take_start_slippi());
  c.searching(2, 3100);
  CHECK(said(c, {"YouGame queue waiting 2 -> both at once"}));
  c.tick(40000);
  CHECK(!c.take_start_slippi() && said(c, {}));
  ++cases;
}

// Both sides given an opponent in the same tick: the first event in wins, either way round.
void same_tick() {
  for (Side first : {Side::Slippi, Side::YouGame}) {
    const Side second = first == Side::Slippi ? Side::YouGame : Side::Slippi;
    Coordinator c(0);
    c.searching(1, 100);
    CHECK(c.take_start_slippi());
    c.take_log();
    CHECK(c.matched(first, 5000));
    CHECK(!c.matched(second, 5000));
    CHECK(said(c, {std::string("matched on ") + yougame::blend::name(first) + " after 5000 ms; cancelled the other",
                   std::string("declined a late ") + yougame::blend::name(second) + " match"}));
    CHECK(winner(c, first) && c.live(first) && !c.live(second));
  }
  ++cases;
}

// A side that fails before any commit leaves the other searching; both failing is the search failing.
void failures() {
  {  // YouGame fails before Slippi started: Slippi starts now.
    Coordinator c(0);
    c.failed(Side::YouGame, 700);
    CHECK(said(c, {"Slippi search started after 700 ms"}) && c.take_start_slippi());
    CHECK(!c.live(Side::YouGame) && !c.over());
    CHECK(!c.matched(Side::YouGame, 800));   // its search is over
    c.take_log();
    CHECK(c.matched(Side::Slippi, 9000));
    CHECK(said(c, {"matched on Slippi after 9000 ms; the other was not searching"}));
  }
  {  // Slippi fails: YouGame goes on alone, and Slippi is never restarted.
    Coordinator c(0);
    c.searching(1, 100);
    CHECK(c.take_start_slippi());
    c.take_log();
    c.failed(Side::Slippi, 2000);
    c.tick(60000);
    CHECK(!c.over() && !c.live(Side::Slippi) && c.live(Side::YouGame) && !c.take_start_slippi() && said(c, {}));
    CHECK(c.matched(Side::YouGame, 61000));
    CHECK(said(c, {"matched on YouGame after 61000 ms; the other was not searching"}));
  }
  {  // Both fail, Slippi first: the search is over.
    Coordinator c(0);
    c.searching(1, 100);
    c.failed(Side::Slippi, 200);
    CHECK(!c.over());
    c.failed(Side::YouGame, 300);
    CHECK(c.over() && !c.committed() && !c.take_start_slippi());
  }
  {  // Both fail, YouGame first (which started Slippi).
    Coordinator c(0);
    c.failed(Side::YouGame, 100);
    CHECK(c.take_start_slippi() && !c.over());
    c.failed(Side::Slippi, 900);
    CHECK(c.over());
  }
  {  // A failure from a side that is not searching changes nothing.
    Coordinator c(0);
    c.failed(Side::Slippi, 100);              // never started
    CHECK(!c.live(Side::Slippi) && !c.over() && said(c, {}));
    c.tick(3000);
    CHECK(c.take_start_slippi());             // the timer still starts it
    c.take_log();
    CHECK(c.matched(Side::YouGame, 4000));
    c.failed(Side::Slippi, 4100);             // the cancelled side ends with its cancel
    CHECK(winner(c, Side::YouGame) && !c.over() && said(c, {"matched on YouGame after 4000 ms; cancelled the other"}));
  }
  ++cases;
}

// The committed side fails to connect: that side's own failure; the cancelled side is never resumed.
void committed_side_fails() {
  for (Side side : {Side::YouGame, Side::Slippi}) {
    Coordinator c(0);
    c.searching(1, 100);
    CHECK(c.take_start_slippi());
    CHECK(c.matched(side, 2000));
    c.take_log();
    c.failed(side, 3000);
    c.tick(99999);
    CHECK(!c.take_start_slippi() && !c.live(Side::YouGame) && !c.live(Side::Slippi));
    CHECK(winner(c, side) && !c.over() && said(c, {}));
    const Side other = side == Side::YouGame ? Side::Slippi : Side::YouGame;
    CHECK(!c.matched(other, 4000));
    CHECK(said(c, {std::string("declined a late ") + yougame::blend::name(other) + " match"}));
  }
  ++cases;
}

// The player leaves: while connecting to the committed opponent, while searching, before Slippi began.
void cancel() {
  {  // during connecting
    Coordinator c(0);
    c.searching(1, 100);
    c.take_start_slippi();
    CHECK(c.matched(Side::Slippi, 4000));
    c.take_log();
    c.stop();
    CHECK(!c.live(Side::Slippi) && !c.live(Side::YouGame) && !c.over());
    CHECK(!c.matched(Side::YouGame, 4100));
    CHECK(said(c, {"declined a late YouGame match"}));
  }
  {  // during YouGame first
    Coordinator c(0);
    c.searching(2, 100);
    c.take_log();
    c.stop();
    c.tick(20000);
    c.searching(1, 20000);
    CHECK(!c.take_start_slippi() && !c.over() && said(c, {}));
    CHECK(!c.matched(Side::YouGame, 20100));
    CHECK(said(c, {"declined a late YouGame match"}));
  }
  {  // a start decided but not yet taken
    Coordinator c(0);
    c.searching(1, 100);
    c.stop();
    CHECK(!c.take_start_slippi());
    c.failed(Side::Slippi, 200);
    CHECK(!c.over());
  }
  ++cases;
}

// Everyone else in YouGame's queue left during the 15 s: Slippi starts then.
void queue_emptied() {
  Coordinator c(0);
  c.searching(3, 200);
  c.searching(2, 1200);
  CHECK(said(c, {"YouGame queue waiting 3 -> YouGame first"}) && !c.take_start_slippi());
  c.searching(1, 6200);
  CHECK(said(c, {"YouGame queue waiting 1 -> both at once", "Slippi search started after 6200 ms"}));
  CHECK(c.take_start_slippi());
  c.tick(15200);
  c.tick(16000);
  CHECK(!c.take_start_slippi() && said(c, {}));
  ++cases;
}

// Edge orders: a commit before Slippi's decided start was taken, a match before any word, a match
// from a side that never started.
void edges() {
  {
    Coordinator c(0);
    c.searching(1, 100);                       // start decided, not taken yet
    CHECK(c.matched(Side::YouGame, 150));
    CHECK(said(c, {"YouGame queue waiting 1 -> both at once", "Slippi search started after 100 ms",
                   "matched on YouGame after 150 ms; cancelled the other"}));
    CHECK(!c.take_start_slippi());
  }
  {
    Coordinator c(0);
    CHECK(c.matched(Side::YouGame, 1000));     // a queue that never said "searching"
    c.tick(3000);
    CHECK(!c.take_start_slippi() && said(c, {"matched on YouGame after 1000 ms; the other was not searching"}));
  }
  {
    Coordinator c(0);
    CHECK(c.matched(Side::YouGame, 500));
    CHECK(!c.matched(Side::YouGame, 600));     // a second word from the winner changes nothing
    CHECK(winner(c, Side::YouGame) && c.live(Side::YouGame));
  }
  {
    Coordinator c(0);
    CHECK(!c.matched(Side::Slippi, 100));      // Slippi never started
    CHECK(said(c, {"declined a late Slippi match"}));
    CHECK(c.matched(Side::YouGame, 200));
  }
  CHECK(yougame::blend::kQueueWordMs == 3000 && yougame::blend::kYouGameFirstMs == 15000);
  ++cases;
}

void write(const std::filesystem::path& file, const std::string& text) {
  std::filesystem::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}

// Why the app's Unranked cannot use a Slippi login: the reason, never the path.
void login_reasons(const std::filesystem::path& root) {
  using slippi::account::Unusable;
  using slippi::account::why_unusable;
  const auto local = root / "MU" / "User" / "Slippi";
  const auto launcher = root / "Roaming" / "Slippi Launcher";
  const std::vector<std::filesystem::path> roots{launcher};
  CHECK(why_unusable(local, roots) == Unusable::FileMissing);
  std::filesystem::create_directories(launcher);
  CHECK(why_unusable(local, roots) == Unusable::FileMissing);
  write(launcher / "netplay" / "User" / "Slippi" / "user.json", R"({"uid":"u","playKey":"k","displayName":"Test"})");
  CHECK(why_unusable(local, roots) == Unusable::NoConnectCode);
  CHECK(!slippi::account::resolve(local, roots));
  write(local / "user.json", "{broken");        // the local file comes first
  CHECK(why_unusable(local, roots) == Unusable::Unreadable);
  write(local / "user.json", R"({"connectCode":"TEST#1"})");
  CHECK(why_unusable(local, roots) == Unusable::Unreadable);
  write(local / "user.json", R"({"uid":"u","playKey":"k","connectCode":""})");
  CHECK(why_unusable(local, roots) == Unusable::NoConnectCode);
  CHECK(std::string(slippi::account::unusable_text(Unusable::FileMissing)) == "file missing");
  CHECK(std::string(slippi::account::unusable_text(Unusable::Unreadable)) == "unreadable");
  CHECK(std::string(slippi::account::unusable_text(Unusable::NoConnectCode)) == "no connect code");
  // why_missing (now sharing the file list) answers as before.
  const auto other = root / "Other";
  CHECK(slippi::account::why_missing(other / "MU", {other / "Launcher"}) == slippi::account::Missing::Launcher);
  std::filesystem::create_directories(other / "Launcher");
  CHECK(slippi::account::why_missing(other / "MU", {other / "Launcher"}) == slippi::account::Missing::SignIn);
  CHECK(slippi::account::why_missing(local, roots) == slippi::account::Missing::ConnectCode);
  ++cases;
}

void set_app_file(const std::filesystem::path& file) {
#ifdef _WIN32
  _wputenv_s(L"YOUGAME_SLIPPI_USER_JSON", file.wstring().c_str());   // empty removes it
#else
  if (file.empty()) unsetenv("YOUGAME_SLIPPI_USER_JSON");
  else setenv("YOUGAME_SLIPPI_USER_JSON", file.c_str(), 1);
#endif
}
std::string read_all(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// The app names the sign-in file (YOUGAME_SLIPPI_USER_JSON): exactly that file is read, never written,
// and no launcher folder is searched, even when one holds a good login. Unset: as before.
void app_login_file(const std::filesystem::path& root) {
  using slippi::account::Unusable;
  const auto local = root / "MU" / "User" / "Slippi";
  const auto launcher = root / "Roaming" / "Slippi Launcher";
  const std::vector<std::filesystem::path> roots{launcher};
  const auto good = [](const char* code) {
    return nlohmann::json{{"uid", "synthetic-test-user"}, {"playKey", "synthetic-test-key"}, {"connectCode", code}}.dump();
  };
  write(local / "user.json", good("LOCAL#1"));
  write(launcher / "netplay" / "User" / "Slippi" / "user.json", good("LAUNCH#1"));
  const auto app = root / "App" / "slippi-user.json";
  // Unset: today's search (the local file first).
  set_app_file({});
  CHECK(slippi::account::app_sign_in().empty());
  auto found = slippi::account::resolve(local, roots);
  CHECK(found && slippi::account::text(found.data, "connectCode") == "LOCAL#1");
  // Set, the file missing: nothing, even with two good logins elsewhere.
  set_app_file(app);
  CHECK(slippi::account::app_sign_in() == app);
  CHECK(!slippi::account::resolve(local, roots));
  CHECK(slippi::account::why_unusable(local, roots) == Unusable::FileMissing);
  // Set, unreadable or incomplete.
  write(app, "{broken");
  CHECK(!slippi::account::resolve(local, roots) && slippi::account::why_unusable(local, roots) == Unusable::Unreadable);
  write(app, R"({"connectCode":"APP#1"})");
  CHECK(!slippi::account::resolve(local, roots) && slippi::account::why_unusable(local, roots) == Unusable::Unreadable);
  write(app, R"({"uid":"u","playKey":"k"})");
  CHECK(!slippi::account::resolve(local, roots) && slippi::account::why_unusable(local, roots) == Unusable::NoConnectCode);
  // Set and good: exactly that file, read only.
  write(app, good("APP#1"));
  const auto before = read_all(app);
  const auto stamp = std::filesystem::last_write_time(app);
  found = slippi::account::resolve(local, roots);
  CHECK(found && found.file == app && slippi::account::text(found.data, "connectCode") == "APP#1");
  CHECK(read_all(app) == before && std::filesystem::last_write_time(app) == stamp);
  CHECK(read_all(local / "user.json") == good("LOCAL#1"));
  set_app_file({});
  found = slippi::account::resolve(local, roots);
  CHECK(found && slippi::account::text(found.data, "connectCode") == "LOCAL#1");
  ++cases;
}

// The app's link line: exact JSON, only the player's own code, nothing that could break the JSON.
void link_line() {
  using yougame::blend::slippi_link_line;
  CHECK(slippi_link_line(true, "TEST#123") == R"(yg {"slippi":{"linked":true,"tag":"TEST#123"}})");
  CHECK(slippi_link_line(false, "") == R"(yg {"slippi":{"linked":false}})");
  CHECK(slippi_link_line(false, "TEST#123") == R"(yg {"slippi":{"linked":false}})");   // no code when not linked
  CHECK(slippi_link_line(true, "A\"B\\C\nD#1 ") == R"(yg {"slippi":{"linked":true,"tag":"ABCD#1"}})");
  for (const auto& line : {slippi_link_line(true, "TEST#123"), slippi_link_line(false, ""), slippi_link_line(true, "\"}\\")}) {
    CHECK(line.rfind("yg ", 0) == 0);
    const auto j = nlohmann::json::parse(line.substr(3), nullptr, false);
    CHECK(j.is_object() && j.size() == 1 && j.count("slippi") && j["slippi"].is_object());
    const auto& link = j["slippi"];
    CHECK(link.count("linked") && link["linked"].is_boolean());
    CHECK(link.size() == (link["linked"].get<bool>() ? 2u : 1u));
  }
  CHECK(std::string(yougame::blend::kSearchBoth) == "Searching Slippi + YouGame");
  CHECK(std::string(yougame::blend::kSearchYouGameOnly) == "Searching YouGame players");
  ++cases;
}
}  // namespace

int main() {
  youGame_first_and_youGame_matches();
  youGame_first_then_slippi_wins();
  alone_both_at_once();
  no_queue_word_in_3s();
  same_tick();
  failures();
  committed_side_fails();
  cancel();
  queue_emptied();
  edges();
  // Fixture files under the working folder, named after this process and removed at the end.
  const auto root = std::filesystem::current_path() / ("yougame-blend-fixture-" + std::to_string(blend_test_pid()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  set_app_file({});
  login_reasons(root / "reasons");
  app_login_file(root / "app");
  link_line();
  std::filesystem::remove_all(root, ec);
  std::printf("yougame_blend: %d coordinator and login-reason groups passed\n", cases);
}
