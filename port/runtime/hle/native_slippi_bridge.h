// Command ids, payload bounds and the call contract the native game uses to reach the Slippi host.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "../abi/mu_ranked_wire.h"
#include "../abi/mu_pick_preview.h"
#include "../abi/mu_lobby_wire.h"
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#include "../abi/mu_squad_wire.h"
#endif

namespace slippi::online {

enum Cmd : uint8_t {
  CMD_ONLINE_INPUTS = 0xB0, CMD_CAPTURE_SAVESTATE = 0xB1, CMD_LOAD_SAVESTATE = 0xB2, CMD_GET_MATCH_STATE = 0xB3, CMD_FIND_OPPONENT = 0xB4,
  CMD_SET_MATCH_SELECTIONS = 0xB5, CMD_OPEN_LOGIN = 0xB6, CMD_LOGOUT = 0xB7, CMD_UPDATE = 0xB8, CMD_GET_ONLINE_STATUS = 0xB9,
  CMD_CLEANUP_CONNECTION = 0xBA, CMD_SEND_CHAT_MESSAGE = 0xBB, CMD_GET_NEW_SEED = 0xBC, CMD_REPORT_GAME = 0xBD, CMD_FETCH_CODE_SUGGESTION = 0xBE,
  CMD_OVERWRITE_SELECTIONS = 0xBF, CMD_GP_COMPLETE_STEP = 0xC0, CMD_GP_FETCH_STEP = 0xC1, CMD_REPORT_SET_COMPLETE = 0xC2,
  CMD_GET_PLAYER_SETTINGS = 0xC3, CMD_REPORT_MATCH_STATUS_UPDATE = 0xC4, CMD_GET_DELAY = 0xD5,
  CMD_GET_RANK = 0xE3, CMD_FETCH_RANK = 0xE4, CMD_GET_RANK_VISIBILITY = 0xE5,
  // This fork's empty-payload query (0xF0-0xF9 belong to the Source Port host): flags bit 0
  // offers Ranked, bit 1 Friends, bit 2 Ranked presentation; then six 128-byte game-text lines
  // (header, opponent, score, stage note, result rating, result change), each NUL-terminated.
  CMD_YOUGAME_RANKED = 0xFA,
  // Fixed-size versioned replies, documented field-by-field in mu_ranked_wire.h.
  CMD_YOUGAME_DATA = MU_YG_COMMAND,
  CMD_YOUGAME_FRIENDS_MENU = 0xE6,
  CMD_YOUGAME_SLIPPI_LINK = 0xE7, // 0 CSS query, 1 Y; 2 menu refusal query, 3 Y; 4 leave
  // Upstream's online-menu query (Melee Unlocked 1.0 numbers it 0xE7, taken above by the Slippi
  // link): the launcher's split rows and Coming soon (slippi_online.cpp, shim/mu_slippi_menu.c).
  CMD_YOUGAME_MENU = 0xEF,
};

// Results of a native command call, mirrored by the host ABI comment in mu_host.h.
enum : int32_t {
  NATIVE_COMMAND_OK = 0,
  NATIVE_COMMAND_UNKNOWN = -1,
  NATIVE_COMMAND_BOUNDS = -2,
  NATIVE_COMMAND_NEEDS_NATIVE_ROLLBACK = -3,
};

namespace bridge_detail {
inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
}  // namespace bridge_detail

// Fixed EXI payload sizes. Unknown commands pass here; the dispatcher reports them itself.
inline bool command_payload_valid(uint8_t cmd, const uint8_t* payload, uint32_t payload_len) {
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  if (cmd == MU_SQUAD_COMMAND) {
    if (!payload || !payload_len) return false;
    switch (payload[0]) {
      case 0: return payload_len == 2 && payload[1] <= 1; // chosen row
      // Count zero is a bounded invalidation transaction: the handler clears the
      // previous lineup and rejects lock-in. Do not filter it out before dispatch.
      case 1: return payload_len >= 2 && payload[1] <= MU_SQUAD_LINEUP &&
                     payload_len == 2u + 2u * payload[1]; // own lineup / clear
      case 2: return payload_len == 9; // begin budget digest, two u32 BE budgets
      case 3: return payload_len >= 10 && payload_len <= 137; // file fact
      case MU_SQUAD_OP_LOBBY_MODE: case 4: case 5: return payload_len == 1; // finish digest / read setup
      case MU_SQUAD_OP_FRIEND: return payload_len == 2 && payload[1] <= MU_SQUAD_FRIEND_INTENT;
      case MU_SQUAD_OP_CONSOLE: // off / arm, or a costume fact laid out as op 3's
        return (payload_len == 2 && payload[1] <= MU_SQUAD_CONSOLE_ARM) ||
               (payload_len >= 11 && payload_len <= 138 && payload[1] == MU_SQUAD_CONSOLE_FACT);
      default: return false;
    }
  }
#endif
  // Live pick previews: {send, wire} every character-screen frame, or {read}.
  if (cmd == MU_PICK_PREVIEW_COMMAND)
    return payload && ((payload_len == 1u + MU_PICK_PREVIEW_WIRE && payload[0] == MU_PICK_PREVIEW_OP_SEND) ||
                       (payload_len == 1 && payload[0] == MU_PICK_PREVIEW_OP_READ));
  // The 4-door Friends lobby select (mu_lobby_wire.h): exact sizes, every field in range.
  if (cmd == MU_LOBBY_COMMAND) return mu_lobby_request_ok(payload, payload_len) != 0;
  // Reason byte: 1 the CSS's hold-Z, 2 the Friends lobby's match is over and its select is back (the room stays).
  if (cmd == CMD_CLEANUP_CONNECTION)
    return payload_len == 0 || (payload_len == 1 && payload && (payload[0] == 1 || payload[0] == 2));
  uint32_t expected = 0;
  switch (cmd) {
    // Slippi 3.7.0: + the input source port and the leaving flag (27); the native local partner
    // appends its nine pad bytes (PAD_WIRE_SIZE: inputs and RNG bits).
    case CMD_ONLINE_INPUTS: return payload && (payload_len == 27 || payload_len == 36);
    case CMD_CAPTURE_SAVESTATE: case CMD_LOAD_SAVESTATE: expected = 32; break;
    case CMD_FIND_OPPONENT: expected = 19; break;
    case CMD_SET_MATCH_SELECTIONS: expected = 9; break;
    case CMD_YOUGAME_FRIENDS_MENU: case CMD_YOUGAME_SLIPPI_LINK: expected = 1; break;
    case CMD_YOUGAME_DATA:
      expected = 1;
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
      if (payload && payload_len && payload[0] == MU_YG_SUB_PRACTICE) expected = 2;
#endif
      break;
    case CMD_SEND_CHAT_MESSAGE: expected = 2; break;
    case CMD_REPORT_GAME: expected = 368; break;
    case CMD_FETCH_CODE_SUGGESTION: expected = 31; break;
    case CMD_OVERWRITE_SELECTIONS: expected = 14; break;
    case CMD_GP_COMPLETE_STEP: expected = 5; break;
    case CMD_GP_FETCH_STEP: case CMD_REPORT_SET_COMPLETE:
    case CMD_REPORT_MATCH_STATUS_UPDATE: expected = 1; break;
    case CMD_GET_MATCH_STATE: case CMD_OPEN_LOGIN: case CMD_LOGOUT: case CMD_UPDATE:
    case CMD_GET_ONLINE_STATUS: case CMD_CLEANUP_CONNECTION: case CMD_GET_NEW_SEED:
    case CMD_GET_PLAYER_SETTINGS: case CMD_GET_DELAY: case CMD_GET_RANK:
    case CMD_FETCH_RANK: case CMD_GET_RANK_VISIBILITY: case CMD_YOUGAME_RANKED:
    expected = 0; break;
    // The online menu's upstream query; one byte when the menu reports its pick (slippi_online.cpp).
    case CMD_YOUGAME_MENU: return payload_len == 0 || (payload_len == 1 && payload);
    default: return true;
  }
  if (payload_len != expected || (expected && !payload)) return false;
  if (cmd == CMD_LOAD_SAVESTATE) {
    // The preserve-block list ends with a zero address. The last 4-byte slot cannot hold a
    // nonzero address because there is no room for its length.
    for (uint32_t i = 4; i + 4 <= expected; i += 8) {
      if (bridge_detail::be32(payload + i) == 0) return true;
      if (i + 8 > expected) return false;
    }
    return false;
  }
  return true;
}

// The whole native call contract. `handle(cmd, payload, size, reply)` returns false for commands
// it does not own. Savestate commands are refused until the host can snapshot the native game.
template <class Handler>
int32_t native_command(uint8_t command, const uint8_t* payload, uint32_t payload_size,
                       uint8_t* response, uint32_t response_capacity, uint32_t* response_size,
                       uint32_t minimum_capacity, bool native_savestates, Handler&& handle) {
  if (!response_size) return NATIVE_COMMAND_BOUNDS;
  *response_size = 0;
  if (!native_savestates && (command == CMD_CAPTURE_SAVESTATE || command == CMD_LOAD_SAVESTATE))
    return NATIVE_COMMAND_NEEDS_NATIVE_ROLLBACK;
  if (!response || response_capacity < minimum_capacity || (payload_size && !payload))
    return NATIVE_COMMAND_BOUNDS;
  if (!command_payload_valid(command, payload, payload_size)) {
#if MU_SQUAD
    // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    // A malformed selection must invalidate match 1 too. Dispatch only the
    // canonical bounded clear, never the rejected bytes, then retain the error.
    if (command == MU_SQUAD_COMMAND && payload_size && payload[0] == 1) {
      const uint8_t clear[] = {1, 0};
      std::vector<uint8_t> ignored;
      handle(command, clear, sizeof clear, ignored);
    }
#endif
    return NATIVE_COMMAND_BOUNDS;
  }
  std::vector<uint8_t> reply;
  if (!handle(command, payload, payload_size, reply)) return NATIVE_COMMAND_UNKNOWN;
  if (reply.size() > response_capacity) return NATIVE_COMMAND_BOUNDS;
  if (!reply.empty()) std::memcpy(response, reply.data(), reply.size());
  *response_size = (uint32_t)reply.size();
  return NATIVE_COMMAND_OK;
}

}  // namespace slippi::online
