"""Run the production room worker with a fake Socket: no helper, sockets, or network."""
from pathlib import Path
import unittest
import online_menu_test as menu
from yougame_console_wiring_test import body
ROOT = Path(__file__).resolve().parents[2]

class FriendsDisconnectTest(unittest.TestCase):
    def test_room_worker_recovery_late_close_and_quit(self):
        source = (ROOT / 'port/runtime/hle/yougame_link.cpp').read_text()
        start = source.index('struct ConsoleBackend {')
        end = source.index('\n};',start)+3
        backend = source[start:end]
        # The direct helper's attachment belongs to its existing tests; this fixture owns no ring.
        backend = backend.replace(body(backend,'attach'),' return true; ')
        menu.OnlineMenuTest().compile_fixture('39-room-worker', r'''
#include "yougame_console_entry.h"
#include <condition_variable>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cstdlib>
int checks=0;
#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();} ++checks; } while(0)
std::atomic<bool> quitting{false}, drop{false}, throw_next{false};
std::atomic<int> drop_code{1006};
namespace host {bool exit_requested(){return quitting;}void log(const char*,...) {}}
using ENetSocket=int;constexpr int ENET_SOCKET_NULL=-1;
void yg_enet_destroy(int) {} void yg_enet_enable(int) {}
namespace yougame {
using json=nlohmann::json;
namespace ranked {std::string str(const json& j,const char* key) {return friends::text(j,key,256);}}
uint64_t now_ms() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
std::mutex mock_mutex;
std::deque<json> wire;
std::vector<json> sent;
std::vector<std::string> sessions;
std::string me;
std::atomic<int> opens{0};
std::atomic<bool> has_console{true};
json roster() {return json::array({{{"id","host"},{"name","Peach"}},{{"id","guest"},{"name","Mario"}}});}
void push(json m) {std::lock_guard<std::mutex> l(mock_mutex);wire.push_back(std::move(m));}
class Socket {
 public:
 bool gone=false;int code=1000;
 bool open_session(const std::string&,const std::string&,const std::string&,std::string*,int,
                   const std::function<bool()>&,const std::string& session) {
   {std::lock_guard<std::mutex> l(mock_mutex);sessions.push_back(session);}
   ++opens;gone=false;code=1000;
   json welcome={{"t","welcome"},{"me",me},{"host","host"},{"players",roster()},{"revision",1}};
   if(has_console)welcome["console"]={{"sessionId",session}};
   push(std::move(welcome));
   return true;
 }
 bool open_session(const std::string& u,const std::string& t,const std::string& r,std::string* e,int ms,const std::string& s) {
   return open_session(u,t,r,e,ms,{},s);
 }
 int connection() const {return opens.load();}
 bool lobby_mismatch() const {return false;}
 int close_code() const {return code;}
 bool recoverable() const {return gone && code==1006;}
 bool rejoinable() const {return gone && (code==1006 || code==4002);}
 bool closed() const {return gone;}
 void close(int=1000) {gone=true;}
 bool send(const json& m) {std::lock_guard<std::mutex> l(mock_mutex);sent.push_back(m);return !gone;}
 bool next(json& m,int) {
   if(throw_next.exchange(false))(void)json(nullptr).at("t"); // any throw inside the worker loop
   if(drop.exchange(false)){gone=true;code=drop_code;return false;}
   {std::lock_guard<std::mutex> l(mock_mutex);if(!wire.empty()) {m=std::move(wire.front());wire.pop_front();return true;}}
   std::this_thread::sleep_for(std::chrono::milliseconds(1));return false;
 }
};
''' + backend + r'''
}
template<class F>void until(F ready) {
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!ready() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 CHECK(ready());
}
void reset(const char* identity) {
 std::lock_guard<std::mutex> l(yougame::mock_mutex);
 yougame::wire.clear();yougame::sent.clear();yougame::sessions.clear();yougame::opens=0;
 yougame::me=identity;quitting=drop=throw_next=false;drop_code=1006;yougame::has_console=true;
}
bool sent_leave() {
 std::lock_guard<std::mutex> l(yougame::mock_mutex);
 for(auto& m:yougame::sent)if(m.value("t","")=="leave")return true;
 return false;
}
int main() {
 using namespace yougame;
 reset("guest");friends::lobby.begin(false);friends::lobby.opened("room123");
 {
   ConsoleBackend b;b.session="session-fixed";b.renew_ticket=[](int){return std::string("fixture-ticket");};
   CHECK(b.open("fixture-ticket","room123",100));until([&]{std::lock_guard<std::mutex> l(b.mutex);return !b.last_roster.is_null();});
   friends::lobby.session_stopped();drop=true;until([&]{return opens==2;});
   until([&]{std::lock_guard<std::mutex> l(mock_mutex);return sessions.size()==2;});
   {std::lock_guard<std::mutex> l(mock_mutex);CHECK(sessions[0]=="session-fixed" && sessions[1]=="session-fixed");}
   std::string line;CHECK(friends::lobby.held() && !friends::lobby.take_return(line));
   // Missing frozen metadata after a recovery is authoritative even if sessionEnded was missed.
   // Rooms' silent-socket close (4002) holds the seat like a 1006 drop: the worker rejoins.
   has_console=false;b.waiting_previous=true;drop_code=4002;drop=true;
   until([&]{return opens==3 && !b.session_live && !b.waiting_previous;});
   CHECK(b.interrupted && friends::lobby.held() && !friends::lobby.take_return(line));
   // A next Start may arrive before the menu tick. Keep it and its queued card chunks.
   push({{"t","start"},{"console",{{"sessionId","new-session"}}}});
   push({{"t","msg"},{"d",{{"kind","mu-card"},{"match","new-session"},{"i",0}}}});
   until([&]{std::lock_guard<std::mutex> l(b.mutex);return b.last_roster.count("console") && b.last_roster["console"]["sessionId"]=="new-session" && b.incoming.size()>=5;});
   CHECK(b.resume_room(false) && !b.waiting_previous);
   json queued;bool saw_start=false,saw_card=false;
   while(b.next(queued,0)) {
     saw_start |= queued.count("console") && queued["console"]["sessionId"]=="new-session";
     saw_card |= queued.value("t","")=="msg" && queued["d"].value("kind","")=="mu-card";
   }
   CHECK(saw_start && saw_card);
   // The link can be gone first; the retained room worker still delivers the final host closure.
   push({{"t","roomClosed"},{"reason","host_left"},{"name","Peach"}});
   until([&]{return friends::lobby.cancelled();});
   CHECK(friends::lobby.take_return(line) && line=="Peach left: lobby closed");b.close();
 }
 reset("host");friends::lobby.begin(true);friends::lobby.opened("room123");
 {
   ConsoleBackend b;b.session="session-fixed";CHECK(b.open("fixture-ticket","room123",100));
   until([&]{std::lock_guard<std::mutex> l(b.mutex);return !b.last_roster.is_null();});
   CHECK(b.resume_room(true) && b.waiting_previous);
   CHECK(b.flush(500));
   {std::lock_guard<std::mutex> l(mock_mutex);CHECK(std::any_of(sent.begin(),sent.end(),[](const json& m){return m.value("t","")=="end-session" && m.value("sessionId","")=="session-fixed";}));}
   push({{"t","leave"},{"player",{{"id","guest"}}},{"players",json::array({{{"id","host"}}})},{"host","host"}});
   push({{"t","sessionEnded"},{"sessionId","session-fixed"},{"reason","away"}});
   until([&]{return b.interrupted.load();});friends::lobby.session_stopped();
   CHECK(friends::lobby.take_reenter()=="room123");std::string line;CHECK(!friends::lobby.take_return(line));
   // Automatic relaunch closes cleanly; it must not be mistaken for the human quitting.
   b.handoff_pending=true;quitting=true;b.close();CHECK(!sent_leave());
 }
 reset("host");friends::lobby.begin(true);friends::lobby.opened("room123");
 {
   ConsoleBackend b;CHECK(b.open("fixture-ticket","room123",100));quitting=true;
   until([&]{return sent_leave();});b.close();CHECK(sent_leave());
 }
 reset("host");friends::lobby.begin(true);friends::lobby.opened("room123");
 {
   // A room message this build cannot use must fail the room, never leave the worker thread:
   // an exception escaping it is std::terminate, which ends the whole game (exit 3).
   ConsoleBackend b;CHECK(b.open("fixture-ticket","room123",100));
   until([&]{std::lock_guard<std::mutex> l(b.mutex);return !b.last_roster.is_null();});
   push({{"t","welcome"},{"me","host"},{"host","host"},{"players",roster()},{"participants",nullptr}});
   push({{"t","leave"},{"player",{{"id","guest"}}},{"players",json::array({{{"id","host"}}})},{"host","host"}});
   // That shape is read safely now: the room stays up and the guest's seat is gone.
   until([&]{std::lock_guard<std::mutex> l(b.mutex);return b.last_roster.value("players",json()).size()==1;});
   CHECK(!b.closed);
   throw_next=true;until([&]{return b.closed.load();});
   CHECK(!b.send({{"t","ping"}}) && !b.flush(0));b.close();
 }
 std::printf("production Friends worker: %d checks passed\n",checks);
}
''')

if __name__ == '__main__': unittest.main()
