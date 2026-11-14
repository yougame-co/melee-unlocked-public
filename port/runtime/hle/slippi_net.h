// Slippi Online networking: SFML-compatible packets, the ENet netplay client, the matchmaking
// client and the user record. Ports of Dolphin's SlippiNetplay/SlippiMatchmaking/SlippiUser
// with the same wire formats, so the port can play against stock Slippi clients.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "slippi_game_prep.h"
#include "opponent_display.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "early_input_rules.h"
#include "../abi/mu_setup_hash.h"
#include "../abi/mu_pick_preview.h"
#include "../abi/mu_friends_lobby.h"
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#include "yougame_squad.h"
#endif

struct _ENetHost;
struct _ENetPeer;
namespace yougame::udp { struct EnetIngress; }
namespace yougame { struct Search; struct Match; }
namespace yougame::lobby { class Control; struct Plan; }
namespace slippi { struct PortInputs; }

namespace slippi {

constexpr int ROLLBACK_MAX_FRAMES = 7;
// How long a peer connection may take (NetplayClient::ThreadFunc), and how long a match setup
// waits for every peer's setup hash before that peer counts as disconnected.
constexpr uint64_t kConnectTimeoutMs = 8000;
constexpr int ONLINE_LOCKSTEP_INTERVAL = 30;
constexpr int REMOTE_PLAYER_MAX = 3;
constexpr int PLAYER_COUNT_MAX = REMOTE_PLAYER_MAX + 1;
constexpr int PAD_FULL_SIZE = 0xC;
constexpr int PAD_DATA_SIZE = 0x8;
constexpr uint8_t CHAT_MSG_CHAT_DISABLED = 0x10;
extern const char* const SLIPPI_SEMVER;   // netplay version this port speaks

enum NetMsg : uint8_t {
  NP_MSG_SLIPPI_PAD = 0x80, NP_MSG_SLIPPI_PAD_ACK = 0x81, NP_MSG_SLIPPI_MATCH_SELECTIONS = 0x82, NP_MSG_SLIPPI_CONN_SELECTED = 0x83,
  NP_MSG_SLIPPI_CHAT_MESSAGE = 0x84, NP_MSG_SLIPPI_COMPLETE_STEP = 0x85, NP_MSG_SLIPPI_SYNCED_STATE = 0x86,
  // Melee Unlocked only: the sender's build (retail or a mod's identity). Slippi Dolphin logs an
  // unknown id and ignores it, so it is safe to send to anyone.
  NP_MSG_MU_BUILD = 0xE0,
  // Melee Unlocked only: an early input (early_input.h), sent only to a peer whose build message
  // said it takes them. A peer without that word (Slippi Dolphin, an older build) never gets one.
  NP_MSG_MU_EARLY_PAD = 0xE1,
  // Melee Unlocked only: a ping before the match (request or reply), sent only to a peer whose build
  // message said it answers them, so auto delay has a ping when the match is set up.
  NP_MSG_MU_PING = 0xE2,
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  NP_MSG_MU_SQUAD = 0xE3,
#endif
  // Melee Unlocked only: the hash of the match setup this side built (mu_setup_hash.h). Sent to
  // every peer; only a peer whose build message carried MU_SETUP_HASH_CAP is waited for.
  NP_MSG_MU_SETUP_HASH = 0xE6,
  // Melee Unlocked only: the sender's live pick on the character screen (mu_pick_preview.h), sent
  // only to a peer whose build message carried MU_PICK_PREVIEW_CAP. Display only: the match is
  // still set up from 0x82 (and 0xE3).
  NP_MSG_MU_PICK_PREVIEW = MU_PICK_PREVIEW_MSG,
};
static_assert(NP_MSG_MU_SETUP_HASH == MU_SETUP_HASH_MSG, "one setup hash id");

// sf::Packet wire format: integers big-endian, bool as one byte, strings as u32 length + bytes.
class Packet {
 public:
  Packet() = default;
  Packet(const uint8_t* data, size_t size) : buf_(data, data + size) {}
  void append(const void* data, size_t size);
  const uint8_t* data() const { return buf_.data(); }
  size_t size() const { return buf_.size(); }
  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  Packet& operator<<(uint8_t v);
  Packet& operator<<(bool v) { return *this << (uint8_t)(v ? 1 : 0); }
  Packet& operator<<(uint16_t v);
  Packet& operator<<(uint32_t v);
  Packet& operator<<(int32_t v) { return *this << (uint32_t)v; }
  Packet& operator<<(const std::string& s);
  Packet& operator>>(uint8_t& v);
  Packet& operator>>(bool& v);
  Packet& operator>>(uint16_t& v);
  Packet& operator>>(uint32_t& v);
  Packet& operator>>(int32_t& v);
  Packet& operator>>(std::string& s);
 private:
  bool check(size_t n);
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool ok_ = true;
};

struct Pad {
  int32_t frame = 0, checksum_frame = 0;
  uint32_t checksum = 0;
  uint8_t buf[PAD_FULL_SIZE] = {};
  explicit Pad(int32_t f) : frame(f) {}
  Pad(int32_t f, const uint8_t* data) : frame(f) { for (int i = 0; i < PAD_DATA_SIZE; ++i) buf[i] = data[i]; }
  Pad(int32_t f, int32_t cf, uint32_t c, const uint8_t* data) : Pad(f, data) { checksum_frame = cf; checksum = c; }
};

struct RemotePadOutput {
  int32_t latest_frame = 0, checksum_frame = 0;
  uint32_t checksum = 0;
  uint8_t player_idx = 0;
  bool is_disconnected = false;
  bool checksum_primary = true; // compare once per machine, though every port carries the checksum
  std::vector<uint8_t> data;
};


struct PlayerSelections {
  uint8_t player_idx = 0, character_id = 0, character_color = 0, team_id = 0;
  bool is_character_selected = false;
  uint16_t stage_id = 0;
  bool is_stage_selected = false;
  uint8_t alt_stage_mode = 0;
  uint32_t rng_offset = 0;
  int message_id = 0;
  bool error = false;
  void Merge(const PlayerSelections& s) {
    rng_offset = s.rng_offset;
    if (s.is_stage_selected) { stage_id = s.stage_id; is_stage_selected = true; alt_stage_mode = s.alt_stage_mode; }
    if (s.is_character_selected) { character_id = s.character_id; character_color = s.character_color; team_id = s.team_id; is_character_selected = true; }
  }
  void Reset() { character_id = character_color = team_id = 0; is_character_selected = false; stage_id = 0; is_stage_selected = false; rng_offset = 0; }
};

struct MatchInfo {
  PlayerSelections local;
  PlayerSelections remote[REMOTE_PLAYER_MAX];
  void Reset() { local.Reset(); for (auto& r : remote) r.Reset(); }
};

struct UserInfo {
  std::string uid, play_key, display_name, connect_code, latest_version;
  int port = 0;
  std::vector<std::string> chat_messages;
  bool is_bot = false;
};

// The user record written by the Slippi Launcher (user.json in the Slippi user folder).
class User {
 public:
  // slippi_only loads a Slippi login without replacing the menu's YouGame identity.
  // Under YouGame this reads only its explicit grant; standalone keeps upstream discovery.
  explicit User(std::string user_dir, bool slippi_only = false);
  // Every YouGame Slippi path uses only the account explicitly granted by its app.
  // No ambient launcher discovery or server request when the grant is absent.
  static std::unique_ptr<User> GrantedSlippiUser(const std::string& user_dir);
  bool AttemptLogin(bool rediscover = false);  // Log In rechecks shared launcher folders too.
  bool IsLoggedIn() const { return logged_in_; }
  UserInfo GetUserInfo() const { std::lock_guard<std::mutex> lock(mutex_); return info_; }
  void LogOut();
  void OverwriteLatestVersion(const std::string& v) { std::lock_guard<std::mutex> lock(mutex_); info_.latest_version = v; }
  std::vector<std::string> GetUserChatMessages() const;
  static std::vector<std::string> GetDefaultChatMessages();
  const std::string& dir() const { return dir_; }
  // Refreshes display name, connect code, latest version and chat messages from Slippi's user
  // API on a background thread. user.json is only rewritten by the Slippi Launcher at login, so a
  // name changed on the website since then is stale in it; the server's copy is what the opponent
  // and the in-game name tag already show.
  void RefreshFromServer();
  // Fetch the current server version, with the existing offline-account fallback.
  using VersionFetch = bool (*)(const std::string&, std::string*, int*);
  bool CheckOnlineCompatibility(std::string* error, VersionFetch fetch = nullptr);
  // One verification shared by a search, its peer connection and first game. A reset starts
  // fresh verification for the next search/rematch, including after a cancelled request.
  bool CheckSessionCompatibility(std::string* error, VersionFetch fetch = nullptr);
  // Game thread only: 0 checking, 1 approved, -1 refused. Also covers rematches which reuse
  // a peer connection. Checking never blocks the character-select/render thread.
  int PollOnlineCompatibility(std::string* error, VersionFetch fetch = nullptr);
  void ResetOnlineCompatibility() { ++compatibility_generation_; compatibility_done_ = false; }
 private:
  std::string dir_;
  bool slippi_only_ = false;
  std::string account_file_;
  bool account_discovered_ = false;
  UserInfo info_;
  bool logged_in_ = false;
  mutable std::mutex mutex_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  bool compatibility_done_ = false, compatibility_ok_ = false;
  std::atomic<uint64_t> compatibility_generation_{0};
  std::string compatibility_error_;
  std::mutex compatibility_request_mutex_;
  uint64_t verified_generation_ = ~uint64_t{0};
  bool verified_ok_ = false;
  std::string verified_error_;
  bool CheckSessionCompatibilityAt(uint64_t generation, std::string* error, VersionFetch fetch);
  // Destroyed before the fields used by their workers: no detached access to a destroyed User.
  std::future<void> refresh_;
  struct VersionResult { bool ok; std::string error; uint64_t generation; };
  std::future<VersionResult> compatibility_;
};

// Connect-code history (direct-codes.json / teams-codes.json in the user folder).
class DirectCodes {
 public:
  DirectCodes(std::string path);
  std::string get(int index) const;
  int length() const { return (int)codes_.size(); }
  void AddOrUpdateCode(const std::string& code);
 private:
  void Load();
  void Save();
  std::string path_;
  std::vector<std::string> codes_;
};

class NetplayClient {
 public:
  enum class ConnectStatus { UNSET, INITIATED, CONNECTED, FAILED, DISCONNECTED };
  enum class DisconnectReason : uint32_t { UNSPECIFIED = 0, POOR_PERFORMANCE = 1
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    , SQUAD_PROTOCOL = 2
#endif
  };

  NetplayClient(std::vector<std::string> addrs, std::vector<uint16_t> ports, uint8_t remote_player_count, uint16_t local_port,
                bool is_decider, uint8_t player_idx, bool own_udp = false, std::vector<std::string> public_fallbacks = {}
#if MU_SQUAD
                // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
                , bool squad_search = false, MuSquadSide squad_side = {}, bool squad_friend_room = false
#endif
                , const yougame::lobby::Plan* lobby_plan = nullptr);
  ~NetplayClient();

  bool IsDecider() const { return is_decider_; }
  uint8_t LocalPlayerPort() const { return player_idx_; }
  ConnectStatus GetSlippiConnectStatus() const { return status_.load(std::memory_order_acquire); }
  bool OwnUDPPortUnavailable() const { return own_udp_port_unavailable_; }
  std::vector<int> GetFailedConnections() const { return failed_connections_; }
  void StartSlippiGame();
  void SendSlippiPad(std::unique_ptr<Pad> pad, std::unique_ptr<Pad> partner = nullptr);
  int RemoteMachineCount() const { return remote_player_count_; }
  int RemotePortCount() const;
  int SecondLocalPort() const;
  bool IsLocalVirtualPort(int index) const;
  void SetMatchSelections(PlayerSelections& s);
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  void SetSquadSelection(const MuSquadSide& side);
  bool SquadSetup(MuSquadSetup* out);
  bool SquadCancelled() const { return squad_cancelled_.load(); }
  // A friend room this side joined in Squad Strike because the friend picked it (their MU_BUILD came
  // with MU_SQUAD_CAP); the game's character select asks for it with MU_SQUAD_OP_FRIEND.
  bool SquadAdopted() const { return squad_adopted_.load(); }
  // A friend room whose friend's build has not arrived yet: it may still turn the room into Squad
  // Strike, so nobody locks in until it does (the joiner's lock-in is held, never sent plain).
  // Reads the build mark before squad_search_: an adoption stores squad_adopted_, squad_search_, then
  // the mark, so "not waiting" here always comes with the adoption already visible.
  bool SquadFriendWaiting() const { return squad_friend_room_ && !squad_peer_build_.load() && !squad_search_.load(); }
  // This game can't load Squad Strike (memory, disc identity): a build without either squad bit
  // tells the friend, whose game cancels the room on screen.
  void SquadFriendRefuse();
  // What the player is told when a squad match is refused (set before CancelSquad).
  const char* SquadCancelText() const { return squad_cancel_text_.load(); }
  void CancelSquad(const char* reason);
  // Cancel with this text for the player (a static string) instead of the generic one.
  void RefuseSquad(const char* text, const char* reason);
  bool SquadPeerDisconnect(uint32_t reason);
#endif
  std::unique_ptr<RemotePadOutput> GetSlippiRemotePad(int index, int max_frame_count, int32_t frame = INT32_MAX);
  void DropOldRemoteInputs(int32_t finalized_frame);
  std::unordered_map<uint8_t, bool> GetActivePlayerIndices() const;
  void ForceDisconnectPlayer(uint8_t player_idx);
  void ForceDisconnect(DisconnectReason reason = DisconnectReason::UNSPECIFIED);
  DisconnectReason GetDisconnectReason() const { return (DisconnectReason)disconnect_reason_.load(std::memory_order_acquire); }
  // A copy: the network thread merges the remote players' selections while the game reads them.
  MatchInfo GetMatchInfo() { std::lock_guard<std::mutex> lk(selection_mutex_); return match_info_; }
  PlayerSelections GetSlippiRemoteChatMessage(bool chat_enabled);
  uint8_t GetSlippiRemoteSentChatMessage(bool chat_enabled);
  int32_t CalcTimeOffsetUs();
  double GetAndResetAvgPingMs();
  int LastPingMs() const { return (int)(last_ping_ms_.load(std::memory_order_relaxed)); }
  // Each remote machine's last ping (Teams has up to three), and how many there are. UI thread safe.
  int RemotePingMs(int index) const {
    return index >= 0 && index < REMOTE_PLAYER_MAX ? (int)remote_ping_ms_[index].load(std::memory_order_relaxed) : 0;
  }
  int RemoteCount() const { return (int)remote_count_seen_.load(std::memory_order_relaxed); }
  // The median of the last pre-match pings (NP_MSG_MU_PING) to the first opponent, 0 if none yet.
  int PreMatchPingMs() const { return (int)(prematch_ping_ms_.load(std::memory_order_relaxed)); }
  void SendChatMessage(int message_id);
  void SendGamePrepStep(const GamePrepStep& step);
  std::array<uint8_t, 6> FetchGamePrepStep(uint8_t index) { return game_prep_.fetch(index); }
  void SendAsync(std::unique_ptr<Packet> packet);
  uint8_t remote_sent_chat_message_id = 0;
  // The opponents' builds (NP_MSG_MU_BUILD), and when the connection completed. early_pads: the
  // build takes early inputs (a trailing byte older builds never sent and never read).
  struct RemoteBuild { bool received = false, mod_view = false; std::string fingerprint, name; bool early_pads = false, pings = false;
                       uint32_t shield_rgb = 0;   // their own shield colour, 0x1000000 | RGB, 0 none sent
                       uint8_t port = 0xFF;
                       bool static_engine = false; std::string ranked_rules_digest;   // their in-game slot
                       bool setup_hash = false;   // they send NP_MSG_MU_SETUP_HASH and wait for ours
                       bool pick_previews = false;   // takes NP_MSG_MU_PICK_PREVIEW
                       uint8_t caps2 = 0; };   // the second capability byte (mu_friends_lobby.h), 0 when absent
  // The shield colour the opponent on in-game `slot` sent (0 none).
  uint32_t RemoteShieldColor(int slot);
  RemoteBuild GetRemoteBuild(int index);
  // Whether every remote machine has sent NP_MSG_MU_BUILD, which plain Slippi Dolphin never does.
  bool EveryRemoteReadsGoneOwnFrames();
  // The match setup hash (mu_setup_hash.h). SendSetupHash queues ours for every peer (any thread);
  // GetSetupHash gives remote `index`'s latest one.
  struct PeerSetupHash { bool received = false; MuSetupHash value{}; };
  void SendSetupHash(uint64_t inputs, uint64_t setup);
  PeerSetupHash GetSetupHash(int index);
  uint64_t ConnectedAtMs() const { return connected_at_ms_.load(std::memory_order_acquire); }
  // Early inputs. SendEarlyPad goes to the peers that take them (any thread: it only queues).
  // EarlyPads gives remote `index`'s early pads for frames after `latest` (its confirmed pads reach
  // `latest`; older entries are dropped), for the game's prediction.
  void SendEarlyPad(int32_t target, const uint8_t pad[host::early_input::kPadBytes]);
  void EarlyPads(int index, int32_t latest, std::vector<host::early_input::Entry>& out);
  // Live pick previews (display only). SendPickPreview: the game thread, every CSS frame; sends
  // to the peers that take them, only while CONNECTED, only on change, at most one per
  // MU_PICK_PREVIEW_INTERVAL frames. RemotePickPreview: remote `index`'s latest preview and its
  // port, false when none arrived since the last game start or that player is gone.
  void SendPickPreview(const MuPickPreview& pick, uint32_t frame);
  bool RemotePickPreview(int index, MuPickPreview* out, uint8_t* port);

 private:
  struct FrameTiming { int32_t frame = 0; uint64_t time_us = 0; };
  struct FrameOffsetData { int idx = 0; std::vector<int32_t> buf; };
  struct ActiveConnectionInfo { uint8_t player_idx = 0; bool is_disconnected = false; };

  // Connection index, used only by machine-scoped messages and clocks.
  uint8_t PlayerIdxFromPort(uint8_t port) const {
    for (int i = 0; i < remote_player_count_; ++i) if (match_info_.remote[i].player_idx == port) return (uint8_t)i;
    return REMOTE_PLAYER_MAX;
  }
  int SenderPort(_ENetPeer* peer) const;
  void OnData(Packet& packet, _ENetPeer* peer);
  void Send(Packet& packet);
  void Disconnect();
  void ThreadFunc();
  bool AreAllPeersDisconnectedForKey(const std::string& key);
  bool AreAllConnectionsDisconnected();
  void WriteSelections(Packet& p, const PlayerSelections& s);
  std::unique_ptr<PlayerSelections> ReadSelections(Packet& p);
  std::unique_ptr<PlayerSelections> ReadChatMessage(Packet& p);

  GamePrepQueue game_prep_;
  std::mutex pad_mutex_, ack_mutex_, async_mutex_;
  // timing_mutex_: last_frame_timing_, has_game_started_, frame_offset_data_ (network thread and game
  // thread). chat_mutex_: remote_chat_message_selection_. selection_mutex_: match_info_ (the network
  // thread merges remote selections, the game thread reads, resets and sets its own; the player_idx
  // fields are written once in the constructor and read without it).
  // Selection receipt takes selection_mutex_, timing_mutex_ and pad_mutex_ together so its
  // pre-start decision and queue reset are atomic. Pad sends take pad_mutex_ before
  // ack_mutex_ and async_mutex_; chat replies take chat_mutex_ before async_mutex_.
  // Neither ACK receipt nor the async drain takes pad_mutex_.
  // async_mutex_ is never held while taking another, so no two are ever taken in opposite orders.
  // SetMatchSelections also queues a lock-in's pick preview under selection_mutex_; that takes the
  // leaves build_mutex_ and pick_mutex_ one at a time, never selection_mutex_ under them.
  std::mutex timing_mutex_, chat_mutex_, selection_mutex_;
#ifdef YOUGAME_AGENT_TEST_HOOK
  struct AsyncPacket {
    std::unique_ptr<Packet> packet;
    uint64_t enqueued_ms, generation;
  };
  std::deque<AsyncPacket> async_queue_;
  std::atomic<uint64_t> net_test_generation_{0};
  // Serializes generation changes against due sends. May precede build_mutex_
  // via Send(), and is never held with async_mutex_.
  std::mutex net_test_generation_mutex_;
#else
  std::deque<std::unique_ptr<Packet>> async_queue_;
#endif
  _ENetHost* client_ = nullptr;
  std::unique_ptr<yougame::udp::EnetIngress> own_udp_ingress_;
  bool own_udp_port_unavailable_ = false;
  std::vector<_ENetPeer*> server_;
  std::vector<std::string> public_fallbacks_; // parallel to remote seats, never additional players
  std::thread thread_;
  uint8_t remote_player_count_ = 0; // real remote machines: server_, ACKs, clocks, builds and setup hashes
  std::atomic<bool> do_loop_{true};
  bool is_decider_ = false, has_game_started_ = false;
  std::array<bool, REMOTE_PLAYER_MAX> remote_game_started_{};   // under timing_mutex_
  uint8_t player_idx_ = 0;
  std::unordered_map<std::string, std::map<_ENetPeer*, ActiveConnectionInfo>> active_connections_;
  std::atomic<bool> player_active_[PLAYER_COUNT_MAX] = {};
  std::deque<std::unique_ptr<Pad>> local_pad_queue_;
  std::unique_ptr<PortInputs> port_inputs_;
  bool machine_pad_format_ = false;
  struct ChecksumEntry { int32_t frame = 0; uint32_t value = 0; } remote_checksums_[REMOTE_PLAYER_MAX];
  uint64_t ping_us_[REMOTE_PLAYER_MAX] = {};
  std::atomic<uint64_t> ping_sample_sum_us_{0}, ping_sample_count_{0};
  std::atomic<uint32_t> last_ping_ms_{0};
  std::atomic<uint32_t> remote_ping_ms_[REMOTE_PLAYER_MAX] = {};
  std::atomic<uint8_t> remote_count_seen_{0};
  std::atomic<uint32_t> prematch_ping_ms_{0};
  std::deque<uint32_t> prematch_samples_;   // network thread only
  void SendPrematchPing();
  int32_t last_frame_acked_[REMOTE_PLAYER_MAX] = {};
  FrameOffsetData frame_offset_data_[REMOTE_PLAYER_MAX];
  FrameTiming last_frame_timing_[REMOTE_PLAYER_MAX];
  std::deque<FrameTiming> ack_timers_[REMOTE_PLAYER_MAX];
  std::atomic<ConnectStatus> status_{ConnectStatus::UNSET};
  std::atomic<uint32_t> pending_disconnect_reason_{0}, disconnect_reason_{0};
  std::vector<int> failed_connections_;
  MatchInfo match_info_;
  std::unique_ptr<PlayerSelections> remote_chat_message_selection_;
  std::mutex build_mutex_;
  RemoteBuild remote_build_[REMOTE_PLAYER_MAX];
  PeerSetupHash setup_hash_[REMOTE_PLAYER_MAX];   // under build_mutex_
  std::atomic<uint64_t> connected_at_ms_{0};
  host::early_input::Buffer early_pads_[REMOTE_PLAYER_MAX];   // under pad_mutex_
  void SendBuild();
  // Pick previews. pick_mutex_ is a leaf: never held while taking another lock (SendAsync and
  // build_mutex_ included). It guards everything below it here.
  void QueuePickPreview(MuPickPreview pick, uint32_t frame, bool locked_now);
  void SendLockedPickPreview();
  std::mutex pick_mutex_;
  MuPickPreviewSender pick_sender_{};
  MuPickPreview pick_local_{};
  bool pick_local_set_ = false;
  uint8_t pick_sent_peers_ = 0;   // remote indices that took previews at the last send
  MuPickPreview pick_remote_[REMOTE_PLAYER_MAX] = {};
  uint8_t pick_remote_port_[REMOTE_PLAYER_MAX] = {};
  bool pick_remote_set_[REMOTE_PLAYER_MAX] = {};
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  // squad_search_ starts as the chosen search; in a friend room it turns on (never off) when the
  // friend's build arrives with MU_SQUAD_CAP, so it is read across threads.
  std::atomic<bool> squad_search_{false};
  bool squad_normal_logged_ = false, squad_friend_room_ = false;
  std::atomic<bool> squad_advertised_{false}, squad_peer_cap_{false}, squad_cancelled_{false}, squad_adopted_{false};
  // Friend rooms: the friend's build arrived (and whether it said it joins Squad Strike); a locked
  // selection went either way or a game started (a late Squad Strike cancels, never adopts); this
  // game refused Squad Strike (SquadFriendRefuse).
  std::atomic<bool> squad_peer_build_{false}, squad_peer_friend_cap_{false}, squad_locked_seen_{false},
      squad_refused_{false};
#ifdef YOUGAME_AGENT_TEST_HOOK
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  bool squad_test_late_lineup_ = false; // guarded by selection_mutex_
#endif
  MuSquadSide squad_local_{}, squad_transmitted_{}, squad_peer_{}, squad_incoming_{}; // selection_mutex_
  bool squad_peer_pending_ = false, squad_peer_selected_ = false;
  bool squad_peer_other_version_ = false; // the peer's last 0xE3 was another MU_SQUAD_VERSION; selection_mutex_
  std::atomic<const char*> squad_cancel_text_{"Squad Strike setup didn't match. Match not started."};
#endif
};

class Matchmaking {
 public:
  // Slippi's mode ids. Ranked is backed by YouGame when launched by its app.
  enum OnlinePlayMode { RANKED = 0, UNRANKED = 1, DIRECT = 2, TEAMS = 3, PARTY = 4 };
  enum ProcessState { IDLE, INITIALIZING, MATCHMAKING, OPPONENT_CONNECTING, CONNECTION_SUCCESS, ERROR_ENCOUNTERED };
  struct MatchSearchSettings { OnlinePlayMode mode = UNRANKED; std::string connect_code; std::vector<std::string> blocked_codes; std::string friend_room;
    // The 4-player Friends lobby (yougame_lobby.h): a YouGame room held by the host or joined from an
    // invite (lobby_room), driven by the 4-door select through lobby_control. Never a Slippi search.
    bool friends_lobby = false;
    std::string lobby_room;
    std::shared_ptr<yougame::lobby::Control> lobby_control;
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    bool squad = false;
    MuSquadSide squad_side{};
    std::string squad_budget;
#endif
  };
  // True when an opponent in the match is on the Unranked block list (codes compared ignoring case
  // and spaces). Public for the unit test.
  static bool IsBlockedMatch(OnlinePlayMode mode, const std::vector<std::string>& blocked,
                             const std::vector<UserInfo>& players, int local_index);
  struct MatchmakeResult { std::string id; std::vector<UserInfo> players; std::vector<uint16_t> stages; uint32_t items = 0; };
  // The match a get-ticket-resp describes, read without trusting any of it. ok is false (and error
  // says why) unless there are 2 to 4 players, every port is 1..4 and used once, and one of them is
  // the local player; then no count or index taken from it can pass the fixed per-player arrays.
  struct ParsedTicket {
    bool ok = false;
    std::string error;
    std::string match_id;
    std::vector<UserInfo> players;          // in the ticket's order
    int local_player_index = 0;             // the local player's port - 1
    std::vector<std::string> remote_ips;    // "ip:port" per remote, in the ticket's order
    std::vector<std::string> public_fallbacks; // same shape; empty when no same-router fallback
    bool is_host = false;
    std::vector<uint16_t> stages;
    uint32_t items = 0;
  };
  // Pure parsing of a get-ticket-resp body (JSON text). Never throws; no sockets, no state.
  static ParsedTicket ParseTicket(const std::string& json_text);
  // Local test peering (no matchmaking server): fixed player index, local port and the address of
  // every other player ("ip:port", in player-index order with the local player left out). One
  // remote is a two-player match; three is a four-player (Teams) match.
  struct LocalPeer {
    bool enabled = false;
    int local_index = 0;
    uint16_t local_port = 0;
    std::vector<std::string> remotes;
    int test_stage = -1;  // deterministic stage selection for local regression runs only
  };

  explicit Matchmaking(User* user);
  ~Matchmaking();
  void FindMatch(MatchSearchSettings settings);
  ProcessState GetMatchmakeState() const { return state_; }
  std::string GetErrorMessage() const { std::lock_guard<std::mutex> lk(result_mutex_); return error_msg_; }
  bool IsSearching() const { return state_ == INITIALIZING || state_ == MATCHMAKING || state_ == OPPONENT_CONNECTING; }
  std::unique_ptr<NetplayClient> GetNetplayClient() { std::lock_guard<std::mutex> lk(result_mutex_); return std::move(netplay_client_); }
  int LocalPlayerIndex() const { std::lock_guard<std::mutex> lk(result_mutex_); return local_player_index_; }
  std::vector<UserInfo> GetPlayerInfo() const { std::lock_guard<std::mutex> lk(result_mutex_); return player_info_; }
  display::Provider DisplayProvider() const { return display_provider_.load(); }
  std::string GetPlayerName(uint8_t port) const;
  std::vector<uint16_t> GetStages() const { std::lock_guard<std::mutex> lk(result_mutex_); return allowed_stages_; }
  // Never more than REMOTE_PLAYER_MAX: callers index fixed per-remote arrays with it.
  uint8_t RemotePlayerCount() const {
    std::lock_guard<std::mutex> lk(result_mutex_);
    const size_t n = player_info_.empty() ? 0 : player_info_.size() - 1;
    return (uint8_t)(n < (size_t)REMOTE_PLAYER_MAX ? n : (size_t)REMOTE_PLAYER_MAX);
  }
  // True when every player but the local one is a bot (asked every frame of a match: no copy).
  bool AllRemotesAreBots() const {
    std::lock_guard<std::mutex> lk(result_mutex_);
    for (size_t i = 0; i < player_info_.size(); ++i) { if ((int)i == local_player_index_) continue; if (!player_info_[i].is_bot) return false; }
    return true;
  }
  MatchmakeResult GetMatchmakeResult() const { std::lock_guard<std::mutex> lk(result_mutex_); return mm_result_; }
  // As Slippi's: Ranked plays Unranked's rules (legal characters, the stage list). Standalone Melee
  // Unlocked never reaches Ranked (native_online_policy.h); YouGame mode does.
  static bool IsFixedRulesMode(OnlinePlayMode m) { return m == UNRANKED || m == RANKED || m == PARTY; }
  static LocalPeer local_peer;          // set from the command line for local 2-4 instance tests
  static uint16_t forced_port;          // 0 = random 41000..50999
  // False in hidden or scripted runs unless --allow-matchmaking: an automated run must never
  // queue on Slippi's servers by accident (local peering is unaffected).
  static bool server_allowed;

 private:
  void MatchmakeThread();
  void startMatchmaking();
  // status: the Unranked status line while it searches (yougame_blend.h kSearch*); null for "searching".
  void startYouGameMatchmaking(const char* status = nullptr);
  bool youGameSearch(yougame::Search* search);     // false: the search has failed (an unreadable code)
  void takeYouGameMatch(const yougame::Match& match);
  // Blended Unranked (yougame_blend.h): YouGame's and Slippi's searches at once, first opponent wins.
  void blendMatchmaking();
  // Slippi's matchmaking client up to a queued ticket: "" once queued, else why not. `stop` ends it
  // early; `avoid` is a port it must not take (the blend's YouGame port).
  std::string openSlippiTicket(const UserInfo& me, const std::function<bool()>& stop, int avoid);
  void dropSlippi();                       // ends Slippi's matchmaking client now
  void handleMatchmaking();
  void handleConnecting();
  void disconnectFromServer();
  void terminateMmConnection();
  struct Ticket;                          // a get-ticket-resp body (defined in slippi_net.cpp)
  bool ingest_ticket(const Ticket& ticket);   // false: the ticket was refused and the search has failed
  void fail(const std::string& message);      // the search ends with this error text

  User* user_;
  // Separate from the menu's YouGame identity; never passed to YouGame's bridge or reporter.
  std::unique_ptr<User> slippi_user_;
  // Matchmaking thread publishes the selected route; only display code consumes it.
  std::atomic<display::Provider> display_provider_{display::Provider::Unknown};
  _ENetHost* client_ = nullptr;
  _ENetPeer* server_ = nullptr;
  bool is_mm_connected_ = false;
  std::atomic<bool> is_mm_terminated_{false};
  std::thread thread_;
  MatchSearchSettings search_settings_;
  std::atomic<ProcessState> state_{IDLE};
  // result_mutex_: what the game thread reads of a search while the matchmaking thread works:
  // error_msg_, local_player_index_, mm_result_, player_info_, allowed_stages_, netplay_client_.
  // The matchmaking thread builds a result aside and swaps it in complete, and stores the error text
  // before the state that announces it. It is a leaf lock: nothing else is taken while it is held
  // (not the user record's mutex, none of the netplay client's), and no netplay client is created or
  // destroyed under it. The matchmaking thread is the only writer after FindMatch, so it reads its
  // own copies without the lock.
  mutable std::mutex result_mutex_;
  std::string error_msg_;
  int local_player_index_ = 0;
  MatchmakeResult mm_result_;
  std::vector<UserInfo> player_info_;
  std::vector<uint16_t> allowed_stages_;
  std::unique_ptr<NetplayClient> netplay_client_;
  // Matchmaking thread only.
  int host_port_ = 0;
  std::vector<std::string> remote_ips_;
  std::vector<std::string> public_fallbacks_;
  bool is_host_ = false;
};

// Launched by the app on the base listing: tells it whether Unranked can use a Slippi login when the
// online menu first opens (yougame::slippi_link), read once off the game thread. Remixes send nothing.
void announce_slippi_link(const std::string& user_dir);

// Helpers shared with the EXI device.
uint64_t time_us();
uint64_t time_ms();
std::string utf8_to_shiftjis(const std::string& s);
std::string shiftjis_to_utf8(const std::string& s);
std::string convert_string_for_game(const std::string& input, int length);
std::string truncate_length_char(const std::string& input, int length);
std::string convert_connect_code_for_game(const std::string& input);
bool enet_ready();   // initializes ENet once

}  // namespace slippi
