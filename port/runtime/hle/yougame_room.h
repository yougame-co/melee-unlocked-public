// Room lifecycle shared verbatim by WinHTTP's adapter and offline fake-broker tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_transport.h"
#include "yougame_doubles_teams.h"
#include "yougame_lobby_setup.h"
#ifdef YOUGAME_AGENT_TEST_HOOK
#include "yougame_agent_input.h"
#include <cstdlib>
#endif
#include <memory>
#include <thread>

namespace yougame::transport {
// Shared by matchmaking and the offline duel contract. These are the pre-Teams rules:
// slots, not roster order or team numbers, determine controller ports and result ids.
inline int connection_seat(const json& participants, const std::string& connection) {
  for (const auto& p : participants)
    if (p.value("connectionId", "") == connection && p.value("localIndex", 0) == 0) return p.value("slot", -1);
  return -1;
}
inline std::vector<std::string> port_participants(const json& participants, int size) {
  std::vector<std::string> ids(size, "");
  for (const auto& p : participants) {
    const int seat = p.value("slot", -1);
    if (seat >= 0 && seat < size) ids[seat] = p.value("id", "");
  }
  return ids;
}

template<class Socket> struct RoomState {
  std::unique_ptr<Socket> socket;
  std::string url, ticket, code;
  json ticket_request;
  Recovery recovery;
  TicketRetry ticket_retry;
  Links links;
  std::thread pump;
  std::atomic<bool> stop{false};
  std::string me, host_connection, match_id;
  int revision = 0;
  json participants = json::array();  // Current server roster, including each participant's hn.
  int size = 2;
  int mode = -2;
  bool ranked = false;
  ranked::Set set;
  std::vector<std::string> port_ids;   // participant id for each port (0-based)
  // Four-seat Teams only. Singles rosters also carry team numbers; they do not enable Teams.
  // Room slot n is Slippi player_idx n / controller port n+1, independent of roster order.
  std::array<int, 4> port_teams = {{-1, -1, -1, -1}};
  // A Doubles match room's lobby (nativeTeams, muDoubles) names every seat's team; it outranks rosters.
  doubles_teams::Seats doubles;
  // The match the host's console begin froze (the start frame), which outranks the lobby; settled once
  // its result came or Rooms reset the room after it: the room plays one match, then closes (4004).
  doubles_teams::Setup setup;
  bool settled = false;
  int seat_team(int player_idx) const {
    if (size != 4 || player_idx < 0 || player_idx >= 4) return -1;
    if (setup.known) return setup.team[player_idx];
    return doubles.known ? doubles.team[player_idx] : port_teams[player_idx];
  }
  // The room teams a result is checked against: the start's when known, else the roster's.
  const int* result_teams() const { return setup.known ? setup.team.data() : port_teams.data(); }
  json pending_team_result;             // legacy socket's complete Teams finish outcome
  bool pending_begin = false;
  uint64_t begin_at = 0;               // when a refused begin may be tried again
  bool in_game = false;                // between game_started and its one game_ended
  Games games;
  int pending_winner = -2;             // -2 none, -1 no single winner, else 0-based port
  // A Friends lobby match (yougame_lobby_setup.h): the plan from the room's frozen setup. Its result is
  // the full ranking by setup id with the setup echoed (lobby::finish_report), as pending_team_result.
  std::shared_ptr<const lobby::Plan> lobby;
};

struct RoomIO {
  Request request;
  std::function<uint64_t()> clock = now_ms;
  std::function<void(int)> pause = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
  std::function<int(unsigned)> link_state;
  std::function<void()> disable_links = [] {};
  // The room relay (yougame_room_relay.h) carries this room's match instead of links: it ends wherever
  // the links would, including when the room has no link ids at all.
  std::function<void()> end_relay = [] {};
  std::function<void(const char*)> log = [](const char*) {};
  std::function<void(const json&)> result = [](const json&) {};
  std::function<void(const std::string&)> started = [](const std::string&) {};
  std::function<void()> settled = [] {};  // a Doubles match room's one match is over (called once, unlocked)
  // Every room message, under the room mutex: a Friends lobby room keeps its lobby current through the
  // match and between matches (yougame_lobby.h Session::receive), so the next select starts from it.
  std::function<void(const json&)> message = [](const json&) {};
  // Each turn, under the room mutex: frames the room's own lobby session owes (appended to the turn's sends).
  std::function<void(std::vector<json>&)> tick = [](std::vector<json>&) {};
  // When the pump stops: true hands the links on (the Friends lobby's next search adopts the room) instead
  // of closing them.
  std::function<bool()> keep_links = [] { return false; };
  // Our own room socket (no app broker) dropped with neither the room nor us closing it: the pump
  // rejoins on a fresh ticket instead of ending the room. A relay match has about 7 s before Slippi
  // gives up on the peer, and Rooms hands a signed-in seat to its new socket at once.
  std::function<bool()> own_drop = [] { return false; };
  // Own socket mid-match: when it last heard anything / a relay frame (0 = never), and closing it as a drop.
  std::function<uint64_t()> own_heard = [] { return uint64_t{0}; };
  std::function<uint64_t()> own_relay_heard = [] { return uint64_t{0}; };
  std::function<void()> own_abandon = [] {};
};
// A Doubles match room's one match is over: its result, or Rooms ending the room after the start.
template<class Room> bool settles(const Room& r, const json& m) {
  if (!r.setup.known || r.settled) return false;
  const auto t = str(m, "t");
  return (t == "result" && str(m, "matchId") == r.setup.match) || (t == "teams-dissolved" && str(m, "reason") == "reset");
}
// What the room is waiting for: a begin (the host, once a game starts) and the finish. Decided
// under the room mutex, sent after it is released (a stalled socket must not hold up the game thread).
template<class Room> std::vector<json> due_messages(Room& r, uint64_t now) {
  if (r.ranked) return r.set.due(now, r.me == r.host_connection, r.revision);
  if (r.socket->brokered()) return r.games.due(now, r.me == r.host_connection, r.revision, r.port_ids);
  std::vector<json> out;
  if (r.pending_begin && r.me == r.host_connection && r.match_id.empty() && now >= r.begin_at) {
    out.push_back({{"t", "begin"}, {"revision", r.revision}});
    r.pending_begin = false;
  }
  if (r.pending_winner != -2 && !r.match_id.empty()) {
    json finish = {{"t", "finish"}, {"matchId", r.match_id}};
    if (r.pending_winner >= 0 && r.pending_winner < (int)r.port_ids.size()) finish["winner"] = r.port_ids[r.pending_winner];
    else finish["void"] = true;
    out.push_back(finish);
    r.pending_winner = -2;
    r.match_id.clear();
  }
  if (!r.pending_team_result.is_null() && !r.match_id.empty()) {
    json finish = r.pending_team_result;
    finish["t"] = "finish"; finish["matchId"] = r.match_id;
    out.push_back(std::move(finish));
    r.pending_team_result = nullptr;
    r.match_id.clear();
  }
  return out;
}

template<class Room> void read_seat_teams(Room& r, const json& m) {
  if (r.size != 4) { r.port_teams.fill(-1); return; }
  if (m.count("participants") && m["participants"].is_array()) {
    r.port_teams.fill(-1);   // a missing team in a fresh roster must not retain an old assignment
    for (const auto& p : m["participants"]) {
      if (!p.is_object() || !p.count("slot") || !p["slot"].is_number_integer()) continue;
      const auto slot = p["slot"].get<int64_t>();
      if (slot < 0 || slot >= r.size || slot >= 4) continue;
      if (p.count("team") && p["team"].is_number_integer() && (p["team"] == 0 || p["team"] == 1))
        r.port_teams[slot] = p["team"].get<int>();
    }
  }
}

template<class Room> void read_participants(Room& r, const json& m) {
  if (m.count("participants"))
    r.participants = m["participants"].is_array() ? m["participants"] : json::array();
  else {
    const auto t = str(m, "t");
    if (t == "welcome" || t == "lobby" || t == "join" || t == "back" || t == "leave" || t == "away")
      r.participants = json::array();  // Membership changed without a usable roster.
  }
}
template<class Room> void read_roster(Room& r, const json& m) {
  read_participants(r, m);
  read_seat_teams(r, m);
  if (r.size == 4) doubles_teams::read(m, r.doubles);
  if (m.count("host") && m["host"].is_string()) r.host_connection = m["host"].get<std::string>();
  if (m.count("revision") && m["revision"].is_number_integer()) r.revision = m["revision"].get<int>();
}

inline std::string leave_id(const json& m, bool brokered) {
  return brokered && m.count("player") ? str(m["player"], "id") : str(m, "id");
}
template<class Room> void end_links(Room& r, const RoomIO& io) {
  io.end_relay();
  if (r.links.ids.empty()) return;
  io.disable_links();
  close_links(r.links, io.request);
}
template<class Room> bool reopen_room(Room& r, const RoomIO& io) {
  const auto open = [&] {
    const auto left = r.recovery.remaining(io.clock());
    std::string error;
    return left && !r.stop && r.socket->open_session(r.url, r.ticket, r.code, &error, (int)std::min<uint64_t>(2000, left));
  };
  const auto renew = [&] {
    const auto left = r.recovery.remaining(io.clock());
    if (!left || r.stop) return;
    renew_room_ticket(r.ticket, r.code, [&] {
      return io.request(r.ticket_request, (int)std::min<uint64_t>(3000, left));
    }, io.log); // a failed renewal leaves the current ticket available
  };
  try { return r.ticket_retry.attempt(open, renew); } catch (...) { return false; }
}

#ifdef YOUGAME_AGENT_TEST_HOOK
inline std::atomic<bool> agent_drop_room_attempted{false};
inline int agent_drop_room_seconds(const char* value) {
  if (!value || !*value) return 0;
  int seconds = 0;
  for (const char* p = value; *p; ++p) {
    if (*p < '0' || *p > '9' || seconds > 900) return 0;
    seconds = seconds * 10 + (*p - '0');
  }
  return seconds >= 330 && seconds <= 900 ? seconds : 0;
}
// Called with the room mutex held; claim the attempt process-wide, including refusals.
template<class Room> bool agent_drop_room_due(Room& r, const RoomIO& io, int seconds) {
  if (!seconds || agent_drop_room_attempted || !r.socket->brokered() || r.socket->closed() ||
      !r.in_game || r.match_id.empty()) return false;
  const auto now = io.clock(), first = r.socket->agent_test_first_open_at();
  if (now < first || now - first < uint64_t(seconds) * 1000) return false;
  if (agent_drop_room_attempted.exchange(true)) return false;
  const auto state = agent_test::room_input_state(0, r.mode, r.ranked, r.participants, r.me);
  if (state.mode < 1 || state.mode > 4 || !state.players || !state.all_agents ||
      !agent_test::gameplay_input_allowed(state)) {
    io.log("agent-test: dropRoomAfter refused: not an agent-only room");
    return false;
  }
  io.log(("agent-test: dropped the room socket (dropRoomAfter " + std::to_string(seconds) + " s)").c_str());
  return true;
}
// dropRoomOnce (Friends console rooms): once per process, after 300 simulation frames on VS character
// select, only when every seat in the server's roster is an agent. Claims the attempt, refusals included.
inline std::atomic<bool> agent_drop_room_once_attempted{false};
inline bool agent_drop_room_once_enabled(const char* value) { return value && value[0] == '1' && !value[1]; }
template<class Json, class Log>
bool agent_drop_room_once_due(bool enabled, uint32_t css_frames, const Json& roster, Log log) {
  if (!enabled || css_frames < 300 || agent_drop_room_once_attempted) return false;
  if (agent_drop_room_once_attempted.exchange(true)) return false;
  const auto me = roster.is_object() && roster.count("me") && roster["me"].is_string() ?
      roster["me"].template get<std::string>() : std::string();
  const auto state = agent_test::room_input_state(0, 1, false,
      roster.is_object() && roster.count("participants") ? roster["participants"] : Json(), me);
  if (state.players < 2 || !state.all_agents) {
    log("agent-test: dropRoomOnce refused: not an agent-only room");
    return false;
  }
  log("agent-test: dropped the room socket (dropRoomOnce at character select)");
  return true;
}
#endif

// Our own socket's way back into the room after a drop: the match's ticket is usually past its five
// minutes by then, so a fresh one first, then the same socket reopened, all within 6 s.
template<class Room> bool rejoin_own(Room& r, const RoomIO& io) {
  const uint64_t start = io.clock(), until = start + 6000;
  io.log("yougame: room socket dropped; rejoining the room");
  bool renewed = false;
  while (!r.stop && io.clock() < until) {
    if (!renewed && !r.ticket_request.is_null()) {
      const auto left = until - io.clock();
      try {
        renewed = renew_room_ticket(r.ticket, r.code, [&] {
          return io.request(r.ticket_request, (int)std::min<uint64_t>(3000, left));
        }, io.log);
      } catch (...) {}
    }
    if (r.stop || io.clock() >= until) break;
    std::string error;
    if (r.socket->open_session(r.url, r.ticket, r.code, &error, (int)std::min<uint64_t>(2000, until - io.clock()))) {
      io.log(("yougame: back in the room after " + std::to_string(io.clock() - start) + " ms").c_str());
      return true;
    }
    if (io.clock() < until) io.pause((int)std::min<uint64_t>(250, until - io.clock()));
  }
  io.log("yougame: could not rejoin the room");
  return false;
}

// A link ending does not end the room: ranked still owes reports and must hear its result.
template<class Room> void pump_room(Room* r, std::mutex& mutex, const RoomIO& io) {
  uint64_t last_ping = 0, reopen_at = 0, probe_at = 0;
  int own_rejoins = 0;
  bool rejoin_pending = false, awaiting_welcome = false, links_ended = false, room_closed = false, recovered_match_known = false;
  std::string recovered_match;
  const bool broker = r->socket->brokered();
#ifdef YOUGAME_AGENT_TEST_HOOK
  const int drop_room_seconds = agent_drop_room_seconds(std::getenv("MELEE_ONLINE_TEST_DROP_ROOM_AFTER"));
#endif
  while (!r->stop) {
#ifdef YOUGAME_AGENT_TEST_HOOK
    bool drop_room = false;
    { std::lock_guard<std::mutex> lock(mutex); drop_room = agent_drop_room_due(*r, io, drop_room_seconds); }
    if (drop_room) r->socket->agent_test_drop_room();
#endif
    // Own socket, a match passing relay frames through it: 1 s of silence sends a ping; no answer in
    // another 1 s means our connection died (TCP takes seconds to say so, and the other seat gives up
    // after a 7 s stall). A pong means the socket is fine and the silence is the other seat's.
    if (!broker && !r->socket->closed()) {
      const uint64_t now = io.clock(), heard = io.own_heard(), relay = io.own_relay_heard();
      const auto since = [now](uint64_t t) { return t >= now ? uint64_t{0} : now - t; };   // stamps from the reader thread
      if (!relay || since(relay) >= 10000) probe_at = 0;
      else if (probe_at && heard >= probe_at) probe_at = 0;
      else if (!probe_at && since(heard) >= 1000) { r->socket->send({{"t", "ping"}}); probe_at = now; }
      else if (probe_at && now - probe_at >= 1000) {
        io.log("yougame: the room stopped answering mid-match; reopening the socket");
        io.own_abandon(); probe_at = 0;
      }
    }
    json m;
    const bool got = r->socket->next(m, broker ? 100 : 250);
    if (!got && r->socket->closed()) {
      { std::lock_guard<std::mutex> lock(mutex); r->participants = json::array(); }
      if (!broker) {
        // Three rejoins per room at most: a room that keeps dropping us ends as before.
        if (r->stop || own_rejoins >= 3 || !io.own_drop()) { room_closed = true; break; }
        ++own_rejoins;
        if (!rejoin_own(*r, io)) { room_closed = true; break; }
        continue;
      }
      if (!r->recovery.dropped(io.clock(), r->socket->recoverable())) { room_closed = true; break; }
      awaiting_welcome = rejoin_pending = true;
      if (io.clock() >= reopen_at) {
        reopen_room(*r, io);
        reopen_at = io.clock() + 1000;
      }
      const auto left = r->recovery.remaining(io.clock());
      if (r->socket->closed() && left && !r->stop) io.pause((int)std::min<uint64_t>(100, left));
    }
    std::vector<json> out;
    bool stop_links = false, settled_now = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (got && settles(*r, m)) { r->settled = settled_now = true; }
      if (got) io.message(m);
      if (got) {
        const auto t = str(m, "t");
        if (broker && (t == "ended" || t == "end" || t == "kick" || t == "kicked" || t == "error" || m.count("setOver"))) stop_links = true;
        if (broker && t == "leave" && !r->recovery.leave(m, io.clock(), !r->match_id.empty())) stop_links = true;
        if (broker && r->recovery.pending() && (t == "welcome" || t == "join" || t == "back" || t == "lobby")) {
          if (!awaiting_welcome || t == "welcome") {
            const bool recovered = r->recovery.restore(m, io.clock(), !links_ended, r->ranked);
            if (r->recovery.expired(io.clock())) room_closed = true;
            if (t == "welcome" && !r->recovery.expired(io.clock())) {
              awaiting_welcome = false;
              recovered_match = str(m, "matchId"); recovered_match_known = true;
            }
            if (recovered) {
              awaiting_welcome = false;
              if (rejoin_pending && r->ranked) r->set.rejoined();
              rejoin_pending = false; r->ticket_retry = {}; reopen_at = 0;
            }
          }
        }
        if (broker && t == "welcome" && !r->ranked) r->games.receive(m);
        if (broker && !awaiting_welcome && !r->recovery.pending() && !r->recovery.expired(io.clock()) && recovered_match_known) {
          r->match_id = std::move(recovered_match); recovered_match_known = false;
          if (r->match_id.empty()) r->recovery.result(); else r->recovery.start(r->match_id);
          r->pending_begin = false;
          if (r->ranked) {
            r->set.receive({{"t", "start"}, {"matchId", r->match_id}});
            if (!r->match_id.empty()) io.started(r->match_id);
          }
          else r->games.started(r->match_id);
        }
        if (r->ranked && r->set.receive(m)) io.result(m);
        if (t == "start") {
          io.log("yougame: room started a match");
          if (r->ranked) io.started(str(m, "matchId"));
          r->match_id = str(m, "matchId");
          if (broker) {
            r->recovery.start(r->match_id);
            if (!r->ranked) r->games.started(r->match_id);
            r->pending_begin = false;
          }
        } else if (t == "result" || t == "game-result") {
          io.log("yougame: room recorded the result");
          if (broker && t == "result") {
            if (!r->ranked) r->games.receive(m);
            if (str(m, "matchId") == r->match_id) { r->match_id.clear(); r->recovery.result(); }
          }
        } else if (t == "command-error" && r->setup.known && str(m, "action") == "begin") {
          // A Doubles begin sent just before the start ("A match is already active or complete"): the match
          // the start froze is the one being played, so the refusal is expected and nothing is retried.
        } else if (t == "command-error") {
          const auto action = str(m, "action");
          // Human text is bounded plain ASCII for logs only; never a protocol decision.
          io.log(("yougame: room refused a command: " + ranked::log_text(action, 32) + ": " + ranked::log_text(str(m, "error"), 160)).c_str());
          if (r->ranked) r->set.refused(m, io.clock());
          else if (broker && r->games.refused(m)) { r->match_id.clear(); r->recovery.result(); }
          if (!r->ranked && action == "begin" && (r->in_game || (broker && r->pending_winner != -2))) {
            r->pending_begin = true; r->begin_at = io.clock() + 2000;
          }
        }
        if (broker || t == "lobby" || t == "join" || t == "back" || t == "leave" || t == "away") read_roster(*r, m);
        else {
          read_participants(*r, m);
          if (t == "welcome" || t == "start") read_seat_teams(*r, m);
        }
      }
      for (unsigned id : r->links.ids) if (io.link_state(id) == 3) stop_links = true;
      if (stop_links) {
        links_ended = true;
        // A definitive peer departure cancels its grace, while our own socket can still recover.
        if (!awaiting_welcome) { r->recovery.cancel_wait(); r->ticket_retry = {}; reopen_at = 0; }
      }
      if (broker && r->recovery.expired(io.clock())) room_closed = true;
      if (!room_closed && !r->recovery.pending() && !awaiting_welcome && !r->socket->closed()) {
        out = due_messages(*r, io.clock());
        io.tick(out);
      }
    }
    if (settled_now) { io.log("yougame: the Doubles match is over; the room closes behind it"); io.settled(); }
    if (stop_links) end_links(*r, io);
    if (room_closed) break;
    if (!r->socket->closed() && io.clock() - last_ping > 5000) {
      out.push_back({{"t", "ping"}, {"at", io.clock()}}); last_ping = io.clock();
    }
    for (auto& j : out) {
      const bool sent = r->socket->send(j);
      if (r->ranked && sent) {
        std::lock_guard<std::mutex> lock(mutex);
        r->set.sent(j);
      }
      if (r->ranked && !sent) {
        io.log("yougame: ranked command delivery failed; retrying after backoff");
        break; // in particular, do not report a subgame whose game-start could not be sent
      }
    }
  }
  bool settled;
  { std::lock_guard<std::mutex> lock(mutex); settled = r->settled; }
  // After a Doubles match's result Rooms closes the room (4004): a normal end. The links stay up for the
  // game's own exit from its results, so no seat sees its partners drop first.
  if (room_closed && settled) io.log("yougame: the Doubles room closed after its match (a normal end)");
  else if (room_closed || !io.keep_links()) end_links(*r, io);
  if (room_closed) {
    std::lock_guard<std::mutex> lock(mutex);
    r->participants = json::array();
    if (r->ranked) r->set.room_closed();
  }
}

// Join before the at-once flush: open/next/send have one pump owner until it has stopped.
template<class Room> void stop_and_flush(Room& r, std::mutex& mutex) {
  r.stop = true;
  if (r.pump.joinable()) r.pump.join();
  for (;;) {
    std::vector<json> last;
    { std::lock_guard<std::mutex> lock(mutex); last = r.set.flush(); }
    if (last.empty()) break;
    for (const auto& j : last) {
      if (!r.socket->send(j)) return;
      std::lock_guard<std::mutex> lock(mutex);
      r.set.sent(j);
    }
  }
}

// A quit can become a concession only after the pump receives its last game receipt.
// Transfer socket ownership to the flush at that point, just as for an immediate quit.
template<class Room> bool drain_ranked_room(Room& r, std::mutex& mutex, const RoomIO& io, uint64_t timeout) {
  const uint64_t until = io.clock() + timeout;
  for (;;) {
    bool pending, concede;
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending = r.set.needs_drain();
      concede = r.set.leaves_at_once();
    }
    if (!pending) return true;
    if (concede) { stop_and_flush(r, mutex); return true; }
    if (io.clock() >= until) return false;
    io.pause(50);
  }
}
} // namespace yougame::transport
