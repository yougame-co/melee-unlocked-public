"""Execute the production tournament scene callbacks with a local, mocked engine/SDK."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HEADERS = (
    "melee/gm/gm_1601.h", "melee/gm/gm_1A3F.h", "melee/gm/gm_1B03.h",
    "melee/gm/gmscdata.h", "melee/gm/gmscene.h", "melee/gm/gmvs.h", "melee/gm/gmvsmelee.h",
    "melee/gm/types.h", "melee/lb/lbdvd.h", "melee/mn/types.h",
    "melee/pl/player.h", "sysdolphin/baselib/controller.h",
)
MOCK = r"""
#pragma once
#include <stddef.h>
typedef signed char s8;
typedef unsigned char u8;
typedef struct { int unused; } StartMeleeData;
typedef struct { int pending_scene_change; } CSSData;
typedef struct GameModeState GameModeState;
struct GameModeState {
    int id, preload, flags;
    void (*on_enter)(GameModeState*);
    void (*on_exit)(GameModeState*);
    struct { int scene_kind; void* enter_data; void* exit_data; } info;
};
typedef struct { int kind; GameModeState* states; } GameMode;
typedef struct { StartMeleeData start; } VsModeData;
typedef struct { int match_result; unsigned int frame_count; } VsSceneState;
enum { GM_MENU = 1, GM_VS = 2, GM_COUNT = 255, GM_GAMEMODESTATE_TERMINATE = 255,
       GS_CSS, GS_VS, GS_RESULTS, lbDvdPreload_3, OUTCOME_NO_CONTEST = 7,
       HSD_PAD_L = 1, HSD_PAD_R = 2, HSD_PAD_A = 4, HSD_PAD_START = 8 };
extern struct ExitInfo { struct { int outcome; } match_end; } gmVsMelee_VsExitInfo;
extern StartMeleeData gmVsMelee_StartData;
extern int gmVsMelee_ResultsEnterData;
extern struct Pad { unsigned int button; } HSD_PadMasterStatus[4];
void* gm_GetGameModeStateEnterData(GameModeState*);
void* gm_GetGameModeStateExitData(GameModeState*);
void gm_801B06B0(CSSData*, int, int, int, int, int, int, int);
void gm_801B0730(CSSData*, s8*, void*, u8*, u8*, void*);
void lbDvd_SetupVsPreloadCache(void);
void gm_SetNextGameModeStateId(int);
void gm_ChangeGameModeAfterCurrentScene(int);
VsModeData* gmVsMelee_GetVsData(void);
void gmVsMelee_EnterVs(GameModeState*, VsModeData*, void*, void*);
void gmVsMelee_ExitVs(GameModeState*, int, int);
void gmVsMelee_EnterResults(GameModeState*);
GameMode* gm_GetAllGameModes(void);
VsSceneState* gmVs_GetSceneState(void);
void fn_8016CF4C(int, int);
int Player_GetStocks(int);
"""
HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include "mu_tourney.c"
static GameModeState vanilla[] = {{255}};
static GameMode modes[] = {{GM_VS, vanilla}, {GM_COUNT, NULL}};
static VsModeData vs;
static VsSceneState scene;
struct ExitInfo gmVsMelee_VsExitInfo;
StartMeleeData gmVsMelee_StartData;
int gmVsMelee_ResultsEnterData;
struct Pad HSD_PadMasterStatus[4];
static int assigned = 1, status = 1, reports, releases, menu = -1, forced, checks, calls;
static int stocks[2] = {4, 4}, winner = -1;
static unsigned char snapshot[9];
#define CHECK(x) do { ++checks; assert(x); } while (0)
int mu_online_abi_command(unsigned int c, const unsigned char* p, unsigned int n,
                          unsigned char* out, unsigned int cap, unsigned int* got) {
    (void)n; (void)cap; CHECK(c == MU_TOURNEY_COMMAND); ++calls; *got = 0;
    if (*p == MU_TOURNEY_LAUNCH && assigned) {
        unsigned char launch[] = {1, 0, 31, 9, 9};
        memcpy(out, launch, 5); *got = 5;
    } else if (*p == MU_TOURNEY_STATUS) { out[0] = status; *got = 1; }
    else if (*p == MU_TOURNEY_RESULT) { ++reports; memcpy(snapshot, p, sizeof snapshot); }
    else if (*p == MU_TOURNEY_EXIT) { CHECK(modes[0].states == vanilla); ++releases; }
    return 0;
}
void* gm_GetGameModeStateEnterData(GameModeState* s) { return s->info.enter_data; }
void* gm_GetGameModeStateExitData(GameModeState* s) { return s->info.exit_data; }
void gm_801B06B0(CSSData* c, int a,int b,int d,int e,int f,int g,int h) {
    (void)c;(void)a;(void)d;(void)e;(void)f;(void)g;(void)h; CHECK(b == 33);
}
void gm_801B0730(CSSData* c,s8* a,void* b,u8* d,u8* e,void* f) {
    (void)c;(void)b;(void)d;(void)e;(void)f; *a = 2;
}
void lbDvd_SetupVsPreloadCache(void) {}
void gm_SetNextGameModeStateId(int id) { (void)id; }
void gm_ChangeGameModeAfterCurrentScene(int mode) { menu = mode; }
VsModeData* gmVsMelee_GetVsData(void) { return &vs; }
void gmVsMelee_EnterVs(GameModeState* s,VsModeData* v,void* a,void* b) {(void)s;(void)v;(void)a;(void)b;}
void gmVsMelee_ExitVs(GameModeState* s,int a,int b) {(void)s;(void)a;(void)b;}
void gmVsMelee_EnterResults(GameModeState* s) {(void)s;}
GameMode* gm_GetAllGameModes(void) { return modes; }
VsSceneState* gmVs_GetSceneState(void) { return &scene; }
void fn_8016CF4C(int a,int outcome) {(void)a; scene.match_result = outcome;}
int Player_GetStocks(int p) { return stocks[p]; }
int mu_online_singles_winner(int method) {(void)method; return winner;}
void mu_replay_apply_game_info(StartMeleeData* d,const unsigned char* p) {(void)p; d->unused = 99;}
int mu_practice_bridge(int op,int* a,int n) { CHECK(op == 6 && n == 1 && *a == GM_MENU); ++forced; return 0; }
static void boot(void) {
    menu = -1; status = 1; scene.match_result = 0;
    CHECK(mu_tourney_boot()); CHECK(active && modes[0].states == states);
    CHECK(!started && !completed && !in_flight && !returning && !expired);
    CHECK(vs.start.unused == 17);
}
static void unloaded_to_menu(void) {
    CHECK(menu == GM_MENU); CHECK(modes[0].states == states);
    const int count = releases;
    // Engine calls this after scene on_exit + mode on_unload, before loading the menu.
    mu_tourney_enter_mode(GM_MENU);
    CHECK(releases == count + 1); CHECK(modes[0].states == vanilla);
    CHECK(!active && !started && !completed && !in_flight && !expired && !returning);
    CHECK(human_character == 33 && human_color == 0 && human_nametag == 0x78);
    CHECK(vs.start.unused == 17);
    CHECK(bracket_stage == 0 && cpu_character == 0 && cpu_level == 0);
}
int main(void) {
    vs.start.unused = 17;
    // Ordinary startup and ticks do not touch the scene table or SDK.
    assigned = 0; CHECK(!mu_tourney_boot()); CHECK(modes[0].states == vanilla);
    const int baseline = calls; mu_tourney_poll(); mu_tourney_quit(); mu_tourney_tick();
    CHECK(calls == baseline); assigned = 1;
    // CSS cancel sends a single best-effort loss and stays open at the main menu.
    boot(); css_data.pending_scene_change = 2; css_decide(&states[0]);
    CHECK(reports == 1 && snapshot[1] == 7 && snapshot[8] == 2);
    mu_tourney_quit(); CHECK(reports == 1); CHECK(in_flight && !completed);
    unloaded_to_menu();
    // A new tournament can run, complete, show results and restore the original VS table.
    boot(); human_character = 2; vs_prep(&states[1]); CHECK(started);
    stocks[0] = stocks[1] = 0; scene.frame_count = 1800;
    mu_tourney_report(2); mu_tourney_report(2); mu_tourney_quit();
    CHECK(reports == 2 && snapshot[6] == 0 && snapshot[7] == 0 && snapshot[8] == 2);
    CHECK(in_flight && !completed); status = 2; mu_tourney_poll();
    CHECK(completed && !in_flight); mu_tourney_report(2); CHECK(reports == 2);
    gmVsMelee_VsExitInfo.match_end.outcome = 2; vs_decide(&states[1]);
    results_decide(&states[2]); unloaded_to_menu();
    // Expiry in CSS leaves through the normal forced scene teardown, with NO forfeit.
    boot(); status = 0; mu_tourney_poll(); CHECK(forced == 1 && expired && returning);
    // Simulate initialization consuming the first leave request during preload.
    mu_tourney_poll(); CHECK(forced == 2);
    css_data.pending_scene_change = 2; css_decide(&states[0]); mu_tourney_quit();
    CHECK(reports == 2); unloaded_to_menu();
    // Expiry during gameplay suppresses LRAS, match-end and orderly-close reports.
    boot(); human_character = 2; vs_prep(&states[1]); status = 0; mu_tourney_poll();
    CHECK(forced == 3); HSD_PadMasterStatus[0].button = 15;
    mu_tourney_tick(); CHECK(scene.match_result == 0);
    mu_tourney_report(2); mu_tourney_quit(); vs_decide(&states[1]);
    CHECK(reports == 2); unloaded_to_menu();
    // Live LRAS still forfeits once; no process-exit command is used before teardown.
    boot(); human_character = 2; vs_prep(&states[1]); mu_tourney_tick();
    CHECK(scene.match_result == 7); mu_tourney_report(7); mu_tourney_quit();
    CHECK(reports == 3 && snapshot[1] == 7 && snapshot[8] == 2);
    gmVsMelee_VsExitInfo.match_end.outcome = 7; vs_decide(&states[1]); unloaded_to_menu();
    printf("%d tournament scene checks passed\n", checks);
    return 0;
}
"""


class TournamentSceneTest(unittest.TestCase):
    def test_production_callbacks_with_mocked_engine(self):
        cc = shutil.which("cc")
        self.assertIsNotNone(cc, "local C compiler is required")
        with tempfile.TemporaryDirectory(prefix="tourney-scene-", dir=ROOT) as tmp:
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


if __name__ == "__main__":
    unittest.main()
