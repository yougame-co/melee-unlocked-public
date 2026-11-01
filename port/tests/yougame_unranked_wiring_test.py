"""Offline route checks for the existing Slippi CSS/rematch loop."""
from pathlib import Path
import unittest
import re
from relay_pads_wiring_test import code, block
def function(text, name):
    # Locate braces in masked C++, but keep string literals in the returned body.
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                    lambda m: ' ' * len(m.group()), text, flags=re.S)
    match = re.search(r'\b' + name + r'\([^;]*?\)\s*\{', masked)
    if not match: raise AssertionError('Missing function: ' + name)
    body = block(masked, match.end())
    return text[match.end():match.end() + len(body)]
ROOT = Path(__file__).resolve().parents[2]
def source(p): return (ROOT / p).read_text(encoding='utf-8-sig')
class UnrankedWiringTest(unittest.TestCase):
    def test_match_end_keeps_peer_and_uses_css(self):
        online = function(source('port/runtime/hle/slippi_online.cpp'), 'prepare_online_match_state')
        transition = online[:online.index('g_last_match_state_retrace =')]
        self.assertIn('g_in_online_match && yougame::unranked::keep_css(yougame::active(), (int)g_last_search.mode)', transition)
        self.assertIn('g_in_online_match = false;', transition)
        for teardown in ('cleanup_connection(', 'end_session(', 'g_netplay =', 'g_matchmaking ='):
            self.assertNotIn(teardown, transition)
        decide = function(source('sourceport/game/shim/mu_slippi_menu.c'), 'vs_decide')
        self.assertIn('int next = MU_SLP_STATE_CSS;', decide)
        self.assertIn('if (slp.mode == MU_SLP_MODE_PARTY)', decide)
        self.assertIn('gm_SetNextGameModeStateId((u8) next);', decide)
    def test_room_stays_and_report_does_not_disconnect(self):
        link = source('port/runtime/hle/yougame_link.cpp')
        self.assertIn('if (search.mode == 1) request["afterMatch"] = "stay";', function(link, 'find_match'))
        ended = function(link, 'game_ended')
        for teardown in ('end_session(', 'close_room(', 'cleanup_connection('): self.assertNotIn(teardown, ended)
        self.assertIn('g_room->games.report(winner)', ended)
    def test_both_training_settings_have_same_scoped_exception(self):
        practice = source('port/runtime/hle/native_practice.cpp')
        self.assertIn('yougame::unranked::allow_training_return(yougame::active(), session_mode_seen)', practice)
        for option in ('back_to_training', 'return_training'):
            self.assertIn('played && allow_training && gx::RenderOptions::live_online_' + option, practice)
        policy = source('port/runtime/hle/yougame_unranked.h')
        self.assertIn('return active && mode == 1;', policy)
        # Direct, Teams, Friends and standalone cannot enter the exception.
        self.assertNotIn('mode == 2', policy)
        self.assertNotIn('mode == 3', policy)
    def test_hold_z_reuses_cleanup_and_start_reuses_search(self):
        css = function(source('sourceport/game/shim/mu_slippi_css.c'), 'mu_slippi_css_inputs')
        self.assertIn('dt.z_hold_timer > DISCONNECT_HOLD_DELAY', css)
        self.assertIn('mu_slippi_cmd(0xBA, &reason, 1, NULL, 0, NULL)', css)
        idle = css[css.index('case MU_SLP_MM_IDLE:'):css.index('case MU_SLP_MM_INITIALIZING:')]
        self.assertIn('trigger & HSD_PAD_START', idle)
        self.assertIn('case MU_SLP_MODE_UNRANKED:', idle)
        self.assertIn('lock_in_and_search(SB_RAND)', idle)
        host = source('port/runtime/hle/slippi_online.cpp')
        for line in ('yougame: match over, back to the character screen', 'yougame: connection ended (Z)', 'yougame: new search (Start)'):
            self.assertEqual(host.count('"' + line + '"'), 1)
        self.assertIn('cleanup_connection(yougame::Leave::Quit); return true;', host)
        self.assertIn('start_find_match(payload); return true;', host)
    def test_ranked_set_over_and_training_destination_remain_on_css(self):
        import online_menu_test as menu
        runtime = source('port/runtime/hle/native_practice.cpp')
        begin = runtime.index('  {\n    static bool session_seen')
        end = runtime.index('\n  // Entering major 8', begin)
        tick = 'void tick() {\n' + runtime[begin:end] + '\n}\n'
        decide = menu.fixture.extract(source('sourceport/game/shim/mu_slippi_menu.c'), 'vs_decide')
        stubs = r"""
#include <cstdio>
#include <cstdlib>
#include <string>
#include <initializer_list>
#include "yougame_unranked.h"
using u8=unsigned char; using s8=signed char; using u32=unsigned; using ssize_t=long;
enum { MU_SLP_STATE_CSS, MU_SLP_STATE_GAME_SETUP, MU_SLP_STATE_RESULTS, MU_SLP_MODE_RANKED=0,
MU_SLP_MODE_UNRANKED=1, MU_SLP_MODE_TEAMS=3, MU_SLP_MODE_PARTY=4, MU_SLP_MM_CONNECTION_SUCCESS=4,
MU_SETUP_NEXT_GAME=1, MU_SETUP_TIEBREAK=2, MU_SETUP_SET_OVER=3,
MU_YG_PRACTICE_WIN=1, MU_YG_PRACTICE_LOSS=2, MU_SLP_WINNER_LOST=0,
gmVsMode_State_Results=0,gmVsMode_State_SuddenDeath=0,GM_MAX_PLAYERS=4,Gm_PKind_NA=3 };
struct GameModeState {};
struct Player { int pkind=3,x20=0,self_destructs=0,x44=0,is_big_loser=0; };
struct MatchEnd { Player player_standings[4]; int outcome=0; u32 x0=0; };
struct MatchExitInfo { MatchEnd match_end; } gmVsMelee_VsExitInfo;
struct { int mode=0, game_local_index=0,is_winner=0,chose_stage=0; } slp;
struct { int local_index=0,connection_state=4; u8 game_info[32]{}; } last_match;
struct MuRankedSet { int score[2]={2,1}; } set;
static unsigned seed; unsigned* HSD_RandSeedPtr=&seed;
int checks=0,next_state=-1,cleanup=0,end_mode=-1,practice_action=-1,finish_result=MU_SETUP_SET_OVER;
bool cpu=false;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort(); } } while(0)
void gmVsMelee_ExitVs(GameModeState*,int,int) {}
MatchExitInfo* gm_GetGameModeStateExitData(GameModeState*) { return &gmVsMelee_VsExitInfo; }
void gm_80162A98(int) {} void gm_RecordSelfDestructs(int) {} void gm_80162A4C(int) {}
void mu_slippi_load_match_state(void*) {} void mu_slippi_cleanup_connections() { ++cleanup; }
int mu_ranked_setup_cpu() { return cpu; } int won_last_game(int i) { return i==0; }
void mu_ranked_setup_game_result(int,int,int) {}
int mu_ranked_setup_finish_game(int,unsigned) { return finish_result; }
struct VsData { struct { struct { unsigned stkind=8; } rules; } start; } vs_data;
VsData* gmVsMelee_GetVsData() { return &vs_data; }
int mu_ranked_practice() { return cpu; }
const MuRankedSet* mu_ranked_set() { return &set; }
void mu_ranked_practice_control(int a) { practice_action=a; }
void mu_slippi_cmd(int,const u8* p,int,void*,int,void*) { end_mode=*p; }
void gm_SetNextGameModeStateId(u8 n) { next_state=n; }
unsigned mu_slippi_new_seed() { return 123; }
enum class MatchMode { Ranked=0,Unranked=1 };
enum class Phase { Idle };
static int session_mode_value=-1,guest_mode=0,training=0,kTrainingMajor=28,g_origin_major=8;
static bool active=true,back_setting=false,return_setting=false,g_restore_training=false,online_scene=true,playing=false;
namespace yougame { bool active() { return ::active; } }
namespace slippi {
namespace online { int session_mode() { return session_mode_value; } bool is_online_match() { return playing; } int local_player_index() { return 0; } }
namespace playback { bool enabled() { return false; } }
}
namespace gx { struct RenderOptions {
static bool live_online_back_to_training() { return back_setting; }
static bool live_online_return_training() { return return_setting; }
}; }
struct Lifecycle { Phase phase() { return Phase::Idle; } int return_after_session() { return 1; } } g_lifecycle;
void run_decision(int) { ++training; }
bool is_online_scene() { return online_scene; } int online_mode() { return guest_mode; }
void enter_training_after_session(MatchMode, int = -1, int = -1) { ++training; }
namespace host { void log(const char*) {} }
"""
        cases = r"""
int main() {
    for(bool local : {false,true}) for(bool win : {false,true}) {
        cpu=local; slp.mode=0; finish_result=MU_SETUP_SET_OVER;
        set.score[0]=win?2:1;set.score[1]=win?1:2;
        GameModeState state;next_state=-1;end_mode=-1;practice_action=-1;
        vs_decide(&state);CHECK(next_state==MU_SLP_STATE_CSS&&end_mode==0);
        if(local)CHECK(practice_action==(win?MU_YG_PRACTICE_WIN:MU_YG_PRACTICE_LOSS));
        for(bool yg : {false,true}) for(bool back : {false,true}) for(bool ret : {false,true}) {
            active=yg;back_setting=back;return_setting=ret;guest_mode=0;
            session_mode_value=0;playing=true;tick();session_mode_value=-1;playing=false;
            int before=training;for(int frame=0;frame<1000;++frame)tick();
            CHECK(training==before&&next_state==MU_SLP_STATE_CSS);
        }
    }
    // Existing Unranked behaviour for BOTH launcher scopes/settings is identical.
    for(bool yg : {false,true})for(bool back : {false,true})for(bool ret : {false,true}) {
        active=yg;back_setting=back;return_setting=ret;guest_mode=1;
        session_mode_value=1;playing=true;tick();session_mode_value=-1;playing=false;
        // Upstream v0.9.91: "back to Training" no longer leaves Unranked's character select after a game;
        // only the plain "go back to Training mode" setting still does (outside YouGame).
        int before=training;tick();CHECK(training-before==int(!yg&&ret));
    }
    // Local Ranked practice cannot inherit the previous Unranked session's destination.
    active=false;back_setting=return_setting=true;session_mode_value=1;guest_mode=1;playing=true;tick();
    session_mode_value=-1;guest_mode=0;playing=false;int before=training;tick();CHECK(training==before);
    std::printf("ranked set-over destination: %d checks; online/practice, win/loss, 64000 idle CSS ticks, Unranked unchanged\n",checks);
}
"""
        menu.OnlineMenuTest().compile_fixture('ranked-return', stubs + decide + tick + cases)

    def test_practice_hint_and_handler_share_exact_predicate(self):
        import online_menu_test as menu
        css = source('sourceport/game/shim/mu_slippi_css.c')
        self.assertIn('mu_slippi_practice_available() && (trigger & HSD_PAD_Z)', function(css, 'mu_slippi_css_inputs'))
        self.assertIn('if (mu_slippi_practice_available())\n        set_text(ST_PRESS_D, "Z: Practice vs CPU");', css)
        predicate = menu.fixture.extract(css, 'mu_slippi_practice_available')
        inputs = function(css, 'mu_slippi_css_inputs')
        idle = inputs[inputs.index('case MU_SLP_MM_IDLE:'):inputs.index('case MU_SLP_MM_INITIALIZING:')]
        hint_start = css.index('    set_text(ST_PRESS_D, conn ==')
        hint_end = css.index('\n#endif', hint_start) + len('\n#endif')
        paths = ('int css_idle_inputs(unsigned trigger) { auto* st=mu_slippi_state(); '
                 'switch(dt.msrb.connection_state) {\n' + idle + '\ndefault:return 0; }}\n'
                 'void css_hint() { int conn=dt.msrb.connection_state;\n' + css[hint_start:hint_end] + '\n}\n')
        stubs = r"""
#include <cstdio>
#include <cstdlib>
#include <string>
enum { MU_SLP_MODE_RANKED=0,MU_SLP_MODE_UNRANKED,MU_SLP_MODE_PARTY,MU_SLP_MODE_DIRECT,MU_SLP_MODE_TEAMS,
MU_SLP_MM_IDLE=0,MU_SLP_MM_CONNECTION_SUCCESS=4,HSD_PAD_X=1,HSD_PAD_START=2,HSD_PAD_Z=4,HSD_PAD_Y=8,
MU_SLP_WINNER_NULL=-1,MU_YG_PRACTICE_START=1,MU_YG_PRACTICE_STOP=0,SB_RAND=0,ST_PRESS_D=8 };
struct { int mode,is_winner,chose_stage; } state;
auto* mu_slippi_state() { return &state; }
struct { struct { int connection_state; } msrb; int chat_window_opened; } dt;
unsigned char chosen;unsigned char* char_chosen=&chosen;
int capability,p2p,ready,allowed,starts,searches;
std::string hint;
void set_text(int,const char* text) { hint=text; }
int mu_ranked_practice_allowed() { return allowed; }
int mu_ranked_practice() { return starts!=0; }
int mu_ranked_practice_control(unsigned action) { if(action==MU_YG_PRACTICE_START)++starts;return 1; }
void lock_in_and_search(int) { ++searches; }
void load_code_entry() { std::abort(); }
int mu_slippi_ranked_available() { return capability; }
int mu_online_p2p_menus() { return p2p; } int gm_801A4BB8() { return ready; }
"""
        cases = r"""
int main() {
    int checks=0;
    for(int mode=0;mode<5;++mode)for(int conn=0;conn<6;++conn)
    for(int flags=0;flags<64;++flags) {
        state.mode=mode;dt.msrb.connection_state=conn;
        capability=flags&1;p2p=flags&2;dt.chat_window_opened=flags&4;
        chosen=bool(flags&8);ready=flags&16;allowed=flags&32;
        bool expected=mode==0&&conn==0&&capability&&!p2p&&!dt.chat_window_opened&&chosen&&ready&&allowed;
        if(bool(mu_slippi_practice_available())!=expected)std::abort();++checks;
    }
    state.mode=0;dt.msrb.connection_state=0;capability=ready=chosen=1;p2p=dt.chat_window_opened=0;
    for(int permit=0;permit<2;++permit)for(int picked=0;picked<2;++picked) {
        allowed=permit;chosen=picked;starts=searches=0;css_hint();
        if(hint!=((permit&&picked) ? "Z: Practice vs CPU" : ""))std::abort();++checks;
        for(unsigned button : {unsigned(HSD_PAD_X),unsigned(HSD_PAD_Y),unsigned(HSD_PAD_X|HSD_PAD_Y)}) {
            starts=searches=0;
            if(css_idle_inputs(button)!=0||starts||searches)std::abort();++checks;
        }
        starts=searches=0;
        if(css_idle_inputs(HSD_PAD_Z)!=0||starts!=(permit&&picked)||searches!=(permit&&picked))std::abort();++checks;
        starts=searches=0;
        if(css_idle_inputs(HSD_PAD_START)!=0||starts||searches!=picked)std::abort();++checks;
    }
    char_chosen=nullptr;if(mu_slippi_practice_available())std::abort();
    std::printf("practice Z, X/Y never queue, Start/picked, agent permission/hint: %d combinations passed\n",checks);
}
"""
        menu.OnlineMenuTest().compile_fixture('practice-predicate', stubs + predicate + paths + cases, '-DYOUGAME_RANKED_BRANCH=1')

    def test_tests_registered_for_linux(self):
        root = source('CMakeLists.txt'); self.assertLess(root.index('foreach(policy yougame_unranked'), root.index('  return()'))
        self.assertIn("'port_yougame_unranked_test'", source('tools/build_linux.py'))
        for part in ('integration', 'engine', 'pad'):
            self.assertIn("'port_yougame_console_" + part + "_test'", source('tools/build_linux.py'))
if __name__ == '__main__': unittest.main()
