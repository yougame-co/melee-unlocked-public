/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 * Production packet writers and receive cases are supplied unchanged by the
 * Python harness. Only ENet delivery, logging and local configuration are fake.
 */
#include "slippi_netplay_receive.h"
#include "slippi_port_inputs.h"
#include "native_slippi_bridge.h"
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#include "yougame_net_test.h"
#include "squad_costume_rule.h"
#include "yougame_lobby_setup.h"
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static std::vector<std::string> logs;
static bool early, suppress_build;
static std::string digest;
static uint32_t shield;
namespace host {
void log(const char* fmt, ...) {
  char bytes[512]; va_list args; va_start(args,fmt);
  std::vsnprintf(bytes,sizeof bytes,fmt,args); va_end(args); logs.emplace_back(bytes);
}
namespace test_env { const char* getenv(const char*) { return suppress_build ? "1" : nullptr; } }
}
namespace gx { uint32_t shield_color_online() { return shield; } }
namespace slippi::online {
struct LocalBuild { bool mod_view = false, static_engine = false; std::string fingerprint, name; } build;
const LocalBuild& local_build() { return build; }
bool early_inputs_accept() { return early; }
int session_mode() { return 1; }
std::string ranked_rules_digest(int) { return digest; }
}
using namespace slippi;

using ENetPeer = _ENetPeer;
class Capture {
 public:
  explicit Capture(uint8_t port, bool row, MuSquadSide own, bool friend_room = false) : player_idx_(port) {
    squad_search_ = row; squad_local_ = own; squad_friend_room_ = friend_room;
    port_inputs_->main = port;
    match_info_.remote[0].player_idx = (uint8_t)(1 - port);
    active_connections_[0][peer] = {(uint8_t)(1-port),false};
  }
  int SenderPort(_ENetPeer* peer) const;
  void SendBuild();
  void WriteSelections(Packet&, const PlayerSelections&);
  std::unique_ptr<PlayerSelections> ReadSelections(Packet&);
  void SetMatchSelections(PlayerSelections&);
  void SetSquadSelection(const MuSquadSide&);
  bool SquadSetup(MuSquadSetup*);
  void Deliver(const std::vector<uint8_t>& bytes);
  void SendAsync(std::unique_ptr<Packet> p) { queue.emplace_back(p->data(), p->data() + p->size()); }
  using DisconnectReason = NetplayClient::DisconnectReason;
  using ConnectStatus = NetplayClient::ConnectStatus;
  std::atomic<bool> do_loop_{true};
  std::atomic<ConnectStatus> status_{ConnectStatus::CONNECTED};
  uint32_t disconnect_reason = 0;
  void CancelSquad(const char*);
  void SquadFriendRefuse();
  void RefuseSquad(const char*, const char*);
  bool SquadPeerDisconnect(uint32_t);
  void ForceDisconnect(DisconnectReason reason) { disconnected = true; disconnect_reason = (uint32_t)reason; }
  void SendPrematchPing() {}
  void SendLockedPickPreview() {}   // pick previews are display only and tested in pick_preview_test.py
  uint8_t PlayerIdxFromPort(uint8_t port) const { return port > player_idx_ ? port - 1 : port; }
  uint8_t player_idx_, remote_player_count_ = 1;
  bool disconnected = false, has_game_started_ = false;
  std::array<bool, REMOTE_PLAYER_MAX> remote_game_started_{};
  std::atomic<bool> squad_search_{false};
  bool squad_normal_logged_ = false, squad_friend_room_ = false;
  std::atomic<bool> squad_advertised_{false}, squad_peer_cap_{false}, squad_cancelled_{false}, squad_adopted_{false};
  bool SquadAdopted() const { return squad_adopted_.load(); }
  // Declared exactly as slippi_net.h declares them (friend rooms).
  std::atomic<bool> squad_peer_build_{false}, squad_peer_friend_cap_{false}, squad_locked_seen_{false},
      squad_refused_{false};
  bool SquadFriendWaiting() const { return squad_friend_room_ && !squad_peer_build_.load() && !squad_search_.load(); }
#ifdef YOUGAME_AGENT_TEST_HOOK
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  bool squad_test_late_lineup_ = false;
#endif
  MuSquadSide squad_local_{}, squad_transmitted_{}, squad_peer_{}, squad_incoming_{};
  bool squad_peer_pending_ = false, squad_peer_selected_ = false;
  bool squad_peer_other_version_ = false;
  // Declared exactly as slippi_net.h declares it: what the player is told on a refused squad match.
  std::atomic<const char*> squad_cancel_text_{"Squad Strike setup didn't match. Match not started."};
  const char* SquadCancelText() const { return squad_cancel_text_.load(); }
  std::mutex selection_mutex_, timing_mutex_, pad_mutex_, build_mutex_;
  MatchInfo match_info_;
  NetplayClient::RemoteBuild remote_build_[REMOTE_PLAYER_MAX];
  std::unique_ptr<PortInputs> port_inputs_ = std::make_unique<PortInputs>();
  host::early_input::Buffer early_pads_[REMOTE_PLAYER_MAX];
  struct Connection { uint8_t player_idx; bool is_disconnected; };
  _ENetPeer* peer = reinterpret_cast<_ENetPeer*>(1);
  std::map<int, std::map<_ENetPeer*,Connection>> active_connections_;
  bool machine_pad_format_ = false;
  std::vector<std::vector<uint8_t>> queue;
};
// Hashing itself is tested elsewhere; this fixture tests the command transaction.
namespace source_port::mods {
struct Sha256 {
  void update(const std::string&) {}
  void update(const void*, size_t) {}
  std::string hex() { return std::string(64, 'a'); }
};
}
Capture* g_netplay = nullptr;
bool g_squad_row;
bool g_squad_friend_intent = false;
std::string g_squad_friend_room;
MuSquadSide g_squad_side{};
std::string g_squad_budget;
std::unique_ptr<source_port::mods::Sha256> g_squad_budget_hash;
slippi::squad_costume::Budgets g_squad_costumes;
slippi::squad_costume::Standard (*g_squad_standard)(const std::string&) = nullptr;
bool g_squad_costume_match = false;
std::unordered_map<std::string, slippi::squad_costume::Standard> g_squad_match_standards;
std::unordered_set<std::string> g_squad_fallback_logged;
slippi::squad_costume::Console g_squad_console;
std::string g_squad_console_over;
std::atomic<bool> g_squad_console_reset{false};
// The host's copy finder: PlFxGr.dat on the disc at 362982 bytes, no English twin (as on the
// vanilla disc); over_path's copy is over its size, a mod profile's when over_mod.
static std::string over_path;
static bool over_mod;
static slippi::squad_costume::Standard standard_copy(const std::string& path) {
  if (path == over_path) return {{7, 900000}, over_mod};
  if (path == "/plfxgr.dat") return {{3, 362982}, false};
  return {};
}
using slippi::online::command_payload_valid;
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
// slippi_online.cpp's state for the extracted first lock-in after connection.
struct { bool squad = false; std::string friend_room; Matchmaking::OnlinePlayMode mode = Matchmaking::DIRECT; } g_last_search;
// The Friends row state squad_command's "Play a friend" asks for (yougame_friends.h in the game).
namespace yougame::friends {
struct { bool entry = false; int requests = 0;
  void request_host() { requests += entry; }
  bool friend_entry() const { return entry; } } events;
}
PlayerSelections g_local_selections;
// slippi_online.cpp's "a Slippi session is searching, connecting or playing".
static bool g_session_up = false;
static bool slippi_session_up() { return g_session_up; }
struct { bool armed = false; } g_friends_lobby;
namespace yougame {
struct FixtureLobby { bool open = false; std::shared_ptr<lobby::Plan> plan; };
static FixtureLobby fixture_lobby;
static FixtureLobby friends_lobby() { return fixture_lobby; }
static int refused;
static void lobby_refuse_setup() { ++refused; }

}
static std::string g_forced_error;
static int cleanups;
static void cleanup_connection() { ++cleanups; g_netplay = nullptr; }
#include "squad_transport_methods.inc"

static MuSquadSide side(unsigned n, unsigned kind) {
  MuSquadSide result{}; result.count = (uint8_t)n;
  for (unsigned i=0;i<n;++i) result.members[i] = {(uint8_t)((kind+i)%26),(uint8_t)(i%4)};
  return result;
}
static PlayerSelections pick() {
  PlayerSelections p; p.character_id=2; p.character_color=1;
  p.is_character_selected=p.is_stage_selected=true; p.stage_id=31; p.rng_offset=0x1234;
  return p;
}
static void send(Capture& a, Capture& b) {
  for (const auto& bytes : a.queue) b.Deliver(bytes);
  a.queue.clear();
  if (a.disconnected) b.SquadPeerDisconnect(a.disconnect_reason);
}
static constexpr const char* kNoCap = "Your opponent's game can't play Squad Strike. Match not started.";
static constexpr const char* kNotPicked = "Your opponent picked Squad Strike and you didn't. Match not started.";
static constexpr const char* kNoLineup = "Your opponent hasn't picked 4 fighters. Match not started.";
static constexpr const char* kOtherVersion = "Your opponent's game has a different Squad Strike version. Update both games.";
static constexpr const char* kGeneric = "Squad Strike setup didn't match. Match not started.";
static bool told(const Capture& c, const char* text) { return !std::strcmp(c.SquadCancelText(), text); }
static bool logged(const char* text) {
  for (const auto& line : logs) if (line == text) return true;
  return false;
}
static bool command(const uint8_t* p, unsigned n, std::vector<uint8_t>& reply) {
  uint8_t response[64]{};
  uint32_t size = 0;
  const auto status = online::native_command(MU_SQUAD_COMMAND, p, n, response,
      sizeof response, &size, 1, true,
      [](uint8_t, const uint8_t* bytes, uint32_t length, std::vector<uint8_t>& out) {
        return squad_command(bytes, length, out);
      });
  reply.assign(response, response + size);
  return status == online::NATIVE_COMMAND_OK;
}

int main() {
  // Complete production MU_BUILD bytes, every legacy caps/digest/shield combination.
  for (unsigned caps=0;caps<16;++caps) for (bool mod : {false,true}) for (bool has_shield : {false,true}) {
    early=(caps&1)!=0; online::build.static_engine=(caps&4)!=0;
    digest=(caps&8)?std::string(64,'a'):"";
    online::build.mod_view=mod; online::build.fingerprint=std::string(64,'b'); online::build.name="Synthetic";
    shield=has_shield?0x1001234:0;
    for (bool row : {false,true}) {
      Capture client(0,row,side(4,14)); client.SendBuild(); assert(client.queue.size()==1);
      std::vector<uint8_t> expected{0xE0,'M','U','B',1,0,(uint8_t)mod,(uint8_t)(mod?64:0)};
      if(mod) expected.insert(expected.end(),64,'b');
      expected.push_back((uint8_t)(mod?9:0));
      if(mod) expected.insert(expected.end(),{'S','y','n','t','h','e','t','i','c'});
      expected.push_back((uint8_t)((caps|2|MU_PICK_PREVIEW_CAP|MU_SETUP_HASH_CAP)|(row?MU_SQUAD_CAP:0)));
      if(caps&8) expected.insert(expected.end(),64,'a');
      expected.push_back(has_shield?1:0);
      if(has_shield) expected.insert(expected.end(),{0,0x12,0x34});
      expected.push_back(MU_BUILD_CAPS2_FRIENDS_LOBBY | MU_BUILD_CAPS2_LOCAL_PARTNER | MU_BUILD_CAPS2_GONE_OWN_FRAMES);   // the second capability byte trails the shield
      assert(client.queue[0]==expected);
    }
  }
  online::build={}; early=false; digest.clear(); shield=0;
  // The second capability byte arrives; a build from before it (no trailing byte) reads as 0.
  for (bool old : {false,true}) for (bool has_shield : {false,true}) {
    shield=has_shield?0x1001234:0;
    Capture a(0,false,side(4,14)),b(1,false,side(4,2)); a.SendBuild();
    std::vector<uint8_t> bytes=a.queue[0]; if(old) bytes.pop_back();
    b.Deliver(bytes);
    int seen=0;
    for (const auto& rb : b.remote_build_) if (rb.received) {
      ++seen; assert(rb.caps2==(old?0:(MU_BUILD_CAPS2_FRIENDS_LOBBY|MU_BUILD_CAPS2_LOCAL_PARTNER|MU_BUILD_CAPS2_GONE_OWN_FRAMES))); assert(rb.setup_hash==!old);
      assert(mu_gone_own_frames_capable(rb.caps2)==!old);   // a peer that stops before the byte reads as absent
      assert(rb.shield_rgb==(has_shield?0x1001234u:0u));
    }
    assert(seen==1);
  }
  // With machine pads a connection speaks only for its own player: a build naming another port is dropped.
  {
    Capture a(0,false,side(4,14)),b(1,false,side(4,2)); a.SendBuild();
    b.machine_pad_format_=true;
    b.active_connections_[0][b.peer].player_idx=2;
    b.Deliver(a.queue[0]);
    for (const auto& rb : b.remote_build_) assert(!rb.received);
  }
  // Without them the slot can be a neighbour's when two players share a public address: the build still counts.
  {
    Capture a(0,false,side(4,14)),b(1,false,side(4,2)); a.SendBuild();
    b.active_connections_[0][b.peer].player_idx=2;
    b.Deliver(a.queue[0]);
    int seen=0; for (const auto& rb : b.remote_build_) seen+=rb.received;
    assert(seen==1);
  }
  shield=0;
  for (bool a_cap : {false,true}) for (bool b_cap : {false,true}) for (bool reverse : {false,true}) {
    Capture a(0,a_cap,side(4,14)),b(1,b_cap,side(4,2));
    a.SendBuild(); b.SendBuild(); send(a,b);send(b,a);
    auto p=pick(); a.SetMatchSelections(p); b.SetMatchSelections(p);
    assert(a.queue.size()==(a_cap?2u:1u) && b.queue.size()==(b_cap?2u:1u));
    for (auto* c : {&a,&b}) if(c->squad_search_) assert(c->queue[0][0]==0xE3 && c->queue[1][0]==0x82);
    // The normal 0x82 is exactly the pre-squad wire, including rng and alternate-stage byte.
    const std::vector<uint8_t> expected{0x82,2,1,1,0,0,31,1,0,0,0x12,0x34,0,0};
    assert(a.queue.back()==expected);
    if(reverse) {send(b,a);send(a,b);} else {send(a,b);send(b,a);}
    MuSquadSetup x{},y{};
    assert(a.SquadSetup(&x)==(a_cap&&b_cap)); assert(b.SquadSetup(&y)==(a_cap&&b_cap));
    // Never a silent 1v1: a squad seat facing a seat without the cap refuses the match on
    // both sides, whichever 0x82 lands first, and each side is told why.
    const bool mixed = a_cap != b_cap;
    assert(a.disconnected == mixed && b.disconnected == mixed);
    assert(a.squad_cancelled_ == mixed && b.squad_cancelled_ == mixed);
    if(mixed) {
      Capture& squad = a_cap ? a : b; Capture& normal = a_cap ? b : a;
      assert(told(squad, kNoCap) && told(normal, kNotPicked));
      assert(!normal.squad_peer_selected_ && !squad.squad_peer_selected_);
    } else {
      assert(told(a, kGeneric) && told(b, kGeneric));
    }
    if(a_cap&&b_cap) {
      assert(!std::memcmp(&x,&y,sizeof x)); assert(x.stocks==4);   // one stock per fighter
      assert(x.sides[0].count==4 && x.sides[1].count==4);
      assert(x.sides[0].members[0].kind==14 && x.sides[1].members[0].kind==2);
      // Fewer than four fighters on the CSS (three included) is no lineup at all.
      a.SetSquadSelection(side(3,9)); assert(!mu_squad_side_valid(&a.squad_local_));
      a.SetSquadSelection(side(2,9)); assert(!mu_squad_side_valid(&a.squad_local_));
      // Local CSS state changes cannot alter the setup whose bytes were sent.
      a.SetSquadSelection(side(4,9)); MuSquadSetup frozen{};
      assert(a.SquadSetup(&frozen) && !std::memcmp(&x,&frozen,sizeof x));
      // A next game has fresh E3 then 82, even when the character selection repeats.
      a.SetMatchSelections(p); send(a,b);
      assert(b.SquadSetup(&y) && y.sides[0].count==4 && y.sides[0].members[0].kind==9);
      // A bare subsequent 82 may never reuse the previous game's valid E3.
      b.Deliver(expected); assert(b.disconnected && b.squad_cancelled_);
    }
  }
  for (int bad : {0,1,2}) { // 0 no lineup, 1 a malformed one, 2 another squad version's
    Capture a(0,true,side(4,14)),b(1,true,side(4,2));
    a.SendBuild();b.SendBuild();send(a,b);send(b,a);
    auto p=pick(); a.SetMatchSelections(p);b.SetMatchSelections(p);
    if(bad==1) { a.queue[0][2]=99; b.Deliver(a.queue[0]); a.queue[0][2]=4; }
    if(bad==2) { a.queue[0][1]=99; b.Deliver(a.queue[0]); a.queue[0][1]=MU_SQUAD_VERSION; }
    b.Deliver(a.queue[1]); MuSquadSetup x{};
    assert(b.disconnected && b.squad_cancelled_ && !b.SquadSetup(&x));
    assert(told(b, bad==2 ? kOtherVersion : kNoLineup));
    send(b,a); assert(a.disconnected && a.squad_cancelled_);
    assert(told(a, kGeneric)); // the peer's cancel: the setup did not match
    assert(logged("squad: peer cancelled protocol error, match cancelled"));
    assert(logged(bad==2 ? "squad: peer lineup is another squad version, match cancelled"
                         : "squad: missing lineup before selections, match cancelled"));
    b.Deliver(a.queue[0]); assert(!b.SquadSetup(&x)); // late E3 cannot undo cancellation
  }
  assert(logged("squad: malformed lineup rejected"));
  assert(logged("squad: peer has no squad cap, match cancelled"));
  assert(logged("squad: peer sent a lineup to a normal match, match cancelled"));
  assert(!logged("squad: peer has no squad cap, normal 1v1"));
  {
    // A version 1 lineup (or any other count) from a squad peer is malformed, never a squad.
    Capture a(0,true,side(4,14)),b(1,true,side(4,2));
    a.SendBuild();b.SendBuild();send(a,b);send(b,a);
    auto p=pick(); a.SetMatchSelections(p);
    for (const std::vector<uint8_t>& old : {
        std::vector<uint8_t>{0xE3,1,3, 3,14,0,15,0,16,0, 3,2,0,3,0,4,0},   // v1 header
        std::vector<uint8_t>{0xE3,2,3, 3,14,0,15,0,16,0, 3,2,0,3,0,4,0},   // v2: three each
        std::vector<uint8_t>{0xE3,3,3, 3,14,0,15,0,16,0, 3,2,0,3,0,4,0},   // three each at v3
        std::vector<uint8_t>{0xE3,3,1, 1,14,0, 1,2,0},                      // v1 1v1 shape
        std::vector<uint8_t>{0xE3,3,5, 5,14,0,15,0,16,0,17,0,18,0, 3,2,0,3,0,4,0}}) { // 5 + 3
      logs.clear(); b.Deliver(old);
      // Another MU_SQUAD_VERSION is named as such; a bad shape at this version is malformed.
      assert(logged(old[1] == MU_SQUAD_VERSION ? "squad: malformed lineup rejected"
                                               : "squad: lineup from another squad version rejected"));
      assert(!b.squad_peer_pending_ && b.squad_peer_other_version_ == (old[1] != MU_SQUAD_VERSION));
    }
    b.Deliver(a.queue[1]); MuSquadSetup x{};
    assert(b.squad_cancelled_ && !b.SquadSetup(&x) && told(b, kNoLineup));
  }
  {
    // A mixed-version friend room: the peer's 3-fighter (v2) lineup is not a missing pick. Its
    // 0x82 cancels with "update both games", never "hasn't picked 4 fighters".
    logs.clear();
    Capture a(0,true,side(4,14)),b(1,true,side(4,2));
    a.SendBuild();b.SendBuild();send(a,b);send(b,a);
    auto p=pick(); a.SetMatchSelections(p);
    b.Deliver(std::vector<uint8_t>{0xE3,2,3, 3,14,0,15,0,16,0, 3,2,0,3,0,4,0});
    b.Deliver(a.queue[1]); MuSquadSetup x{};
    assert(b.squad_cancelled_ && !b.SquadSetup(&x) && told(b, kOtherVersion));
    assert(logged("squad: peer lineup is another squad version, match cancelled"));
  }
  {
    // The only normal 1v1 a capped peer can get: neither side sent a lineup for this 0x82.
    logs.clear();
    Capture a(0,true,side(4,14)),b(1,false,side(4,2));
    a.SendBuild();b.SendBuild();send(a,b);send(b,a);
    const std::vector<uint8_t> bare{0x82,2,1,1,0,0,31,1,0,0,0x12,0x34,0,0};
    b.Deliver(bare); MuSquadSetup x{};
    assert(!b.disconnected && !b.squad_cancelled_ && !b.SquadSetup(&x));
    assert(logged("squad: neither side asked for squads, normal 1v1"));
  }
  // Reject match 2's lineup through the production ABI: match 1 cannot survive.
  std::vector<uint8_t> reply;
  const uint8_t choose[]{0,1}, good[]{1,4,2,0,3,1,4,2,5,0}, bad[]{1,4,2,0,26,0,4,2,5,0}, empty[]{1,0};
  assert(command(choose, sizeof choose, reply) && reply == std::vector<uint8_t>{1});
  assert(command(good, sizeof good, reply) && g_squad_side.count == 4);
  assert(!command(bad, sizeof bad, reply) && !mu_squad_side_valid(&g_squad_side));
  assert(command(good, sizeof good, reply));
  assert(!command(empty, sizeof empty, reply) && !mu_squad_side_valid(&g_squad_side));
  {
    // The budget transaction records the costume facts; only a standard copy over its size
    // refuses: 2 for the disc's own, 3 for a mod's. A twin the disc lacks (PlFxGr.usd) holds.
    const uint8_t floors[]{2,0,0xC9,0x68,0,0x01,0x10,0,0}, finish[]{4};
    const uint8_t costume[]{3,2,0,0,0,1,0,5,0x89,0xE6,'P','l','F','x','G','r','.','d','a','t',0};
    const uint8_t data[]{3,0,0,0,0,9,0,0,4,0,'P','l','F','x','.','d','a','t',0};
    g_squad_standard = standard_copy;
    assert(command(floors, sizeof floors, reply) && command(costume, sizeof costume, reply) &&
           command(data, sizeof data, reply));
    assert(g_squad_costumes.budget("/plfxgr.dat").bytes == 362982 && g_squad_costumes.budget("/plfxgr.usd").twin);
    assert(!g_squad_costumes.budget("/plfx.dat").bytes);
    assert(command(finish, sizeof finish, reply) && reply == std::vector<uint8_t>{1});
    over_path = "/plfxgr.usd";   // an English copy the disc has, over its size
    assert(command(finish, sizeof finish, reply) && reply == std::vector<uint8_t>{2});   // cached identity too
    assert(logged("squad: the disc's own /plfxgr.usd is over its budget, search refused"));
    over_mod = true;
    assert(command(finish, sizeof finish, reply) && reply == std::vector<uint8_t>{3});
    assert(logged("squad: the mod's /plfxgr.usd is over its budget, search refused"));
    over_path.clear(); over_mod = false;
    assert(command(finish, sizeof finish, reply) && reply == std::vector<uint8_t>{1});
    // Op 5 checks the copies again in the match's view, then arms the lookup with them.
    Capture a(0,true,side(4,14)), b(1,true,side(4,2));
    a.SendBuild(); b.SendBuild(); send(a,b); send(b,a);
    auto p=pick(); a.SetMatchSelections(p); b.SetMatchSelections(p); send(a,b); send(b,a);
    const uint8_t setup[]{5};
    g_netplay = &a;
    g_squad_fallback_logged.insert("/plfxgr.dat");
    assert(command(setup, sizeof setup, reply) && !reply.empty() && g_squad_costume_match);
    assert(g_squad_match_standards.size() == 2 && g_squad_match_standards["/plfxgr.dat"].file.entry == 3);
    assert(g_squad_match_standards["/plfxgr.usd"].file.entry == -1 && g_squad_fallback_logged.empty());
    // A new row choice disarms it; so does a normal 1v1 (no squad setup).
    assert(command(choose, sizeof choose, reply) && !g_squad_costume_match && g_squad_match_standards.empty());
    g_netplay = &b;
    assert(command(setup, sizeof setup, reply) && g_squad_costume_match);
    g_netplay = nullptr;
    assert(command(setup, sizeof setup, reply) && reply.empty() && !g_squad_costume_match);
    // A copy over its size in the match's view (it changed after the search): the match is
    // cancelled with the reason on screen, the game sets up nothing of the squad, nothing armed.
    over_path = "/plfxgr.dat"; over_mod = true;
    logs.clear();
    g_netplay = &a;
    assert(command(setup, sizeof setup, reply) && reply.empty() && !g_squad_costume_match);
    assert(a.disconnected && a.squad_cancelled_);
    assert(told(a, "This mod's costumes are too big for Squad Strike online. Match not started."));
    assert(logged("squad: the mod's /plfxgr.dat is over its budget, match cancelled"));
    send(a,b); assert(b.squad_cancelled_ && told(b, kGeneric));
    g_netplay = nullptr;
    // A frozen Friends Squad refuses through the same cleanup even without a netplay client.
    auto plan = std::make_shared<yougame::lobby::Plan>();
    plan->ok = true; plan->rules.squad = true; plan->ports = plan->humans = 2;
    for (int i=0;i<2;++i) {
      plan->port[i].kind = yougame::lobby::Kind::Human;
      for(int j=0;j<4;++j) plan->port[i].lineup.push_back({i*4+j,0,0});
    }
    yougame::fixture_lobby = {true, plan};
    const uint8_t mode[]{MU_SQUAD_OP_LOBBY_MODE};
    assert(command(mode, sizeof mode, reply) && reply == std::vector<uint8_t>{1});
    assert(command(setup, sizeof setup, reply) && reply.empty());
    assert(cleanups == 1 && yougame::refused == 1 && !g_forced_error.empty());   // parked, no result: the host calls the setup off
    plan->rules.squad = false;
    assert(command(mode, sizeof mode, reply) && reply == std::vector<uint8_t>{0});
    assert(command(setup, sizeof setup, reply) && reply.empty() && cleanups == 1);
    yougame::fixture_lobby = {};
    over_path.clear(); over_mod = false;
    // Op 12, Friends' synced select: the same copies, armed by the game while its squads are in
    // play. Facts first (an arm with none answers 0), then the copy for each path by entry.
    const uint8_t arm[]{12, MU_SQUAD_CONSOLE_ARM}, off[]{12, MU_SQUAD_CONSOLE_OFF};
    const uint8_t fact[]{12,3,2,0,0,0,1,0,5,0x89,0xE6,'P','l','F','x','G','r','.','d','a','t',0};
    slippi::squad_costume::Budget budget{};
    slippi::squad_costume::Standard copy{};
    assert(!g_squad_costume_match);
    assert(command(off, sizeof off, reply) && reply == std::vector<uint8_t>{1});
    assert(command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{0} && !g_squad_console.armed());
    assert(command(fact, sizeof fact, reply) && reply == std::vector<uint8_t>{1});
    assert(command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{1} && g_squad_console.armed());
    assert(g_squad_console.rule("/plfxgr.dat", &budget, &copy) && budget.bytes == 362982 && copy.file.entry == 3);
    assert(!g_squad_console.rule("/plfx.dat", &budget, &copy));
    // An online row choice leaves the console's rule alone (separate state).
    assert(command(choose, sizeof choose, reply) && g_squad_console.armed());
    // A standard copy over its size: nothing armed, the reason logged once, 2 or 3 by owner.
    assert(command(off, sizeof off, reply) && !g_squad_console.armed());
    assert(command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{0});   // facts cleared
    over_path = "/plfxgr.usd";
    logs.clear();
    assert(command(fact, sizeof fact, reply) && command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{2});
    assert(!g_squad_console.armed() && g_squad_console_over == "/plfxgr.usd");
    assert(logged("squad: the disc's own /plfxgr.usd is over its budget, Friends squads refused"));
    over_mod = true;
    assert(command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{3} && !g_squad_console.armed());
    over_path.clear(); over_mod = false;
    assert(command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{1} && g_squad_console.armed());
    assert(g_squad_console_over.empty());
    assert(command(off, sizeof off, reply) && !g_squad_console.armed());
    // A console session's start or end, or an in-process host reset (squad_console_reset), forgets
    // the armed rule, its over-size line and its fallback lines before the next op 12 runs: the
    // next arm answers 0 and the game sends its facts again.
    assert(command(fact, sizeof fact, reply) && command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{1});
    g_squad_console_over = "/plfxgr.usd";
    g_squad_fallback_logged.insert("/plfxgr.dat");
    g_squad_console_reset.store(true);
    assert(command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{0} && !g_squad_console.armed());
    assert(g_squad_console_over.empty() && g_squad_fallback_logged.empty() && !g_squad_console_reset.load());
    assert(command(fact, sizeof fact, reply) && command(arm, sizeof arm, reply) && reply == std::vector<uint8_t>{1});
    assert(command(off, sizeof off, reply) && !g_squad_console.armed());
    // Malformed op 12: unknown sub-op, a bare fact, a short fact.
    for (const std::vector<uint8_t>& bad12 : {std::vector<uint8_t>{12}, {12,2}, {12,3}, {12,3,2,0,0,0,1}})
      assert(!command(bad12.data(), (unsigned)bad12.size(), reply));
    g_squad_standard = nullptr;
  }
  for (const std::vector<uint8_t>& malformed : {
      std::vector<uint8_t>{1}, {1,4,2,0}, {1,5,2,0}, {1,1,2},
      // Exact-length lineups of the wrong size: one, two, three (handler), five and six (bounds).
      {1,1,2,0}, {1,2,2,0,3,1}, {1,3,2,0,3,1,4,2}, {1,5,2,0,3,1,4,2,5,0,6,0},
      {1,6,2,0,3,1,4,2,5,0,6,0,7,0},
      {1,4,2,0,3,1}, {1,4,2,0,3,1,4,2,5,0,6,0}}) {
    assert(command(good, sizeof good, reply));
    assert(!command(malformed.data(), (unsigned)malformed.size(), reply));
    assert(!mu_squad_side_valid(&g_squad_side) && reply.empty());
  }
  Capture invalid(0,true,side(4,0)), waiting(1,true,side(4,8));
  invalid.SendBuild(); waiting.SendBuild(); send(invalid,waiting); send(waiting,invalid);
  g_netplay = &invalid;
  assert(!command(bad, sizeof bad, reply));
  assert(!mu_squad_side_valid(&invalid.squad_local_));
  g_netplay = nullptr;
  auto bad_pick = pick();
  invalid.SetMatchSelections(bad_pick);
  assert(invalid.queue.empty() && invalid.disconnected && invalid.squad_cancelled_);
  send(invalid,waiting); assert(waiting.disconnected && waiting.squad_cancelled_);
  // Duplicate inbound/outbound and public-fallback identities all own the same port.
  for (int key : {0,7,99}) {
    Capture sender(0,true,side(4,1)),receiver(1,true,side(4,5));
    sender.SendBuild(); receiver.SendBuild(); send(sender,receiver); send(receiver,sender);
    auto selection=pick(); sender.SetMatchSelections(selection);
    receiver.peer = reinterpret_cast<_ENetPeer*>(uintptr_t(key+10));
    receiver.active_connections_[key][receiver.peer] = {0,false};
    send(sender,receiver);
    assert(receiver.squad_peer_selected_ && !receiver.disconnected);
  }
  // A test-suppressed build (like Slippi Dolphin) did NOT advertise a cap. The squad seat facing
  // it refuses the match and says why: never a silent 1v1, and both sides end up cancelled.
  Capture a(0,true,side(4,0)),b(1,true,side(4,8));
  suppress_build=true;a.SendBuild();suppress_build=false;b.SendBuild();send(b,a);
  auto p=pick();a.SetMatchSelections(p);b.SetMatchSelections(p);
  assert(a.queue.size()==1 && a.queue[0][0]==0x82); // nothing advertised, no lineup sent
  send(a,b);send(b,a);
  MuSquadSetup x{};assert(!a.SquadSetup(&x)&&!b.SquadSetup(&x));
  assert(b.disconnected && b.squad_cancelled_ && told(b, kNoCap));
  assert(a.disconnected && a.squad_cancelled_);
  // Rematch race: the peer's E3+82 lands while this seat's lineup is between commands (empty).
  {
    Capture a(0,true,side(4,1)),b(1,true,side(4,5));
    a.SendBuild();b.SendBuild();send(a,b);send(b,a);
    a.SetSquadSelection(MuSquadSide{});
    auto q=pick(); b.SetMatchSelections(q); send(b,a);
    assert(!a.disconnected && !a.squad_cancelled_ && a.squad_peer_selected_);
    a.SetSquadSelection(side(4,1)); a.SetMatchSelections(q); send(a,b);
    MuSquadSetup x{},y{}; assert(a.SquadSetup(&x)&&b.SquadSetup(&y)&&!std::memcmp(&x,&y,sizeof x));
  }
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  // The client is built with the search lineup. A lineup command between search and connection
  // reaches only g_squad_side (no client yet); the first lock-in after connection refreshes it.
  g_last_search.squad = true;
  g_local_selections = pick();
  {
    // Rejected after search, before connection: the first lock-in cancels, nothing is sent.
    logs.clear();
    g_netplay = nullptr;
    assert(command(good, sizeof good, reply) && g_squad_side.count == 4);
    Capture a(0,true,g_squad_side), b(1,true,side(4,8));
    a.SendBuild(); b.SendBuild(); send(a,b); send(b,a);
    assert(!command(bad, sizeof bad, reply) && !mu_squad_side_valid(&g_squad_side));
    assert(mu_squad_side_valid(&a.squad_local_));   // the client still holds the search lineup
    g_netplay = &a; first_lock_in(); g_netplay = nullptr;
    assert(a.queue.empty() && a.disconnected && a.squad_cancelled_);
    assert(logged("squad: invalid outgoing lineup, match cancelled"));
    assert(logged("squad: cancel: Squad Strike protocol error"));
    send(a,b); MuSquadSetup setup{};
    assert(b.disconnected && b.squad_cancelled_ && !b.SquadSetup(&setup));
    assert(logged("squad: peer cancelled protocol error, match cancelled"));
  }
  {
    // Unchanged lineup: the refresh is a no-op, the lock-in sends 0xE3 then 0x82 as before.
    logs.clear();
    assert(command(good, sizeof good, reply) && g_squad_side.count == 4);
    Capture a(0,true,g_squad_side), b(1,true,side(4,8));
    a.SendBuild(); b.SendBuild(); send(a,b); send(b,a);
    g_netplay = &a; first_lock_in(); g_netplay = nullptr;
    assert(a.queue.size() == 2 && a.queue[0][0] == 0xE3 && a.queue[1][0] == 0x82);
    assert(!a.disconnected && !a.squad_cancelled_);
    auto q = pick(); b.SetMatchSelections(q); send(a,b); send(b,a);
    MuSquadSetup x{}, y{};
    assert(a.SquadSetup(&x) && b.SquadSetup(&y) && !std::memcmp(&x,&y,sizeof x));
    assert(x.sides[0].count == 4 && x.sides[0].members[0].kind == 2 && x.sides[0].members[1].kind == 3 &&
           x.sides[0].members[2].kind == 4 && x.sides[0].members[3].kind == 5);
    for (const auto& line : logs) assert(line.find("cancel") == std::string::npos);
  }
  {
    // A lineup CHANGED (still valid) before connection is the one sent, not the search lineup.
    assert(command(good, sizeof good, reply));
    Capture a(0,true,g_squad_side), b(1,true,side(4,8));
    a.SendBuild(); b.SendBuild(); send(a,b); send(b,a);
    const uint8_t other[]{1,4,9,0,10,1,11,2,12,0};
    assert(command(other, sizeof other, reply) && g_squad_side.count == 4);
    g_netplay = &a; first_lock_in(); g_netplay = nullptr;
    assert(a.queue.size() == 2 && a.queue[0][0] == 0xE3);
    send(a,b); auto q = pick(); b.SetMatchSelections(q); send(b,a);
    MuSquadSetup x{}; assert(b.SquadSetup(&x) && x.sides[0].count == 4 && x.sides[0].members[0].kind == 9);
  }
  g_last_search.squad = false;
  // Friend rooms (Direct): the friend picked Squad Strike, this game joins it from their MU_BUILD.
  static constexpr const char* kFriendNoSquad = "Your friend's game can't play Squad Strike. Match not started.";
  const auto caps_of = [](const std::vector<uint8_t>& build) { return build[8 + build[7] + 1 + build[8 + build[7]]]; };
  {
    logs.clear();
    Capture a(0,true,side(4,14),true), b(1,false,MuSquadSide{},true);
    a.SendBuild(); b.SendBuild();          // a squad seat sends before connecting, b after
    assert(caps_of(a.queue[0]) & MU_SQUAD_CAP && caps_of(a.queue[0]) & MU_SQUAD_FRIEND_CAP);
    assert(!(caps_of(b.queue[0]) & MU_SQUAD_CAP) && caps_of(b.queue[0]) & MU_SQUAD_FRIEND_CAP);
    send(b,a);                             // b's first build: it can join, so a waits
    assert(!a.disconnected && !a.squad_cancelled_ && !a.squad_peer_cap_ && !a.SquadFriendWaiting());
    // Until a's build is here, b's game may not lock in: the host answers "waiting".
    g_last_search.friend_room = "room";
    const uint8_t ask[]{MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_QUERY};
    assert(b.SquadFriendWaiting());
    g_netplay = &b; assert(command(ask, sizeof ask, reply) && reply == std::vector<uint8_t>{MU_SQUAD_ROOM_WAITING});
    g_netplay = nullptr;
    send(a,b);                             // a's build: b joins and says so in a second build
    assert(!b.SquadFriendWaiting());
    assert(b.squad_search_ && b.SquadAdopted() && b.squad_advertised_ && b.queue.size() == 1);
    assert(caps_of(b.queue[0]) & MU_SQUAD_CAP && caps_of(b.queue[0]) & MU_SQUAD_FRIEND_CAP);
    assert(logged("squad: friend picked Squad Strike, joining it"));
    send(b,a); assert(a.squad_peer_cap_ && !a.SquadAdopted());
    // The host tells only the joining side's character select, and only in a friend room.
    g_netplay = &a; assert(command(ask, sizeof ask, reply) && reply == std::vector<uint8_t>{0});
    assert(!g_last_search.squad);
    g_netplay = &b; assert(command(ask, sizeof ask, reply) && reply == std::vector<uint8_t>{1});
    assert(g_last_search.squad && logged("squad: friend room squad mode (friend's pick), character select told"));
    g_last_search.friend_room.clear(); g_last_search.squad = false;
    assert(command(ask, sizeof ask, reply) && reply == std::vector<uint8_t>{0});
    g_netplay = nullptr;
    // Each side picks 4 and locks in: lineups go as 0xE3 before each 0x82, the setups agree.
    auto q = pick(); a.SetMatchSelections(q);
    b.SetSquadSelection(side(4,2)); b.SetMatchSelections(q);
    assert(b.queue.size() == 2 && b.queue[0][0] == 0xE3 && b.queue[1][0] == 0x82);
    send(a,b); send(b,a);
    MuSquadSetup x{}, y{};
    assert(a.SquadSetup(&x) && b.SquadSetup(&y) && !std::memcmp(&x,&y,sizeof x));
    assert(x.sides[0].members[0].kind == 14 && x.sides[1].members[0].kind == 2 && x.stocks == 4);
    assert(!a.disconnected && !b.disconnected);
  }
  {
    // A lock-in sent before the joining side's lineup reached the client: cancelled, never a 1v1.
    Capture a(0,true,side(4,14),true), b(1,false,MuSquadSide{},true);
    a.SendBuild(); b.SendBuild(); send(b,a); send(a,b); send(b,a);
    auto q = pick(); b.SetMatchSelections(q);
    assert(b.queue.empty() && b.disconnected && b.squad_cancelled_);
    send(b,a); assert(a.disconnected && a.squad_cancelled_);
  }
  {
    // The friend's game can neither play nor join Squad Strike: refused at its build, before picks.
    logs.clear();
    Capture a(0,true,side(4,14),true), b(1,false,MuSquadSide{});
    a.SendBuild(); b.SendBuild(); send(b,a);
    assert(a.disconnected && a.squad_cancelled_ && told(a, kFriendNoSquad));
    assert(logged("squad: friend's game has no friend Squad Strike, match cancelled"));
    send(a,b); assert(!b.squad_search_ && !b.SquadAdopted());
  }
  {
    // Neither friend picked Squad Strike: nobody joins anything, a normal friend match.
    logs.clear();
    Capture a(0,false,MuSquadSide{},true), b(1,false,MuSquadSide{},true);
    a.SendBuild(); b.SendBuild(); send(a,b); send(b,a);
    assert(!a.squad_search_ && !b.squad_search_ && !a.SquadAdopted() && !b.SquadAdopted());
    auto q = pick(); a.SetMatchSelections(q); b.SetMatchSelections(q);
    assert(a.queue.size() == 1 && b.queue.size() == 1);
    send(a,b); send(b,a);
    MuSquadSetup none{};
    assert(!a.disconnected && !b.disconnected && !a.squad_cancelled_ && !b.squad_cancelled_);
    assert(!a.SquadSetup(&none) && !b.SquadSetup(&none));
  }
  {
    // Unranked (no friend room) never joins: the existing on-screen refusal still applies.
    Capture a(0,true,side(4,14)), b(1,false,side(4,2));
    a.SendBuild(); b.SendBuild(); send(a,b);
    assert(!b.squad_search_ && !b.SquadAdopted() && !(caps_of(b.queue[0]) & MU_SQUAD_FRIEND_CAP));
  }
  {
    // "Play a friend": the host opens the friend picker only while the Friends row is selected,
    // and only then carries the intent (INTENT answers 1 until the picker ends).
    const uint8_t invite[]{MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_INVITE}, past[]{MU_SQUAD_OP_FRIEND, 4};
    const uint8_t intent[]{MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_INTENT};
    g_last_search = {};
    yougame::friends::events.entry = true;
    assert(command(invite, sizeof invite, reply) && reply == std::vector<uint8_t>{1});
    assert(yougame::friends::events.requests == 1 && logged("squad: play a friend, opening the friend picker"));
    assert(g_squad_friend_intent && command(intent, sizeof intent, reply) && reply == std::vector<uint8_t>{1});
    yougame::friends::events.entry = false;
    assert(command(invite, sizeof invite, reply) && reply == std::vector<uint8_t>{0});
    assert(yougame::friends::events.requests == 1 && !g_squad_friend_intent);
    assert(command(intent, sizeof intent, reply) && reply == std::vector<uint8_t>{0});
    // The last Squad Strike friend room's search outlives it: it is never an intent (a cancelled
    // "Play a friend" back at the menu turns Squad Strike off).
    g_last_search.friend_room = "room"; g_last_search.squad = true;
    assert(command(intent, sizeof intent, reply) && reply == std::vector<uint8_t>{0});
    // The picked room's search took g_squad_friend_room and runs while the menu is still drawn:
    // INTENT stays 1 so the menu keeps Squad Strike on (the live 72 inviter fell to a plain CSS).
    g_session_up = true;
    assert(command(intent, sizeof intent, reply) && reply == std::vector<uint8_t>{1});
    g_last_search.squad = false;
    assert(command(intent, sizeof intent, reply) && reply == std::vector<uint8_t>{0});
    g_last_search.squad = true; g_last_search.mode = Matchmaking::UNRANKED;
    assert(command(intent, sizeof intent, reply) && reply == std::vector<uint8_t>{0});
    g_session_up = false;
    g_last_search = {};
    assert(!command(past, sizeof past, reply));
  }
  {
    // The reviewer's race: the joiner locks in before the inviter's build reached it. The host
    // holds that lock-in (SquadFriendWaiting), so it never goes out plain; if one did anyway
    // (an older game), the late build cancels the room instead of adopting into a mixed match.
    logs.clear();
    Capture a(0,true,side(4,14),true), b(1,false,MuSquadSide{},true);
    a.SendBuild(); b.SendBuild();
    send(b,a);                             // b's build (friend cap) reaches a
    assert(b.SquadFriendWaiting() && !a.SquadFriendWaiting());
    auto q = pick(); b.SetMatchSelections(q);   // forced past the hold: a plain locked 0x82
    assert(b.queue.size() == 1 && b.queue[0][0] == 0x82 && b.squad_locked_seen_);
    send(b,a);                             // a judges it: friend-specific, never "can't play"
    assert(a.squad_cancelled_ && a.disconnected);
    assert(told(a, "Your friend locked in before their game switched to Squad Strike. Match not started."));
    Capture c(0,true,side(4,14),true), d(1,false,MuSquadSide{},true);
    c.SendBuild(); d.SendBuild(); send(d,c);
    q = pick(); d.SetMatchSelections(q); d.queue.clear();
    send(c,d);                             // the late build: d cancels, never adopts
    assert(!d.SquadAdopted() && !d.squad_search_ && d.squad_cancelled_ && d.disconnected);
    assert(told(d, "Your friend picked Squad Strike after a lock-in. Match not started."));
    assert(logged("squad: friend's Squad Strike arrived after a lock-in, match cancelled"));
  }
  {
    // A late build after a received lock-in, or after a game started, cancels too.
    for (int when = 0; when < 2; ++when) {
      Capture a(0,false,MuSquadSide{},true), b(1,false,MuSquadSide{},true);
      a.SendBuild(); b.SendBuild(); send(a,b); send(b,a);
      auto q = pick(); a.SetMatchSelections(q);
      if (when == 0) send(a,b); else { a.queue.clear(); b.squad_locked_seen_.store(true); }
      assert(b.squad_locked_seen_);
      Capture late(0,true,side(4,14),true); late.SendBuild();
      b.Deliver(late.queue[0]);
      assert(!b.SquadAdopted() && b.squad_cancelled_);
    }
  }
  {
    // The joiner's game can't load Squad Strike (memory, disc identity): its REFUSE sends a build
    // without either squad bit, and the inviter cancels on screen instead of waiting.
    logs.clear();
    Capture a(0,true,side(4,14),true), b(1,false,MuSquadSide{},true);
    a.SendBuild(); b.SendBuild(); send(b,a); send(a,b); send(b,a);
    assert(b.SquadAdopted() && a.squad_peer_cap_);
    const uint8_t refuse[]{MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_REFUSE};
    g_last_search = {}; g_last_search.friend_room = "room";
    g_netplay = &b; assert(command(refuse, sizeof refuse, reply) && reply == std::vector<uint8_t>{1});
    g_netplay = nullptr;
    assert(b.squad_refused_ && b.queue.size() == 1 && !(caps_of(b.queue[0]) & (MU_SQUAD_CAP | MU_SQUAD_FRIEND_CAP)));
    assert(!b.squad_advertised_ && told(b, "Your game can't load Squad Strike lineups. Match not started."));
    send(b,a);
    assert(a.squad_cancelled_ && a.disconnected && told(a, "Your friend's game can't load Squad Strike. Match not started."));
    assert(logged("squad: friend's game can't load Squad Strike, match cancelled"));
    send(a,b);                             // a's cancel reaches b: b shows its own line
    assert(b.squad_cancelled_ && told(b, "Your game can't load Squad Strike lineups. Match not started."));
    // Outside a friend room there is nobody to tell.
    g_last_search = {};
    Capture e(0,true,side(4,14),true);
    g_netplay = &e; assert(command(refuse, sizeof refuse, reply) && reply == std::vector<uint8_t>{0});
    g_netplay = nullptr; assert(!e.squad_refused_);
  }
  {
    // The inviter answers Squad Strike for its own room at once; a plain room with the friend's
    // build in answers plain; no netplay yet answers waiting; outside a friend room, plain.
    const uint8_t ask[]{MU_SQUAD_OP_FRIEND, MU_SQUAD_FRIEND_QUERY};
    g_last_search = {}; g_last_search.friend_room = "room"; g_last_search.squad = true;
    assert(command(ask, sizeof ask, reply) && reply == std::vector<uint8_t>{MU_SQUAD_ROOM_SQUAD});
    g_last_search.squad = false;
    assert(command(ask, sizeof ask, reply) && reply == std::vector<uint8_t>{MU_SQUAD_ROOM_WAITING});
    Capture a(0,false,MuSquadSide{},true), b(1,false,MuSquadSide{},true);
    a.SendBuild(); b.SendBuild(); send(a,b);
    g_netplay = &b; assert(command(ask, sizeof ask, reply) && reply == std::vector<uint8_t>{MU_SQUAD_ROOM_PLAIN});
    g_last_search.mode = Matchmaking::UNRANKED;
    assert(command(ask, sizeof ask, reply) && reply == std::vector<uint8_t>{MU_SQUAD_ROOM_PLAIN});
    g_netplay = nullptr; g_last_search = {};
  }
#ifdef YOUGAME_AGENT_TEST_HOOK
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  // B's invalid ABI lineup waits for the held E3, then cancels on the real outgoing path.
  {
    Capture a(0,true,side(4,1)),b(1,true,side(4,5));
    a.SendBuild(); b.SendBuild(); send(a,b); send(b,a);
    auto q=pick(); a.SetMatchSelections(q);
    yougame::net_test::Config config;
    assert(yougame::net_test::parse_squad_delay("60:20", &config));
    config.squad_cancel_test = true;
    yougame::net_test::install(config);
    g_netplay = &b;
    assert(!command(bad, sizeof bad, reply));
    g_netplay = nullptr;
    b.SetMatchSelections(q);
    assert(b.queue.empty() && !b.disconnected && !mu_squad_side_valid(&b.squad_local_));
    assert(a.queue[0][0] == 0xE3);
    b.Deliver(a.queue[0]);
    assert(b.squad_test_late_lineup_ && b.disconnected && b.squad_cancelled_);
    assert(logged("squad: cancel race: late 0xE3 after invalid lineup"));
    assert(logged("squad: invalid outgoing lineup, match cancelled"));
    send(b,a);
    MuSquadSetup setup{};
    assert(a.squad_cancelled_ && !a.SquadSetup(&setup) && !b.SquadSetup(&setup));
    b.Deliver(a.queue[1]); // queued 82 still cannot admit a match
    assert(!b.SquadSetup(&setup));
  }
#endif
  std::puts("PASS: production MU_BUILD bytes, E3/82 reliable ordering, two-copy in-memory exchange, fallback, missing/malformed/late E3, rematch, immutable setup, first lock-in after connection refreshes the lineup, friend rooms join the friend's Squad Strike, hold lock-in until the friend's build, cancel a late build, and pass on a refusal");
}
