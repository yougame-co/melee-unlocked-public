/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#include "yougame_squad.h"
#include "squad_test_spec.h"
#include "yougame_compatibility.h"
#include "yougame_mode_policy.h"
#include "native_slippi_bridge.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#if MU_SQUAD
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)

static MuSquadSide side(unsigned count, unsigned seed) {
  MuSquadSide result{};
  result.count = (unsigned char)count; // a count past the array is kept, never written past it
  for (unsigned i = 0; i < count && i < MU_SQUAD_LINEUP; ++i) {
    const unsigned kind = (seed + i) % 26;
    result.members[i] = {(unsigned char)kind, (unsigned char)(i % mu_squad_colors(kind))};
  }
  return result;
}

static void harness_parser() {
  MuSquadSide out{};
  for (const char* good : {"0/0,1/0,2/0,3/0", "25/4,24/0,0/5,9/3", "4/0,14/0,18/0,2/0", "19/0,14/0,4/0,19/1"}) {
    CHECK(slippi::squad_test::parse(good, &out));
    CHECK(out.count == MU_SQUAD_LINEUP);
  }
  // Exactly four fighters: version 1's valid one, two and version 2's three fighter specs are refused.
  for (const char* bad : {"0/0", "25/4", "4/0,14/0", "4/0,14/0,18/0", "0/0,1/0,2/0", "0/0,1/0,2/0,3/0,4/0",
                          "", "-1/0", "+1/0", "26/0", "4/6", "2/4", "4", "4/", "/0",
                          "4/0,", ",4/0", "4/0,,2/0", "4/0/2", "4/0x", "4/0:2/0",
                          " 4/0", "4/0 ", "999999999999999999999/0",
                          "4/0,14/0,18/0,2/0,", "4/0,14/0,18/0,26/0", "4/0,14/0,18/0,4/6", "4/0,,14/0,18/0,2/0",
                          " 4/0,14/0,18/0,2/0", "4/0,14/0,18/0,2/0 "}) {
    const auto before = out;
    CHECK(!slippi::squad_test::parse(bad, &out));
    CHECK(!std::memcmp(&out, &before, sizeof out));
  }
  CHECK(!slippi::squad_test::parse(nullptr, &out));
  CHECK(!slippi::squad_test::parse("4/0,14/0,18/0,2/0", nullptr));
  for (unsigned ck = 0; ck < 26; ++ck) for (unsigned color = 0; color < 6; ++color) {
    char spec[32]; std::snprintf(spec, sizeof spec, "%u/%u,0/0,1/0,2/0", ck, color);
    CHECK(bool(slippi::squad_test::parse(spec, &out)) == (color < mu_squad_colors(ck)));
    std::snprintf(spec, sizeof spec, "%u/%u", ck, color);
    CHECK(!slippi::squad_test::parse(spec, &out)); // one fighter is never a squad
  }
}

static void codec() {
  static_assert(MU_SQUAD_VERSION == 3 && MU_SQUAD_LINEUP == 4 && MU_SQUAD_WIRE_MAX == 20, "v3 wire");
  static_assert(sizeof(MuSquadSide{}.members) / sizeof(MuSquadFighter) == MU_SQUAD_LINEUP, "4 slots");
  // Every count but exactly four is refused by side_valid, combine and encode (version 2 took exactly 3,
  // version 1 took 1..4).
  for (unsigned a = 0; a <= 5; ++a) for (unsigned b = 0; b <= 5; ++b) {
    if (a == MU_SQUAD_LINEUP && b == MU_SQUAD_LINEUP) continue;
    auto p1 = side(a, 14), p2 = side(b, 18);
    CHECK(!mu_squad_side_valid(&p1) || !mu_squad_side_valid(&p2));
    MuSquadSetup setup{}, untouched{};
    CHECK(!mu_squad_combine(0, &p1, 1, &p2, &setup));
    CHECK(!std::memcmp(&setup, &untouched, sizeof setup));
    setup.stocks = (unsigned char)(a > b ? a : b); setup.sides[0] = p1; setup.sides[1] = p2;
    std::array<unsigned char, 64> bytes{};
    CHECK(!mu_squad_setup_valid(&setup));
    CHECK(!mu_squad_encode(&setup, bytes.data(), bytes.size()));
  }
  // Hand-built payloads with other counts, at version 3, are refused even when the size adds up.
  {
    MuSquadSetup decoded{}, untouched{};
    const std::vector<std::vector<unsigned char>> refused{
        {3,1, 1,2,0, 1,3,0},                                   // version 1's 1v1 squad shape
        {3,2, 2,2,0,3,0, 2,4,0,5,0},                           // two per side
        {3,3, 3,2,0,3,0,4,0, 3,6,0,7,0,8,0},                   // three per side (version 2's shape)
        {3,5, 5,2,0,3,0,4,0,5,0,6,0, 3,6,0,7,0,8,0},           // 5 + 3 = 20 bytes, the v3 size
        {3,4, 3,2,0,3,0,4,0, 5,4,0,5,0,6,0,7,0,8,0},           // 3 + 5 = 20 bytes
        {3,5, 5,2,0,3,0,4,0,5,0,6,0, 5,6,0,7,0,8,0,9,0,10,0},  // five per side, past the max
        {3,4, 4,2,0,3,0,4,0,5,0, 3,6,0,7,0,8,0},               // four versus three
        {3,4, 0, 4,6,0,7,0,8,0,9,0},                           // an empty side
        {2,3, 3,2,0,3,0,4,0, 3,6,0,7,0,8,0},                   // a version 2 payload of three
        {2,4, 4,2,0,3,0,4,0,5,0, 4,6,0,7,0,8,0,9,0},           // four per side at version 2
        {1,4, 4,2,0,3,0,4,0,5,0, 4,6,0,7,0,8,0,9,0},           // a version 1 payload of four
    };
    for (const auto& payload : refused) {
      CHECK(!mu_squad_decode(payload.data(), payload.size(), &decoded));
      CHECK(!std::memcmp(&decoded, &untouched, sizeof decoded));
    }
    const std::vector<unsigned char> accepted{3,4, 4,2,0,3,0,4,0,5,0, 4,6,0,7,0,8,0,9,0};
    CHECK(accepted.size() == MU_SQUAD_WIRE_MAX && mu_squad_decode(accepted.data(), accepted.size(), &decoded));
    CHECK(decoded.stocks == 4 && decoded.sides[0].count == 4 && decoded.sides[1].members[3].kind == 9);
    // Pick order is fight order on the wire: member i is the i-th fighter picked, for both sides.
    for (unsigned p = 0; p < 2; ++p) for (unsigned i = 0; i < 4; ++i)
      CHECK(decoded.sides[p].members[i].kind == accepted[3 + 9*p + 2*i] && decoded.sides[p].members[i].color == 0);
  }
  for (unsigned a = MU_SQUAD_LINEUP; a <= MU_SQUAD_LINEUP; ++a)
    for (unsigned b = MU_SQUAD_LINEUP; b <= MU_SQUAD_LINEUP; ++b) {
    auto p1 = side(a, 14), p2 = side(b, 18);
    MuSquadSetup setup{}, reversed{}, decoded{};
    CHECK(mu_squad_combine(0, &p1, 1, &p2, &setup));
    CHECK(mu_squad_combine(1, &p2, 0, &p1, &reversed));
    CHECK(!std::memcmp(&setup, &reversed, sizeof setup));
    CHECK(setup.stocks == (a > b ? a : b));
    std::array<unsigned char, MU_SQUAD_WIRE_MAX + 1> bytes{};
    auto n = mu_squad_encode(&setup, bytes.data(), MU_SQUAD_WIRE_MAX);
    CHECK(n == 4 + 2 * (a + b) && n == MU_SQUAD_WIRE_MAX && setup.stocks == 4);   // one stock per fighter
    CHECK(bytes[0] == MU_SQUAD_VERSION && bytes[1] == setup.stocks && bytes[2] == a);
    CHECK(mu_squad_decode(bytes.data(), n, &decoded));
    CHECK(!std::memcmp(&setup, &decoded, sizeof setup));
    for (size_t cap = 0; cap < n; ++cap) CHECK(!mu_squad_encode(&setup, bytes.data(), cap));
    for (size_t length = 0; length < n; ++length) {
      auto untouched = decoded;
      CHECK(!mu_squad_decode(bytes.data(), length, &decoded));
      CHECK(!std::memcmp(&untouched, &decoded, sizeof decoded));
    }
    CHECK(!mu_squad_decode(bytes.data(), n + 1, &decoded));
    for (unsigned value = 0; value < 256; ++value) {
      auto malformed = bytes;
      malformed[0] = (unsigned char)value;
      CHECK(mu_squad_decode(malformed.data(), n, &decoded) == (value == MU_SQUAD_VERSION));
      malformed = bytes; malformed[1] = (unsigned char)value;
      CHECK(mu_squad_decode(malformed.data(), n, &decoded) == (value == setup.stocks));
      for (unsigned offset : {2u, 3u + 2u*a}) {
        malformed = bytes; malformed[offset] = (unsigned char)value;
        CHECK(mu_squad_decode(malformed.data(), n, &decoded) == (value == MU_SQUAD_LINEUP));
      }
      for (unsigned offset : {3u, 4u + 2u*a}) {
        malformed = bytes; malformed[offset] = (unsigned char)value;
        CHECK(mu_squad_decode(malformed.data(), n, &decoded) ==
              (value < 26 && malformed[offset + 1] < mu_squad_colors(value)));
        malformed = bytes; malformed[offset + 1] = (unsigned char)value;
        CHECK(mu_squad_decode(malformed.data(), n, &decoded) == (value < mu_squad_colors(bytes[offset])));
      }
    }
    CHECK(!mu_squad_decode(nullptr, n, &decoded));
    CHECK(!mu_squad_decode(bytes.data(), n, nullptr));
    CHECK(!mu_squad_encode(&setup, nullptr, n));
    CHECK(!mu_squad_combine(2, &p1, 1, &p2, &decoded));
    CHECK(!mu_squad_combine(0, &p1, 0, &p2, &decoded));
    CHECK(!mu_squad_combine(0, nullptr, 1, &p2, &decoded));
  }
  // Every fighter's actual costume range, including Zelda/Sheik and G&W tints.
  for (unsigned kind = 0; kind < 256; ++kind) for (unsigned color = 0; color < 256; ++color) {
    MuSquadSide candidate{}; candidate.count = MU_SQUAD_LINEUP;
    candidate.members[0] = {(unsigned char)kind, (unsigned char)color};
    candidate.members[1] = {0, 0}; candidate.members[2] = {1, 0}; candidate.members[3] = {2, 0};
    CHECK(mu_squad_side_valid(&candidate) == (kind < 26 && color < mu_squad_colors(kind)));
    // The colour clamp (character select's) always makes a valid member of a real fighter.
    if (kind < 26) {
      MuSquadSide clamped = candidate; clamped.count = MU_SQUAD_LINEUP;
      clamped.members[0].color = mu_squad_color(kind, color);
      CHECK(mu_squad_side_valid(&clamped));
      CHECK(mu_squad_color(kind, color) == (color < mu_squad_colors(kind) ? color : mu_squad_colors(kind) - 1));
    } else CHECK(mu_squad_color(kind, color) == 0);
    for (unsigned count : {0u, 1u, 2u, 3u, 5u, 255u}) {
      candidate.count = (unsigned char)count;
      CHECK(!mu_squad_side_valid(&candidate));
    }
  }
  // Bounded fuzz, also executed under ASan/UBSan by the Python harness.
  unsigned rng = 0x12345678;
  for (unsigned run = 0; run < 5000; ++run) {
    std::array<unsigned char, 128> bytes{};
    for (auto& value : bytes) { rng = rng * 1664525u + 1013904223u; value = (unsigned char)(rng >> 24); }
    MuSquadSetup decoded{};
    if (mu_squad_decode(bytes.data(), run % bytes.size(), &decoded)) CHECK(mu_squad_setup_valid(&decoded));
  }
}

static void decisions() {
  for (unsigned a : {0u, 0x10u}) for (unsigned b : {0u, 0x10u})
    for (int own : {0, 1}) for (int peer : {0, 1}) {
      const auto value = mu_squad_decide(a, b, own, peer);
      CHECK(value == mu_squad_decide(b, a, peer, own));
      // A normal 1v1 only when neither side asked for squads; a lineup without a shared cap
      // or without the other side's lineup is refused, never quietly played as a 1v1.
      CHECK(value == (!own && !peer ? MU_SQUAD_NORMAL : !(a && b) ? MU_SQUAD_CANCEL :
                      own && peer ? MU_SQUAD_PLAY : MU_SQUAD_CANCEL));
      if (own || peer) CHECK(value != MU_SQUAD_NORMAL);
    }
  // A squad search against a peer without the cap: CANCEL, lineup or not, never NORMAL.
  CHECK(mu_squad_decide(MU_SQUAD_CAP, 0, 1, 0) == MU_SQUAD_CANCEL);
  CHECK(mu_squad_decide(MU_SQUAD_CAP, 0, 1, 1) == MU_SQUAD_CANCEL);
  CHECK(mu_squad_decide(0, 0, 1, 0) == MU_SQUAD_CANCEL);
  // A stray lineup sent to a normal match refuses it too.
  CHECK(mu_squad_decide(0, MU_SQUAD_CAP, 0, 1) == MU_SQUAD_CANCEL);
  CHECK(mu_squad_decide(MU_SQUAD_CAP, MU_SQUAD_CAP, 0, 1) == MU_SQUAD_CANCEL);
  CHECK(mu_squad_decide(MU_SQUAD_CAP, MU_SQUAD_CAP, 1, 0) == MU_SQUAD_CANCEL);
  CHECK(mu_squad_decide(MU_SQUAD_CAP, MU_SQUAD_CAP, 1, 1) == MU_SQUAD_PLAY);
  CHECK(mu_squad_decide(MU_SQUAD_CAP, MU_SQUAD_CAP, 0, 0) == MU_SQUAD_NORMAL);
  CHECK(mu_squad_decide(0, 0, 0, 0) == MU_SQUAD_NORMAL);
  // Other caps bits never stand in for the squad bit.
  CHECK(mu_squad_decide(0xEF, 0xEF, 1, 1) == MU_SQUAD_CANCEL);
}

static void policies() {
  using namespace yougame;
  for (int mode = 0; mode <= 4; ++mode) for (bool row : {false, true}) {
    const bool active = mode == 1 && row;
    CHECK(squad::selected(mode, row) == active);
    // A friend room plays squads from the row; its tickets and keys stay the plain Friends ones.
    CHECK(squad::selected(mode, row, true) == (mode == 2 && row));
    CHECK(squad::blend_allowed(mode, row) == !active);
    for (bool login : {false,true}) for (bool agent : {false,true})
      for (bool friends : {false,true}) for (bool vanilla : {false,true}) {
        // A chosen Squad Strike search never reaches Slippi; every other search takes the ordinary
        // route, Slippi included, exactly as a build without Squad Strike.
        for (bool yougame_mode : {false, true}) {
          const auto route = squad::matchmaking_route(mode, yougame_mode, login, agent, friends, vanilla, row);
          const auto base = mode_policy::matchmaking_route(mode, yougame_mode, login, agent, friends, vanilla);
          CHECK(route == (active && !friends ? (yougame_mode ? mode_policy::MatchmakingRoute::YouGame :
              mode_policy::MatchmakingRoute::Locked) : base));
        }
        const auto route = squad::matchmaking_route(mode, true, login, agent, friends, vanilla, row);
        if (active && !friends) {
          CHECK(route != mode_policy::MatchmakingRoute::Blend && route != mode_policy::MatchmakingRoute::Slippi);
          CHECK(squad::matchmaking_route(mode, false, login, agent, friends, vanilla, row) ==
                mode_policy::MatchmakingRoute::Locked);
        }
      }
    CHECK(std::string(squad::ticket_mode(mode, row, "singles")) == "singles");
    for (unsigned original = 0; original < 16; ++original) {
      CHECK(squad::caps((unsigned char)original, mode, row) == (original | (active ? 0x10 : 0)));
      if (mode == 0) CHECK(!(squad::caps((unsigned char)original, mode, row) & MU_SQUAD_CAP));
    }
    for (const std::string branch : {"", "feature"}) for (const char* timer : std::array<const char*, 3>{nullptr, "30", "-1"}) {
      const auto original_tag = compat_tag_with_test_timer(branch, timer);
      const auto tag = squad::pool_tag(mode, row, original_tag, "");
      const auto key = compatibility_key("0.9.96", false, "", false, {}, tag);
      const auto old = compatibility_key("0.9.96", false, "", false, {}, original_tag);
      CHECK(key == (active ? old + "-squad" : old));
      if (mode == 0) CHECK(key.find("squad") == std::string::npos);
      const auto budget = squad::pool_tag(mode, row, original_tag, std::string(64, 'a'));
      CHECK(budget == (active ? (original_tag.empty() ? "" : original_tag + "-") +
                         std::string("baaaaaaaaaaaaaaaa-squad") : original_tag));
    }
  }
  // Long branch tags used to truncate mu-0.9.96 to mu- or mu-0.9.
  // Test the requested 11..17 window and both exact-fit sides of every boundary.
  for (unsigned n = 0; n <= 40; ++n) for (bool mod : {false, true})
    for (bool engine : {false, true}) for (bool rules : {false, true}) {
      const std::string branch(n, 'x'), fp(64, 'a'), rd = rules ? fp : "";
      const auto marked = squad::build_tag(branch);
      const auto tag = squad::pool_tag(1, true, marked, fp);
      const auto ordinary = compatibility_key("0.9.96", mod, fp, engine, rd, marked);
      const size_t base = std::string("mu-0.9.96").size() + (mod ? 21 : 0) +
          (engine ? 3 : 0) + (rules ? 10 : 0) + 1;
      const size_t needed = base + tag.size();
      const auto a = squad::compatibility("0.9.96", mod, fp, engine, rd, branch, 1, true, fp);
      const auto b = squad::compatibility("0.9.97", mod, fp, engine, rd, branch, 1, true, fp);
      CHECK(squad::key_fits("0.9.96", mod, engine, rd, tag) == (needed <= 64));
      CHECK(a.empty() == (needed > 64));
      if (!a.empty()) {
        CHECK(a != b && a.rfind("mu-0.9.96", 0) == 0);
        CHECK(a == ordinary + "-baaaaaaaaaaaaaaaa-squad");
      }
      // Squad builds never share a key with a non-squad build, in any mode or branch.
      const auto normal = base + marked.size() <= 64 ? ordinary : std::string();
      CHECK(squad::compatibility("0.9.96", mod, fp, engine, rd, branch, 1, false, "") == normal);
      CHECK(squad::compatibility("0.9.96", mod, fp, engine, rd, branch, 0, true, fp) == normal);
      if (!normal.empty())
        CHECK(normal != compatibility_key("0.9.96", mod, fp, engine, rd, branch) &&
              normal.size() >= 3 && normal.compare(normal.size() - 3, 3, "-sq") == 0);
    }
  // Positional MU_BUILD tail: digest and shield bytes never move when no row is chosen.
  for (unsigned early = 0; early <= 1; ++early) for (unsigned engine = 0; engine <= 1; ++engine)
    for (unsigned digest = 0; digest <= 1; ++digest) for (unsigned shield = 0; shield <= 1; ++shield) {
      unsigned char original = (unsigned char)(early | 2 | (engine ? 4 : 0) | (digest ? 8 : 0));
      std::vector<unsigned char> legacy{0xE0,'M','U','B',1,0,0,0,0,original};
      if (digest) legacy.insert(legacy.end(), 64, 'a');
      legacy.push_back((unsigned char)shield);
      if (shield) legacy.insert(legacy.end(), {1,2,3});
      auto current = legacy;
      current[9] = squad::caps(original, 1, false);
      CHECK(current == legacy);
      current[9] = squad::caps(original, 0, true);
      CHECK(current == legacy);
      current[9] = squad::caps(original, 1, true);
      CHECK((current[9] ^ legacy[9]) == 0x10);
      current[9] = legacy[9]; CHECK(current == legacy);
    }
  using namespace slippi::online;
  const unsigned char clear_lineup[] = {1, 0};
  CHECK(command_payload_valid(MU_SQUAD_COMMAND, clear_lineup, sizeof clear_lineup));
  for (unsigned op = 0; op < 256; ++op) for (unsigned length = 0; length < 160; ++length) {
    unsigned char payload[160]{}; payload[0] = (unsigned char)op; payload[1] = 1;
    const bool expected = (op == 0 && length == 2) || (op == 1 && length == 4) ||
        (op == 2 && length == 9) || (op == 3 && length >= 10 && length <= 137) ||
        ((op == 4 || op == 5 || op == MU_SQUAD_OP_LOBBY_MODE) && length == 1) || (op == MU_SQUAD_OP_FRIEND && length == 2) ||
        (op == MU_SQUAD_OP_CONSOLE && length == 2); // payload[1] == 1: ARM
    CHECK(command_payload_valid(MU_SQUAD_COMMAND, payload, length) == expected);
  }
  // Friends' costume rule: {op, OFF or ARM}, or {op, FACT, op 3's fact from its second byte}.
  for (unsigned sub = 0; sub < 256; ++sub) for (unsigned length = 0; length < 160; ++length) {
    unsigned char payload[160]{}; payload[0] = MU_SQUAD_OP_CONSOLE; payload[1] = (unsigned char)sub;
    const bool expected = (length == 2 && sub <= MU_SQUAD_CONSOLE_ARM) ||
        (sub == MU_SQUAD_CONSOLE_FACT && length >= 11 && length <= 138);
    CHECK(command_payload_valid(MU_SQUAD_COMMAND, payload, length) == expected);
  }
  // Friend rooms: {op, action}, action 0 (query), 1 (invite), 2 (refuse) or 3 (intent) only.
  for (unsigned action = 0; action < 256; ++action) {
    const unsigned char friend_op[] = {MU_SQUAD_OP_FRIEND, (unsigned char)action};
    CHECK(command_payload_valid(MU_SQUAD_COMMAND, friend_op, sizeof friend_op) == (action <= MU_SQUAD_FRIEND_INTENT));
  }
  CHECK(MU_SQUAD_FRIEND_CAP == 0x80 && !(MU_SQUAD_FRIEND_CAP & (MU_SQUAD_CAP | 0x40 | 0x20 | 0x0F)));
  // Own lineup: any exact-length count up to MU_SQUAD_LINEUP reaches the handler (which wants
  // exactly four, or zero to clear); a fifth fighter never reaches the four-slot array.
  for (unsigned count = 0; count < 79; ++count) {
    unsigned char payload[160]{}; payload[0] = 1; payload[1] = (unsigned char)count;
    CHECK(command_payload_valid(MU_SQUAD_COMMAND, payload, 2 + 2 * count) == (count <= MU_SQUAD_LINEUP));
    CHECK(!command_payload_valid(MU_SQUAD_COMMAND, payload, 3 + 2 * count));
  }
}
int main() {
  harness_parser(); codec(); decisions(); policies();
  std::printf("PASS: squad codec, malformed fields, canonical setup, decision table, ranked/blend isolation, MU_BUILD tails (%u checks)\n", checks);
}
#else
int main() { std::puts("PASS: squad host helpers absent with switch OFF"); }
#endif
