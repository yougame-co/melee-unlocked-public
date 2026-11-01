// YouGame online: what replaces Slippi's servers when the YouGame desktop app launches the game.
//
// Slippi's matchmaking server, user record and game reporter are swapped for YouGame's: the
// player's identity and match tickets come from the desktop app's bridge, matchmaking and results
// go through YouGame's rooms Worker. With hello.netBroker.direct the app owns all networking:
// brokered rooms and sealed datagrams through a shared ring, with virtual ENet addresses. Internet
// builds keep STUN and punching before the Slippi netplay client takes over. The game's
// menus, the rollback engine and the netplay wire format do not change.
//
// YouGame mode is on only when the app started the game: YOUGAME_STDIO=1 (the app's sandboxed
// launch, the bridge on stdin/stdout) or YOUGAME_BRIDGE + YOUGAME_TOKEN (its loopback bridge). See
// docs/MELEE_UNLOCKED.md in the YouGame repository. Off, nothing here runs and nothing changes,
// Unranked with a local Slippi login uses Slippi's matchmaking and native UDP instead.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
struct _XINPUT_STATE;
#ifndef _WIN32
#include "yougame_pad_mirror.h"
#endif

namespace yougame {
namespace relay_pads {
// After the host selects --log-file; releases controller messages buffered during startup.
void logging_ready();
bool active();
bool read(unsigned slot, _XINPUT_STATE* state);
// Serialized with activation, so even the probe/rumble workers cannot open native pads
// after the app takes over. poll serves the relay or invokes the native fallback.
bool poll(unsigned slot, _XINPUT_STATE* state, const std::function<bool()>& native);
void rumble(const std::function<void()>& native);
// The app has sent a raw HID pad (kind "hid") this run, so HID pads are its own from then on.
bool owns_hid();
#ifdef YOUGAME_AGENT_TEST_HOOK
void arm_rumble_test(double boot_ms);
#endif
}
namespace mode_policy { enum class ModRefusal; }

// Call once at startup, before the window exists: takes the bridge's address and token out of the
// environment (so nothing the game starts inherits them) and, in stdio mode, starts reading the
// app's lines, so the player box's commands are heard from the first frame.
void start();

// Launched by the YouGame desktop app (including Slippi-routed Unranked).
bool active();
// The bridge is this process's stdin/stdout (the app's sandboxed launch): stdout carries only
// bridge lines (the log goes to stderr), and the GameCube adapter comes from the app's pad mirror.
bool stdio_bridge();

// Best-effort compatibility code for an explicit listing layer. At most one request per process;
// any reply (including an old app's unknown-request reply) suffices. Returns within 500 ms.
void notify_mod_refused(bool explicit_layer, mode_policy::ModRefusal refusal,
                        const std::string& mod = {}, const std::string& version = {});

// The game draws in the YouGame player box (YOUGAME_EMBED=1); the app places its window.
bool embedded();
// Windows launch contract, consumed before window creation; false/zero elsewhere.
bool embed_toplevel();
uint32_t app_pid();
void embed_window_ready(uintptr_t hwnd = 0);
void console_boot_ready();
// The player box's commands ("rect x y w h", "hide", "show", "focus", "blur", "volume v", "quit"),
// delivered on the bridge's reader thread. Lines that arrive before a handler is set are kept.
void set_box_handler(std::function<void(const std::string&)> handler);
#ifdef MELEE_LINUX_X11_EMBED
// Linux full host: ordered, bounded commands buffered from startup, drained by the video thread.
std::vector<std::string> take_box_commands();
#endif
// The pad mirror the app shares (a section handle in this process, 0 until it arrives): the
// GameCube adapter as the YouGame app's controller helper reads it (gc_adapter.cpp).
uintptr_t pad_mirror_handle();
#ifndef _WIN32
// Inherited, borrowed O_RDONLY descriptor and advertised mapping size.
pad_mirror::Attachment pad_mirror_attachment();
#endif

struct Identity {
  bool ok = false;
  std::string player_id;   // this game's player id for the signed-in account (the room's player id)
  std::string name;        // display name, as the room shows it to opponents
  std::string code;        // connect code, "TAG#123"; empty when YouGame has not assigned one
  std::string error;       // why there is no identity (signed out, the app is not answering)
};
// The signed-in YouGame player, from a cache: never blocks (the game thread reads it every frame
// in the online menus). The first call starts asking the app in the background and answers "not
// signed in yet"; a failed answer is asked again after a few seconds. refresh_identity() asks
// again now, in the background (the online menu's "log in", which re-read user.json for Slippi).
Identity identity();
void refresh_identity();
// The first answer, waiting for it until `ms` after the first call of the process, never longer in
// total (the online menu's status query at boot: answered before the identity arrived, the menus
// would draw Slippi's "log in" screen; a slow app must not stall the game thread again and again).
Identity wait_identity(int ms);

// A key relayed while the game has the keyboard: "shift-escape" leaves app full screen.
// Plain "escape" is also sent for older apps. Sent as `yg {"key":"shift-escape"}`.
void relay_key(const char* key);

// The page pause's state for the page (yougame_ux.h PagePause::Result), sent over the stdio bridge as
// `yg {"event":{"type":"pause","pausable":B,"paused":P,"reason":R}}` (reason left out when null).
void pause_event(bool pausable, bool paused, const char* reason);

// The host picker runs off the simulation thread while code entry remains visible.
// Throws on cancellation, a page error, or an invalid answer; opens no room.
std::string pick_friend_room(const std::function<bool()>& cancelled, const std::string& held = {});
std::string slippi_link_status(const std::string& status);
void blend_slippi_stopped(uint64_t epoch, const std::string& reason);

// Retire any previous YouGame room before selecting plain UDP for a Slippi search.
bool prepare_slippi_match(const std::function<bool()>& cancelled, uint64_t epoch = 0);
// Reserve a generation on the caller before launching a helper. Retires the previous room.
uint64_t begin_search();
// Serialize a blend commit with the helper's guarded global effects.
bool guard_search(uint64_t epoch, const std::function<void()>& commit);

// Runs `create` with ENet sockets on native UDP, then puts back the selection a YouGame search made.
// ENet picks a socket's transport when it is created, so a host made here stays native: blended
// Unranked makes Slippi's matchmaking client while YouGame's search (app-owned links) runs.
void with_native_udp(const std::function<void()>& create);

// Whether the base listing's Unranked can use a Slippi login, for the app: `yg {"slippi":...}` over the
// stdio bridge the first time and whenever it changes (yougame_blend.h slippi_link_line). `tag` is the
// player's own connect code; nothing else of the login ever goes out. Remixes never call it.
void slippi_link(bool linked, const std::string& tag);

// One matchmaking search: the game's online menu choice.
struct Search {
  uint64_t epoch = 0;        // reserved before launching a blend helper; 0: reserve on entry
  int mode = 1;               // Slippi's ids: 0 ranked, 1 unranked, 2 direct, 3 teams
  std::string direct_code;    // the opponent's connect code (Direct)
  std::string friend_room;    // internal invite; never an opponent connect code
  uint16_t udp_port = 0;      // the netplay port; STUN and the punch run from it so the NAT mapping is the one ENet uses
  // Blended Unranked only (yougame_blend.h), called on the search's thread. on_searching: the queue's
  // "searching" count (players searching there, this one included). claim: the queue named a room;
  // false declines it (Slippi's side was first): briefly join and leave the assigned room.
  std::function<void(int waiting)> on_searching;
  std::function<bool()> claim;
  std::string status;         // the status line while it searches (yougame_blend.h kSearch*); empty: "searching"
};

struct Player {
  int port = 0;               // 1-based, the in-game port (room seat + 1)
  bool local = false;
  std::string id, name, code; // id: the room's player id
  std::string address;        // virtual "ip:port" in direct mode, otherwise the punched address; empty locally
};

struct Match {
  bool ok = false;
  uint16_t udp_port = 0;      // actual own-UDP bound port, handed to the gameplay socket
  std::string error;          // shown in the game's error box
  bool retry = false;         // the connection failed after a match was found: search again
  std::string match_id;       // a fresh id per match (never the room's code, which stays private)
  bool host = false;          // seat 0: the decider
  int local_index = 0;        // 0-based local port
  std::vector<Player> players;
  bool update_required = false; // Only an explicit update_required server code, never error text.
};

// Runs a whole search on the matchmaking thread: ticket, queue or direct room, compatibility
// exchange and app-owned links (or legacy punching). Returns when the players can reach each other, on an error, or when `cancelled` turns true.
// The room stays open afterwards for results (game_started / game_ended) until end_session().
Match find_match(const Search& search, const std::function<bool()>& cancelled);

// Room slot == Slippi player index (controller port - 1). Unknown or no room: -1.
int seat_team(int player_idx);

// The online game the room is recording started (the host asks the room to start a match).
void game_started(int64_t rng, int stage);
// One game's end, from the game's Game End record (the Source Port) or Slippi's report (the Static
// Recomp). A game is reported once, however many places say it ended.
struct GameEnd {
  int winner = -1;              // 0-based port of the single winner as the caller read it, -1 none
  int end_method = 0;           // Slippi's: 1 TIME!, 2 GAME!, 7 No Contest (LRA-Start, disconnect, desync)
  int lras = -1;                // the port a No Contest names, -1 none
  bool has_placements = false;  // placements[]: the record's four placement bytes (0 first, 0xFF none)
  int placements[4] = {-1, -1, -1, -1};
  int teams[4] = {-1, -1, -1, -1}; // actual negotiated match teams, by Slippi player index
  int stage = -1, frames = -1;  // the game's stage id and length, for the ranked report's detail
  int characters[2] = {-1, -1};
  int64_t rng = -1;             // the match's rng offset (both games agree on it): the report's id
};
void game_ended(const GameEnd& end);
// Ranked: one best-of-3 set per room (yougame_ranked.h), decided from the games the room settles.
void ranked_set_complete(uint8_t end_mode);   // C2 (a future GameSetup port)
void ranked_match_status(uint8_t status);     // C4: 40 is Slippi's "abandoned"
void ranked_game_setup();                     // the game sent a GameSetup step: retain pair for C2 + result
// Game thread, every online menu poll: a game's result is still on its way (no next game may start
// yet), or the set is decided or its room is gone (break the pair, back to the search).
bool ranked_hold();
bool ranked_set_over();
// No ranked room is open or still finishing, and one was (the --online-test ranked harness).
bool ranked_room_retired();
// This client's games with unique room receipts (the --online-test quit-after harness).
int ranked_games_received();
// The Ranked character select's header ("Ranked Mode", "Ranked - Placements 2/5",
// "Ranked - Gold II - 1534"). Each CSS visit reads mpRank at most once, with failure back-off.
std::string ranked_header();
void ranked_screen_enter();
std::array<std::string, 6> ranked_lines();
uint8_t opponent_rank();
// The last ranked standing is cached per process (E3/E5 and the match-state rank bytes).
std::vector<uint8_t> rank_response();
std::vector<uint8_t> rank_record_response(); // FE: self/opponent and the authoritative set result
uint8_t rank_visibility();
uint8_t local_rank();

// Why the game leaves the room. Quit: the local player left (leaving the online menus, holding Z,
// a new search); Lost: the connection to the opponent broke; Over: the ranked set is decided.
enum class Leave { Quit, Lost, Over };
// Leaves the room: the session is over (the game's online cleanup). Only Ranked reads `why`.
void end_session(Leave why);

// Simulation thread: starts only from a positively known idle game scene and set state.
// Once started the caller holds the scene loop until the renderer requests the normal clean exit.
// online_screen: an Online menu or online CSS is showing, so the accept line ends in " online".
bool update_safe_point(bool idle_scene, bool online_busy, bool ranked_set, bool online_screen);
bool update_handoff_visible();
void update_notice_presented(); // renderer, after drawing Updating...

// Status for logs and the YouGame page: "searching", "connecting", "playing", the opponent.
std::string status_line();

}  // namespace yougame
