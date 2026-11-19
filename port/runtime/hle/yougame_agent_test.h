// Test-only startup defaults for app-launched native games. Read agent-test.json once, only
// with an active YouGame bridge, nonempty YOUGAME_DATA and YOUGAME_AGENT_COPY=1 (an AGENT TEST app copy). Missing files are silent. Optional
// keys: mute:true, online:"unranked"|"ranked"|"direct"|"teams", character:0..25 (with online),
// consoleRollback:true|false (Source only, MELEE_CONSOLE_ROLLBACK),
// code:1..12 ASCII [A-Z0-9#], timer:5..600 seconds, quitAfter:1..3, padBack:10..900 seconds, fight:true. Invalid/unknown keys
// are ignored together in one log line. Prepend defaults so the existing CLI parser handles
// modes, character defaults and volume, and later explicit flags win. No file watching.
// Hook builds also accept script (a bounded file in the data dir; Source permits subdirectories),
// rng_seed:uint32 and time_base:uint64 (0 keeps the CLI's wall-clock default).
// Source-only hook key: match:"<stage>:<kind>[/c<level>][/x<costume>][:...]" (1..6 players,
// at most 128 ASCII [a-z0-9_:/] characters, numeric IDs; refused whenever online is present).
// bootMatch:true (Source, needs match) starts that VS match from the main menu, skipping the
// character and stage screens; results return to the main menu.
// Bare hook-build runs (no YouGame session) get the same with the CLI flag --boot-match plus --match.
// Both engines: frames:600..36000, stopAt:60..36000 (host::die at that frame: exit 3 and a last stderr
// line "crash: agent test: stop at frame N", for the app's "stopped" notice). Static-only:
// captureEvery:30..600 (agent-captures/cap.ppm).
// Source-only hook keys: frame_times, sim_times, replay_dir, record_native, slippi_menus,
// no_gc_adapter, early_rng_seed; netDelayMs:0..250, netJitterMs:0..100 (positive delay, Direct only).
// padTrace:"<relative file>" appends changed game-poll pad states (both engines).
// Output paths stay inside the data directory.
// Source hook gate 1: handoffSave:"<file>" or handoffLoad:"<file>", handoffAt:300..30000
// engine bodies after first local VS CSS (2/0); identical test-only CSS boot request in both.
// handoffHashLog:"<file>" (per-span + total every 60 bodies for 600 after handoff),
// handoffDiffDump:"<file>" (raw spans on either save or load),
// handoffKeep:[["0xADDR",positive_length],...] preserves B's bytes, bounds checked at handoff.
// Known host API words and handoffKeep are omitted from comparison hashes, never raw dumps.
// keys:"overlay@20,escape@30" emits stdio protocol taps after the first presented frame.
// Hook-only, both online engines: dropRoomAfter:330..900 seconds (agent-only rooms, never ranked).
// Hook-only, Source Friends: dropRoomOnce:true closes the room socket client-side once, 5 s into VS
// character select (agent-only rooms), so the held-seat rejoin runs without root or network tools.
// openSettings:"video" opens the Video settings tab after boot (both engines).
// uiClick:{"frame":1..36000,"x":0..16384,"y":0..16384} clicks window pixels after first present.
// openMenu:"phob:<1-4>" opens Controls -> PhobGCC on the chosen socket at boot.
// Source renderer hook: present_log:"<name>", present_frames:"<directory-name>:<1..600>".
// Source Squad Strike builds: squadLineup:[[kind,colour],[kind,colour],[kind,colour],[kind,colour]]
// (CSS kinds 0..25 except Sheik, 19, who has no portrait; a colour that fighter has; four different
// fighters). Each time the online Squad Strike character select opens, the game fills its four
// slots in that order and presses START, so lock-in runs the same path a player's picks do. An
// invalid shape is ignored with a log line. squadRefuse:true makes this copy refuse every Friends Squad
// Strike setup as if a fighter were over its budget (a guest's end-session, timed by a live pair).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include "friends_handoff_join.h"
#ifdef YOUGAME_AGENT_TEST_HOOK
#include "yougame_agent_paths.h"
#include "yougame_agent_pad_trace.h"
#include "yougame_agent_keys.h"
#include "yougame_agent_friends.h"
#include "yougame_doubles.h"
#include "yougame_agent_present.h"
#include "yougame_box_present.h"
#include "yougame_net_test.h"
#include "handoff_state.h"
#if MU_SQUAD
#include "../abi/mu_squad_wire.h"
#endif
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace yougame::agent_test {
enum class Engine { Source, Static };
#ifdef YOUGAME_AGENT_TEST_HOOK
inline std::atomic<int> phob_menu_socket{0};
inline std::atomic<bool> open_settings_video{false};
inline std::atomic<int> ui_click_frame{0}, ui_click_x{0}, ui_click_y{0};
struct UiClick { int frame = 0, x = 0, y = 0; };
struct Handoff {
  std::string save, load, hash_log, diff_dump, error;
  uint32_t at = 0;
  std::vector<handoff_state::Range> keep;
  bool enabled() const { return !save.empty() || !load.empty() || !error.empty(); }
};
inline Handoff handoff;
inline std::atomic<bool> boot_match{false};
#if MU_SQUAD
// squadLineup for the game's MU_SQUAD_OP_AGENT_LINEUP query: one byte per fighter (kind 0..25 in
// bits 0-4, colour 0..5 in bits 5-7) at bits 0..31, bit 48 set when a lineup was given. Read by the
// host's command handler, never sent to a peer.
inline constexpr size_t kSquadLineupBytes = 2 * MU_SQUAD_LINEUP; // kind, colour per fighter
static_assert(MU_SQUAD_LINEUP <= 6, "one packed byte per fighter, below the flag bit");
inline std::atomic<uint64_t> squad_lineup{0};
inline uint64_t squad_lineup_pack(const std::array<uint8_t, kSquadLineupBytes>& bytes) {
  uint64_t packed = uint64_t(1) << 48;
  for (size_t i = 0; i < MU_SQUAD_LINEUP; ++i)
    packed |= uint64_t((bytes[2 * i] & 0x1F) | (bytes[2 * i + 1] & 0x7) << 5) << (8 * i);
  return packed;
}
inline bool squad_lineup_bytes(uint8_t out[kSquadLineupBytes]) {
  const uint64_t packed = squad_lineup.load(std::memory_order_acquire);
  if (!(packed >> 48)) return false;
  for (size_t i = 0; i < MU_SQUAD_LINEUP; ++i) {
    const uint8_t fighter = uint8_t(packed >> (8 * i));
    out[2 * i] = fighter & 0x1F;
    out[2 * i + 1] = fighter >> 5;
  }
  return true;
}
#endif
#endif

struct Settings {
  std::vector<std::string> arguments;
  std::vector<std::pair<std::string, std::string>> environment;
  std::string log;
  std::string unsupported_online;
  bool agent_file = false;  // a valid agent-test.json was read: an agent runs this copy
  bool static_online_test = false;
  int pad_back_seconds = 0;
  bool fight = false;
#ifdef YOUGAME_AGENT_TEST_HOOK
  std::vector<ScriptedKey> key_script;
  std::string pad_trace;
  bool agent_script = false;
  bool ranked_cpu = false;
  bool rumble_test = false;
  bool box_present_compositor = false;
  net_test::Config net;
  agent_present::Config present;
  int stop_at_frame = 0;
  int phob_socket = 0;
  bool open_settings_video = false;
  UiClick ui_click;
  nlohmann::json console_request;
#ifdef MELEE_DOUBLES
  std::string doubles_room; bool doubles_search=false; int doubles_humans=1;
#endif
  Handoff handoff;
  bool friends_handoff_join = false; // Source-only opt-in; independent of gate-1 save/load.
  bool boot_match = false;           // Source-only, with match: VS straight from the main menu
#if MU_SQUAD
  std::array<uint8_t, kSquadLineupBytes> squad_lineup{}; // kind, colour per slot, in fight order
  bool squad_lineup_set = false;
  bool squad_refuse = false;   // squadRefuse: refuse every Friends Squad setup (hook builds)
#endif
#endif

  std::vector<std::string> prepend(int argc, char* const* argv) const {
    std::vector<std::string> result;
    result.emplace_back(argv[0]);
    result.insert(result.end(), arguments.begin(), arguments.end());
    for (int i = 1; i < argc; ++i) result.emplace_back(argv[i]);
    return result;
  }
};

inline bool integer_in(const nlohmann::json& value, int minimum, int maximum) {
  if (value.is_number_unsigned()) {
    const auto n = value.get<uint64_t>();
    return n >= static_cast<uint64_t>(minimum) && n <= static_cast<uint64_t>(maximum);
  }
  if (!value.is_number_integer()) return false;
  const auto n = value.get<int64_t>();
  return n >= minimum && n <= maximum;
}

#ifdef YOUGAME_AGENT_TEST_HOOK
inline bool script_name_valid(const std::string& name) {
  return !name.empty() && name.size() <= 64 && name.find("..") == std::string::npos &&
      name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == std::string::npos;
}

inline bool source_script_path_valid(const std::string& name) {
  return agent_paths::relative_valid(name);
}

inline bool source_match_valid(const std::string& text) {
  if (text.empty() || text.size() > 128 ||
      text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_:/") != std::string::npos) return false;
  // Dry parse source_port::set_match's base-0 numbers, six player slots and options in
  // either order. Calling set_match itself would change the process's match override.
  const char* next = text.c_str();
  auto number = [&]() {
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtol(next, &end, 0);
    if (end == next || errno == ERANGE || value < 0 || value > INT32_MAX) return false;
    next = end;
    return true;
  };
  if (!number()) return false;
  size_t players = 0;
  while (*next == ':') {
    ++next;
    if (++players > 6 || !number()) return false;
    while (*next == '/') {
      if (next[1] != 'c' && next[1] != 'x') return false;
      next += 2;
      if (!number()) return false;
    }
  }
  return *next == '\0' && players != 0;
}

inline bool early_rng_seed_valid(const std::string& text) {
  // Match both engines' base-0 strtoul grammar, but reject truncation, signs and overflow.
  const char* next = text.c_str();
  for (int field = 0; field < 2; ++field) {
    if (*next < '0' || *next > '9') return false;
    char* end = nullptr;
    errno = 0;
    const auto number = std::strtoull(next, &end, 0);
    if (errno == ERANGE || number > UINT32_MAX || (field == 1 && number == 0)) return false;
    if (field == 0) { if (*end != '@') return false; next = end + 1; }
    else if (end != text.c_str() + text.size()) return false;
  }
  return true;
}

inline bool source_output_path(const std::string& name, const std::filesystem::path& data_dir,
                               bool directory, std::filesystem::path& path,
                               const agent_paths::Root& capability) {
  (void)data_dir;
  const auto& root = capability.path;
  if (!source_script_path_valid(name) || root.empty()) return false;
  auto normalized = name;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  const auto relative = std::filesystem::u8path(normalized);
  if (relative.filename().empty() || relative.filename() == ".") return false;
  std::error_code ec;
  const auto base = std::filesystem::symlink_status(root, ec);
  if (ec || !std::filesystem::is_directory(base)) return false;
  path = root;
  for (const auto& component : relative) {
    path /= component;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return false;
    ec.clear();
    if (std::filesystem::is_symlink(status)) return false;
    if (std::filesystem::exists(status) &&
        !(path == root / relative && !directory ? std::filesystem::is_regular_file(status) :
                                                    std::filesystem::is_directory(status))) return false;
  }
  if (directory) return agent_paths::directory(root, normalized, true, capability);
  else if (!std::filesystem::is_directory(path.parent_path(), ec)) return false;
  return !ec;
}

inline bool unsigned_in(const nlohmann::json& value, uint64_t maximum) {
  if (value.is_number_unsigned()) return value.get<uint64_t>() <= maximum;
  return value.is_number_integer() && value.get<int64_t>() >= 0 &&
      static_cast<uint64_t>(value.get<int64_t>()) <= maximum;
}

#if MU_SQUAD
// squadLineup: exactly MU_SQUAD_LINEUP [kind,colour] pairs that mu_squad_side_valid accepts, with
// no fighter twice (character select refuses a repeat, so such a lineup could never lock in).
static_assert(2 * MU_SQUAD_LINEUP == 8, "squadLineup is four kind/colour pairs");
inline bool squad_lineup_value(const nlohmann::json& value, std::array<uint8_t, kSquadLineupBytes>& out) {
  constexpr int sheik = 19; // a CSS kind with no portrait: no player can pick it
  MuSquadSide side{};
  if (!value.is_array() || value.size() != MU_SQUAD_LINEUP) return false;
  side.count = MU_SQUAD_LINEUP;
  for (size_t i = 0; i < value.size(); ++i) {
    const auto& pair = value[i];
    if (!pair.is_array() || pair.size() != 2 || !integer_in(pair[0], 0, 25) || !integer_in(pair[1], 0, 5) ||
        pair[0].get<int>() == sheik)
      return false;
    side.members[i].kind = uint8_t(pair[0].get<int>());
    side.members[i].color = uint8_t(pair[1].get<int>());
    for (size_t j = 0; j < i; ++j)
      if (side.members[j].kind == side.members[i].kind) return false;
  }
  if (!mu_squad_side_valid(&side)) return false;
  for (size_t i = 0; i < MU_SQUAD_LINEUP; ++i) {
    out[2 * i] = side.members[i].kind;
    out[2 * i + 1] = side.members[i].color;
  }
  return true;
}
#endif
#endif

inline Settings map_keys(const nlohmann::json& object, Engine engine = Engine::Source,
                         const std::filesystem::path& data_dir = {}, bool receive_injector_set = false
#ifdef YOUGAME_AGENT_TEST_HOOK
                         , const agent_paths::Root* capability = nullptr
#endif
                         ) {
  Settings out;
  std::string keys, online;
#ifdef YOUGAME_AGENT_TEST_HOOK
  // Direct mapping has no load/session prerequisite. Keep its root local; load()
  // instead supplies the capability retained before agent-test.json was read.
  agent_paths::Root mapping_root;
  if (!capability) {
    if (!data_dir.empty()) mapping_root.load(data_dir);
    capability = &mapping_root;
  }
  std::string diagnostics;
  auto diagnostic = [&](const std::string& text) {
    diagnostics += "\nagent test: " + text + "; ignored";
  };
#else
  (void)data_dir;
  (void)receive_injector_set;
#endif
  int character = -1;
  bool ignored = false;
  auto applied = [&](const char* key) {
    if (!keys.empty()) keys += ", ";
    keys += key;
  };
  for (auto it = object.begin(); it != object.end(); ++it) {
    const auto& key = it.key();
    const auto& value = it.value();
#if defined(MELEE_DOUBLES) && defined(YOUGAME_AGENT_TEST_HOOK)
    if (key=="doublesHost" || key=="doublesDuoHost" || key=="doublesJoin") {
      if (engine==Engine::Source && !object.count("online") && !object.count("match") &&
          (((key=="doublesHost" || key=="doublesDuoHost") && value==true) || (key=="doublesJoin" && !friends::room(nlohmann::json{{"room",value}}).empty()))) {
        out.doubles_room=key!="doublesJoin"?"new":value.get<std::string>();
        out.doubles_search=key!="doublesJoin"; out.doubles_humans=key=="doublesDuoHost"?2:1; applied(key.c_str());
      } else ignored=true;
      continue;
    }
#endif
#if MU_SQUAD && defined(YOUGAME_AGENT_TEST_HOOK)
    if (key == "squadLineup") {
      if (engine == Engine::Source && squad_lineup_value(value, out.squad_lineup)) {
        out.squad_lineup_set = true; applied("squadLineup");
      } else {
        out.squad_lineup = {};
        diagnostics += engine == Engine::Source ? "\nagent-test: squadLineup invalid value; ignored"
                                                : "\nagent-test: squadLineup not supported on the Static Recomp; ignored";
      }
      continue;
    }
    if (key == "squadRefuse") {
      if (engine == Engine::Source && value.is_boolean()) { out.squad_refuse = value.get<bool>(); applied("squadRefuse"); }
      else diagnostics += engine == Engine::Source ? "\nagent-test: squadRefuse needs true or false; ignored"
                                                   : "\nagent-test: squadRefuse not supported on the Static Recomp; ignored";
      continue;
    }
#endif
#ifdef YOUGAME_AGENT_TEST_HOOK
    if (key == "friendsHandoffJoin") {
      if (engine == Engine::Source && value.is_boolean()) {
        out.friends_handoff_join = value.get<bool>(); applied(key.c_str());
      } else ignored = true;
      continue;
    }
    if (key == "openSettings") {
      if (value.is_string() && value == "video") {
        out.open_settings_video = true; applied("openSettings");
      } else diagnostics += "\nagent-test: openSettings invalid value; ignored";
      continue;
    }
    if (key == "uiClick") {
      if (value.is_object() && value.size() == 3 && value.count("frame") &&
          value.count("x") && value.count("y") && integer_in(value["frame"], 1, 36000) &&
          integer_in(value["x"], 0, 16384) && integer_in(value["y"], 0, 16384)) {
        out.ui_click = {value["frame"].get<int>(), value["x"].get<int>(), value["y"].get<int>()};
        applied("uiClick");
      } else diagnostics += "\nagent-test: uiClick invalid value; ignored";
      continue;
    }
    if (key == "bootMatch") {
      auto invalid = [&](const char* reason) { diagnostics += std::string("\nagent-test: bootMatch ") + reason + "; ignored"; };
      if (engine != Engine::Source) invalid("not supported on the Static Recomp");
      else if (!value.is_boolean()) invalid("invalid value");
      else if (!object.count("match") || object.count("online")) invalid("needs match, offline only");
      else { out.boot_match = value.get<bool>(); applied("bootMatch"); }
      continue;
    }
    if (key.compare(0, 7, "handoff") == 0) {
      auto invalid = [&](const char* why) { out.handoff.error = std::string("invalid ") + key + ": " + why; };
      if (engine != Engine::Source) { ignored = true; continue; }
      if (key == "handoffAt") {
        if (!integer_in(value, 300, 30000)) invalid("expected 300..30000");
        else { out.handoff.at = value.get<uint32_t>(); applied(key.c_str()); }
      } else if (key == "handoffKeep") {
        if (!value.is_array() || value.size() > 256) invalid("expected at most 256 ranges");
        else for (const auto& item : value) {
          handoff_state::Range range;
          if (!item.is_array() || item.size() != 2 || !item[0].is_string() ||
              !unsigned_in(item[1], UINT32_MAX) ||
              !handoff_state::parse_range(item[0].get<std::string>(), item[1].get<uint64_t>(), range)) {
            invalid("invalid range"); break;
          }
          out.handoff.keep.push_back(range);
        }
        if (out.handoff.error.empty()) applied(key.c_str());
      } else if (key == "handoffSave" || key == "handoffLoad" || key == "handoffHashLog" || key == "handoffDiffDump") {
        std::filesystem::path path;
        if (!value.is_string() || !source_output_path(value.get<std::string>(), data_dir, false, path, *capability))
          invalid("invalid data-directory path");
        else {
          auto& target = key == "handoffSave" ? out.handoff.save : key == "handoffLoad" ? out.handoff.load :
              key == "handoffHashLog" ? out.handoff.hash_log : out.handoff.diff_dump;
          target = agent_paths::slashes(value.get<std::string>()); applied(key.c_str());
        }
      } else invalid("unknown key");
      continue;
    }
    if (key == "console") {
      if (engine == Engine::Source && !object.count("online") && !object.count("match") && console_key(value, out.console_request)) applied("console");
      else ignored = true;
      continue;
    }
    if (key == "openMenu") {
      if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.size() == 6 && text.compare(0, 5, "phob:") == 0 && text[5] >= '1' && text[5] <= '4') {
          out.phob_socket = text[5] - '0'; applied("openMenu"); continue;
        }
      }
      ignored = true; continue;
    }
    if (key == "rankedCpu") {
      if (engine == Engine::Source && value.is_boolean()) {
        out.ranked_cpu = value.get<bool>(); applied("rankedCpu");
      } else ignored = true;
      continue;
    }
    if (key == "padTrace") {
      std::filesystem::path path;
      if (!value.is_string() || !source_output_path(value.get<std::string>(), data_dir, false, path, *capability))
        diagnostics += "\nagent-test: padTrace invalid output path; ignored";
      else { out.pad_trace = agent_paths::slashes(value.get<std::string>()); applied("padTrace"); }
      continue;
    }
    if (key == "rumbleTest" && value.is_boolean()) {
      out.rumble_test = value.get<bool>(); applied("rumbleTest"); continue;
    }
    if (key == "boxPresent" && value == "compositor") {
      out.box_present_compositor = true; applied("boxPresent"); continue;
    }
    if (key == "boxPresent" && value == "vsync") { applied("boxPresent"); continue; }   // the default since 0b002a
    if (key == "present_log" || key == "present_frames") {
      auto invalid = [&](const char* reason) { diagnostics += "\nagent-test: " + key + " " + reason + "; ignored"; };
      std::string name;
      unsigned every = 0;
      const bool frames = key == "present_frames";
      if (engine != Engine::Source) invalid("not supported on the Static Recomp");
      else if (!value.is_string() ||
          !(frames ? agent_present::frame_spec(value.get<std::string>(), name, every) :
                     agent_present::plain_name(name = value.get<std::string>()))) invalid("invalid name or interval");
      else {
        std::filesystem::path path;
        if (!source_output_path(name, data_dir, frames, path, *capability)) invalid("invalid output path");
        else {
          if (frames) { out.present.directory = name; out.present.every = every; }
          else out.present.log = name;
          applied(key.c_str());
        }
      }
      continue;
    }
    if (key == "netDelayMs" || key == "netJitterMs") {
      const bool delay = key == "netDelayMs";
      auto invalid = [&](const char* reason) { diagnostics += "\nagent-test: " + key + " " + reason + "; ignored"; };
      if (engine != Engine::Source) invalid("not supported on the Static Recomp");
      else if (!integer_in(value, 0, delay ? 250 : 100)) invalid("invalid value");
      else if (receive_injector_set) {
        // One refusal for the pair, even if both keys are present.
        if (diagnostics.find("receive-side injector") == std::string::npos)
          diagnostics += "\nagent-test: net delay keys conflict with receive-side injector; ignored";
      } else if (!delay && out.net.delay_ms == 0) {
        invalid("requires netDelayMs > 0");
      } else {
        if (delay) out.net.delay_ms = value.get<int>();
        else out.net.jitter_ms = value.get<int>();
        applied(key.c_str());
      }
      continue;
    }
    if (key == "frame_times" || key == "sim_times" || key == "replay_dir" ||
        key == "record_native" || key == "slippi_menus" || key == "no_gc_adapter" || key == "early_rng_seed") {
      auto invalid = [&](const char* reason) { diagnostics += "\nagent-test: " + key + " " + reason + "; ignored"; };
      if (engine != Engine::Source) invalid("not supported on the Static Recomp");
      else if (key == "frame_times" || key == "sim_times" || key == "replay_dir") {
        std::filesystem::path path;
        if (!value.is_string() || !source_output_path(value.get<std::string>(), data_dir, key == "replay_dir", path, *capability))
          invalid("invalid output path");
        else {
          if (key == "sim_times") out.environment.emplace_back("MELEE_SIM_TIMES", path.u8string());
          else out.arguments.insert(out.arguments.end(), {key == "frame_times" ? "--frame-times" : "--replay-dir", path.u8string()});
          applied(key.c_str());
        }
      } else if (key == "early_rng_seed") {
        if (!value.is_string() || !early_rng_seed_valid(value.get<std::string>())) invalid("invalid value");
        else { out.environment.emplace_back("MELEE_TEST_EARLY_RNG_SEED", value.get<std::string>()); applied(key.c_str()); }
      } else if (!value.is_boolean()) invalid("invalid value");
      else if (key == "slippi_menus") {
        out.arguments.insert(out.arguments.end(), {"--slippi-menus", value.get<bool>() ? "on" : "off"});
        applied(key.c_str());
      } else if (value.get<bool>()) {
        if (key == "record_native") out.arguments.emplace_back("--record-native");
        else out.environment.emplace_back("MELEE_NO_GC_ADAPTER", "1");
        applied(key.c_str());
      }
      continue;
    }
    if (key == "keys") {
      std::string why;
      if (!value.is_string()) why = "expected a string";
      else out.key_script = parse_key_script(value.get<std::string>(), why);
      if (!why.empty()) diagnostics += "\nagent-test: keys ignored: " + why;
      else applied("keys");
      continue;
    }
    if (key == "dropRoomOnce") {
      if (engine != Engine::Source || value != true) ignored = true;
      else {
        out.environment.emplace_back("MELEE_ONLINE_TEST_DROP_ROOM_ONCE", "1");
        applied("dropRoomOnce");
      }
      continue;
    }
    if (key == "dropRoomAfter") {
      if (!integer_in(value, 330, 900)) ignored = true;
      else {
        out.environment.emplace_back("MELEE_ONLINE_TEST_DROP_ROOM_AFTER", std::to_string(value.get<int>()));
        applied("dropRoomAfter");
      }
      continue;
    }
    if (key == "rng_seed" || key == "time_base") {
      const bool seed = key == "rng_seed";
      if (!unsigned_in(value, seed ? UINT32_MAX : UINT64_MAX)) {
        diagnostics += "\nagent-test: " + key + " invalid value; ignored";
      } else {
        out.arguments.insert(out.arguments.end(), {seed ? "--rng-seed" : "--time-base",
                                                   std::to_string(value.get<uint64_t>())});
        applied(seed ? "rng_seed" : "time_base");
      }
      continue;
    }
    if (key == "match") {
      auto invalid = [&](const char* reason) { diagnostics += std::string("\nagent-test: match ") + reason + "; ignored"; };
      if (engine != Engine::Source) invalid("not supported on the Static Recomp");
      else if (object.count("online")) invalid("conflicts with online (offline only)");
      else if (!value.is_string() || !source_match_valid(value.get<std::string>())) invalid("invalid value");
      else {
        out.arguments.insert(out.arguments.end(), {"--match", value.get<std::string>()});
        applied("match");
      }
      continue;
    }
    if (key == "script") {
      auto script_diagnostic = [&](const char* text) {
        diagnostics += std::string("\nagent-test: script ") + text + "; ignored";
      };
      if (!value.is_string() || !(engine == Engine::Static ? script_name_valid(value.get<std::string>()) :
                                                           source_script_path_valid(value.get<std::string>()))) {
        script_diagnostic("invalid name");
      } else if (data_dir.empty()) {
        script_diagnostic("missing data directory");
      } else {
        auto name = value.get<std::string>();
        std::replace(name.begin(), name.end(), '\\', '/');
        const auto& root = capability->path;
        const auto path = root / std::filesystem::u8path(name);
        std::string snapshot;
        if (!agent_paths::read(root, name, agent_paths::script_limit, snapshot, *capability)) {
          script_diagnostic(errno == EFBIG ? "exceeds 64 KiB" : "missing or not a regular file");
        } else {
          out.arguments.insert(out.arguments.end(), {"--script", path.u8string()});
          out.agent_script = true;
          applied("script");
        }
      }
      continue;
    }
    if (key == "stopAt") {
      if (!integer_in(value, 60, 36000)) diagnostic("stopAt invalid value");
      else { out.stop_at_frame = value.get<int>(); applied("stopAt"); }
      continue;
    }
    if (key == "captureEvery" || key == "frames") {
      const bool capture = key == "captureEvery";
#if defined(MELEE_LINUX) && !defined(__APPLE__)
      if (capture && engine == Engine::Source) {
        diagnostics += "\nagent-test: captureEvery not supported on Linux; ignored";
        continue;
      }
#endif
      // The Mac Source Port's GL renderer writes --capture-every frames like the Static Recomp does.
#ifdef __APPLE__
      const bool capture_here = true;
#else
      const bool capture_here = engine == Engine::Static;
#endif
      if ((capture && !capture_here) || !integer_in(value, capture ? 30 : 600, capture ? 600 : 36000)) {
        if (!capture && engine == Engine::Source) diagnostics += "\nagent-test: frames invalid value; ignored";
        else diagnostic(key + (engine != Engine::Static ? " not supported on the Source Port" : " invalid value"));
      } else if (capture) {
        std::filesystem::path directory;
        if (!source_output_path("agent-captures", data_dir, true, directory, *capability)) diagnostic("captureEvery cannot create directory");
        else {
          out.arguments.insert(out.arguments.end(), {"--capture", (directory / "cap.ppm").u8string(),
                                                     "--capture-every", std::to_string(value.get<int>())});
          applied("captureEvery");
        }
      } else {
        out.arguments.insert(out.arguments.end(), {"--frames", std::to_string(value.get<int>())});
        applied("frames");
      }
      continue;
    }
#endif
    if (key == "consoleRollback" && engine == Engine::Source && value.is_boolean()) {
      out.environment.emplace_back("MELEE_CONSOLE_ROLLBACK", value.get<bool>() ? "1" : "0");
      applied("consoleRollback");
    } else if (key == "mute" && value.is_boolean() && value.get<bool>()) {
      out.arguments.insert(out.arguments.end(), {"--volume", "0"});
      applied("mute");
    } else if (key == "online" && value.is_string() &&
               (engine == Engine::Static || value == "unranked" || value == "ranked" || value == "direct" || value == "teams")) {
      online = value.get<std::string>();
    } else if (key == "character" && integer_in(value, 0, 25)) {
      character = value.get<int>();
    } else if (key == "code" && value.is_string() && engine == Engine::Source) {
      const auto code = value.get<std::string>();
      if (code.empty() || code.size() > 12 || code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#") != std::string::npos) {
        ignored = true;
        continue;
      }
      out.environment.emplace_back("MELEE_ONLINE_TEST_CODE", code);
      applied("code");
    } else if (key == "fight" && value.is_boolean()) {
      out.fight = value.get<bool>();
      if (out.fight) applied("fight");
    } else if (key == "padBack" && integer_in(value, 10, 900)) {
      out.pad_back_seconds = value.get<int>();
      applied("padBack");
    } else if ((key == "timer" && integer_in(value, 5, 600)) ||
               (key == "quitAfter" && integer_in(value, 1, 3))) {
      out.environment.emplace_back(key == "timer" ? "MELEE_ONLINE_TEST_TIMER" : "MELEE_ONLINE_TEST_QUIT_AFTER",
                                   std::to_string(value.get<int>()));
      applied(key == "timer" ? "timer" : "quitAfter");
    } else {
      ignored = true;
    }
  }
  if (engine == Engine::Static) {
    if (online == "unranked" || online == "teams") {
      out.environment.emplace_back("MELEE_PRACTICE_AUTOSTART", online);
      out.static_online_test = true;
      applied("online");
    } else if (object.count("online") && object["online"].is_string()) {
      out.unsupported_online = "agent test: online " + online + " not supported on the Static Recomp";
    }
    if (character >= 0) ignored = true;
  } else if (!online.empty()) {
    applied("online");
    if (character >= 0) {
      online += ":" + std::to_string(character);
      applied("character");
    }
    out.arguments.insert(out.arguments.end(), {"--online-test", online});
  } else if (character >= 0) {
    ignored = true;  // A character alone does not opt into a match or modify an explicit CLI spec.
  }
  if (!keys.empty()) out.log = "agent-test.json applied default keys: " + keys;
  if (ignored) {
    if (out.log.empty()) out.log = "agent-test.json";
    out.log += "; invalid/unknown or inapplicable keys ignored";
  }
#ifdef YOUGAME_AGENT_TEST_HOOK
  if (out.handoff.enabled()) {
    if (!out.handoff.at || (!out.handoff.save.empty() && !out.handoff.load.empty()))
      out.handoff.error = "handoff requires handoffAt and exactly one save/load";
    if (object.count("online") || object.count("match") || object.count("console"))
      out.handoff.error = "handoff requires offline local CSS";
    const std::array<std::string, 4> names{{out.handoff.save, out.handoff.load, out.handoff.hash_log, out.handoff.diff_dump}};
    for (size_t i = 0; i < names.size(); ++i) for (size_t j = i+1; j < names.size(); ++j)
      if (!names[i].empty() && names[i] == names[j]) out.handoff.error = "handoff paths must be distinct";
  }
  if (!diagnostics.empty()) out.log += diagnostics.substr(out.log.empty() ? 1 : 0);
#endif
  return out;
}
inline Settings load(bool active, const char* data_dir, Engine engine = Engine::Source) {
  Settings out;
#ifdef YOUGAME_AGENT_TEST_HOOK
  phob_menu_socket.store(0);
  open_settings_video.store(false);
  ui_click_frame.store(0);
  ui_click_x.store(0);
  ui_click_y.store(0);
  pad_trace.stop();
  agent_present::recorder().stop();
  agent_paths::policy = {};
  agent_paths::session.reset();
  handoff = {};
  console::friends_handoff_join_test.store(false, std::memory_order_release);
#if MU_SQUAD
  squad_lineup.store(0, std::memory_order_release);
  net_test::squad_refuse.store(false, std::memory_order_release);
#endif
  // All early returns and exceptions release the root unless mapping activated a session.
  struct LoadScope {
    bool active = false;
    ~LoadScope() { if (!active) { agent_paths::policy = {}; agent_paths::session.reset(); } }
  } scope;
#endif
  if (!active || !data_dir || !*data_dir) {
#ifdef YOUGAME_AGENT_TEST_HOOK
    // Test builds say why nothing was read (a staging walk otherwise sees no line at all).
    if (std::getenv("YOUGAME_AGENT_COPY"))
      out.log = !active ? "agent-test.json not looked for: no YouGame bridge or launcher"
                        : "agent-test.json not looked for: YOUGAME_DATA is empty";
#endif
    return out;
  }
  // Only an AGENT TEST copy of the app passes YOUGAME_AGENT_COPY=1 (desktop native-policy agentGameEnv; an installed
  // app's game environment is an allowlist without it), so an agent-test.json copied into a player's data folder does
  // nothing.
  const char* agent_copy = std::getenv("YOUGAME_AGENT_COPY");
  if (!agent_copy || agent_copy[0] != '1' || agent_copy[1] != '\0') {
    std::error_code missing;
    if (std::filesystem::exists(std::filesystem::u8path(data_dir) / "agent-test.json", missing))
      out.log = "agent-test.json ignored: not an agent test copy";
    return out;
  }
  // Bound the actual read BEFORE parsing; the same handle is checked and read.
  std::array<char, 4097> bytes{};
#ifdef YOUGAME_AGENT_TEST_HOOK
  if (!agent_paths::session.load(std::filesystem::u8path(data_dir))) {
    out.log = "agent-test.json ignored: the data folder is not a plain local folder";
    return out;
  }
  const auto& root = agent_paths::session.path;
  FILE* file = agent_paths::open(root, "agent-test.json", agent_paths::Mode::Read);
  if (!file) {
    std::error_code missing;
    if (std::filesystem::exists(std::filesystem::u8path(data_dir) / "agent-test.json", missing))
      out.log = "agent-test.json ignored: could not open it";
    else
      out.log = "agent-test.json not found in the game's data folder (YOUGAME_DATA)";
    return out;
  }
  const auto size = std::fread(bytes.data(), 1, bytes.size(), file);
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
#else
  std::ifstream file(std::filesystem::u8path(data_dir) / "agent-test.json", std::ios::binary);
  if (!file) return out;
  file.read(bytes.data(), bytes.size());
  const auto size = file.gcount();
  const bool failed = file.bad();
#endif
  if (size > 4096) {
    out.log = "agent-test.json ignored: exceeds 4 KiB";
    return out;
  }
  if (failed) {
    out.log = "agent-test.json ignored: read failed";
    return out;
  }
  // The JSON lexer treats a raw NUL as EOF; reject it rather than accepting a truncated file.
  if (std::find(bytes.data(), bytes.data() + size, '\0') != bytes.data() + size) {
    out.log = "agent-test.json ignored: invalid JSON object";
    return out;
  }
  const auto object = nlohmann::json::parse(bytes.data(), bytes.data() + size, nullptr, false);
  if (object.is_discarded() || !object.is_object()) {
    out.log = "agent-test.json ignored: invalid JSON object";
    return out;
  }
  out = map_keys(object, engine, std::filesystem::u8path(data_dir),
      std::getenv("MELEE_NET_LAG_MS") != nullptr || std::getenv("MELEE_NET_JITTER_MS") != nullptr
#ifdef YOUGAME_AGENT_TEST_HOOK
      , &agent_paths::session
#endif
      );
  out.agent_file = true;
#ifdef YOUGAME_AGENT_TEST_HOOK
#ifdef MELEE_DOUBLES
  if (!out.doubles_room.empty()) doubles::ui.open(out.doubles_room,out.doubles_search,out.doubles_humans);
#endif
  if (!out.console_request.is_null()) console::requests.push(out.console_request);
  const bool active_session =
#ifdef MELEE_DOUBLES
      !out.doubles_room.empty() ||
#endif
#if MU_SQUAD
      out.squad_lineup_set ||
#endif
      !out.console_request.is_null() || !out.arguments.empty() || !out.environment.empty() || !out.key_script.empty() ||
      out.phob_socket > 0 || !out.pad_trace.empty() || out.rumble_test || out.box_present_compositor || out.static_online_test || out.pad_back_seconds > 0 || out.fight || out.net.delay_ms > 0 || out.present.enabled() || out.handoff.enabled() || out.friends_handoff_join || out.boot_match || out.open_settings_video || out.ui_click.frame > 0;
  if (!active_session) return out;
  // Consumers must reopen through the capability, not through a checked pathname.
  agent_paths::policy = {};
  agent_paths::policy.root = root;
  for (size_t i = 0; i < out.arguments.size(); ++i) {
    const auto& arg = out.arguments[i];
    if (i + 1 == out.arguments.size()) continue;
    if (arg == "--replay-dir") agent_paths::policy.replay_directory = out.arguments[i + 1];
    if (arg == "--script") agent_paths::policy.script = out.arguments[i + 1];
    if (arg == "--frame-times") agent_paths::policy.frame_times = out.arguments[i + 1];
    if (arg == "--capture") agent_paths::policy.capture_directory =
        std::filesystem::u8path(out.arguments[i + 1]).parent_path().u8string();
  }
  for (const auto& item : out.environment)
    if (item.first == "MELEE_SIM_TIMES") agent_paths::policy.sim_times = item.second;
  if (!agent_present::recorder().arm(out.present, agent_paths::session)) {
    if (!out.log.empty()) out.log += '\n';
    out.log += "agent-test: present output capability refused; ignored";
  }
  if (!pad_trace.arm(out.pad_trace, agent_paths::session)) {
    if (!out.log.empty()) out.log += '\n';
    out.log += "agent-test: padTrace output capability refused; ignored";
  }
  phob_menu_socket.store(out.phob_socket);
  open_settings_video.store(out.open_settings_video);
  ui_click_x.store(out.ui_click.x);
  ui_click_y.store(out.ui_click.y);
  ui_click_frame.store(out.ui_click.frame, std::memory_order_release);
  handoff = out.handoff;
  boot_match.store(out.boot_match, std::memory_order_release);
  console::friends_handoff_join_test.store(out.friends_handoff_join, std::memory_order_release);
#if MU_SQUAD
  if (out.squad_lineup_set) squad_lineup.store(squad_lineup_pack(out.squad_lineup), std::memory_order_release);
  net_test::squad_refuse.store(out.squad_refuse, std::memory_order_release);
#endif
  scope.active = true;
#endif
  return out;
}
}  // namespace yougame::agent_test
