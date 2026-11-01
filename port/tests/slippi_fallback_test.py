"""Execute the production ENet connection loop against a deterministic socket-free fake."""
from pathlib import Path
import unittest
import online_menu_test as menu
ROOT = Path(__file__).resolve().parents[2]

class SlippiFallbackTest(unittest.TestCase):
    def test_ticket_parser_preserves_one_entry_per_remote(self):
        source = (ROOT / 'port/runtime/hle/slippi_net.cpp').read_text()
        from yougame_console_wiring_test import body
        helpers = source[source.index('static const json* json_member('):source.index('uint64_t time_us()')]
        parser = source[source.index('static Matchmaking::ParsedTicket parse_ticket('):source.index('bool Matchmaking::ingest_ticket(')]
        fixture = ('#include "slippi_net.h"\n#include "slippi_address.h"\n'
                   '#include <nlohmann/json.hpp>\n#include <cctype>\n'
                   'using json=nlohmann::json;\nnamespace slippi {\n' + helpers +
                   'std::vector<std::string> User::GetDefaultChatMessages() {' + body(source,'GetDefaultChatMessages') + '}\n' +
                   'bool Matchmaking::IsBlockedMatch(OnlinePlayMode mode,const std::vector<std::string>& blocked,'
                   'const std::vector<UserInfo>& players,int local_index) {' + body(source,'IsBlockedMatch') + '}\n' +
                   parser + '}\n' + (ROOT / 'port/tests/slippi_ticket_test.cpp').read_text())
        menu.OnlineMenuTest().compile_fixture('39-slippi-ticket', fixture, '-I', str(ROOT / 'port/runtime/host'))

    def test_production_connect_loop(self):
        source = (ROOT / 'port/runtime/hle/slippi_net.cpp').read_text()
        start = source.index('void NetplayClient::ThreadFunc() {')
        end = source.index('  // Test only. MELEE_NET_LAG_MS', start)
        loop = source[start:end] + '}\n'
        menu.OnlineMenuTest().compile_fixture('39-slippi-fallback', r'''
#include "slippi_address.h"
#include <atomic>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } ++checks; } while(0)
int checks=0, scenario=0, resets=0, connects=0;
uint64_t clock_ms=0, fallback_at=0;
struct ENetAddress { unsigned host=0; uint16_t port=0; };
unsigned public_host() { const unsigned char b[4]={203,0,113,2}; unsigned h; std::memcpy(&h,b,4); return h; }
struct ENetPeer { ENetAddress address; } lan{{1,41000}}, pub{{public_host(),42000}};
struct ENetHost { int socket=0,intercept=0; } host_object;
struct ENetPacket { unsigned char* data=nullptr; size_t dataLength=0; };
struct ENetEvent { int type=0; ENetPeer* peer=nullptr; ENetPacket* packet=nullptr; };
constexpr int ENET_EVENT_TYPE_RECEIVE=1, ENET_EVENT_TYPE_DISCONNECT=2, ENET_EVENT_TYPE_CONNECT=3;
int yg_enet_is_socket(int) { return 0; } bool yg_enet_enabled() { return true; }
void enet_packet_destroy(ENetPacket*) {}
void enet_peer_reset(ENetPeer*) { ++resets; }
ENetPeer* enet_host_connect(ENetHost*,const ENetAddress* a,int,int) {
 ++connects;fallback_at=clock_ms; CHECK(a->host==public_host() && a->port==42000); return &pub;
}
int enet_host_service(ENetHost*,ENetEvent* e,int ms) {
 clock_ms+=ms;
 if (scenario==0 && clock_ms==500) {e->type=ENET_EVENT_TYPE_CONNECT;e->peer=&lan;return 1;}
 if (scenario==1 && clock_ms==2500) {e->type=ENET_EVENT_TYPE_CONNECT;e->peer=&pub;return 1;}
 if (scenario==3 && clock_ms==2500) {e->type=ENET_EVENT_TYPE_CONNECT;e->peer=&lan;return 1;}
 if (scenario==3 && clock_ms==3000) {e->type=ENET_EVENT_TYPE_CONNECT;e->peer=&pub;return 1;}
 return 0;
}
namespace host { void log(const char*,...) {} }
namespace slippi {
uint64_t time_ms() {return clock_ms;}
constexpr int intercept_callback=1;
struct Packet {Packet(unsigned char*,size_t) {}};
struct ActiveConnectionInfo {int player_idx=0;};
unsigned peer_key(ENetPeer* p) {return p->address.host;}
class NetplayClient {
 public:
 enum class ConnectStatus { INITIATED,CONNECTED,FAILED };
 std::atomic<ConnectStatus> status_{ConnectStatus::INITIATED};
 std::atomic<bool> do_loop_{true};
 std::atomic<uint64_t> connected_at_ms_{0};
 int remote_player_count_=1,builds=0;
 ENetHost* client_=&host_object;
 std::vector<ENetPeer*> server_{&lan};
 std::vector<std::string> public_fallbacks_{"203.0.113.2:42000"};
 std::vector<int> failed_connections_;
 std::map<unsigned,std::map<ENetPeer*,ActiveConnectionInfo>> active_connections_;
 std::atomic<bool> player_active_[4]{};
 struct {struct {int player_idx=1;}remote[3];}match_info_;
 void* own_udp_ingress_=nullptr;
 void OnData(Packet&,ENetPeer*) {std::abort();}
 void SendBuild() {++builds;}
 void ThreadFunc();
};
''' + loop + r'''
}
int main() {
 using namespace slippi;
 for(scenario=0;scenario<4;++scenario) {
   clock_ms=fallback_at=0;resets=connects=0;NetplayClient client;
   client.ThreadFunc();
   if(scenario==0) {
     CHECK(client.status_==NetplayClient::ConnectStatus::CONNECTED);
     CHECK(connects==0 && resets==0 && clock_ms==500 && client.server_[0]==&lan);
   } else {
     CHECK(connects==1 && fallback_at==2000 && client.server_.size()==1);
     CHECK(resets==(scenario==3 ? 2 : 1));
     if(scenario==2) {
       CHECK(client.status_==NetplayClient::ConnectStatus::FAILED && clock_ms==8000);
       CHECK(client.failed_connections_==std::vector<int>{0} && client.builds==0);
     } else {
       CHECK(client.status_==NetplayClient::ConnectStatus::CONNECTED && client.server_[0]==&pub);
       CHECK(client.active_connections_[public_host()][&pub].player_idx==1 && client.builds==1);
     }
   }
 }
 printf("production fallback loop: %d checks passed\n",checks);
}
''')

if __name__ == '__main__': unittest.main()
