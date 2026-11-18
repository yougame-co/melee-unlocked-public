// Arena spectator feed: what a fighter's recording becomes on the spectator's side.
// A recording is made the way the game makes one (NativeReplayStream with its event sink), the
// Sender trims it, the Receiver writes the live file, and the replay parser the viewer uses reads
// that file back.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "arena_feed.h"
#include "slippilib/SlippiGame.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

using slippi::NativeReplayStream;
using slippi::arena_feed::Receiver;
using slippi::arena_feed::Sender;
namespace feed = slippi::arena_feed;

namespace {

void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void putf(uint8_t* p, float f) { uint32_t v; std::memcpy(&v, &f, 4); put32(p, v); }

// A fighter's game: records frames the way the native game does, into a Sender.
struct Fighter {
  NativeReplayStream recording;
  Sender sender;
  std::vector<std::vector<uint8_t>> sent;   // every feed message, in order

  explicit Fighter(uint32_t tag) {
    sender.set_game_tag(tag);
    sender.set_out([this](const uint8_t* d, uint32_t n) { sent.emplace_back(d, d + n); });
    recording.set_event_sink([this](const uint8_t* d, uint32_t n) { sender.event(d, n); });
  }
  void start() {
    std::vector<uint8_t> start(0x2F8);
    start[0] = 3; start[1] = 18;
    const std::vector<uint8_t> codes(1500, 0xAB);   // a code list long enough to need three parts
    CHECK(recording.begin_slippi(start, codes, 1700000000));
  }
  // One frame with two fighters (slots 0 and 1); `stick` is what the inputs were on this pass.
  void frame(int32_t f, float stick, int32_t final_frame, int fighters = 2) {
    uint8_t fs[12] = {};
    put32(fs, uint32_t(f)); put32(fs + 4, 0x1000u + uint32_t(f));
    CHECK(recording.append(0x3A, fs, sizeof fs));
    for (int slot = 0; slot < fighters; ++slot) {
      uint8_t pre[66] = {};
      put32(pre, uint32_t(f)); pre[4] = uint8_t(slot);
      putf(pre + 0x18, stick + float(slot));   // joystick X
      CHECK(recording.append(0x37, pre, sizeof pre));
    }
    const uint8_t item[44] = {};
    CHECK(recording.append(0x3B, item, sizeof item));   // an item: never sent
    for (int slot = 0; slot < fighters; ++slot) {
      uint8_t post[92] = {};
      put32(post, uint32_t(f)); post[4] = uint8_t(slot); post[6] = uint8_t(slot ? 0x13 : 0x02);
      CHECK(recording.append(0x38, post, sizeof post));
    }
    uint8_t end[8];
    put32(end, uint32_t(f)); put32(end + 4, uint32_t(final_frame));
    CHECK(recording.append(0x3C, end, sizeof end));
  }
  void game_end() {
    const uint8_t end[6] = {2, 0xFF, 0, 1, 0xFF, 0xFF};
    CHECK(recording.append(0x39, end, sizeof end));
  }
};

std::string temp_file(const char* name) {
  return (std::filesystem::temp_directory_path() / name).u8string();
}

void remove_file(const std::string& path) {
  std::error_code ec;   // a file that cannot be removed is not this test's failure
  std::filesystem::remove(std::filesystem::u8path(path), ec);
}

size_t count_events(const std::string& path, uint8_t command) {
  // Walks the live file's events with the sizes its own table declares.
  FILE* f = std::fopen(path.c_str(), "rb");
  CHECK(f);
  std::vector<uint8_t> d;
  uint8_t chunk[4096];
  for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) d.insert(d.end(), chunk, chunk + n);
  std::fclose(f);
  CHECK(d.size() > 17 && d[15] == 0x35);
  int sizes[256];
  for (int& s : sizes) s = -1;
  for (size_t i = 17; i + 2 < 16u + d[16]; i += 3) sizes[d[i]] = d[i + 1] << 8 | d[i + 2];
  size_t found = 0;
  for (size_t pos = 16u + d[16]; pos < d.size();) {
    const uint8_t c = d[pos];
    CHECK(sizes[c] >= 0);
    if (c == command) ++found;
    pos += 1u + size_t(sizes[c]);
  }
  return found;
}

}  // namespace

int main() {
  const std::vector<uint8_t> codes(1500, 0xAB);

  // 1. A plain match: frames become final two frames late, then Game End.
  {
    const std::string path = temp_file("arena_feed_test_1.slp");
    Fighter a(7);
    Receiver r;
    CHECK(r.open(path, codes, 7));
    a.start();
    for (int32_t f = -123; f <= -100; ++f) a.frame(f, 0.25f, f - 2);
    CHECK(a.sender.stats().frames == 22);          // -123..-102 are final so far
    CHECK(a.sender.stats().max_waiting == 3);
    a.game_end();
    CHECK(a.sender.stats().frames == 24);
    CHECK(a.sender.stats().late == 0 && a.sender.stats().superseded == 0);
    for (const auto& m : a.sent) CHECK(r.message(0, m.data(), uint32_t(m.size())));
    CHECK(r.started() && r.ended() && r.stats().frames == 24 && r.stats().gaps == 0);
    // A frame message for two fighters: header 5, frame 4, length 2, frame start 13, two pre-frames 134.
    CHECK(a.sent[2].size() == 5 + 4 + 2 + 13 + 2 * 67);
    auto game = Slippi::SlippiGame::FromFile(path);
    CHECK(game && game->AreSettingsLoaded() && game->IsProcessingComplete());
    CHECK(game->GetLatestIndex() == -100 && game->GetLastFinalizedFrame() == -100);
    for (int32_t f = -123; f <= -100; ++f) {
      CHECK(game->DoesFrameExist(f));
      const Slippi::FrameData* fd = game->GetFrame(f);
      CHECK(fd->inputsFullyFetched && fd->randomSeedExists && fd->randomSeed == 0x1000u + uint32_t(f));
      CHECK(fd->players.size() == 2 && fd->players.at(0).joystickX == 0.25f && fd->players.at(1).joystickX == 1.25f);
    }
    CHECK(count_events(path, 0x3A) == 24 && count_events(path, 0x37) == 48 && count_events(path, 0x38) == 48);
    CHECK(count_events(path, 0x3B) == 0 && count_events(path, 0x39) == 1 && count_events(path, 0x10) == 3);
    game.reset();
    remove_file(path);
  }

  // 2. A rollback: frames -117..-110 are simulated again with other inputs before they are final.
  {
    const std::string path = temp_file("arena_feed_test_2.slp");
    Fighter a(1);
    Receiver r;
    CHECK(r.open(path, codes, 1));
    a.start();
    for (int32_t f = -123; f <= -110; ++f) a.frame(f, 0.5f, f < -118 ? f : -118);
    CHECK(a.sender.stats().frames == 6);           // -123..-118
    for (int32_t f = -117; f <= -110; ++f) a.frame(f, -0.75f, f);
    CHECK(a.sender.stats().superseded == 8 && a.sender.stats().late == 0 && a.sender.stats().frames == 14);
    a.game_end();
    for (const auto& m : a.sent) CHECK(r.message(0, m.data(), uint32_t(m.size())));
    auto game = Slippi::SlippiGame::FromFile(path);
    CHECK(game && game->GetLatestIndex() == -110 && game->IsProcessingComplete());   // asking for the last frame reads the file
    for (int32_t f = -123; f <= -110; ++f)
      CHECK(game->GetFrame(f)->players.at(0).joystickX == (f < -117 ? 0.5f : -0.75f));
    CHECK(count_events(path, 0x3A) == 14);         // each frame once
    game.reset();
    remove_file(path);
  }

  // 3. Two fighters send the same match; the spectator takes each frame from whoever is first, and
  //    the file can be read while it is still being written.
  {
    const std::string path = temp_file("arena_feed_test_3.slp");
    Fighter a(3), b(3);
    Receiver r;
    CHECK(r.open(path, codes, 3));
    a.start(); b.start();
    for (int32_t f = -123; f <= -64; ++f) { a.frame(f, 0.1f, f); b.frame(f, 0.1f, f); }
    CHECK(a.sent.size() == 61 && b.sent.size() == 61);   // start + 60 frames each
    CHECK(r.message(1, b.sent[0].data(), uint32_t(b.sent[0].size())));
    CHECK(r.message(0, a.sent[0].data(), uint32_t(a.sent[0].size())));   // the second start is ignored
    auto game = Slippi::SlippiGame::FromFile(path);
    CHECK(game);
    CHECK(!game->DoesFrameExist(-123));
    // Fighter A is ahead for the first 30 frames, B for the rest.
    for (size_t i = 1; i <= 30; ++i) CHECK(r.message(0, a.sent[i].data(), uint32_t(a.sent[i].size())));
    CHECK(game->DoesFrameExist(-94) && !game->DoesFrameExist(-93) && game->GetLastFinalizedFrame() == -94);
    CHECK(!game->IsProcessingComplete());
    for (size_t i = 1; i <= 60; ++i) CHECK(r.message(1, b.sent[i].data(), uint32_t(b.sent[i].size())));
    for (size_t i = 31; i <= 60; ++i) CHECK(r.message(0, a.sent[i].data(), uint32_t(a.sent[i].size())));
    CHECK(r.stats().frames == 60 && r.stats().from[0] == 30 && r.stats().from[1] == 30);
    CHECK(r.stats().duplicates == 60 && r.stats().gaps == 0);
    CHECK(game->DoesFrameExist(-64) && game->GetFrame(-64)->inputsFullyFetched);
    // One fighter leaves: the feed goes on from the other until Game End.
    a.sender.stop();
    CHECK(r.message(0, a.sent.back().data(), uint32_t(a.sent.back().size())));
    CHECK(!r.ended());
    b.game_end();
    CHECK(r.message(1, b.sent.back().data(), uint32_t(b.sent.back().size())));
    CHECK(r.ended() && game->GetLatestIndex() == -64 && game->IsProcessingComplete());
    CHECK(count_events(path, 0x3A) == 60);
    game.reset();
    remove_file(path);
  }

  // 4. Messages that do not belong are refused or ignored, and nothing is written for them.
  {
    const std::string path = temp_file("arena_feed_test_4.slp");
    Fighter a(9), other(8);
    Receiver r;
    CHECK(r.open(path, codes, 9));
    a.start(); other.start();
    a.frame(-123, 0.0f, -123); other.frame(-123, 0.0f, -123);
    CHECK(r.message(0, other.sent[0].data(), uint32_t(other.sent[0].size())) && !r.started());   // another game's tag
    CHECK(r.message(0, a.sent[1].data(), uint32_t(a.sent[1].size())) && r.stats().frames == 0);  // a frame before the start
    CHECK(r.message(0, a.sent[0].data(), uint32_t(a.sent[0].size())) && r.started());
    std::vector<uint8_t> cut = a.sent[1];
    cut.pop_back();
    CHECK(!r.message(0, cut.data(), uint32_t(cut.size())) && r.stats().frames == 0);
    std::vector<uint8_t> wrong = a.sent[1];
    wrong[5 + 6] = 0x3B;                                            // not a frame start
    CHECK(!r.message(0, wrong.data(), uint32_t(wrong.size())) && r.stats().frames == 0);
    CHECK(!r.message(0, a.sent[1].data(), 3));
    CHECK(r.message(0, a.sent[1].data(), uint32_t(a.sent[1].size())) && r.stats().frames == 1);
    r.close();
    remove_file(path);
  }

  // 5. A game's tag: the two fighters' Game Start records differ in the player names (each side's
  //    own view) but carry the same settings and seed; another game's seed gives another tag.
  {
    std::vector<uint8_t> a(0x2F8, 0), b2;
    for (size_t i = 0; i < a.size(); ++i) a[i] = uint8_t(i * 7);
    b2 = a;
    for (size_t i = 0x1A4; i < 0x2F0; ++i) b2[i] ^= 0x5A;   // names and codes as the other side has them
    CHECK(feed::game_tag(a.data(), a.size()) == feed::game_tag(b2.data(), b2.size()));
    std::vector<uint8_t> other = a;
    other[0x13D] ^= 1;                                     // another seed
    CHECK(feed::game_tag(a.data(), a.size()) != feed::game_tag(other.data(), other.size()));
    CHECK(feed::game_tag(a.data(), 0) != 0);               // never 0
    // A Sender stamps each game with its own tag unless told one.
    Fighter f(0);
    f.start();
    CHECK(f.sender.tag() != 0 && f.sender.in_game());
  }

  // 6. Who won, from Game End (and the teams from Game Start).
  {
    const uint8_t teams_none[4] = {0, 0, 0, 0};
    const uint8_t singles[6] = {2, 0xFF, 1, 0, 0xFF, 0xFF};        // port 1 placed first
    CHECK(feed::my_result(singles, 6, 1, teams_none, false) == 1);
    CHECK(feed::my_result(singles, 6, 0, teams_none, false) == 0);
    const uint8_t doubles[6] = {3, 0xFF, 0, 2, 1, 3};              // per player: port 0 first
    const uint8_t teams[4] = {0, 0, 1, 1};
    CHECK(feed::my_result(doubles, 6, 1, teams, true) == 1);       // port 1 is port 0's teammate
    CHECK(feed::my_result(doubles, 6, 0, teams, true) == 1);
    CHECK(feed::my_result(doubles, 6, 2, teams, true) == 0);
    CHECK(feed::my_result(doubles, 6, 3, teams, true) == 0);
    const uint8_t no_contest[6] = {7, 1, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(feed::my_result(no_contest, 6, 0, teams_none, false) == -1);
    const uint8_t tie[6] = {1, 0xFF, 0, 0, 0xFF, 0xFF};             // two first places
    CHECK(feed::my_result(tie, 6, 0, teams_none, false) == -1);
    CHECK(feed::my_result(singles, 5, 1, teams_none, false) == -1);
    CHECK(feed::my_result(singles, 6, 4, teams_none, false) == -1);
    std::vector<uint8_t> start(0x2F8, 0);
    for (int port = 0; port < 4; ++port) start[4 + 0x60 + 0x24 * port + 9] = uint8_t(port / 2);
    uint8_t got[4];
    feed::port_teams(start.data(), start.size(), got);
    CHECK(got[0] == 0 && got[1] == 0 && got[2] == 1 && got[3] == 1);
    feed::port_teams(start.data(), 0x20, got);
    CHECK(got[0] == 0 && got[3] == 0);
  }

  // The upload a fighter pays: bytes per second of match for two fighters at 60 frames a second.
  {
    Fighter a(1);
    a.start();
    const uint64_t before = a.sender.stats().bytes;
    for (int32_t f = -123; f < -123 + 600; ++f) a.frame(f, 0.0f, f);
    const uint64_t per_second = (a.sender.stats().bytes - before) / 10;
    std::printf("arena feed: %llu bytes per second for two fighters (start message %llu bytes)\n",
                (unsigned long long)per_second, (unsigned long long)before);
    CHECK(per_second < 12000);
  }
  std::puts("arena_feed_test: ok");
  return 0;
}
