"""Build 39 phase A: run state/bridge helpers offline, plus integration wiring."""
from pathlib import Path
import unittest
import online_menu_test as menu
from yougame_console_wiring_test import body
ROOT = Path(__file__).resolve().parents[2]
def read(p): return (ROOT / p).read_text()

class FriendsOrderTest(unittest.TestCase):
    def compile(self, name, code, *flags):
        menu.OnlineMenuTest().compile_fixture(name, code, *flags)

    def test_cancel_reinvite_input_and_frozen_pick(self):
        self.compile('39-lobby', r'''
#include "yougame_console_entry.h"
#include <cstdlib>
#include <cstdio>
#define CHECK(x) do {if(!(x))std::abort();++checks;}while(0)
int main(){int checks=0;using namespace yougame::friends;
 lobby.begin(true);lobby.opened("room123");CHECK(!lobby.shown() && !lobby.take_invite());
 lobby.report_ready();lobby.shown({18,3,2});CHECK(lobby.ready() && lobby.take_invite());
 auto q=picker(lobby.room());CHECK(q["room"]=="room123" && q["type"]=="mpFriend" && !q.count("nativeBuild"));
 try{throw std::runtime_error("Cancelled");}catch(const std::exception&){}
 CHECK(lobby.held() && lobby.room()=="room123" && lobby.shown());
 yougame::console::Entry input;CHECK(input.begin());
 CHECK(!input.poll(2,true,0)); CHECK(!input.poll(2,true,0x100));CHECK(input.poll(2,true,0x1000));
 input.poll(2,true,0);CHECK(input.poll(2,true,0x10));CHECK(lobby.take_invite());CHECK(picker(lobby.room())==q);
 CHECK(!input.poll(2,true,0x200) && lobby.held() && !lobby.cancelled());input.poll(2,true,0); // B un-picks in CSS
 auto frozen=lobby.freeze("Mario");CHECK(frozen==CssPick({18,3,2}).settings());
 lobby.shown({1,0,0});CHECK(lobby.freeze("Mario")==frozen);CHECK(lobby.line()=="Mario joined: loading together");
 CHECK(input.poll(2,true,0x100));input.poll(2,true,0);CHECK(input.poll(2,true,0x200));
 CHECK(lobby.cancelled() && !lobby.held() && lobby.room().empty());
 std::string line;CHECK(lobby.take_return(line));CHECK(!lobby.take_return(line));
 lobby.leave();CHECK(!lobby.take_return(line)); // a second Leave outside a held room tears nothing down
 for(auto s:{"friends1:26:0:0","friends1:1:6:0","friends1:1:0:4","friends1:01:0:0","friends1:1:0:0:bad"})CHECK(!css_pick(s));
 for(int c=0;c<26;++c)for(int x=0;x<6;++x)for(int p=0;p<4;++p)CHECK(css_pick(CssPick({c,x,p}).settings())->character==c);
 CHECK(closed_line({{"t","roomClosed"},{"reason","host_left"},{"name","Peach"}})=="Peach left: lobby closed");
 CHECK(closed_line({{"t","roomClosed"},{"reason","other"}}).empty());
 printf("friends state: %d checks passed\n",checks);
}''')

    def test_css_read_and_restore_execute_real_c_helpers(self):
        patch=read('sourceport/patches/melee-native.patch')
        start=patch.index('+int mu_friends_css_pick(int* a)')
        end=patch.index('+void mu_friends_css_enter(CSSData* css);',start)
        capture='\n'.join(s[1:] for s in patch[start:end].splitlines())
        restore=body(read('sourceport/game/shim/mu_practice.c'),'mu_friends_css_enter')
        self.compile('39-css', r'''
#include <cstdlib>
#include <cstdio>
#include <cstddef>
struct CSSDoor {int p_kind,sel_icon,costume;};struct {CSSDoor doors[4];} mnCharSel_803F0DFC;
struct Icon {int char_kind;} icons[26];int CSS_NONE=25;
struct Player {int ckind,color;};struct CSSData {struct {struct {Player players[4];} start;} vs;} css;
CSSData* mnCharSel_804D6CB0=&css;
int mu_friends_css_frames=2;
''' + capture + r'''
int MU_PR_SET_EVENT_BACKUP=5;int saved[3]{},calls=0;
int mu_practice_bridge(int,int* a,int){for(int n=0;n<3;++n)saved[n]=a[n];return 0;}
int mu_online_abi_command(unsigned int q,const unsigned char* p,unsigned int n,unsigned char* out,unsigned int cap,unsigned int* got){
 if(q!=0xE6 || *p!=10 || n!=1 || cap!=4)std::abort();out[0]=18;out[1]=3;out[2]=2;out[3]=1;*got=4;++calls;return 0;}
void mu_friends_css_enter(CSSData* css){
''' + restore + r'''
}
int main(){int a[3];mu_friends_css_frames=0;if(mu_friends_css_pick(a)!=-1)std::abort();mu_friends_css_frames=2;
 for(auto& d:mnCharSel_803F0DFC.doors)d={3,25,0};
 mnCharSel_803F0DFC.doors[2]={0,7,3};icons[7].char_kind=18;
 if(mu_friends_css_pick(a) || a[0]!=18 || a[1]!=3 || a[2]!=2)std::abort();
 mu_friends_css_enter(&css);mu_friends_css_enter(&css);
 if(calls!=1 || saved[0]!=18 || saved[1]!=3 || saved[2]!=2 || css.vs.start.players[0].ckind!=18 || css.vs.start.players[0].color!=3)std::abort();
 puts("CSS capture/boot restore: 8 checks passed");}
''')

    def test_agent_skips_discovery_and_refresh_before_io(self):
        self.compile('39-agent', r'''
#include "slippi_account.h"
#include <cstdio>
#include <cstdlib>
int main(){if(host::test_env::agent_copy())std::abort();host::test_env::agent_run=true;if(!host::test_env::agent_copy() || !slippi::account::launcher_roots().empty() || slippi::account::resolve("must-not-read"))std::abort();puts("agent discovery: 4 checks passed");}
''','-DYOUGAME_AGENT_TEST_HOOK')
        net=read('port/runtime/hle/slippi_net.cpp')
        self.assertIn('if (host::test_env::agent_copy()) return;',body(net,'RefreshFromServer'))
        login=body(net,'AttemptLogin')
        self.assertLess(login.index('if (host::test_env::agent_copy()) return false;'),login.index('account::resolve'))

    def test_room_css_picker_and_leave_wiring(self):
        native=body(read('port/runtime/hle/native_practice.cpp'),'tick')
        self.assertLess(native.index('!friends_lobby.room().empty()'),native.index('request_major(2)'))
        self.assertLess(native.index('request_major(2)'),native.index('yougame::pick_friend_room'))
        self.assertIn('current_major() == 2 && current_minor() == 0',native)
        self.assertIn('friends_lobby.reported()',native)
        self.assertIn('yougame::console::friends_entry("new")',native)
        self.assertIn('friends_lobby.take_return(leave_line)',native)
        link=read('port/runtime/hle/yougame_link.cpp')
        self.assertIn('push.value("cmd",json()) == "leave"',link)
        leave=body(link,'leave_friends_room')
        self.assertIn('{"t","leave"}',leave)
        retire=leave[leave.index('g_console_retired.begin([b,preparation]'):]
        self.assertNotIn('friends_backend',retire)
        self.assertNotIn('entry.available()',retire)
        # The session is read after the opening entry finished (it may commit one meanwhile);
        # end-session, then leave, are queued before the flush and the close.
        self.assertLess(retire.index('preparation.wait()'),retire.index('session = b->session'))
        self.assertLess(retire.index('session = b->session'),retire.index('{"t","end-session"}'))
        self.assertLess(retire.index('{"t","end-session"}'),retire.index('{"t","leave"}'))
        self.assertLess(retire.index('{"t","leave"}'),retire.index('b->flush(1000)'))
        self.assertLess(retire.index('b->flush(1000)'),retire.index('b->close()'))
        self.assertEqual(retire.count('"mpRoom"'),1)
        self.assertIn('preparation.wait()',retire)
        close=body(link,'close_friends_room')
        self.assertIn('release()',close)
        self.assertNotIn('b->close()',close)
        self.assertLess(native.index('leave_friends_room()'),native.index('console_engine().end(yougame::console::kSelfLeft)'))
        self.assertLess(native.index('console_engine().end(yougame::console::kSelfLeft)'),native.index('close_friends_room(std::move(retire))'))
        attach=body(link,'attach')
        self.assertLess(attach.index('wait_console_retired('),attach.index('yg_enet_enable(1)'))
        opening=body(link,'friends_entry')
        self.assertLess(opening.index('wait_console_retired()'),opening.index('track_friends_backend(b)'))
        self.assertIn('g_console_retired.wait(2000)',link)
        self.assertIn('if (b->retiring) return;',link)
        self.assertIn('b->retired_links.push_back(q)',link)
        for required in ['requests.clear()', 'friends::events.current_room({})','"pending.json"','"mpRoom"']:
            self.assertIn(required,leave)
        self.assertIn('recoverable() const { return broker_ && broker_->close_code() == 1006;',link)
        self.assertIn('room->close_code() == 4012',link)
        # Live roster identity belongs to Link during relink; both lobby observe
        # paths still process authoritative room closure through the real helper.
        self.assertIn('friends::lobby.observe(control)',link)
        # Only rosters that can carry a held host's NEW id are stripped; leave/lobby close a released host.
        self.assertIn('(type == "back" || type == "join" || type == "welcome")',link)
        self.assertIn('closed_line(m)',body(read('port/runtime/hle/yougame_friends_lobby.h'),'observe'))
        self.assertIn('mu_slippi_return_to_friends();',read('sourceport/game/shim/mu_practice.c'))
        self.assertIn('MU_SLP_OPT_FRIENDS',body(read('sourceport/game/shim/mu_slippi_menu.c'),'mu_slippi_return_to_friends'))
        self.assertIn('yougame::friends::lobby.line()',read('port/runtime/gx/replay_bar.cpp'))
        self.assertIn('if (friends::lobby.host()) return {};',body(read('port/runtime/hle/yougame_console_entry.h'),'waiting_status'))

    def test_friends_return_opens_at_menu_entry_not_from_onenter(self):
        # Build 40 guest crash (gobjplink.c:109): mn_80229894 freed a NULL menu proc when the
        # pending Friends return ran from the main menu's OnEnter. It now sets the menu state at entry.
        shim = read('sourceport/game/shim/mu_slippi_menu.c')
        enter = body(shim, 'mu_slippi_menu_enter')
        self.assertIn('if (friends_return) {', enter)
        self.assertIn('MU_SLP_OPT_FRIENDS', enter)
        self.assertNotIn('mu_slippi_return_to_friends()', body(shim, 'mu_slippi_main_menu_loaded'))
        self.assertLess(shim.index('static int friends_return;'), shim.index('mu_slippi_menu_enter('))

    def test_leave_blocks_late_manual_launch_and_handoff(self):
        self.compile('39-cancel-load', r'''
#include "yougame_console_entry.h"
#include <cstdlib>
#include <cstdio>
using namespace yougame::console;
int main(){
 auto data=std::filesystem::temp_directory_path()/("friends39-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 SessionConfig c;c.room="room123";c.session_id="session-fixed";c.ports={1,2};c.my_port=1;c.seed=42;c.epoch_ms=1793443200000LL;c.input_delay=2;
 if(!mark_session_left(data,c.session_id))std::abort();
 // Model an uninterruptible pending writer that finishes AFTER Leave deleted pending.json.
 if(!pending_write(data,session_launch(c),100,"build"))std::abort();
 bool accepted=false;
 if(pending_take(data,101,"build",[&](const json&,const auto&){accepted=true;return true;},[](const char*){}))std::abort();
 if(accepted || std::filesystem::exists(data/"console"/"pending.json") || !session_left(data,c.session_id))std::abort();
 std::atomic<bool> cancelled{false};HandoffIO io;io.cancelled=[&]{return cancelled.load();};
 io.choice=[&](const std::filesystem::path&,const SessionConfig&){cancelled=true;return true;};
 bool sent=false;
 if(handoff(c,true,data,100,"build",[&](const json&){sent=true;},[](const std::string&){},[](const char*){},io) || sent)std::abort();
 std::filesystem::remove_all(data);puts("leave during load: 6 checks passed");
}
''')

    def test_disconnect_roles_and_authoritative_room_departure(self):
        self.compile('39-disconnect', r'''
#include "yougame_console_entry.h"
#include <cstdlib>
#include <cstdio>
#define CHECK(x) do { if(!(x)) std::abort(); ++checks; } while(0)
int main() {
 int checks=0; using namespace yougame::friends;
 const json host={{"id","host"},{"connectionId","host"},{"name","Peach"},{"user","u-peach"}};
 const json guest={{"id","guest"},{"connectionId","guest"},{"name","Mario"}};
 auto welcome=[&](const char* me){return json{{"t","welcome"},{"me",me},{"host","host"},{"players",{host,guest}}};};
 Lobby h; h.begin(true);h.opened("room123");h.observe(welcome("host"));h.shown({18,3,0});h.freeze("Mario");
 h.observe({{"t","leave"},{"player",guest},{"players",{host}},{"host","host"}});
 h.session_stopped();CHECK(h.host() && h.held());CHECK(h.take_reenter()=="room123");
 h.shown({18,3,0});CHECK(h.line()==kRoomOpen);CHECK(h.take_invite());h.invite();CHECK(h.take_invite());
 std::string line;CHECK(!h.take_return(line));h.closed(4012);CHECK(h.held());
 auto start_guest=[&](Lobby& g){g.begin(false);g.opened("room123");g.observe(welcome("guest"));};
 Lobby g;start_guest(g);g.session_stopped(); // peer link has ended; authoritative room still lives
 CHECK(g.held() && !g.take_return(line));CHECK(g.line().find("Waiting for the room")!=std::string::npos);
 g.closed(1006);CHECK(g.held() && !g.take_return(line));
 auto away=host;away["away"]=true;
 g.observe({{"t","away"},{"player",away},{"players",{away,guest}},{"host","host"}});
 g.observe({{"t","lobby"},{"players",{away,guest}},{"host","host"}});CHECK(g.held());
 g.observe({{"t","back"},{"player",host},{"players",{host,guest}},{"host","host"}});CHECK(!g.take_return(line));
 g.observe({{"t","roomClosed"},{"reason","host_left"},{"name","Peach"}});
 CHECK(g.take_return(line) && line=="Peach left: lobby closed" && !g.held());
 start_guest(g);g.session_stopped();g.closed(4012);CHECK(g.take_return(line) && line=="Peach left: lobby closed");
 start_guest(g);g.session_stopped();
 g.observe({{"t","leave"},{"player",host},{"players",{guest}},{"host","guest"}});
 CHECK(g.take_return(line) && line=="Peach left: lobby closed");
 start_guest(g);g.observe({{"t","lobby"},{"players",{guest}},{"host","guest"}});
 CHECK(g.take_return(line) && line=="Peach left: lobby closed");
 // A held host back under a new connection id is the same account, not a Leave.
 auto moved=host;moved["id"]=moved["connectionId"]="host2";
 start_guest(g);g.observe({{"t","lobby"},{"players",{moved,guest}},{"host","host2"}});CHECK(g.held() && !g.take_return(line));
 g.observe({{"t","leave"},{"player",moved},{"players",{guest}},{"host","guest"}});
 CHECK(g.take_return(line) && line=="Peach left: lobby closed");
 // Explicit host stop is not room closure; the seat remains in the roster.
 start_guest(g);g.observe({{"t","sessionEnded"},{"reason","host_ended"}});CHECK(!g.take_return(line));
 printf("disconnect roles: %d checks passed\n",checks);
}
''')
        native=body(read('port/runtime/hle/native_practice.cpp'),'tick')
        self.assertIn('g_shared_was_active && !shared_active',native)
        self.assertIn('friends_entry(held_room,friends_lobby.host(),true)',native)
        link=read('port/runtime/hle/yougame_link.cpp')
        self.assertIn('std::shared_ptr<ConsoleBackend> friends_backend',link)
        self.assertIn('io.previous_session_active = [b]',link)
        self.assertIn('friends::lobby.observe(m)',link)
        self.assertIn('if (!friends::lobby.held() || host::exit_requested())',link)

    def test_round38_compiler_uses_cxx(self):
        from unittest import mock
        import os
        with mock.patch.dict(os.environ,{'CXX':'c++ -O0'}):
            self.assertEqual(menu.compiler_command(),['c++','-O0'])
        for p in ROOT.glob('port/tests/round38*_test.py'):
            self.assertNotIn("['clang++'",p.read_text())

if __name__=='__main__':unittest.main()
