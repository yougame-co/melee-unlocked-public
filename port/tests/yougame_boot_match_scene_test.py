"""Execute the agent-test bootMatch scene (shim/mu_boot_match.c) with a local, mocked engine."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADERS = (
    "melee/gm/gm_1601.h", "melee/gm/gm_1A3F.h", "melee/gm/gm_1B03.h", "melee/gm/gmscdata.h",
    "melee/gm/gmscene.h", "melee/gm/gmvs.h", "melee/gm/gmvsmelee.h", "melee/gm/types.h",
    "melee/lb/lbdvd.h",
)
MOCK = r"""
#pragma once
#include <stddef.h>
typedef struct { int unused; } StartMeleeData;
typedef struct GameModeState GameModeState;
struct GameModeState {
    int id, preload, flags;
    void (*on_enter)(GameModeState*);
    void (*on_exit)(GameModeState*);
    struct { int scene_kind; void* enter_data; void* exit_data; } info;
};
typedef struct { int kind; GameModeState* states; } GameMode;
typedef struct { StartMeleeData start; } VsModeData;
struct gm_80479D58_t { int unk_C; };
enum { GM_MENU = 1, GM_VS = 2, GM_COUNT = 255, GM_GAMEMODESTATE_TERMINATE = 255,
       GS_VS, GS_RESULTS, lbDvdPreload_3 };
extern struct ExitInfo { struct { int outcome; } match_end; } gmVsMelee_VsExitInfo;
extern StartMeleeData gmVsMelee_StartData;
extern int gmVsMelee_ResultsEnterData;
void* gm_GetGameModeStateEnterData(GameModeState*);
void gm_SetNextGameModeStateId(int);
void gm_ChangeGameModeAfterCurrentScene(int);
int gm_GetCurrentGameMode(void);
VsModeData* gmVsMelee_GetVsData(void);
void gmVsMelee_EnterVs(GameModeState*, VsModeData*, void*, void*);
void gmVsMelee_ExitVs(GameModeState*, int, int);
void gmVsMelee_EnterResults(GameModeState*);
GameMode* gm_GetAllGameModes(void);
"""
HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include "mu_boot_match.c"
static GameModeState vanilla[] = {{255}};
static GameMode modes[] = {{GM_MENU, NULL}, {GM_VS, vanilla}, {GM_COUNT, NULL}};
static VsModeData vs;
static struct gm_80479D58_t engine;
struct ExitInfo gmVsMelee_VsExitInfo;
StartMeleeData gmVsMelee_StartData;
int gmVsMelee_ResultsEnterData;
static int current = GM_MENU, next_mode = -1, next_state = -1, applied, entered, overridden = 1, checks;
static const unsigned char* last_info;
#define CHECK(x) do { ++checks; assert(x); } while (0)
int mu_match_request(int* stage, int* kind, int* cpu, int* level, int* costume, int count) {
    // "31:2:9/c9": Battlefield, Fox (human, port 1), Marth (CPU level 9, port 2), the rest empty.
    int i;
    if (!overridden) return 0;
    CHECK(count == 6);
    *stage = 31;
    for (i = 0; i < count; ++i) { kind[i] = -1; cpu[i] = 0; level[i] = 0; costume[i] = 0; }
    kind[0] = 2; kind[1] = 9; cpu[1] = 1; level[1] = 9; costume[1] = 3;
    return 1;
}
void mu_replay_apply_game_info(StartMeleeData* d, const unsigned char* p) { last_info = p; d->unused = 99; ++applied; }
struct gm_80479D58_t* mu_gm_engine_state(void) { return &engine; }
void* gm_GetGameModeStateEnterData(GameModeState* s) { return s->info.enter_data; }
void gm_SetNextGameModeStateId(int id) { next_state = id; }
void gm_ChangeGameModeAfterCurrentScene(int mode) { next_mode = mode; }
int gm_GetCurrentGameMode(void) { return current; }
VsModeData* gmVsMelee_GetVsData(void) { return &vs; }
void gmVsMelee_EnterVs(GameModeState* s, VsModeData* v, void* a, void* b) {
    (void)s; (void)a; (void)b; CHECK(v == &vs && v->start.unused == 99 && applied == 1); ++entered;
}
void gmVsMelee_ExitVs(GameModeState* s, int a, int b) { (void)s; (void)a; (void)b; }
void gmVsMelee_EnterResults(GameModeState* s) { (void)s; }
GameMode* gm_GetAllGameModes(void) { return modes; }
static void boot(void) {
    next_mode = next_state = -1; engine.unk_C = 0; applied = entered = 0;
    CHECK(mu_boot_match_boot() == 31);
    CHECK(modes[1].states == states && next_mode == GM_VS && engine.unk_C == 1);
}
static void back_to_menu(void) {
    mu_boot_match_enter_mode(GM_VS);           // the match itself: the table stays
    CHECK(modes[1].states == states);
    mu_boot_match_enter_mode(GM_MENU);         // after the VS mode's unload
    CHECK(modes[1].states == vanilla && vs.start.unused == 17);
}
int main(void) {
    vs.start.unused = 17;
    // No --match, or anywhere but the main menu: refused, nothing touched.
    overridden = 0; CHECK(mu_boot_match_boot() == -1); overridden = 1;
    current = GM_VS; CHECK(mu_boot_match_boot() == -1); current = GM_MENU;
    CHECK(modes[1].states == vanilla && next_mode == -1 && engine.unk_C == 0);
    mu_boot_match_enter_mode(GM_MENU); CHECK(modes[1].states == vanilla);
    // A decided match: VS, results, main menu, original table back.
    boot();
    CHECK(mu_boot_match_boot() == -1);         // one at a time
    const unsigned char* p = info + 96;
    CHECK(info[14] == 0 && info[15] == 31);
    CHECK(p[0] == 2 && p[1] == 0 && p[4] == 0 && p[15] == 1);
    CHECK(p[36] == 9 && p[37] == 1 && p[39] == 3 && p[40] == 1 && p[51] == 9);
    for (int i = 2; i < 6; ++i) CHECK(p[i * 36] == 33 && p[i * 36 + 1] == 3);
    vs_prep(&states[0]);
    CHECK(entered == 1 && applied == 2 && last_info == info && gmVsMelee_StartData.unused == 99);
    gmVsMelee_VsExitInfo.match_end.outcome = 2; vs_decide(&states[0]);
    CHECK(next_state == 1 && next_mode == GM_VS);
    results_decide(&states[1]); CHECK(next_mode == GM_MENU);
    back_to_menu();
    // A no contest skips the results.
    boot(); vs_prep(&states[0]);
    gmVsMelee_VsExitInfo.match_end.outcome = 7; next_state = -1; vs_decide(&states[0]);
    CHECK(next_state == -1 && next_mode == GM_MENU);
    back_to_menu();
    printf("%d boot match scene checks passed\n", checks);
    return 0;
}
"""


class BootMatchSceneTest(unittest.TestCase):
    def test_production_callbacks_with_mocked_engine(self):
        cc = shutil.which("cc")
        self.assertIsNotNone(cc, "local C compiler is required")
        with tempfile.TemporaryDirectory(prefix="boot-match-scene-", dir=ROOT) as tmp:
            tmp = Path(tmp)
            (tmp / "mock.h").write_text(MOCK)
            for header in HEADERS:
                path = tmp / header
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "mock.h"\n')
            (tmp / "test.c").write_text(HARNESS)
            compiled = subprocess.run([
                cc, "-std=c11", "-Werror=implicit-function-declaration", "-I", str(tmp),
                "-I", str(ROOT / "sourceport/game/shim"),
                "-I", str(ROOT / "sourceport/game/include"),
                "-I", str(ROOT / "port/runtime/abi"), str(tmp / "test.c"), "-o", str(tmp / "test"),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(tmp / "test")], capture_output=True, text=True, timeout=5)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            print(ran.stdout.strip())

    def test_both_bridges_and_host_wiring(self):
        practice = (ROOT / "sourceport/game/shim/mu_practice.c").read_text(encoding="utf-8-sig")
        self.assertIn("case MU_PR_BOOT_MATCH:\n        return mu_boot_match_boot();", practice)
        self.assertIn("mu_boot_match_enter_mode(mode);", practice)
        noslippi = (ROOT / "sourceport/game/shim/mu_noslippi.c").read_text(encoding="utf-8-sig")
        self.assertIn("if (op == 35) {", noslippi)
        self.assertIn("void mu_practice_enter_mode(int mode) { mu_boot_match_enter_mode(mode); }", noslippi)
        self.assertIn("MU_PRACTICE_BOOT_MATCH == 35", (ROOT / "sourceport/game/shim/mu_entry.c").read_text())
        host = (ROOT / "port/app/source_host.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("g_game.practice(MU_PRACTICE_BOOT_MATCH, nullptr, 0)", host)
        self.assertIn('"agent-test: bootMatch requesting VS (stage %d)"', host)
        self.assertIn('"agent-test: bootMatch match started (frame %u)"', host)
        self.assertIn("    boot_match_tick();\n", host)


if __name__ == "__main__":
    unittest.main()
