// Compile the production presentation model without a disc or game renderer.
// SPDX-License-Identifier: GPL-3.0-or-later
#define MU_RANKED_REVEAL_TEST
#include "../../sourceport/game/shim/mu_ranked_reveal.c"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
static bool near(float a, float b) { return std::fabs(a-b) < .001f; }
static MuYgRank rank(int tier, int order, const char* label, bool placed = true) {
    MuYgRank r{}; r.known = 1; r.placed = placed; r.tier = tier; r.order = order;
    r.division = tier == 6 ? 0 : 2; r.matches = placed ? 5 : 2; r.placement_total = 5;
    std::strncpy(r.label, label, 23); return r;
}
static MuYgResult result(MuYgRank a, MuYgRank b, int before = 11400, int after = 11560) {
    MuYgResult r{}; r.status = MU_YG_RESULT_RECORDED;
    r.before = before; r.after = after; r.rank_before = a; r.rank_after = b; return r;
}
static MuRevealPose at(MuReveal& s, unsigned frame) { s.frame = frame; return reveal_pose(&s); }
int main() {
    MuReveal s{}; MuYgResult empty{};
    reveal_accept(&s, &empty); CHECK(!s.active); CHECK(!reveal_pose(&s).old_alpha);
    auto gold = rank(3, 11, "Gold II"); auto platinum = rank(4, 13, "Platinum IV");
    auto r = result(gold, platinum);
    // Room recording -> recorded. Never expose garbage numbers while pending.
    auto recording = r; recording.status = MU_YG_RESULT_RECORDING;
    reveal_accept(&s, &recording);
    CHECK(s.active && s.frame == 0); CHECK(std::string(reveal_note(&s.result)) == "Result being recorded");
    CHECK(!s.result.before && !s.result.after && !s.result.rank_before.known);
    CHECK(reveal_pose(&s).new_alpha == 0);
    for (int i = 0; i < 500; ++i) { reveal_tick(&s); reveal_accept(&s, &recording); }
    CHECK(s.frame == REVEAL_HOLD_END && !reveal_done(&s));
    reveal_accept(&s, &r); CHECK(s.frame == 0);
    auto p = at(s, 0);
    CHECK(p.direction == 1 && p.tier_up && p.changed);
    CHECK(p.old_alpha == 1 && p.new_alpha == 0 && p.rating == r.before);
    CHECK(std::string(reveal_note(&r)) == "Promoted to");
    // Timing table: old .8-.1.2s, incoming 1.1s, pop .8s, glow 1s.
    CHECK(REVEAL_OLD_START == 48 && REVEAL_OLD_END == 72);
    CHECK(REVEAL_NEW_START == 66 && REVEAL_NEW_END == 102);
    CHECK(REVEAL_POP_END == 114 && REVEAL_GLOW_END == 126);
    CHECK(at(s, 48).old_alpha == 1); CHECK(near(at(s, 60).old_alpha, .5f));
    CHECK(near(at(s, 60).old_y, -9)); CHECK(at(s, 72).old_alpha == 0);
    CHECK(near(at(s, 66).scale, .65f) && at(s, 66).new_alpha == 0);
    CHECK(at(s, 97).scale > 1.11f); CHECK(near(at(s, 114).scale, 1));
    CHECK(at(s, 66).glow == 0 && near(at(s, 96).glow, 1) && at(s, 126).glow == 0);
    // Monotonic tick-up, exact endpoint; repeated host polls do not restart.
    int prev = r.before;
    for (unsigned f = 0; f <= 140; ++f) {
        p = at(s, f); CHECK(!(p.old_alpha > 0 && p.new_alpha > 0)); CHECK(p.rating >= prev && p.rating <= r.after); prev = p.rating;
        reveal_accept(&s, &r); CHECK(s.frame == f);
    }
    CHECK(at(s, 15).rating == r.before && at(s, 99).rating == r.after);
    // Division promotion slides up from 22px (no tier pop).
    r = result(rank(3, 10, "Gold III"), gold);
    reveal_accept(&s, &r);
    CHECK(std::string(reveal_note(&r)) == "Division promotion");
    CHECK(!at(s, 66).tier_up && near(at(s, 66).new_y, 22));
    CHECK(near(at(s, 84).new_y, 11) && near(at(s, 84).new_alpha, .5f));
    CHECK(at(s, 102).new_y == 0 && at(s, 102).new_alpha == 1);
    // Demotion is ordered by rank, even with a positive rating delta.
    r = result(platinum, gold, 13000, 13050); reveal_accept(&s, &r);
    CHECK(at(s, 66).direction == -1 && !at(s, 66).tier_up);
    CHECK(at(s, 66).new_y == -22); CHECK(std::string(reveal_note(&r)) == "Demoted to");
    // Challenger can be gained on a loss, or lost on a win.
    auto challenger = rank(6, 21, "Challenger");
    r = result(rank(5, 20, "Diamond I"), challenger, 17000, 16900); reveal_accept(&s, &r);
    CHECK(reveal_pose(&s).tier_up && reveal_pose(&s).direction == 1);
    CHECK(std::string(reveal_note(&r)) == "Promoted to");
    prev = r.before;
    for (unsigned f = 0; f <= 99; ++f) { p = at(s, f); CHECK(p.rating <= prev && p.rating >= r.after); prev = p.rating; }
    r = result(challenger, rank(5, 20, "Diamond I"), 16900, 17000); reveal_accept(&s, &r);
    CHECK(at(s, 66).new_y == -22 && !reveal_pose(&s).tier_up);
    // Placement progress and initial placement use the supplied rank snapshots.
    auto unplaced = rank(0, 0, "Placements 2/5", false);
    auto progress = rank(0, 0, "Placements 3/5", false);
    r = result(unplaced, progress); reveal_accept(&s, &r);
    CHECK(std::string(reveal_note(&r)) == "Placement progress");
    CHECK(at(s, 0).old_alpha == 1 && at(s, 102).new_alpha == 1);
    r = result(unplaced, gold); reveal_accept(&s, &r);
    CHECK(std::string(reveal_note(&r)) == "Rank revealed");
    CHECK(at(s, 0).old_alpha == 1 && at(s, 114).new_alpha == 1);
    // Same rank, zero rating change is a recorded result, never "pending".
    r = result(gold, gold, 11400, 11400); reveal_accept(&s, &r);
    CHECK(std::string(reveal_note(&r)) == "Ranked result");
    CHECK(!reveal_pose(&s).changed && reveal_pose(&s).new_alpha == 1);
    CHECK(reveal_pose(&s).rating == 11400 && s.result.status == MU_YG_RESULT_RECORDED);
    // Unrated / void / error never show cached, inferred or supplied junk numbers.
    for (int status : {MU_YG_RESULT_UNRATED, MU_YG_RESULT_ERROR}) {
        for (int reason = MU_YG_REASON_NONE; reason <= MU_YG_REASON_MALFORMED; ++reason) {
            r.status = status; r.reason = reason; reveal_accept(&s, &r);
            CHECK(s.active && !s.result.rank_after.known && !s.result.before && !s.result.after);
            CHECK(reveal_pose(&s).old_alpha == 0 && reveal_pose(&s).new_alpha == 0);
            CHECK(std::strlen(reveal_reason(reason)) > 5);
        }
    }
    CHECK(std::string(reveal_reason(MU_YG_REASON_VOID)) == "Set was voided");
    r = result(gold, platinum); r.rank_before.known = 0; reveal_accept(&s, &r);
    CHECK(s.result.status == MU_YG_RESULT_ERROR && s.result.reason == MU_YG_REASON_MALFORMED);
    r.status = 255; reveal_accept(&s, &r); CHECK(s.result.status == MU_YG_RESULT_ERROR);
    // Reduced motion shows the final state immediately, including no glow/pop.
    r = result(gold, platinum); reveal_accept(&s, &r); s.reduced = 1;
    for (unsigned f : {0u, 48u, 66u, 97u, 114u, 126u}) {
        p = at(s, f); CHECK(p.old_alpha == 0 && p.new_alpha == 1 && near(p.scale, 1));
        CHECK(p.old_y == -18 && p.new_y == 0 && p.glow == 0 && p.rating == r.after);
    }
    // Signed tenths at both wire extremes do not overflow interpolation.
    r = result(gold, platinum, INT32_MIN, INT32_MAX); reveal_accept(&s, &r); s.reduced = 0;
    CHECK(at(s, 0).rating == INT32_MIN && at(s, 99).rating == INT32_MAX);
    CHECK(at(s, 50).rating > INT32_MIN && at(s, 50).rating < INT32_MAX);
    // A final result holds 3s after the glow, then gives the CSS back.
    CHECK(REVEAL_HOLD_END == REVEAL_GLOW_END + 180);
    s.frame = REVEAL_HOLD_END - 1; CHECK(!reveal_done(&s));
    for (int i = 0; i < 10000; ++i) reveal_tick(&s);
    CHECK(s.frame == REVEAL_HOLD_END && reveal_done(&s));
    reveal_accept(&s, &r); CHECK(s.frame == REVEAL_HOLD_END && reveal_done(&s));
    reveal_accept(&s, &empty); CHECK(!s.active && s.frame == 0);
    // One plain log line per new rated result: ranks, ratings, change; no names or ids.
    char line[160];
    CHECK(!reveal_shown_line(&s, line));
    auto bronze = rank(1, 2, "Bronze I");
    r = result(bronze, bronze, 8720, 8680); reveal_accept(&s, &r);
    CHECK(reveal_shown_line(&s, line));
    CHECK(std::string(line) == "yougame: ranked reveal shown: Bronze I 872 -> Bronze I 868 (-4)\n");
    CHECK(!reveal_shown_line(&s, line));
    reveal_tick(&s); reveal_accept(&s, &r); CHECK(!reveal_shown_line(&s, line));
    r = result(gold, platinum, 11285, 11320); reveal_accept(&s, &r);
    CHECK(reveal_shown_line(&s, line));
    CHECK(std::string(line) == "yougame: ranked reveal shown: Gold II 1128.5 -> Platinum IV 1132 (+3.5)\n");
    r = result(gold, gold, 11400, 11400); reveal_accept(&s, &r);
    CHECK(reveal_shown_line(&s, line) && std::string(line) == "yougame: ranked reveal shown: Gold II 1140 -> Gold II 1140 (0)\n");
    r = result(gold, platinum, INT32_MIN, INT32_MAX); reveal_accept(&s, &r);
    CHECK(reveal_shown_line(&s, line));
    CHECK(std::string(line) == "yougame: ranked reveal shown: Gold II -214748364.8 -> Platinum IV 214748364.7 (+429496729.5)\n");
    auto full = rank(2, 5, "Silver"); std::memset(full.label, 'S', sizeof full.label);
    r = result(full, full, 9000, 8995); reveal_accept(&s, &r);
    CHECK(reveal_shown_line(&s, line) && std::string(line).find(std::string(24, 'S') + " 900 -> ") != std::string::npos);
    CHECK(std::string(line).find(std::string(25, 'S')) == std::string::npos);
    r = result(gold, gold, 11400, 11395); reveal_accept(&s, &r);
    CHECK(reveal_shown_line(&s, line) && std::string(line) == "yougame: ranked reveal shown: Gold II 1140 -> Gold II 1139.5 (-0.5)\n");
    // Pending, unrated and error results carry no numbers and log nothing.
    for (int status : {MU_YG_RESULT_RECORDING, MU_YG_RESULT_UNRATED, MU_YG_RESULT_ERROR}) {
        r.status = status; reveal_accept(&s, &r); CHECK(!reveal_shown_line(&s, line));
    }
    // A result that comes back after a pending poll is new, so it logs again.
    r = result(bronze, bronze, 8720, 8680); reveal_accept(&s, &r); CHECK(reveal_shown_line(&s, line));
    reveal_accept(&s, &empty); CHECK(!reveal_shown_line(&s, line));
    std::cout << "rank reveal: " << checks << " checks passed\n";
}
