// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_tourney.h"
#include "yougame_tourney_queue.h"
#include "yougame_tourney_delivery.h"
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace yougame::tourney;
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static json hello() {
  return {{"tournament", {{"size", 8}}}, {"tourneyMatch", {{"tournament_id", "12345678-1234-4234-8234-123456789abc"},
    {"round", 1}, {"slot", 2}, {"stage", 31}, {"cpu", {{"character", 9}, {"level", 9}}},
    {"deadline", "2099-10-30T20:00:00.000Z"}}}};
}
static std::array<uint8_t, 8> sample(uint8_t method, uint32_t frames, uint8_t human, uint8_t cpu, uint8_t winner) {
  return {method, uint8_t(frames >> 24), uint8_t(frames >> 16), uint8_t(frames >> 8), uint8_t(frames), human, cpu, winner};
}
static void test_queue() {
  Queue q;
  q.hello(json::object()); q.join(); CHECK(!q.available() && !q.visible() && q.request(0).empty());
  for (const json& bad : {json(), json(true), json(1), json(3), json(16.0), json("16")}) {
    q.hello({{"tournament", {{"size", bad}}}}); CHECK(!q.available());
  }
  q.hello({{"tournament", {{"size", 16}, {"experimental", true}}}});
  CHECK(q.available()); q.join(); q.join();
  CHECK(q.request(0) == "tourney.join"); CHECK(q.request(1).empty());
  q.answer("tourney.join", {{"count", 2}, {"size", 16}}, 0);
  q.join(); CHECK(q.request(0) == "tourney.state");
  json state = {{"phase", "queue"}, {"size", 16}, {"rounds", 4}, {"round", 0}, {"count", 3},
    {"fillsAt", "2099-10-30T20:00:00Z"}, {"bracket", json::array()}};
  q.answer("tourney.state", state, 0);
  CHECK(q.text() == "Tournament: 3 of 16 players | CPUs fill 20:00:00 UTC | Hold B to leave");
  // State retains bracket size but cannot advertise a row after hello revokes it.
  q.hello(json::object()); q.answer("tourney.state", state, 0); CHECK(!q.available());
  q.hello({{"tournament", {{"size", 16}}}}); CHECK(q.available());
  for (int now = 0; now < 1000; ++now) CHECK(q.request(now).empty());
  CHECK(q.request(1000) == "tourney.state"); CHECK(q.request(10000).empty());
  q.answer("tourney.state", state, 10000);
  q.leave(); CHECK(!q.visible() && !q.joined()); CHECK(q.request(10000) == "tourney.leave");
  CHECK(q.request(11000).empty()); q.answer("tourney.leave", {{"ok", true}}, 11000);
  CHECK(q.request(12000).empty());
  // Leave while a join is outstanding sends leave after its answer, without a second join.
  q.join(); CHECK(q.request(12000) == "tourney.join"); q.leave();
  CHECK(q.request(12001).empty()); q.answer("tourney.join", state, 12002);
  CHECK(!q.visible() && q.request(12002) == "tourney.leave");
  q.answer("tourney.leave", json::object(), 12003);
  q.join(); CHECK(q.request(13000) == "tourney.join"); q.answer("tourney.join", state, 13000);
  CHECK(q.request(13000) == "tourney.state");
  const auto assignment = hello()["tourneyMatch"];
  state["phase"] = "ready"; state["round"] = 1; state["tournament_id"] = assignment["tournament_id"];
  state["deadline"] = assignment["deadline"];
  state["bracket"] = json::array({{{"round", 1}, {"slot", 7}, {"stage", 32}, {"winner", nullptr},
    {"p1", {{"you", true}}}, {"p2", {{"you", false}, {"cpu", {{"character", 9}}}}}}});
  q.answer("tourney.state", state, 13000);
  const auto offer = [&] {
    json launch = hello();
    for (const char* field : {"round", "slot", "stage"})
      launch["tourneyMatch"][field] = state["bracket"][0][field];
    launch["tourneyMatch"]["cpu"]["level"] = 3; // authoritative hello, never invented by state
    return q.assignment(launch);
  };
  CHECK(q.assignment(json::object()).is_null());
  json wrong = hello(); CHECK(q.assignment(wrong).is_null()); // another stage/slot
  Session session; session.hello(json::object()); session.assignment(offer());
  CHECK(session.pending()); const auto match = session.launch();
  CHECK(match && match->stage == 32 && match->slot == 7 && match->level == 3);
  session.assignment(offer()); CHECK(!session.launch());
  session.release(); session.assignment(offer()); CHECK(!session.launch());
  // A waiting round offers no game, retaining the view after the VS scene unloads.
  state["phase"] = "waiting"; state["round"] = 2;
  q.answer("tourney.state", state, 14000); CHECK(offer().is_null() && q.visible());
  for (int round=2;round<=4;++round) {
    state["phase"] = "ready"; state["round"] = round; state["bracket"][0]["round"] = round;
    state["bracket"][0]["slot"] = 0;
    q.answer("tourney.state", state, 15000+round*1000);
    session.assignment(offer()); CHECK(session.pending());
    CHECK(session.launch()->round == round); session.release();
  }
  // Malformed, other-player, decided, and human-only pairings cannot start CPU games.
  for (int bad=0;bad<7;++bad) {
    json j=state;
    if (bad==0) j["bracket"][0]["p1"]["you"] = false;
    if (bad==1) j["bracket"][0]["p2"]["you"] = true;
    if (bad==2) j["bracket"][0]["winner"] = 1;
    if (bad==3) j["bracket"][0]["p2"].erase("cpu");
    if (bad==4) j["bracket"][0]["stage"] = 99;
    if (bad==5) j["bracket"][0]["slot"] = 1; // final has only slot zero
    if (bad==6) j["deadline"] = "tomorrow";
    q.answer("tourney.state", j, 20000); CHECK(offer().is_null());
  }
  // Different sizes, including a two-player final, use the state's size and rounds.
  state["size"] = 2; state["rounds"] = 1; state["round"] = 1; state["count"] = 2;
  state["bracket"][0]["round"] = 1; q.answer("tourney.state", state, 21000);
  CHECK(!offer().is_null() && q.text().find("2 of 2 players") != std::string::npos);
  state["phase"] = "out"; state["place"] = "2nd"; q.answer("tourney.state", state, 22000);
  CHECK(q.text() == "Tournament place: 2nd" && !q.joined());
  q.tick(26999); CHECK(q.visible()); q.tick(27000); CHECK(!q.visible());
  q.join(); CHECK(q.request(28000) == "tourney.join"); q.answer("tourney.join", json::object(), 28000);
  state["phase"] = "champion"; q.answer("tourney.state", state, 29000);
  CHECK(q.text() == "Tournament place: 1st"); q.tick(34000); CHECK(!q.visible());
  q.join(); CHECK(q.request(35000) == "tourney.join");
  q.answer("tourney.join", {{"error", "refused"}}, 35000);
  CHECK(!q.joined() && q.visible() && q.text().find("unavailable") != std::string::npos);
  q.leave(); CHECK(q.request(35001) == "tourney.leave");
}
int main() {
  const json good = hello();
  test_queue();
  auto match = parse(good); CHECK(match);
  CHECK(match->stage == 31 && match->character == 9 && match->level == 9);
  for (const auto& empty : {json(), json::object(), json::array(), json(4), json("bad")}) CHECK(!parse(empty));
  for (const char* field : {"tournament_id", "round", "slot", "stage", "cpu", "deadline"}) {
    json j = good; j["tourneyMatch"].erase(field); CHECK(!parse(j));
    for (const auto& bad : {json(), json::array(), json::object(), json(true), json(1.5)}) {
      j = good; j["tourneyMatch"][field] = bad; CHECK(!parse(j));
    }
  }
  for (const char* field : {"round", "slot", "stage"}) {
    json j = good; j["tourneyMatch"][field] = "1"; CHECK(!parse(j));
    j["tourneyMatch"][field] = uint64_t(-1); CHECK(!parse(j));
  }
  for (const char* field : {"character", "level"}) {
    json j = good; j["tourneyMatch"]["cpu"].erase(field); CHECK(!parse(j));
    for (const auto& bad : {json(), json("9"), json(true), json(9.0), json(-1), json(26), json(uint64_t(-1))}) {
      j = good; j["tourneyMatch"]["cpu"][field] = bad; CHECK(!parse(j));
    }
  }
  for (int stage : {2, 3, 8, 28, 31, 32}) {
    json j = good; j["tourneyMatch"]["stage"] = stage; CHECK(parse(j));
  }
  for (int stage : {-1, 0, 1, 9, 30, 33, 65535}) {
    json j = good; j["tourneyMatch"]["stage"] = stage; CHECK(!parse(j));
  }
  for (int character = 0; character < 26; ++character) {
    json j = good; j["tourneyMatch"]["cpu"]["character"] = character;
    CHECK(bool(parse(j)) == (character != 18 && character != 19));
  }
  for (int round = 0; round <= 4; ++round) for (int slot = -1; slot <= 4; ++slot) {
    json j = good; j["tourneyMatch"]["round"] = round; j["tourneyMatch"]["slot"] = slot;
    CHECK(bool(parse(j, nullptr, 8)) == (round >= 1 && round <= 3 && slot >= 0 && slot < (8 >> round)));
  }
  for (const char* bad : {"bad", "2026-02-29T20:00:00Z", "2026-13-01T20:00:00Z", "2026-10-00T20:00:00Z",
      "2026-10-30T24:00:00Z", "2026-10-30T20:60:00Z", "2026-10-30T20:00:00+99:00"}) {
    json j = good; j["tourneyMatch"]["deadline"] = bad; CHECK(!parse(j));
  }
  for (const char* valid : {"2028-02-29T20:00:00Z", "2026-10-30T20:00:00+00:00"}) {
    json j = good; j["tourneyMatch"]["deadline"] = valid; CHECK(parse(j));
  }
  { json j = good; j["tourneyMatch"]["tournament_id"] = "not-a-uuid"; CHECK(!parse(j)); }

  // Exercise the production game setup, all six stage IDs and CSS character IDs.
  for (int stage : {2, 3, 8, 28, 31, 32}) for (int human = 0; human < 26; ++human) {
    uint8_t block[312];
    mu_tourney_game_info(block, stage, human, 3, 7, 9, 9);
    CHECK(block[14] == 0 && block[15] == stage);
    CHECK((block[2] & 0x80) && block[0] == 0x32 && block[2] == 0x8E && block[8] == 0 && block[11] == 0xFF);
    CHECK(block[0x23] == 0xF8 && block[0x27] == 0x0F);
    CHECK(block[16] == 0 && block[17] == 0 && block[18] == 1 && block[19] == 0xE0); // 480 s
    CHECK(block[96] == human && block[97] == 0 && block[98] == 4 && block[99] == 3 && block[106] == 7);
    CHECK(block[132] == 9 && block[133] == 1 && block[134] == 4 && block[146] == 4 && block[147] == 9);
    for (int slot = 2; slot < 6; ++slot) CHECK(block[96 + slot * 36] == 33 && block[97 + slot * 36] == 3);
  }
  {
    uint8_t block[312];
    mu_tourney_game_info(block, 31, 9, 0, 0x78, 9, 9);
    CHECK(block[99] == 0 && block[135] == 1); // distinguish identical fighters
    mu_tourney_game_info(block, 31, 9, 2, 0x78, 9, 9);
    CHECK(block[99] == 2 && block[135] == 0);
  }
  const auto report = [&](uint8_t method, uint32_t frames, uint8_t h, uint8_t c, uint8_t winner) {
    auto p = sample(method, frames, h, c, winner); return result(*match, p.data(), p.size());
  };
  auto win = report(2, 1800, 2, 0, 1);
  CHECK(win == json({{"type", "tourney.result"}, {"tournament_id", match->tournament_id}, {"round", 1},
    {"slot", 2}, {"won", true}, {"frames", 1800}, {"end", "stocks"}, {"stocks", {2, 0}}, {"winner", 1}}));
  auto loss = report(2, 1800, 0, 3, 2); CHECK(loss["won"] == false && loss["winner"] == 2);
  CHECK(report(1, 28800, 2, 1, 1)["end"] == "time");
  CHECK(report(1, 28800, 1, 2, 2)["won"] == false);
  CHECK(report(1, 28800, 2, 2, 0).is_null()); // raw unresolved wire results remain invalid
  // Production game code resolves MU's exact timeout tie before completion/ABI delivery.
  for (int winner : {-1, 0, 1}) {
    const int side = mu_tourney_result_winner(1, winner);
    CHECK(side == (winner == 0 ? 1 : 2));
    auto timeout = report(1, 28800, 2, 2, uint8_t(side));
    CHECK(timeout["end"] == "time" && timeout["winner"] == side);
    CHECK(timeout["won"] == (side == 1) && timeout["frames"] == 28800);
    CHECK(timeout["stocks"] == json({2, 2}));
  }
  CHECK(mu_tourney_result_winner(2, -1) == 2); // stock tie is a CPU win
  for (int winner : {0, 1}) CHECK(mu_tourney_result_winner(2, winner) == winner + 1);
  // Encode the site's result validator, then exercise every terminal game state through
  // the production winner helper and payload builder (including actual simultaneous KO stocks).
  const auto site_accepts = [](const json& j, double elapsed = 480, double deadline_left = 1) {
    if (!j.is_object() || !j["won"].is_boolean() || !integer(j["winner"], 1, 2) ||
        !integer(j["frames"], 0, 72000) || !j["end"].is_string()) return false;
    const std::string end = j["end"];
    if (end.empty() || end.size() > 16 || !j["stocks"].is_array() || j["stocks"].size() != 2) return false;
    for (const auto& stock : j["stocks"]) if (!integer(stock, 0, 4)) return false;
    if (deadline_left <= 0 || elapsed < j["frames"].get<double>() / 60 - 10) return false;
    const int winner = j["winner"];
    if (j["won"] != (winner == 1) || (j["won"] == true && j["frames"] < 600)) return false;
    return end != "stocks" || (j["stocks"][winner - 1] > 0 && j["stocks"][2 - winner] == 0);
  };
  for (auto state : {sample(2, 1800, 2, 0, mu_tourney_result_winner(2, 0)),
                     sample(2, 1800, 0, 3, mu_tourney_result_winner(2, 1)),
                     sample(1, 28800, 2, 1, mu_tourney_result_winner(1, 0)),
                     sample(1, 28800, 2, 2, mu_tourney_result_winner(1, -1)),
                     sample(2, 1800, 0, 0, mu_tourney_result_winner(2, -1)),
                     sample(7, 30, 4, 1, mu_tourney_result_winner(7, -1)),
                     sample(0, 30, 4, 1, mu_tourney_result_winner(0, -1))}) {
    CHECK(site_accepts(result(*match, state.data(), state.size())));
  }
  CHECK(!site_accepts(win, 0));
  CHECK(!site_accepts(win, 480, 0));
  auto stock_tie = report(2, 1800, 0, 0, mu_tourney_result_winner(2, -1));
  CHECK(stock_tie["end"] == "tie" && stock_tie["stocks"] == json({0, 0}));
  CHECK(stock_tie["winner"] == 2 && stock_tie["won"] == false);
  Session tied; tied.hello(good);
  auto tie = sample(1, 28800, 2, 2, uint8_t(mu_tourney_result_winner(1, -1)));
  auto tied_report = tied.finish(tie.data(), tie.size());
  CHECK(tied_report["won"] == false && tied_report["winner"] == 2 && tied_report["end"] == "time");
  CHECK(tied.in_flight() && !tied.finished());
  CHECK(tied.finish(tie.data(), tie.size()).is_null());
  tied.answered(); CHECK(!tied.in_flight() && tied.finished());
  CHECK(tied.finish(tie.data(), tie.size()).is_null());
  auto forfeit = sample(7, 28800, 2, 2, 0);
  CHECK(tied.finish(forfeit.data(), forfeit.size()).is_null());
  for (int method : {0, 7}) {
    auto quit = report(method, 30, 4, 1, 1);
    CHECK(quit["won"] == false && quit["end"] == "quit" && quit["winner"] == 2 && quit["stocks"] == json({4, 1}));
  }
  CHECK(report(2, 599, 1, 0, 1)["frames"] == 599); // never pad a short win up to the server minimum
  CHECK(report(2, 72000, 1, 0, 1)["frames"] == 72000);
  CHECK(report(2, 72001, 1, 0, 1).is_null());
  for (auto p : {sample(2, 600, 4, 1, 1), sample(2, 600, 0, 2, 1),
                 sample(2, 600, 5, 0, 1), sample(3, 600, 1, 0, 1), sample(2, 600, 1, 0, 0)})
    CHECK(result(*match, p.data(), p.size()).is_null());
  CHECK(result(*match, nullptr, 8).is_null());
  auto p = sample(2, 600, 1, 0, 1); CHECK(result(*match, p.data(), 7).is_null());
  Session session; CHECK(session.finish(p.data(), p.size()).is_null());
  session.hello(good); CHECK(!session.finish(p.data(), p.size()).is_null());
  CHECK(session.finish(p.data(), p.size()).is_null());
  session.hello(json::object()); CHECK(session.match()->slot == 2);
  Session normal; normal.hello(json::object()); normal.hello(good); CHECK(!normal.match());
  Session malformed; malformed.hello({{"tourneyMatch", 42}}); malformed.hello(good); CHECK(!malformed.match());
  // Boot polls without awaiting hello. The same first answer can launch once from a safe menu.
  Session delayed;
  for (int poll = 0; poll < 10; ++poll) CHECK(!delayed.pending() && !delayed.launch());
  delayed.hello(good); CHECK(delayed.pending());
  const auto launch = delayed.launch(); CHECK(launch && launch->stage == 31 && launch->level == 9);
  CHECK(!delayed.pending() && !delayed.launch());
  delayed.hello(good); CHECK(!delayed.launch());
  CHECK(!normal.pending() && !normal.launch());
  CHECK(!malformed.pending() && !malformed.launch());
  Session immediate; immediate.hello(good); CHECK(immediate.launch()); CHECK(!immediate.launch());

  // UTC parsing, fractional seconds/offsets, 60-second grace, and monotonic expiry.
  json dated = good;
  dated["tourneyMatch"]["deadline"] = "2026-10-30T21:05:00.123456+00:00";
  const auto date = parse(dated); CHECK(date);
  dated["tourneyMatch"]["deadline"] = "2026-10-30T21:05:00.123456Z";
  CHECK(parse(dated)->deadline_utc == date->deadline_utc);
  dated["tourneyMatch"]["deadline"] = "2026-10-30T22:05:00.123456+01:00";
  CHECK(parse(dated)->deadline_utc == date->deadline_utc);
  CHECK(std::chrono::duration_cast<std::chrono::microseconds>(date->deadline_utc.time_since_epoch()).count() == 1793394300123456LL);
  const auto now = Session::Clock::now();
  Session past;
  CHECK(past.hello(dated, date->deadline_utc + std::chrono::seconds(61), now) == "tournament: past deadline");
  CHECK(!past.pending(now) && !past.launch(now));
  Session grace;
  CHECK(grace.hello(dated, date->deadline_utc + std::chrono::seconds(59), now).empty());
  CHECK(grace.launch(now));
  CHECK(!grace.expired(now + std::chrono::seconds(1)));
  CHECK(grace.expired(now + std::chrono::seconds(2)));
  CHECK(grace.finish(p.data(), p.size(), now + std::chrono::seconds(2)).is_null());
  Session css;
  css.hello(dated, date->deadline_utc, now);
  CHECK(css.pending(now + std::chrono::seconds(60)));
  CHECK(!css.launch(now + std::chrono::seconds(61)));
  for (int character : {18, 19}) {
    json j = good; j["tourneyMatch"]["cpu"]["character"] = character;
    Session unsupported;
    CHECK(unsupported.hello(j) == "tournament: CPU character " + std::to_string(character) + " not offered by YouGame");
    CHECK(!unsupported.pending());
  }
  Session invalid_date;
  dated["tourneyMatch"]["deadline"] = "tomorrow";
  CHECK(invalid_date.hello(dated) == "tournament: malformed tourneyMatch");
  CHECK(!invalid_date.pending());
  // Returning to menu allows later bracket coordinates, never the same game again.
  immediate.release(); CHECK(immediate.awaiting() && !immediate.match());
  immediate.hello(good); CHECK(!immediate.pending());
  immediate.hello(json::object()); CHECK(immediate.awaiting());
  json next = good; next["tourneyMatch"]["round"] = 2; next["tourneyMatch"]["slot"] = 0;
  immediate.hello(next); CHECK(immediate.pending() && immediate.launch());

  // The same absolute deadline includes a contended lock and a stopped stdout reader.
  {
    const auto start = DeliveryClock::now();
    std::mutex mutex;
    mutex.lock();
    auto waiting = detached([&mutex, start] {
      std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
      return lock_until(lock, start + std::chrono::milliseconds(40));
    });
    CHECK(waiting.wait_until(start + std::chrono::seconds(1)) == std::future_status::ready);
    CHECK(!waiting.get()); mutex.unlock();
    CHECK(DeliveryClock::now() - start < std::chrono::seconds(1));
  }
  {
    std::promise<void> release;
    auto unblock = release.get_future().share();
    const auto start = DeliveryClock::now();
    { auto blocked = detached([unblock] { unblock.wait(); });
      CHECK(blocked.wait_until(start + std::chrono::milliseconds(40)) == std::future_status::timeout); }
    CHECK(DeliveryClock::now() - start < std::chrono::seconds(1)); // future destruction never joins
    release.set_value();
  }
#ifndef _WIN32
  {
    int pipefd[2]; CHECK(pipe(pipefd) == 0);
    int flags = fcntl(pipefd[1], F_GETFL);
    CHECK(fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK) == 0);
    std::string fill(4096, 'x');
    while (write(pipefd[1], fill.data(), fill.size()) > 0) {}
    CHECK(fcntl(pipefd[1], F_SETFL, flags) == 0);
    // Darwin adds a flag on the fixture's first F_SETFL; compare the writer against
    // the actual blocking descriptor it receives, including every existing flag.
    flags = fcntl(pipefd[1], F_GETFL); CHECK((flags & O_NONBLOCK) == 0);
    const auto start = DeliveryClock::now();
    CHECK(!pipe_write_until(pipefd[1], "yg result\n", start + std::chrono::milliseconds(40)));
    CHECK(DeliveryClock::now() - start < std::chrono::seconds(1));
    CHECK(fcntl(pipefd[1], F_GETFL) == flags);
    char drain[8192]; CHECK(read(pipefd[0], drain, sizeof drain) > 0);
    CHECK(pipe_write_until(pipefd[1], "yg result\n", DeliveryClock::now() + std::chrono::seconds(1)));
    close(pipefd[0]); close(pipefd[1]);
  }
#endif

  for (bool no_answer : {false, true}) for (bool reconnects : {false, true}) {
    std::vector<json> sent; int reconnect_count = 0;
    int attempts = deliver(win, [&](const json& body) { sent.push_back(body); return no_answer ? Reply::NoAnswer : Reply::Answer; },
      [&] { ++reconnect_count; return reconnects; });
    CHECK(attempts == (no_answer && reconnects ? 2 : 1));
    CHECK(reconnect_count == (no_answer ? 1 : 0));
    CHECK(sent.size() == size_t(attempts));
    for (const auto& body : sent) CHECK(body == win);
  }
  std::printf("%d tournament contract checks passed\n", checks);
}
