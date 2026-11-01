// YouGame online (see yougame_link.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "yougame_link.h"
#include "yougame_udp.h"
#include "yougame_embed_toplevel.h"
#include "opponent_display.h"
#include "yougame_parity.h"
#include "yougame_shortcuts.h"
#include "yougame_relay_pads.h"
#include "yougame_relay_rumble.h"
#include "yougame_agent_rumble.h"
#include "yougame_ux.h"
#include "yougame_friends.h"
#include "yougame_slippi_link.h"
#include "yougame_compatibility.h"
#include "slippi_online.h"
#include "yougame_exit_reason.h"
#include "yougame_ranked.h"
#include "yougame_room.h"
#include "yougame_console_link.h"
#include "yougame_console_card.h"
#include "yougame_console_entry.h"
#include "yougame_update.h"
#include "yougame_blend.h"
#include "yougame_search.h"
#include "gameplay_policy.h"
#ifdef YOUGAME_AGENT_TEST_HOOK
#include "yougame_agent_static.h"
#endif
#include <enet/yg_enet_socket.h>
#include "host.h"
#include "lcancel.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <xinput.h>
// After the Windows headers: it brings <windows.h> in through input_bindings.h.
#include "yougame_controller_relay.h"
#pragma comment(lib, "winhttp.lib")
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

using json = nlohmann::json;

namespace yougame {
namespace {

#ifdef _WIN32
using SocketLength = int;
#else
using SocketLength = socklen_t;
#endif

uint64_t now_ms() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void log_friend_version() {
  if (friends::events.report_version()) host::log("friends: friend on a different version");
}

uint64_t random64() {
  static std::mutex m;
  static std::mt19937_64 rng(((uint64_t)std::random_device{}() << 32) ^ std::random_device{}() ^ now_ms());
  std::lock_guard<std::mutex> lock(m);
  return rng();
}

std::string hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
  return buf;
}

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

struct Url {
  std::wstring host, path;
  INTERNET_PORT port = 0;
  bool secure = false;
};
bool parse_url(std::string value, Url& out) {
  if (value.rfind("wss://", 0) == 0) value.replace(0, 6, "https://");
  else if (value.rfind("ws://", 0) == 0) value.replace(0, 5, "http://");
  std::wstring w = wide(value);
  URL_COMPONENTS u{};
  u.dwStructSize = sizeof u;
  u.dwHostNameLength = u.dwUrlPathLength = u.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(w.c_str(), (DWORD)w.size(), 0, &u)) return false;
  out.host.assign(u.lpszHostName, u.dwHostNameLength);
  out.path.assign(u.lpszUrlPath, u.dwUrlPathLength);
  if (u.dwExtraInfoLength) out.path.append(u.lpszExtraInfo, u.dwExtraInfoLength);
  if (out.path.empty()) out.path = L"/";
  out.port = u.nPort;
  out.secure = u.nScheme == INTERNET_SCHEME_HTTPS;
  return true;
}

HINTERNET http_session() {
  static HINTERNET session = [] {
    HINTERNET s = WinHttpOpen(L"MeleeUnlocked-YouGame/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);
    return s;
  }();
  return session;
}

// ------------------------------------------------------------------ the desktop app's bridge
// Taken out of the environment once (start()), so a program the game opens never inherits the token.
std::string g_bridge_url, g_bridge_token;
bool g_stdio = false, g_embedded = false;
bool g_embed_toplevel = false;
uint32_t g_app_pid = 0;
#ifdef _WIN32
bool g_toplevel_dpi_failed = false;
DWORD g_toplevel_dpi_error = 0;
#endif

// Stdio mode: requests go out on stdout as `yg {"id":N,...}` lines and answers come back on stdin as
// `yg {"re":N,...}`; the reader thread also takes the player box's `embed ...` lines.
struct Stdio {
  std::mutex mutex;
  std::condition_variable cv;
  std::map<int64_t, json> answers;
  std::set<int64_t> pending_requests;
  int64_t next_id = 1;
  std::mutex write_mutex;
  std::function<void(const std::string&)> box;
  std::deque<std::string> box_backlog;
#ifdef MELEE_LINUX_X11_EMBED
  bool box_overflow = false;
#endif
  std::atomic<bool> eof{false};
};
Stdio g_io;
relay_pads::State g_relay_pads([](const std::string& line) { host::log("%s", line.c_str()); }, false);
std::atomic<uintptr_t> g_pad_mirror{0};
#ifndef _WIN32
std::mutex g_pad_mirror_mutex;
pad_mirror::Attachment g_pad_mirror_attachment;
#endif
transport::Events g_net_events;
std::atomic<bool> g_broker{false}, g_direct{false};
search_policy::Generation g_search_generation;
thread_local std::function<bool()> g_search_stopped;
thread_local search_policy::BusyRetry* g_ticket_retry = nullptr;
update::Notice g_update_notice;
bool search_stopped() { return g_search_stopped && g_search_stopped(); }
std::mutex g_link_mutex;
std::map<unsigned, int> g_link_events;
std::map<unsigned, std::string> g_link_routes;
std::map<unsigned, std::vector<uint64_t>> g_link_rtts;
std::atomic<bool> g_relaunch{false};
std::shared_ptr<console::RelaunchWait> g_relaunch_wait; // atomic shared_ptr publication, like handoff channels
int direct_state(unsigned id) {
  const int state = yg_enet_link_state(id);
  if (state == 2 || state == 3) return state;
  std::lock_guard<std::mutex> lock(g_link_mutex);
  const auto it = g_link_events.find(id);
  return it == g_link_events.end() ? state : it->second;
}

void write_line(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_io.write_mutex);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  std::string data = line + "\n";
  DWORD wrote = 0;
  // One WriteFile per line, so nothing else written to stdout can land inside it.
  if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, data.data(), (DWORD)data.size(), &wrote, nullptr);
}

std::mutex g_relay_rumble_mutex;
relay_pads::Rumble g_relay_rumble;
#ifdef YOUGAME_AGENT_TEST_HOOK
agent_test::RumblePulse g_rumble_pulse;
#endif
void relay_rumble(host::CaptureDevice family, int index, bool on) {
  const int seat = relay_pads::seat_for(g_relay_pads.snapshot(), family, index);
  if (seat < 0) return;
  std::lock_guard<std::mutex> lock(g_relay_rumble_mutex);
  g_relay_rumble.set(seat, on, relay_pads::steady_ms());
}
void relay_rumble_writer() {
  relay_pads::Frame previous;
  for (;;) {
    g_relay_pads.timing_tick();
    const auto frame = g_relay_pads.snapshot();
    const double now = relay_pads::steady_ms();
    relay_pads::RumbleCommand command;
    bool pending = false;
    {
      std::lock_guard<std::mutex> lock(g_relay_rumble_mutex);
      for (int seat = 0; seat < 4; ++seat) {
        const auto& a = previous.pads[seat]; const auto& b = frame.pads[seat];
        if (a.present && (!b.present || a.id != b.id || a.kind != b.kind || a.family_index != b.family_index))
          g_relay_rumble.set(seat, false, now);
      }
#ifdef YOUGAME_AGENT_TEST_HOOK
      std::array<bool, 4> present{};
      for (int seat = 0; seat < 4; ++seat) present[seat] = frame.pads[seat].present;
      g_rumble_pulse.tick(now, present, [&](int seat) { g_relay_rumble.pulse(seat, now); },
          [](const char* line) { host::log("%s", line); });
#endif
      g_relay_rumble.tick(now);
      pending = g_relay_rumble.take(command);
    }
    previous = frame;
    // No queue/state mutex held across stdout: a stopped reader never stalls PADRead.
    if (pending) write_line(relay_pads::rumble_line(command));
    else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_io.eof.load()) return;
  }
}
thread_local relay_pads::RouteLog g_relay_routes;
void relay_route(host::CaptureDevice family, int index, int port) {
  const auto line = g_relay_routes.changed(relay_pads::game_frame, family, index, port);
  if (!line.empty()) host::log("%s", line.c_str());
}

// The app's pads in the game's controller families (yougame_controller_relay.h), for the game's
// input threads: a copy of the latest accepted line.
host::controller_relay::Snapshot relayed_controllers() {
  if (!host::controller_relay::game_poll) return relay_pads::controller_snapshot(g_relay_pads.snapshot());
  thread_local uint64_t serial = 0;
  if (serial != host::controller_relay::poll_serial) {
    serial = host::controller_relay::poll_serial;
    relay_pads::game_frame = g_relay_pads.game_snapshot();
    g_relay_routes.begin(relay_pads::game_frame);
  }
  return relay_pads::controller_snapshot(relay_pads::game_frame);
}

void handle_app_line(const std::string& line) {
  if (g_update_notice.command(line)) {
    const auto id = g_update_notice.take_ready_log();
    if (!id.empty()) host::log("update: ready %s (waiting for a safe point)", id.c_str());
    return;
  }
  if (line.rfind("yg-pads ", 0) == 0) {
    // The provider goes in before the first line is accepted, so a reader that sees relay mode
    // already sees the families. A line that does not parse installs nothing: native pads stay.
    if (!host::controller_relay::active()) {
      relay_pads::Frame frame; std::string reason;
      if (relay_pads::parse(line, frame, reason)) host::controller_relay::install(relayed_controllers);
    }
    g_relay_pads.accept(line, now_ms());
    return;
  }
  if (line.rfind("yg ", 0) == 0) {
    json j = json::parse(line.substr(3), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    if (j.count("caps")) console::read_caps(j, g_relaunch);
    if (j.count("re") && j["re"].is_number_integer()) {
      // The id first: json's operator= takes its argument by value, so moving j into it would empty
      // j before an index read from j on the left is evaluated (C++17 sequences the right side first).
      const int64_t id = j["re"].get<int64_t>();
      std::lock_guard<std::mutex> lock(g_io.mutex);
      if (g_io.pending_requests.count(id)) g_io.answers[id] = std::move(j);
      g_io.cv.notify_all();
    }
    // Pushes from the app: the pad mirror's handle ({"push":{"type":"pads","handle":N}}).
    if (j.count("push") && j["push"].is_object()) {
      const json& push = j["push"];
      if (auto wait = std::atomic_load(&g_relaunch_wait)) wait->push(push);
      if (console::launch_request(push)) parity::signals.console_boot_ready();
      if (g_stdio) ux::page_pause.push(push);
      if (push.value("cmd",json()) == "leave") { friends::lobby.leave(); console::entry.stop(); } // no-op outside a held room
      if (friends::events.push(push, now_ms()) && friends::version_reason(push)) log_friend_version();
      if (push.value("type", "") == "net") g_net_events.push(push);
      if (push.value("type", "") == "direct") {
        const auto notice = console::direct_notice(push);
        std::lock_guard<std::mutex> lock(g_link_mutex);
        if (notice.link) {
          if (notice.state) g_link_events[notice.link] = notice.state;
          if (!notice.path.empty()) g_link_routes[notice.link] = notice.path;
          if (notice.rtt) {
            auto& samples = g_link_rtts[notice.link];
            if (samples.size() < 32) samples.push_back(*notice.rtt);
          }
        }
      }
      if (push.value("type", "") == "pads" && push.count("handle") && push["handle"].is_number_unsigned())
        g_pad_mirror = (uintptr_t)push["handle"].get<uint64_t>();
#ifndef _WIN32
      if (push.value("type", "") == "pads") {
        pad_mirror::Attachment info;
        if (pad_mirror::attachment(push, info)) {
          std::lock_guard<std::mutex> lock(g_pad_mirror_mutex);
          g_pad_mirror_attachment = info;
        }
      }
#endif
    }
    return;
  }
  if (line.rfind("embed ", 0) == 0) {
    if (shortcuts::commands.dispatch(line.substr(6), [](const char* text) { host::log("%s", text); })) return;
#ifdef MELEE_LINUX_X11_EMBED
    if (g_embedded) {
      // Keep command order, including during boot. Never run Xlib on this reader thread.
      if (line.size() > 159 || line.find('\0') != std::string::npos) return;
      std::lock_guard<std::mutex> lock(g_io.mutex);
      if (g_io.box_backlog.size() == 4096) g_io.box_overflow = true;
      if (!g_io.box_overflow) g_io.box_backlog.push_back(line.substr(6));
      return;
    }
#endif
    const std::string cmd = line.substr(6);
    std::function<void(const std::string&)> box;
    {
      std::lock_guard<std::mutex> lock(g_io.mutex);
      box = g_io.box;
      if (!box) {
        // Before the window exists only the latest of each kind matters (the page sends a rect on
        // every scroll while the game boots): it replaces the one it supersedes.
        auto kind = [](const std::string& c) {
#ifdef _WIN32
          if (c.rfind("view", 0) == 0) return std::string("view");
#endif
          return c.rfind("rect", 0) == 0 ? "rect" : c == "hide" || c == "show" ? "shown" : c == "focus" || c == "blur" ? "focus"
               : c.rfind("volume", 0) == 0 ? "volume" : c;
        };
        auto& backlog = g_io.box_backlog;
        backlog.erase(std::remove_if(backlog.begin(), backlog.end(), [&](const std::string& c) { return std::string(kind(c)) == kind(cmd); }), backlog.end());
        backlog.push_back(cmd);
        return;
      }
    }
    box(cmd);
  }
}

void read_stdin() {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  std::string pending;
  char buf[4096];
  DWORD got = 0;
#ifdef MELEE_LINUX_X11_EMBED
  bool discard = false;
#endif
  while (in && in != INVALID_HANDLE_VALUE && ReadFile(in, buf, sizeof buf, &got, nullptr) && got) {
#ifdef MELEE_LINUX_X11_EMBED
    if (g_embedded) {
      // Discard an entire oversized line through its newline; a suffix is never a command.
      for (DWORD i = 0; i < got; ++i) {
        if (buf[i] == '\n') {
          if (!discard) {
            if (!pending.empty() && pending.back() == '\r') pending.pop_back();
            try { handle_app_line(pending); }
            catch (const std::exception&) { host::log("yougame: bad line from the app"); }
          }
          pending.clear(); discard = false;
        } else if (!discard) {
          if (pending.size() == (1u << 20) || buf[i] == '\0') {
            pending.clear(); discard = true;
          } else pending.push_back(buf[i]);
        }
      }
      continue;
    }
#endif
    pending.append(buf, got);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      try {
        handle_app_line(line);
      } catch (const std::exception& e) {
        host::log("yougame: bad line from the app");
      }
    }
    if (pending.size() > (1u << 20)) pending.clear();
  }
  // The app is gone (it quit, or stopped this game): no answer will come, and nothing would show or
  // stop this window any more. Waiting requests fail at once and the game ends.
  g_io.eof = true;
  g_relay_pads.reset_on_eof();
  g_relay_pads.flush();
  g_io.cv.notify_all();
  host::log("yougame: the YouGame app closed the bridge; quitting");
  host::request_exit(0);
}

json bridge_stdio(const json& body, int timeout_ms, const std::function<bool()>& cancelled = {}) {
  if (g_io.eof) throw std::runtime_error("The YouGame app is not answering");
  int64_t id;
  {
    std::lock_guard<std::mutex> lock(g_io.mutex);
    id = g_io.next_id++;
    g_io.pending_requests.insert(id);
  }
  json request = body;
  request["id"] = id;
  write_line("yg " + request.dump());
  std::unique_lock<std::mutex> lock(g_io.mutex);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!g_io.answers.count(id) && !g_io.eof) {
    if (cancelled && cancelled()) {
      g_io.pending_requests.erase(id);
      throw std::runtime_error("Cancelled");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      g_io.pending_requests.erase(id);
      throw std::runtime_error("The YouGame app did not answer");
    }
    g_io.cv.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
  }
  g_io.pending_requests.erase(id);
  if (!g_io.answers.count(id)) throw std::runtime_error("The YouGame app is not answering");
  json answer = std::move(g_io.answers[id]);
  g_io.answers.erase(id);
  return answer;
}

// Native WinHTTP cancels synchronous requests by closing their handle. The POSIX shim polls its
// callback from libcurl/upgrade waits instead (closing its raw request concurrently is unsafe).
class PendingHttp {
 public:
  PendingHttp(HINTERNET request, const std::function<bool()>& cancelled) : request_(request) {
#ifdef _WIN32
    if (request && cancelled) watcher_ = std::thread([this, cancelled] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!done_) {
        if (cancelled()) { close(); break; }
        cv_.wait_for(lock, std::chrono::milliseconds(25), [&] { return done_; });
      }
    });
#else
    if (request) WinHttpSetCancelCallback(request, cancelled);
#endif
  }
  ~PendingHttp() {
#ifdef _WIN32
    { std::lock_guard<std::mutex> lock(mutex_); done_ = true; }
    cv_.notify_all();
    if (watcher_.joinable()) watcher_.join();
#endif
    close();
  }
 private:
  void close() { if (auto request = request_.exchange(nullptr)) WinHttpCloseHandle(request); }
  std::atomic<HINTERNET> request_;
#ifdef _WIN32
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread watcher_;
#endif
};

// POST <bridge>/sdk {type, ...} -> {ok, result | error}. Only the app's loopback address is
// accepted: the token must never travel anywhere else.
json bridge_http(const json& body, int timeout_ms, const std::function<bool()>& cancelled = {}) {
  if (g_bridge_url.empty() || g_bridge_token.empty()) throw std::runtime_error("Start Melee from the YouGame app to play online");
  Url u;
  if (!parse_url(g_bridge_url + "/sdk", u) || u.secure || u.host != L"127.0.0.1") throw std::runtime_error("The YouGame app's address is invalid");
  HINTERNET session = http_session();
  if (!session) throw std::runtime_error("Networking is unavailable");
  HINTERNET conn = WinHttpConnect(session, u.host.c_str(), u.port, 0);
  if (!conn) throw std::runtime_error("The YouGame app is not answering");
  HINTERNET req = WinHttpOpenRequest(conn, L"POST", u.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  std::string result;
  bool sent = false;
  {
    PendingHttp pending(req, cancelled);
    if (req) {
      DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
      WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
      WinHttpSetTimeouts(req, 5000, 5000, timeout_ms, timeout_ms);
      std::wstring headers = wide("Authorization: Bearer " + g_bridge_token + "\r\nContent-Type: application/json\r\n");
      std::string data = body.dump();
      sent = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1, data.data(), (DWORD)data.size(), (DWORD)data.size(), 0) &&
             WinHttpReceiveResponse(req, nullptr);
      char buf[4096];
      DWORD got = 0;
      while (sent && WinHttpReadData(req, buf, sizeof buf, &got) && got) {
        result.append(buf, got);
        if (result.size() > (1u << 20)) { sent = false; break; }
      }
    }
  }
  WinHttpCloseHandle(conn);
  if (!sent && cancelled && cancelled()) throw std::runtime_error("Cancelled");
  if (!sent) throw std::runtime_error("The YouGame app is not answering");
  json j = json::parse(result, nullptr, false);
  if (j.is_discarded() || !j.is_object()) throw std::runtime_error("The YouGame app sent an invalid answer");
  return j;
}

// The site's refusal code from this thread's last not-ok bridge() answer (letters and _ only, <= 24), empty when
// the last call threw for another reason (no answer, cancelled). For logs that must never carry the error text.
thread_local std::string g_bridge_refusal;
json bridge(const json& body, int timeout_ms = 15000, const std::function<bool()>& cancelled = {}) {
  g_bridge_refusal.clear();
  search_policy::BusyRetry local_retry;
  auto& retry = g_ticket_retry ? *g_ticket_retry : local_retry;
  json j;
  for (;;) {
    if (cancelled && cancelled()) throw std::runtime_error("Cancelled");
    j = g_stdio ? bridge_stdio(body, timeout_ms, cancelled) : bridge_http(body, timeout_ms, cancelled);
    // The app forwards the site's code, including HTTP 409. Text/status alone never gates a build.
    const auto& refused = update::refusal(j);
    if (update::required(refused)) {
      g_bridge_refusal = "update_required";
      if ((cancelled && cancelled()) || search_stopped()) throw std::runtime_error("Cancelled");
      if (!search_stopped()) {
        g_update_notice.refuse();
        host::log("yougame: update required (current %s)", update::current(refused).c_str());
      }
      throw update::Required();
    }
    // Inspect busy metadata before bridge converts a failure into display text.
    const int delay = body.value("type", "") == "mpTicket" ? retry.delay(j) : 0;
    if (!delay) break;
    host::log("yougame: site busy, retrying the ticket in %d s (try %d/3)", delay, retry.tries());
    if (!search_policy::wait(uint64_t(delay) * 1000, now_ms, [](int ms) { Sleep(ms); },
        [&] { return cancelled && cancelled(); })) throw std::runtime_error("Cancelled");
  }
  // A delivered assignment still needs its scoped join/leave cleanup if cancel won the race.
  const bool assigned = body.value("type", "") == "mpTicket" && j.value("ok", false) &&
      j.count("result") && j["result"].is_object() && !ranked::str(j["result"], "room").empty();
  if (cancelled && cancelled() && !assigned) throw std::runtime_error("Cancelled");
  if (!j.value("ok", false)) {
    g_bridge_refusal = ranked::str(j, "code");
    if (g_bridge_refusal.empty() || g_bridge_refusal.size() > 24 ||
        g_bridge_refusal.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) g_bridge_refusal = "refused";
    std::string error = j.count("error") && j["error"].is_string() ? j["error"].get<std::string>() : "YouGame request failed";
    throw std::runtime_error(friends::version_request(body) && friends::version_reason(j) ? friends::kVersion :
                             body.value("ranked", false) ? ranked::ticket_error(j) : error);
  }
  // Linking accepts the documented direct answer as well as the bridge result envelope.
  if (body.value("type", "") == "slippiLink") return j;
  return j.count("result") ? j["result"] : json::object();
}

// ------------------------------------------------------------------ WebSocket (WinHTTP)
// A reader thread queues each JSON message; next() waits on the queue, so the caller can poll its
// cancel flag. Closing the handle from another thread ends the blocking receive.
class Socket {
 public:
  ~Socket() { close(); }
  bool open_session(const std::string& url, const std::string& ticket, const std::string& room, std::string* error, int timeout = 10000, const std::function<bool()>& cancelled = {}, const std::string& session = {}) {
    if (cancelled && cancelled()) return false;
    cancel_open_ = cancelled;
    if (!g_broker) return open(url + (room.empty() ? "/match" : "/room/" + url_encode(room)) + "?ticket=" + url_encode(ticket), error, timeout, cancelled);
    if (!broker_) broker_ = std::make_unique<transport::Stream>(g_net_events, [this](const json& q, int ms) { return bridge(q, ms, cancel_open_); });
    try {
      const bool opened = cancelled ? broker_->open(ticket, room, timeout, session, cancelled)
                                    : broker_->open(ticket, room, timeout, session);
      cancel_open_ = {};
      if (opened) return true;
    } catch (const update::Required&) { cancel_open_ = {}; throw; }
    catch (...) {}
    cancel_open_ = {};
    *error = "Lost the connection to YouGame";
    return false;
  }
  bool open_session(const std::string& url, const std::string& ticket, const std::string& room,
                    std::string* error, int timeout, const std::string& session) {
    return open_session(url, ticket, room, error, timeout, {}, session);
  }
  int connection() const { return broker_ ? broker_->conn() : 0; }
  bool recoverable() const { return broker_ && broker_->close_code() == 1006; }
  // Friends rooms also hold the seat after Rooms' silent-socket close (4002, rooms/src/room-rules.ts
  // SILENT_CLOSE): it is not final, so the console backend rejoins as it does after a 1006 drop.
  bool rejoinable() const { return recoverable() || (broker_ && broker_->close_code() == 4002); }
  bool lobby_mismatch() const { return broker_ && broker_->lobby_mismatch(); }
  int close_code() const { return broker_ ? broker_->close_code() : 1006; }
  bool brokered() const { return broker_ != nullptr; }
#ifdef YOUGAME_AGENT_TEST_HOOK
  uint64_t agent_test_first_open_at() const { return broker_ ? broker_->agent_test_first_open_at() : 0; }
  void agent_test_drop_room() { if (broker_) broker_->agent_test_drop_room(); }
#endif
  bool open(const std::string& address, std::string* error, int timeout, const std::function<bool()>& cancelled) {
    Url u;
    if (!parse_url(address, u) || (!u.secure && u.host != L"127.0.0.1" && u.host != L"localhost")) {
      *error = "Invalid YouGame rooms address";
      return false;
    }
    HINTERNET conn = WinHttpConnect(http_session(), u.host.c_str(), u.port, 0);
    if (!conn) { *error = "Could not reach YouGame"; return false; }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       u.secure ? WINHTTP_FLAG_SECURE : 0);
    DWORD status = 0, size = sizeof status;
    DWORD forever = 0;
    {
      PendingHttp pending(req, cancelled);
      if (req) WinHttpSetTimeouts(req, timeout, timeout, timeout, timeout);
      bool ok = req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr) &&
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                    WINHTTP_NO_HEADER_INDEX) &&
                status == 101;
      if (ok) ws_ = WinHttpWebSocketCompleteUpgrade(req, 0);
      if (ws_) WinHttpSetOption(ws_, WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof forever);
    }
    WinHttpCloseHandle(conn);
    if (cancelled && cancelled()) { if (ws_) { WinHttpCloseHandle(ws_); ws_ = nullptr; } return false; }
    if (!ws_) {
      *error = status == 403 ? "YouGame refused the ticket" : status ? "YouGame refused the connection (HTTP " + std::to_string(status) + ")"
                                                                  : "Could not connect to YouGame";
      return false;
    }
    reader_ = std::thread([this] { read_loop(); });
    return true;
  }
  bool send(const json& j) {
    if (broker_) return broker_->send(j);
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!ws_ || closed_) return false;
    std::string data = j.dump();
    return WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, data.data(), (DWORD)data.size()) == NO_ERROR;
  }
  // Up to timeout_ms for the next message; false on timeout or once the socket has closed and drained.
  bool next(json& out, int timeout_ms) {
    if (broker_) return broker_->next(out, timeout_ms);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }
  bool closed() const { return broker_ ? broker_->closed() : closed_.load(); }
  void close(int code = 4000) {
    if (broker_) { broker_->close(1000, code); return; }
    HINTERNET ws = nullptr;
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      ws = ws_;
      ws_ = nullptr;
    }
    if (ws) {
      WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
      WinHttpCloseHandle(ws);
    }
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
    closed_ = true;
    cv_.notify_all();
  }

 private:
  std::function<bool()> cancel_open_;
  void read_loop() {
    std::string data;
    std::vector<char> buf(16384);
    for (;;) {
      HINTERNET ws;
      { std::lock_guard<std::mutex> lock(send_mutex_); ws = ws_; }
      if (!ws) break;
      DWORD got = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
      if (WinHttpWebSocketReceive(ws, buf.data(), (DWORD)buf.size(), &got, &type) != NO_ERROR) break;
      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
      if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
      data.append(buf.data(), got);
      if (data.size() > (1u << 20)) break;
      if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
      json j = json::parse(data, nullptr, false);
      data.clear();
      if (j.is_discarded() || !j.is_object()) continue;
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(j));
      cv_.notify_all();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
  }
  std::unique_ptr<transport::Stream> broker_;
  HINTERNET ws_ = nullptr;
  std::thread reader_;
  std::mutex mutex_, send_mutex_;
  std::condition_variable cv_;
  std::deque<json> queue_;
  std::atomic<bool> closed_{false};
};

// ------------------------------------------------------------------ addresses
std::string addr_string(const sockaddr_in& a) {
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}
bool parse_addr(const std::string& s, sockaddr_in& out) {
  size_t colon = s.rfind(':');
  if (colon == std::string::npos) return false;
  std::memset(&out, 0, sizeof out);
  out.sin_family = AF_INET;
  const auto port = udp::port(s.c_str() + colon + 1);
  if (!port.valid || inet_pton(AF_INET, s.substr(0, colon).c_str(), &out.sin_addr) != 1) return false;
  out.sin_port = htons(port.value);
  return true;
}
bool same_addr(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

// The address this machine uses toward the internet (no packet is sent: connect() on UDP only picks a route).
std::string lan_ip() {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return "";
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
  std::string out;
  sockaddr_in me{};
  SocketLength len = sizeof me;
  if (connect(s, (sockaddr*)&to, sizeof to) == 0 && getsockname(s, (sockaddr*)&me, &len) == 0) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
    out = ip;
  }
  closesocket(s);
  return out;
}

bool wait_readable(SOCKET s, int timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#if defined(__linux__) || defined(__APPLE__)
  return select(s + 1, &set, nullptr, nullptr, &tv) > 0;
#else
  return select(0, &set, nullptr, nullptr, &tv) > 0;
#endif
}

// The external address of `s` as a STUN server sees it (RFC 5389 binding request,
// XOR-MAPPED-ADDRESS). This is the address the opponent's packets must be sent to.
bool stun(SOCKET s, std::string& out, const std::function<bool()>& cancelled) {
  udp::StunIngress ingress(&udp::own_udp_drops);
  static const char* servers[][2] = {{"stun.cloudflare.com", "3478"}, {"stun.l.google.com", "19302"}};
  for (auto& server : servers) {
    if (cancelled()) return false;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server[0], server[1], &hints, &res) != 0 || !res) continue;
    sockaddr_in to = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    uint64_t t1 = random64(), t2 = random64();
    std::memcpy(req + 8, &t1, 8);
    std::memcpy(req + 16, &t2, 4);
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (sendto(s, (const char*)req, sizeof req, 0, (sockaddr*)&to, sizeof to) != sizeof req) continue;
      uint64_t until = now_ms() + 500;
      ingress.open({to.sin_addr.s_addr, ntohs(to.sin_port)}, req, until);
      while (now_ms() < until) {
        if (!wait_readable(s, (int)(until - now_ms()))) break;
        uint8_t buf[512];
        sockaddr_in from{};
        SocketLength fl = sizeof from;
        int n = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 0) continue;
        if (!ingress.accept({from.sin_addr.s_addr, ntohs(from.sin_port)}, buf, size_t(n), now_ms(),
            [](const char* message) { host::log("%s", message); })) continue;
        if (udp::stun_reply(buf, size_t(n), req, out)) { ingress.close(); return true; }
      }
      ingress.close();
    }
  }
  return false;
}

// ------------------------------------------------------------------ the punch
// Every player sends small probes from the netplay port to every address a peer published (its LAN
// address and its STUN address) and answers each probe it receives. The first address a peer's
// packet arrives from is the one to use: it works in that direction, and having sent to it opens our
// NAT for the reply. A peer is reachable once its ack shows it got ours too. Each pair of players
// shares secrets passed point to point through the room: a probe carries the token its sender handed
// the receiver, so nobody else (another player in a Teams room included) can answer for a peer.
// ENet drops a late probe as malformed.
constexpr uint8_t kProbe = 0, kAck = 1;
struct PunchPeer {
  int port = 0;                       // 1-based
  uint64_t mine = 0;                  // the token I gave this peer: my probes to it carry it
  uint64_t theirs = 0;                // the token this peer gave me: its probes to me carry it
  std::vector<sockaddr_in> candidates;
  std::vector<const char*> kinds;     // "lan" or "ext", one per candidate (logged, never the address)
  bool found = false, acked = false;
  sockaddr_in chosen{};
};
void send_probe(SOCKET s, const sockaddr_in& to, uint8_t type, uint64_t token, int from_port) {
  uint8_t p[14] = {'Y', 'G', 'P', '2', type};
  std::memcpy(p + 5, &token, 8);
  p[13] = (uint8_t)from_port;
  sendto(s, (const char*)p, sizeof p, 0, (const sockaddr*)&to, sizeof to);
}
// Which published address a reply came from: "other" means the peer's NAT used a port its STUN
// answer did not show (a symmetric NAT); logs name the kind, never the address.
const char* answered_via(const PunchPeer& p, const sockaddr_in& from) {
  for (size_t i = 0; i < p.candidates.size() && i < p.kinds.size(); ++i)
    if (same_addr(p.candidates[i], from)) return p.kinds[i];
  return "other";
}
bool punch(SOCKET s, int my_port, std::vector<PunchPeer>& peers, udp::Ingress& ingress, const std::function<bool()>& cancelled) {
  const uint64_t start = now_ms(), deadline = start + 8000;
  ingress.clear(); // STUN's solicited endpoint is never a gameplay/punch peer.
  for (const auto& peer : peers) for (const auto& a : peer.candidates)
    ingress.allow({a.sin_addr.s_addr, ntohs(a.sin_port)});
  uint64_t next_send = 0, done_at = 0;
  for (;;) {
    const uint64_t now = now_ms();
    bool all = std::all_of(peers.begin(), peers.end(), [](const PunchPeer& p) { return p.found && p.acked; });
    if (all && !done_at) done_at = now;
    // Keep answering for a moment after we are done: the other side may still be waiting for our ack.
    if (done_at && now - done_at > 600) return true;
    if (cancelled()) return false;
    // Not while answering after success: a peer whose ack lands in the last 600 ms must not fail on one side only.
    if (now > deadline && !done_at) {
      // Every failed own-UDP connect leaves one line per peer (10-30: strict-NAT pairs have no fallback yet).
      for (const auto& p : peers) {
        std::string tried;
        for (const char* k : p.kinds) tried += (tried.empty() ? "" : "+") + std::string(k);
        host::log("yougame: connect failed: player %d after %llu ms, tried %s, heard %s, acked %s", p.port,
                  (unsigned long long)(now - start), tried.empty() ? "nothing" : tried.c_str(),
                  p.found ? "yes" : "no", p.acked ? "yes" : "no");
      }
      return false;
    }
    if (now >= next_send) {
      for (auto& p : peers) {
        if (p.found) send_probe(s, p.chosen, kProbe, p.mine, my_port);
        else for (auto& c : p.candidates) send_probe(s, c, kProbe, p.mine, my_port);
      }
      next_send = now + 100;
    }
    if (!wait_readable(s, 20)) continue;
    uint8_t buf[64];
    sockaddr_in from{};
    SocketLength fl = sizeof from;
    int n = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
    if (n < 0) continue;
    // SECURITY: source allowlist precedes every read of the datagram payload.
    if (!ingress.accept({from.sin_addr.s_addr, ntohs(from.sin_port)}, now,
        [](const char* message) { host::log("%s", message); })) continue;
    udp::Probe probe;
    if (!udp::probe(buf, size_t(n), probe)) continue;
    for (auto& p : peers) {
      if (p.port != probe.player || probe.token != p.theirs ||
          std::none_of(p.candidates.begin(), p.candidates.end(), [&](const auto& a) { return same_addr(a, from); })) continue;
      if (!p.found) {
        p.found = true;
        p.chosen = from;
        host::log("yougame: player %d answered after %llu ms via %s", p.port, (unsigned long long)(now - start), answered_via(p, from));
      }
      if (probe.type == kProbe) send_probe(s, from, kAck, p.mine, my_port);
      else p.acked = true;
    }
  }
}

// ------------------------------------------------------------------ session state
std::mutex g_mutex;
Identity g_identity;
bool g_identity_asked = false;
uint64_t g_identity_at = 0;   // GetTickCount64 of the last answer
std::string g_status = "offline";
uint64_t g_status_since = 0;
search_policy::LinkFailures g_direct_link_failures; // g_mutex; survives replacement searches

// The room of the current online session. Stays open through the match for results.
struct Room : transport::RoomState<Socket> {
  bool friends = false;
  ~Room() {
    // Also runs on cancelled/failed setup: no abandoned links occupy the three slots.
    transport::close_links(links, [](const json& q, int ms) { return bridge(q, ms); });
  }
};
std::unique_ptr<Room> g_room;
std::atomic<unsigned> g_update_searches{0};
bool g_slippi_match = false;  // g_mutex; identity refresh must not switch a live Slippi match's ENet

void select_match_transport(bool slippi) {
  g_slippi_match = slippi;
#ifdef _WIN32
  yg_enet_select(!slippi && g_direct);
#else
  yg_enet_select(!slippi && g_broker);
#endif
}
#ifdef YOUGAME_AGENT_TEST_HOOK
bool g_pending_input = false;
struct PendingInputRoster {
  int mode = -2;
  bool ranked = false;
  json participants = json::array();
  std::string me;
} g_pending_input_roster;
#endif
std::shared_future<void> g_retired; // a new search waits until the previous room has left
int g_ranked_rooms = 0;             // ranked rooms this process has played in
ranked::RankCache g_rank; // the newest ranked standing (ticket, mpRank or a set's result) for this process
ranked::RankRead g_rank_read;
ranked::ResultsPanel g_rank_results;
bool g_rank_result_visible = false;
std::string g_rank_opponent, g_rank_score;
int g_rank_local = 0;
constexpr uint64_t kDrainMs = 30000;   // a ranked room stays open this long at most for its result

std::string g_blend_reason; // guarded by g_mutex; reset for each search
void set_status(const std::string& s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (search_stopped()) return;
  const std::string shown = !g_blend_reason.empty() && s == blend::kSearchBoth ? blend::kSearchYouGameOnly : s;
  if (g_status != shown) host::log("yougame: %s", shown.c_str());
  g_status = shown;
  g_status_since = now_ms();
}

using transport::read_roster;

transport::RoomIO room_io(Room& r) {
  transport::RoomIO io;
  io.request = [&r](const json& q, int ms) {
    return bridge(q, ms, [&] { return q.value("type", "") == "mpTicket" && r.stop; });
  };
  io.link_state = direct_state;
  io.disable_links = [] { yg_enet_enable(0); };
  io.log = [](const char* message) { host::log("%s", message); };
  io.started = [](const std::string& id) { g_rank_results.pending(id); };
  io.result = [&r](const json& m) {
    if (!g_rank_results.receive(m, r.set.local_participant())) g_rank_results.result_error();
    g_rank_result_visible = true;
    g_rank_read.recovery_at = 0;
    const bool won = m.count("ranking") && m["ranking"].is_array() && !m["ranking"].empty() &&
                     m["ranking"][0] == r.set.local_participant();
    const bool none = m.count("void") && m["void"].is_boolean() && m["void"].get<bool>();
    host::log("yougame: ranked set result: %s, %s", ranked::str(m, "disposition").c_str(),
              none ? "no winner" : won ? "this player won" : "the opponent won");
    if (g_rank.result(m, r.set.local_participant())) host::log("yougame: ranked standing: %s", g_rank.header().c_str());
  };
  return io;
}
void end_links(Room& r) { transport::end_links(r, room_io(r)); }
void pump_room(Room* r) { transport::pump_room(r, g_mutex, room_io(*r)); }

void shutdown_room(std::unique_ptr<Room> r) {
  if (!r) return;
  r->stop = true;
  if (r->pump.joinable()) r->pump.join();
  end_links(*r);
  if (r->socket) {
    r->socket->send({{"t", "leave"}});
    r->socket->close();
  }
}
// Takes the room out of the session at once and leaves it on another thread: the online cleanup
// runs on the game thread, and a search that starts right after must not find the old room.
// Ranked (yougame_ranked.h Set): a quitter concedes; a player whose opponent's connection broke
// claims the set once its own games are settled; a decided set sends its final. A quitter whose
// concession is decided sends what it owes and closes at once, so the room's leave rule can award
// the opponent; everyone else stays for the room's result, kDrainMs at most.
uint64_t close_room(Leave why) {
  std::unique_ptr<Room> r;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_search_generation.begin();
#ifdef YOUGAME_AGENT_TEST_HOOK
    g_pending_input = false;
    g_pending_input_roster = {};
#endif
    friends::events.current_room({});
    r = std::move(g_room);
    if (!r) {
#ifdef YOUGAME_AGENT_TEST_HOOK
      if (agent_test::static_quit_requested.load()) host::request_exit(0);
#endif
      return g_search_generation.value();
    }
#ifdef YOUGAME_AGENT_TEST_HOOK
    const bool agent_test_quit = !r->ranked && agent_test::static_quit_requested.load();
#endif
    if (r->friends && why == Leave::Quit && !r->in_game) host::log("friends: cancelled");
    bool at_once = true;
    if (r->ranked) {
      if (why == Leave::Quit) r->set.quit();
      else if (why == Leave::Lost) r->set.lost();
      at_once = r->set.leaves_at_once() || !r->set.needs_drain();
      if (r->set.leaves_at_once()) host::log("yougame: ranked set concession: the opponent won");
      host::log("yougame: leaving the ranked room (%s)%s", why == Leave::Quit ? "quit" : why == Leave::Lost ? "connection lost" : "set over",
                at_once ? "" : "; staying for its result");
      // Keep the last set visible while its exact room drains its authoritative result.
      g_rank_result_visible = r->set.started();
      g_rank_score = r->set.score_text();
    }
    auto done = std::make_shared<std::promise<void>>();
    g_retired = done->get_future().share();
    const auto result_match = g_rank_results.match();
    std::thread([r = std::move(r), done, at_once, result_match
#ifdef YOUGAME_AGENT_TEST_HOOK
                 , agent_test_quit
#endif
                ]() mutable {
      // The game thread is already back in its menus. Drain this exact room, with its frozen
      // participant ids, before leaving. A replacement search waits on g_retired below.
      if (r->ranked && at_once) {
        transport::stop_and_flush(*r, g_mutex);
      } else if (r->ranked) {
        if (!transport::drain_ranked_room(*r, g_mutex, room_io(*r), kDrainMs))
          host::log("yougame: ranked room left before its result");
      }
#ifdef YOUGAME_AGENT_TEST_HOOK
      if (agent_test_quit) {
        const bool settled = agent_test::drain_static_results([&] {
          std::lock_guard<std::mutex> lock(g_mutex);
          return r->socket->brokered() ? r->games.agent_test_pending()
                                      : r->pending_begin || r->pending_winner != -2;
        }, now_ms, [](int ms) { Sleep(ms); });
        if (!settled) host::log("agent test: quitAfter room left before its result");
      }
#endif
      const bool ranked_room = r->ranked;
      shutdown_room(std::move(r));
      if (ranked_room) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_rank_results.match() == result_match && g_rank_results.state() == ranked::ResultsPanel::State::Pending) {
          if (g_rank_read.unsupported) g_rank_results.recovered_standing(result_match, ranked::RankCache(), true);
          else g_rank_read.recover(now_ms());
        }
      }
      done->set_value();
#ifdef YOUGAME_AGENT_TEST_HOOK
      if (agent_test_quit) host::request_exit(0);
#endif
    }).detach();
    return g_search_generation.value();
  }
}

// What two builds must share to play: the netplay protocol and this build's game. The rooms Worker
// only matches players with the same key (lobby compatibility).
std::string compatibility(int mode) {
#ifdef MELEE_PORT_VERSION
  std::string v = MELEE_PORT_VERSION;
#else
  std::string v = "dev";
#endif
  const auto& build = slippi::online::local_build();
  return compatibility_key(v, build.mod_view, build.fingerprint, build.static_engine,
                           slippi::online::ranked_rules_digest(mode),
                           compat_tag_with_test_timer(compat_tag, std::getenv("MELEE_ONLINE_TEST_TIMER")));
}

thread_local bool g_friend_search = false;
thread_local bool g_friend_failed = false;
// Blended Unranked's YouGame side (yougame_blend.h): cancelled because Slippi's side was first, it
// goes quiet ("offline"), not "error: Cancelled" under a Slippi match.
thread_local bool g_blend_search = false;
Match fail(const std::string& why, bool retry = false, bool update_required = false) {
  Match m;
  m.error = why;
  m.retry = retry && !update_required;
  m.update_required = update_required;
  if (search_stopped()) return m;
  if (update_required) {
    m.retry = false;
    set_status(update::kMessage);
    return m;
  }
  if (g_friend_search) {
    if (!g_friend_failed) {
      if (why == friends::kVersion) log_friend_version();
      else host::log("%s", why == "Cancelled" ? "friends: cancelled" : "friends: invite failed: connection failed");
      g_friend_failed = true;
    }
    set_status(why == friends::kVersion ? friends::kVersion : why == "Cancelled" ? "offline" : friends::kFailed);
  } else set_status(why == search_policy::kLinkRetry ? why :
                    g_blend_search && why == "Cancelled" ? "offline" : "error: " + why);
  return m;
}

std::atomic<bool> g_identity_fetching{false};

Identity fetch_identity(const std::function<bool()>& cancelled = nullptr) {
  static std::once_flag said;
  std::call_once(said, [] { host::log("yougame: YouGame mode (%s%s)", g_stdio ? "stdio bridge" : "loopback bridge", g_embedded ? ", in the player box" : ""); });
  Identity id;
  try {
    json hello = bridge({{"type", "hello"}}, 15000, cancelled);
    if ((cancelled && cancelled()) || search_stopped()) return id;
    { std::lock_guard<std::mutex> lock(g_mutex);
      if (search_stopped()) return id;
      friends::events.hello(hello);
      console::launch_hello(hello);
      if (console::requests.pending()) parity::signals.console_boot_ready();
      if (hello.count("caps")) console::read_caps(hello, g_relaunch);
    }
#ifdef _WIN32
    const auto mode = transport::Mode::hello(hello);
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (search_stopped()) return id;
      g_broker = g_stdio && mode.broker;
      g_direct = g_stdio && mode.direct;
      select_match_transport(g_slippi_match);
    }
    if (g_stdio && mode.unsupported) throw std::runtime_error("This YouGame direct-link version is not supported");
#else
    const auto route = transport::posix_datagram_route(hello);
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (search_stopped()) return id;
      if (route != transport::DatagramRoute::Standalone) g_broker = true;
      g_direct = g_stdio && route == transport::DatagramRoute::Ring;
      select_match_transport(g_slippi_match); // refuse mode also prevents native sockets
    }
    if (g_broker && !g_direct) throw std::runtime_error("YouGame broker requires direct links v1 (1200-byte datagrams)");
#endif
    const json& p = hello.count("player") && hello["player"].is_object() ? hello["player"] : json::object();
    id.player_id = p.value("id", "");
    id.name = p.value("name", "");
    try {
      json code = bridge({{"type", "mpCode"}}, 15000, cancelled);
      if ((cancelled && cancelled()) || search_stopped()) return id;
      id.code = code.value("code", "");
    } catch (const std::exception& e) {
      host::log("yougame: no connect code: %s", e.what());
    }
    id.ok = !id.player_id.empty();
    if (!id.ok) id.error = "Sign in to YouGame to play online";
    // Until the player shares their name with this game the SDK calls them "Player"; the tag of
    // their connect code is their own (made from their YouGame name) and reads better online.
    if (id.ok && !hello.value("nameShared", false) && !id.code.empty()) id.name = id.code.substr(0, id.code.find('#'));
  } catch (const std::exception& e) {
    id.error = e.what();
  }
  if ((cancelled && cancelled()) || search_stopped()) return id;
  if (id.ok) host::log("yougame: signed in as %s (%s)", id.name.c_str(), id.code.empty() ? "no code" : id.code.c_str());
  else host::log("yougame: not signed in: %s", id.error.c_str());
  std::lock_guard<std::mutex> lock(g_mutex);
  if (search_stopped()) return id;
  g_identity = id;
  g_identity_asked = true;
  g_identity_at = GetTickCount64();
  return id;
}

}  // namespace

// The room worker owns Socket after open. SessionLink only queues sends and polls messages;
// it never waits for a bridge answer from pump(wait_ms).
namespace {
console::FriendsRetire g_console_retired;
void wait_console_retired(const std::function<bool()>& stop = {}) {
  if (!g_console_retired.wait(5000,stop) && !(stop && stop())) host::log("console: previous Friends room still closing");
}
struct ConsoleBackend {
  std::unique_ptr<Socket> room;
  std::thread worker;
  std::atomic<bool> stop{false}, closed{false};
  std::atomic<bool> rejoining{false};
  std::atomic<bool> retiring{false};
  std::shared_future<void> preparation_finished;
  std::vector<json> retired_links; // mutex; SessionLink's destructor queues directClose here
  std::mutex mutex, close_mutex;
  std::condition_variable cv;
  std::deque<json> incoming, outgoing;
  size_t sending = 0; // guarded by mutex; outgoing may be empty while the worker is sending
  ENetSocket datagrams = ENET_SOCKET_NULL;
  std::atomic<int> conn{0};
  int players = 2;
  bool private_queue = true; // the relaunched shared session's ticket carries no queue
  std::atomic<bool> peers_present{true}, handoff_pending{false}, end_previous_on_welcome{false};
  std::string session;
  std::atomic<bool> session_live{false}, interrupted{false}, waiting_previous{false};
  json last_roster;
  std::function<std::string(int)> renew_ticket;
  ~ConsoleBackend() { close(); }
  bool open(const std::string& ticket, const std::string& code, int ms) {
    room = std::make_unique<Socket>(); std::string error;
    if (!room->open_session("",ticket,code,&error,ms,session)) {
      host::log("console: room open refused (close %d, %s)", room->close_code(),
                room->lobby_mismatch() ? "lobby_mismatch" : "unclassified");
      return false;
    }
    conn = room->connection();
    worker = std::thread([this,code] {
      uint64_t ping = now_ms(), next_card = 0;
#ifdef YOUGAME_AGENT_TEST_HOOK
      const bool drop_once = transport::agent_drop_room_once_enabled(std::getenv("MELEE_ONLINE_TEST_DROP_ROOM_ONCE"));
#endif
      for (;;) {
#ifdef YOUGAME_AGENT_TEST_HOOK
        if (drop_once && !transport::agent_drop_room_once_attempted && console::console_css_frames() >= 300 &&
            room->brokered() && !room->closed()) {
          bool due = false;
          { std::lock_guard<std::mutex> lock(mutex);
            due = transport::agent_drop_room_once_due(true,console::console_css_frames(),last_roster,
                [](const char* s) { host::log("%s",s); }); }
          if (due) room->agent_test_drop_room(); // 1006 locally: the held-seat rejoin below takes it
        }
#endif
        if (host::exit_requested()) {
          if (!handoff_pending) room->send({{"t","leave"}});
          break; // an intentional shared-session relaunch retains its seat
        }
        json m;
        if (!stop && room->next(m,10)) {
          const auto type = ranked::str(m,"t");
          if (session_live && !handoff_pending && (type == "back" || type == "join" || type == "welcome")) {
            // The live Link validates frozen slots/accounts and new connection ids.
            // Lobby's old host id must not turn a held host's new id into a Leave;
            // leave/lobby rosters still pass, so a released host closes the guest.
            auto control = m; control.erase("players"); control.erase("participants");
            friends::lobby.observe(control); // final room closure still applies immediately
          } else friends::lobby.observe(m);
          if (friends::lobby.cancelled()) console::entry.stop();
          bool ended_session = false;
          if (type == "sessionEnded") {
            std::lock_guard<std::mutex> lock(mutex);
            ended_session = ranked::str(m,"sessionId") == session;
            if (ended_session) { session_live = waiting_previous = false; interrupted = true; }
          }
          else if (m.count("console") && m["console"].is_object()) session_live = true;
          else if (m.count("room") && m["room"].is_object() && m["room"].count("console") && m["room"]["console"].is_object()) session_live = true;
          else if (type == "welcome" || type == "lobby") {
            if (session_live.exchange(false)) interrupted = true;
            waiting_previous = false; // reconnect after expiry can miss sessionEnded
          }
          std::lock_guard<std::mutex> lock(mutex);
          if (type == "welcome") last_roster = m;
          else if (!last_roster.is_null()) {
            for (const auto* key : {"players","participants","host","revision"})
              if (m.count(key)) last_roster[key] = m[key];
            if (type == "leave" && m.count("player") && last_roster.count("participants")) {
              auto& seats = last_roster["participants"];
              const auto id = ranked::str(m["player"],"id");
              seats.erase(std::remove_if(seats.begin(),seats.end(),[&](const json& p) {
                return ranked::str(p,"connectionId") == id || ranked::str(p,"id") == id;
              }),seats.end());
            }
          }
          // Preserve a new Start that beats the simulation's return tick, including its save chunks.
          const json* frozen = m.count("console") && m["console"].is_object() ? &m["console"] :
            m.count("room") && m["room"].is_object() && m["room"].count("console") && m["room"]["console"].is_object() ? &m["room"]["console"] : nullptr;
          if (!last_roster.is_null()) {
            if (frozen) {
              last_roster["console"] = *frozen;
              last_roster["matchId"] = m.value("matchId",frozen->value("sessionId",json()));
            } else if (ended_session || type == "welcome" || type == "lobby") {
              last_roster.erase("console"); last_roster.erase("matchId");
              if (last_roster.count("room") && last_roster["room"].is_object()) last_roster["room"].erase("console");
            }
          }
          if (type == "welcome" && end_previous_on_welcome.exchange(false) && frozen &&
              ranked::str(*frozen,"sessionId") == session) {
            waiting_previous = true;
            outgoing.push_back({{"t","end-session"},{"sessionId",session}});
          }
          if (m.count("players") && m["players"].is_array()) {
            const auto me = ranked::str(last_roster,"me");
            peers_present = m["players"].size() == size_t(players) &&
              std::none_of(m["players"].begin(),m["players"].end(),[&](const json& p) {
                return ranked::str(p,"id") != me && p.value("away",json()) == true;
              });
          }
          if (incoming.size() >= 1024) { closed = true; break; }
          incoming.push_back(std::move(m)); cv.notify_all();
          if (type == "welcome") rejoining = false; // publish the fresh roster before allowing opens
        }
        const auto quit = [this] { return stop.load() || friends::lobby.cancelled() || host::exit_requested(); };
        if (room->closed() && room->rejoinable() && !quit()) {
          rejoining = true;
          // Rooms holds a dropped seat for 90 s (AWAY_MS); 60 s rides out a Rooms deploy's ~30 s reset.
          const auto until = now_ms()+60000;
          bool recovered = false;
          uint64_t backoff = 250; // a refused ticket must not become a request storm
          host::log("console: room transport dropped (%d); trying held-seat rejoin",room->close_code());
          while (!quit() && now_ms() < until && room->rejoinable()) {
            const auto budget = int(std::min<uint64_t>(2000,until-now_ms()));
            try {
              const auto ticket = renew_ticket ? renew_ticket(budget) : std::string();
              std::string current_session;
              { std::lock_guard<std::mutex> lock(mutex); current_session = session; }
              std::string error;
              if (!ticket.empty() && now_ms() < until && room->open_session("",ticket,code,&error,
                  int(std::min<uint64_t>(2000,until-now_ms())),quit,current_session)) {
                conn = room->connection(); recovered = true; break;
              }
            } catch (...) { /* bounded retry, never log a ticket or bridge exception */ }
            const auto left = until > now_ms() ? until-now_ms() : 0;
            { // close() wakes this, so a Leave never waits out a backoff
              std::unique_lock<std::mutex> lock(mutex);
              cv.wait_for(lock,std::chrono::milliseconds(std::min(backoff,left)),[this] { return stop.load(); });
            }
            backoff = std::min<uint64_t>(backoff*2,4000);
          }
          if (recovered) { host::log("console: held-seat rejoin connected"); continue; }
        }
        if (room->closed()) {
          if (room->close_code() == 4012) friends::lobby.closed(4012); // 1006 keeps the held seat
          if (friends::lobby.cancelled()) console::entry.stop();
          closed = true; cv.notify_all(); break;
        }
        std::deque<json> send;
        { std::lock_guard<std::mutex> lock(mutex); send.swap(outgoing); sending = send.size(); }
        if (now_ms()-ping >= 5000) { send.push_back({{"t","ping"},{"at",now_ms()}}); ping = now_ms(); }
        for (size_t k = 0; k < send.size(); ++k) {
          const auto& value = send[k];
          // Pace actual Room sends too: a delayed worker must not burst a queued save over 120/s.
          if (value.count("d") && value["d"].is_object() && ranked::str(value["d"],"kind") == "mu-card") {
            const auto now = now_ms();
            if (now < next_card) std::this_thread::sleep_for(std::chrono::milliseconds(next_card-now));
            next_card = now_ms()+12;
          }
          if (!room->send(value)) {
            if (!room->closed()) continue; // a timeout loses one message, not the stream
            // Dropped socket: keep the rest for after the held-seat rejoin (or the final close).
            std::lock_guard<std::mutex> lock(mutex); outgoing.insert(outgoing.begin(),send.begin()+k,send.end()); break;
          }
        }
        { std::lock_guard<std::mutex> lock(mutex); sending = 0; cv.notify_all(); }
        if (closed || stop) break; // flush queued session-end notices before closing the stream
      }
    });
    return true;
  }
  bool resume_room(bool host) {
    std::lock_guard<std::mutex> lock(mutex);
    if (closed || !last_roster.is_object()) return false;
    auto strip_previous = [this](json& m) {
      const json* frozen = m.count("console") && m["console"].is_object() ? &m["console"] :
        m.count("room") && m["room"].is_object() && m["room"].count("console") && m["room"]["console"].is_object() ? &m["room"]["console"] : nullptr;
      if (frozen && ranked::str(*frozen,"sessionId") == session) {
        m.erase("console"); m.erase("matchId");
        if (m.count("room") && m["room"].is_object()) m["room"].erase("console");
      }
    };
    auto roster = last_roster;
    waiting_previous = session_live && (!roster.count("console") || ranked::str(roster["console"],"sessionId") == session);
    strip_previous(roster);
    for (auto& m : incoming) strip_previous(m);
    incoming.push_front(std::move(roster));
    interrupted = false;
    // A failed peer link cannot resume the old simulation at its last frame.
    // End only that session; the host keeps the room for the next shared boot.
    if (host && waiting_previous) outgoing.push_back({{"t","end-session"},{"sessionId",session}});
    return true;
  }
  bool next(json& m, int ms) {
    std::unique_lock<std::mutex> lock(mutex);
    if (ms > 0) cv.wait_for(lock,std::chrono::milliseconds(ms),[&] { return !incoming.empty() || closed; });
    if (incoming.empty()) return false;
    m = std::move(incoming.front()); incoming.pop_front(); return true;
  }
  bool send(const json& m) {
    std::lock_guard<std::mutex> lock(mutex);
    if (closed || outgoing.size() >= 256) return false;
    outgoing.push_back(m); return true;
  }
  bool flush(int ms) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock,std::chrono::milliseconds(ms),[&] {
      return closed || (outgoing.empty() && sending == 0);
    }) && !closed;
  }
  bool attach(int ms) {
    if (retiring) return false; // a cancelled preparing entry must not wait on its own retire
    wait_console_retired([this] { return retiring.load(); }); // every console-session attach shares the process-global channel
    if (ms <= 0) return false;
    if (retiring) return false;
    if (!yg_enet_channel_ready()) {
      // Like directOpen: a refused attach answers {ok:false, code} (no_direct when the links helper is
      // missing or won't start). Log the code (letters and _ only) instead of throwing it away.
      json raw = g_stdio ? bridge_stdio({{"type","directAttach"}},ms) : bridge_http({{"type","directAttach"}},ms);
      if (!raw.value("ok",false)) {
        std::string code = ranked::str(raw,"code");
        if (code.empty() || code.size() > 24 || code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) code = "unknown";
        host::log("console: link helper attach refused (%s)", code.c_str());
        return false;
      }
      const auto h = raw.count("result") ? raw["result"] : json::object();
#ifdef _WIN32
      if (h.value("version",0) != 1 || yg_enet_attach_handles(h.value("section",uintptr_t(0)),
          h.value("toApp",uintptr_t(0)),h.value("toGame",uintptr_t(0)))) return false;
#else
      transport::PosixAttachment p;
      if (!transport::posix_attachment(h,p) || yg_enet_attach_posix(p.section,p.wake,p.wait,p.size)) return false;
#endif
    }
    yg_enet_select(1); yg_enet_enable(1);
    datagrams = yg_enet_create(); // explicitly virtual: never falls back to an OS UDP socket
    return datagrams != ENET_SOCKET_NULL;
  }
  void close() {
    std::lock_guard<std::mutex> closing(close_mutex);
    { std::lock_guard<std::mutex> lock(mutex); stop = true; }
    cv.notify_all();
    if (worker.joinable()) worker.join();
    closed = true; cv.notify_all();
    // A clean close preserves Rooms' relaunch hold. Never send an explicit leave.
    if (room) { room->close(1000); room.reset(); }
    if (datagrams != ENET_SOCKET_NULL) { yg_enet_destroy(datagrams); datagrams = ENET_SOCKET_NULL; yg_enet_enable(0); }
  }
};
}

namespace console {
namespace {
std::mutex friends_backend_mutex;
std::mutex friends_report_mutex;
std::shared_ptr<ConsoleBackend> friends_backend;
void track_friends_backend(const std::shared_ptr<ConsoleBackend>& b) {
  std::shared_ptr<ConsoleBackend> old;
  {
    std::lock_guard<std::mutex> lock(friends_backend_mutex);
    if (b && b->retiring) return; // a Leave already took it; never reinstall a retiring backend
    old = std::exchange(friends_backend,b);
  }
  // old's destructor joins its worker here, outside the lock the simulation thread also takes
}
std::string current_build() {
  static const auto build = [] {
    wchar_t exe[32768]{};
    if (!GetModuleFileNameW(nullptr,exe,32768)) return std::string();
    return build_fingerprint(std::filesystem::path(exe));
  }();
  return build;
}
int64_t epoch_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
SessionIO console_io(const std::shared_ptr<ConsoleBackend>& b) {
  SessionIO io;
  // Console boots before the ordinary mod/card scan. Use native Direct's canonical
  // content key in both the waiting game and the fresh boot (no ranked rules).
  // The suffix is the console session protocol: console4 adds game-local save choices and rejoin agreement
  // to the shared host save and straight-to-VS boot. Other protocols must never share a session.
#ifdef MELEE_PORT_VERSION
  io.compatibility = compatibility_key(MELEE_PORT_VERSION,false,"",false) + "-console4";
#else
  io.compatibility = compatibility_key("dev",false,"",false) + "-console4";
#endif
  io.host_save = [] {
    const auto read = yougame::console::host_save_reader.load(std::memory_order_acquire);
    return read ? read() : Bytes{};
  };
  io.persist_card = [](const SessionConfig& c) {
    const char* data = std::getenv("YOUGAME_DATA");
    return data && stage_card(std::filesystem::u8path(data),c);
  };
  io.discard_card = [](const SessionConfig& c) {
    const char* data = std::getenv("YOUGAME_DATA");
    if (data && !discard_staged_card(std::filesystem::u8path(data),c))
      host::log("console: couldn't remove staged save; default launch ignores it");
  };
  io.restore_card = [](SessionConfig& c) {
    const char* data = std::getenv("YOUGAME_DATA");
    return data && take_card_choice(std::filesystem::u8path(data),c) && load_card(std::filesystem::u8path(data),c);
  };
  io.exclusive = true;
  io.background_service = true;
  io.random = random64;
  io.request = [b](const json& q,int ms) {
    if (ranked::str(q,"type") == "directClose" && b->retiring) {
      std::lock_guard<std::mutex> lock(b->mutex); b->retired_links.push_back(q);
      return json::object();
    }
    // A refused directOpen answers {ok:false, code} (no_direct, not_room, not_peer, exists, rate_links, ended).
    // bridge() would throw that away; hand the code (letters and _ only) to the link log instead.
    if (ranked::str(q,"type") == "directOpen") {
      json j = g_stdio ? bridge_stdio(q,ms) : bridge_http(q,ms);
      if (j.value("ok",false)) return j.count("result") ? j["result"] : json::object();
      std::string code = ranked::str(j,"code");
      if (code.empty() || code.size() > 24 || code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) code = "unknown";
      return json{{"link",0},{"refused",code}};
    }
    // A refused console room ticket: log the site's code (never its text) before start_session's catch,
    // so "couldn't connect to your friend" before the friend has even accepted names a reason.
    if (ranked::str(q,"type") == "mpTicket") {
      try { return bridge(q,ms); }
      catch (const update::Required&) { throw; }
      catch (const std::runtime_error&) {
        host::log("console: room ticket refused (%s)", g_bridge_refusal.empty() ? "no answer" : g_bridge_refusal.c_str());
        // bridge() reports an update refusal as "Cancelled" when no search is running (Friends has none). The
        // player needs the update notice, not "couldn't connect" (doubles-lead 11-01: a newer row went up mid-run).
        if (g_bridge_refusal == "update_required") { g_update_notice.refuse(); throw update::Required(); }
        throw;
      }
    }
    return bridge(q,ms);
  };
  io.open_room = [b,compatibility = io.compatibility](const std::string& ticket,const std::string& room,int ms) {
    b->renew_ticket = [room,compatibility,players = b->players,private_queue = b->private_queue](int budget) {
      // Same shape as the session's first ticket (yougame_console_link.cpp), so Rooms sees the same lobby.
      json q = {{"type","mpTicket"},{"room",room},{"players",players},{"mode","console"},{"ranked",false},
        {"lobby",{{"version",1},{"minPlayers",players},{"maxLocalPlayers",1},{"compatibility",compatibility}}}};
      if (private_queue) q["queue"] = "private";
      const auto reply = bridge(q,budget);
      return ranked::str(reply,"ticket");
    };
    return b->open(ticket,room,ms);
  };
  io.room_mismatch = [b] { return b->room && b->room->lobby_mismatch(); };
  io.next_room = [b](json& m,int ms) { return b->next(m,ms); };
  io.send_room = [b](const json& m) { return b->send(m); };
  io.flush_room = [b](int ms) { return b->flush(ms); };
  io.room_closed = [b] { return b->closed.load(); };
  io.room_rejoining = [b] { return b->rejoining.load(); };
  io.rebound = [](const std::string& old_id,const std::string& new_id) { friends::lobby.rebound(old_id,new_id); };
  io.connection = [b] { return b->conn.load(); };
  io.attach = [b](int ms) { return b->attach(ms); };
  io.state = direct_state;
  io.route = [](unsigned id) { std::lock_guard<std::mutex> lock(g_link_mutex); return g_link_routes[id]; };
  io.rtt = [](unsigned id) {
    std::lock_guard<std::mutex> lock(g_link_mutex);
    return std::exchange(g_link_rtts[id],{});
  };
  io.send = [b](unsigned id,const Bytes& bytes) {
    ENetAddress address; if (yg_enet_link_address(id,&address)) return;
    ENetBuffer buffer; buffer.data = const_cast<uint8_t*>(bytes.data()); buffer.dataLength = bytes.size();
    yg_enet_send(b->datagrams,&address,&buffer,1);
  };
  io.receive = [b](unsigned& id,Bytes& bytes) {
    bytes.resize(1200); ENetAddress address{}; ENetBuffer buffer;
    buffer.data = bytes.data(); buffer.dataLength = bytes.size();
    const int n = yg_enet_receive(b->datagrams,&address,&buffer,1);
    if (n <= 0) { if (n < 0) b->closed = true; return false; }
    id = ENET_NET_TO_HOST_32(address.host) & 65535; bytes.resize(size_t(n)); return true;
  };
  io.wait = [b](int ms) { enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE; if (yg_enet_wait(b->datagrams,&condition,ms) < 0) b->closed = true; };
  io.close = [b] {
    if (b->retiring) return; // Leave's retire owns flush/close after Engine::end joins its worker
    if (friends::lobby.cancelled() || host::exit_requested()) { b->send({{"t","leave"}}); b->flush(1000); }
    // Keep receiving authoritative room departure after a peer link ends the game.
    if (!friends::lobby.held() || host::exit_requested()) { b->close(); friends::events.current_room({}); }
  };
  io.log = [](const std::string& line) { host::log("%s",line.c_str()); };
  return io;
}
} // namespace
std::unique_ptr<SessionLink> start_session(const json& request, SessionConfig& config, std::string& error) {
  const char* data = std::getenv("YOUGAME_DATA");
  if (friends::lobby.cancelled() || (data && session_left(std::filesystem::u8path(data),session_id(request)))) {
    error = "left"; return nullptr; // a page Leave during the load must never rejoin
  }
  bool busy;
  { std::lock_guard<std::mutex> lock(g_mutex); busy = bool(g_room) || g_slippi_match; }
  friends::events.current_room(friends::room(request));
  if (!active() || !wait_identity(10000).ok || !g_broker || !g_direct || busy) {
    error = "console sessions need the YouGame link helper";
    host::log("console: start failed: %s",error.c_str()); friends::events.current_room({}); return nullptr;
  }
  if (request.value("players",2) == 2) {
    friends::lobby.begin(false); // welcome supplies the host identity, including migrated ports
    friends::lobby.opened(friends::room(request));
  }
  auto b = std::make_shared<ConsoleBackend>();
  b->session = friends::text(request,"sessionId",64);
  b->players = request.value("players",2);
  b->private_queue = false;
  auto io = console_io(b);
  track_friends_backend(b);
  io.cancelled = [] { return friends::lobby.cancelled() || host::exit_requested(); };
  return start_session(request,config,error,std::move(io));
}
void take_pending_launch() {
  const char* data = std::getenv("YOUGAME_DATA");
  if (!data || !active()) return;
  pending_take(std::filesystem::u8path(data),epoch_now(),current_build(),
      [](const json& q,const auto& consume) { return requests.push(q,true,consume); },
      [](const char* text) { host::log("%s",text); });
}
std::function<void()> leave_friends_room() {
  entry.stop();
  std::shared_ptr<ConsoleBackend> b;
  std::shared_future<void> preparation;
  { std::lock_guard<std::mutex> lock(friends_backend_mutex); b = std::exchange(friends_backend,{}); }
  if (b) {
    b->retiring = true;
    std::lock_guard<std::mutex> lock(b->mutex); preparation = b->preparation_finished;
  }
  requests.clear();
  const char* data = std::getenv("YOUGAME_DATA");
  if (data) { std::error_code ec; std::filesystem::remove(std::filesystem::u8path(data) / "console" / "pending.json",ec); }
  friends::events.current_room({});
  return g_console_retired.begin([b,preparation] {
    const auto began = std::chrono::steady_clock::now();
    host::log("console: Friends leave closing in background");
    // An entry may still be opening this exact room. Let it finish using the backend
    // before closing it; never consult the mutable Entry or replacement backend.
    if (preparation.valid() && preparation.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
      host::log("console: Friends leave waiting for the room to finish opening");
      preparation.wait();
    }
    if (b) {
      // Read the session only now: an opening entry may have committed one meanwhile.
      // FIFO: end the frozen session (if any), then leave lobby membership, then close.
      std::string session;
      { std::lock_guard<std::mutex> lock(b->mutex); session = b->session; }
      const char* data = std::getenv("YOUGAME_DATA");
      if (data && !session.empty() && !mark_session_left(std::filesystem::u8path(data),session))
        host::log("console: couldn't record cancelled launch");
      if (!session.empty()) b->send({{"t","end-session"},{"sessionId",session}});
      b->send({{"t","leave"}});
      b->flush(1000);
    }
    {
      std::lock_guard<std::mutex> report_lock(friends_report_mutex);
      try { bridge({{"type","mpRoom"},{"code",nullptr},{"players",json::array()}},1000); } catch (...) {}
    }
    if (b) {
      for (const auto& q : b->retired_links) { try { bridge(q,1000); } catch (...) {} }
      b->close();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count();
    host::log("console: Friends leave closed in %lld ms",static_cast<long long>(ms));
  });
}
void close_friends_room(std::function<void()> release) {
  // Engine::end has joined its datagram worker; only the detached retire may close now.
  release();
}
bool wait_friends_retired(int ms) { return g_console_retired.wait(ms); }
void friends_entry(const std::string& room, bool host_reenter, bool held_reenter) {
  if (!entry.begin()) return;
  if (held_reenter) {
    if (!friends::lobby.resume()) { entry.finished(); return; }
  } else friends::lobby.begin(room == "new" || host_reenter);
  if (host_reenter) friends::lobby.opened(room);
  friends::events.current_room(room == "new" ? std::string() : room);
  host::log("console: Friends opening room");
  std::thread([room,host_reenter,held_reenter] {
    wait_console_retired(); // also order the new room/page reports after the old empty report
    if (entry.cancelled() || friends::lobby.cancelled()) { entry.finished(); return; }
    auto done = std::make_shared<std::promise<void>>();
    struct Finished {
      std::shared_ptr<std::promise<void>> done;
      ~Finished() { done->set_value(); }
    } finished{done};
    std::shared_ptr<ConsoleBackend> b;
    const auto preparing = done->get_future().share();
    if (host_reenter || held_reenter) {
      // Publish this entry on the reused backend in the same step that reads it, so a Leave that
      // takes it next waits for this entry before its retire closes it.
      std::lock_guard<std::mutex> lock(friends_backend_mutex); b = friends_backend;
      if (b) { std::lock_guard<std::mutex> backend_lock(b->mutex); b->preparation_finished = preparing; }
    }
    if (held_reenter && !host_reenter && b && b->closed) {
      friends::lobby.session_stopped(false); entry.finished(); return;
    }
    const bool reuse = b && !b->closed;
    if (!reuse) {
      std::string previous;
      if (b) { std::lock_guard<std::mutex> lock(b->mutex); previous = b->session; }
      b = std::make_shared<ConsoleBackend>(); b->session = std::move(previous);
      b->end_previous_on_welcome = host_reenter && !b->session.empty();
    }
    { std::lock_guard<std::mutex> lock(b->mutex); b->preparation_finished = preparing; }
    track_friends_backend(b);
    auto io = console_io(b);
    if (reuse) {
      io.open_room = [b,host_reenter](const std::string&,const std::string&,int) {
        return b->resume_room(host_reenter);
      };
    }
    io.previous_session_active = [b] { return b->waiting_previous.load(); };
    io.friends_lobby = true;
    io.handoff_join = friends_handoff_join_enabled();
    if (io.handoff_join) {
      const auto info = std::atomic_load_explicit(&handoff_join::published_info, std::memory_order_acquire);
      if (info) io.handoff_info = *info;
    }
    io.handoff_css = [] { return handoff_join::host_css.load(std::memory_order_acquire); };
    io.handoff_begin = [] {
      auto channel = std::make_shared<handoff_join::Channel>();
      std::atomic_store_explicit(&handoff_join::pending_channel,channel,std::memory_order_release);
      return channel;
    };
    io.held_reenter = host_reenter || held_reenter;
    io.committed = [b](const SessionConfig& c) {
      std::lock_guard<std::mutex> lock(b->mutex); b->session = c.session_id; b->interrupted = false;
    };
    io.opened = [](const std::string& code) { friends::events.current_room(code); friends::lobby.opened(code); };
    io.roster = [](const std::string& code,const json& participants,const std::string& me,int revision) {
      std::lock_guard<std::mutex> report_lock(friends_report_mutex);
      if (friends::lobby.cancelled() || entry.cancelled()) return;
      std::string player;
      for (const auto& p : participants) if (ranked::str(p,"connectionId") == me) player = ranked::str(p,"id");
      // This report completes the page's held-room contract before mpFriend.room is sent.
      bridge({{"type","mpRoom"},{"code",code},{"queue","private"},{"size",2},{"me",player},
          {"players",participants},{"participants",participants},{"revision",revision}},1000);
      friends::lobby.report_ready();
    };
    io.css_ready = [b] { return friends::lobby.ready() && b->peers_present.load(); };
    io.freeze_pick = [](const std::string& name) { return friends::lobby.freeze(name); };
    io.cancelled = [] { return friends::lobby.cancelled() || entry.cancelled() || host::exit_requested(); };
    io.confirm_start = [] { return entry.start(); };
    std::string last;
    io.waiting = [&last](const std::string& text,bool ready) {
      entry.status(text,ready);
      if (last != text) { host::log("console: %s",text.c_str()); last = text; }
    };
    bool prepared = false; std::string prepared_session; int64_t handed_at = 0;
    std::shared_ptr<RelaunchWait> relaunch_wait;
    const bool relaunching = g_stdio && g_relaunch;
    HandoffIO handoff_io; handoff_io.clock = io.clock; handoff_io.cancelled = io.cancelled;
    io.prepared = [&,handoff_io](const SessionConfig& c) {
      if (friends::lobby.cancelled() || entry.cancelled()) return;
      prepared_session = c.session_id;
      { std::lock_guard<std::mutex> lock(b->mutex); b->session = c.session_id; }
      handed_at = epoch_now();
      if (relaunching) {
        relaunch_wait = std::make_shared<RelaunchWait>(prepared_session,handed_at);
        std::atomic_store(&g_relaunch_wait,relaunch_wait);
      }
      const char* data = std::getenv("YOUGAME_DATA");
      b->handoff_pending = true;
      prepared = handoff(c,relaunching,data ? std::filesystem::u8path(data) : std::filesystem::path(),
          epoch_now(),current_build(), [](const json& q) { write_line("yg " + q.dump()); },
          [](const std::string& line) { host::log("%s",line.c_str()); },
          [](const char* text) { entry.status(text); },handoff_io);
      if (!prepared) b->handoff_pending = false;
    };
    // Keep the frozen room alive while the user closes/relaunches the game. Rooms owns the gap.
    io.close = [] {};
    SessionConfig c; std::string error;
    try {
      if (!g_broker || !g_direct) {
        error = "console sessions need the YouGame link helper";
        host::log("console: start failed: %s",error.c_str());
      } else start_session({{"room",room},{"players",2},{"inputDelay",2}},c,error,std::move(io));
      if (!prepared && !error.empty() && !entry.cancelled())
        entry.status(error == update::kMessage ? std::string(update::kMessage) :
                     session_notice(error) ? session_notice(error) : "Couldn't start playing together: " + error);
      while (prepared && !friends::lobby.cancelled() && !entry.cancelled() && !host::exit_requested() && !b->closed) {
        json m; b->next(m,50);
        if (relaunch_wait && relaunch_wait->reason()) {
          host::log("console: relaunch dropped by the app: %s",relaunch_wait->reason());
          entry.status(kRelaunchDropped); break;
        }
        // The app closes this game and starts the session; still here long after means it didn't.
        if (relaunch_wait && relaunch_wait->timed_out(epoch_now())) {
          host::log("console: no relaunch 45 s after the request; the app didn't start the session");
          entry.status("Couldn't start playing together. Close the game and try again."); break;
        }
        if (ranked::str(m,"t") == "sessionEnded" && ranked::str(m,"sessionId") == prepared_session) {
          const auto reason = friends::text(m,"reason",160);
          host::log("console: session ended: %s",reason.c_str());
          entry.status(session_notice(reason) ? session_notice(reason) : "Lost connection to your friend."); break;
        }
      }
    } catch (...) {
      host::log("console: start failed: couldn't start playing together");
      entry.status("Couldn't start playing together");
    }
    std::atomic_store(&g_relaunch_wait,std::shared_ptr<RelaunchWait>{});
    if ((b->interrupted || (b->closed && !friends::lobby.host())) && !friends::lobby.cancelled() && !entry.cancelled()) {
      friends::lobby.session_stopped(!b->closed);
      entry.finished();
      return; // same room/socket; host's simulation tick reenters local CSS
    }
    if (!b->retiring && (!prepared || friends::lobby.cancelled() || entry.cancelled())) {
      std::string session;
      { std::lock_guard<std::mutex> lock(b->mutex); session = b->session; }
      const char* data = std::getenv("YOUGAME_DATA");
      if (data && !session.empty()) mark_session_left(std::filesystem::u8path(data),session);
      if (!session.empty()) b->send({{"t","end-session"},{"sessionId",session}});
      b->send({{"t","leave"}}); b->flush(1000);
      if (!friends::lobby.cancelled()) friends::lobby.leave(entry.status());
    }
    if (!b->retiring) b->close();
    friends::events.current_room({});
    entry.finished();
  }).detach();
}
} // namespace console

namespace relay_pads {
void logging_ready() {
  g_relay_pads.logging_ready();
  static std::once_flag once;
  std::call_once(once, [] {
    if (yougame::active() && *compat_tag)
      host::log("yougame: branch build \"%s\": matches only the same branch", compat_tag);
#ifdef _WIN32
    if (g_embed_toplevel && g_toplevel_dpi_failed)
      host::log("embed: toplevel DPI awareness failed err=%lu", (unsigned long)g_toplevel_dpi_error);
#endif
    std::atexit([] { g_console_retired.wait(2000); g_relay_pads.flush(); host::log_flush(); });
  });
}
bool active() { return g_relay_pads.active(); }
bool read(unsigned slot, _XINPUT_STATE* state) { return g_relay_pads.read(slot, state); }
bool poll(unsigned slot, _XINPUT_STATE* state, const std::function<bool()>& native) {
  return g_relay_pads.poll(slot, state, native);
}
void rumble(const std::function<void()>& native) {
  g_relay_pads.rumble(native, host::controller_relay::rumble_writer.load(std::memory_order_acquire) != nullptr);
}
bool owns_hid() { return g_relay_pads.owns_hid(); }
#ifdef YOUGAME_AGENT_TEST_HOOK
void arm_rumble_test(double boot_ms) {
  std::lock_guard<std::mutex> lock(g_relay_rumble_mutex);
  g_rumble_pulse.arm(boot_ms);
}
#endif
}

void start() {
  static bool started = false;
  if (started) return;
  started = true;
  auto take = [](const char* name) {
    const char* v = std::getenv(name);
    std::string out = v ? v : "";
    if (v) _putenv_s(name, "");
    return out;
  };
  g_bridge_url = take("YOUGAME_BRIDGE");
  g_bridge_token = take("YOUGAME_TOKEN");
  g_stdio = take("YOUGAME_STDIO") == "1";
  slippi_linking::presentation.enabled = take("YOUGAME_SLIPPI_LINKABLE") == "1";
  host::test_env::yougame_session = active();
#ifdef MELEE_LINUX_X11_EMBED
  // The shared X11 helper checks this non-secret flag at window attachment.
  const char* embed = std::getenv("YOUGAME_EMBED");
  g_embedded = embed && std::strcmp(embed, "1") == 0;
#else
  g_embedded = take("YOUGAME_EMBED") == "1";
#endif
#ifdef _WIN32
  g_embed_toplevel = embed_toplevel_policy::enabled(g_embedded, take("YOUGAME_EMBED_TOPLEVEL"));
  g_app_pid = embed_toplevel_policy::app_pid(take("YOUGAME_APP_PID"));
  if (g_embed_toplevel) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
      g_toplevel_dpi_error = GetLastError();
      g_toplevel_dpi_failed = true;
    }
  }
#endif
  if (g_stdio) {
    std::thread(read_stdin).detach();
    // Toplevel caps wait for successful creation with the requested styles.
    if (!g_embed_toplevel) parity::signals.start(true, [](const char* line) { write_line(line); });
    host::controller_relay::install_rumble(relay_rumble);
    host::controller_relay::install_route(relay_route);
    std::thread(relay_rumble_writer).detach();
  }
  // Nothing is logged here: this runs before the log file is open, and an early line would send the
  // whole log to stdout, which in stdio mode is the bridge. fetch_identity says which mode this is.
}

bool active() { return g_stdio || (!g_bridge_url.empty() && !g_bridge_token.empty()); }
uintptr_t pad_mirror_handle() { return g_pad_mirror.load(); }
#ifndef _WIN32
pad_mirror::Attachment pad_mirror_attachment() {
  std::lock_guard<std::mutex> lock(g_pad_mirror_mutex);
  return g_pad_mirror_attachment;
}
#endif
bool stdio_bridge() { return g_stdio; }
bool embedded() { return g_embedded; }
bool embed_toplevel() { return g_embed_toplevel; }
uint32_t app_pid() { return g_app_pid; }
void console_boot_ready() { parity::signals.console_boot_ready(); }

void embed_window_ready(uintptr_t hwnd) {
  if (g_embed_toplevel) {
    parity::signals.start(g_stdio, [](const char* line) { write_line(line); }, true);
    // After the caps, once per created window (again if it is ever recreated).
    if (hwnd) parity::signals.emit(embed_toplevel_policy::hwnd_line(hwnd).c_str());
  }
}

void notify_mod_refused(bool explicit_layer, mode_policy::ModRefusal refusal,
                        const std::string& mod, const std::string& version) {
  const auto request = mode_policy::mod_exit_request(active(), stdio_bridge(), explicit_layer, refusal, mod, version);
  if (request.is_null()) return;
  static std::atomic<bool> sent{false};
  if (sent.exchange(true)) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  try {
    // WriteFile and the stdout mutex can stall too. Keep them off the exiting thread so the
    // entire notification, including writing the request, has the same bounded grace period.
    std::packaged_task<void()> send([request] {
      try { (void)bridge_stdio(request, 500); } catch (...) { }
    });
    auto done = send.get_future();
    std::thread(std::move(send)).detach();
    done.wait_until(deadline);
  } catch (...) { }  // A failed bridge must never replace the original fatal exit.
}

void set_box_handler(std::function<void(const std::string&)> handler) {
  std::deque<std::string> backlog;
  {
    std::lock_guard<std::mutex> lock(g_io.mutex);
    g_io.box = handler;
    backlog.swap(g_io.box_backlog);
  }
  if (handler) for (auto& line : backlog) handler(line);
}

#ifdef MELEE_LINUX_X11_EMBED
std::vector<std::string> take_box_commands() {
  std::lock_guard<std::mutex> lock(g_io.mutex);
  if (g_io.box_overflow) {
    g_io.box_backlog.clear();
    return {"quit"};  // An overloaded bridge cannot leave an uncontrollable child behind.
  }
  std::vector<std::string> commands;
  commands.reserve(g_io.box_backlog.size());
  while (!g_io.box_backlog.empty()) {
    commands.push_back(std::move(g_io.box_backlog.front()));
    g_io.box_backlog.pop_front();
  }
  return commands;
}
#endif

void refresh_identity() {
  if (g_identity_fetching.exchange(true)) return;
  std::thread([] {
    try {
      fetch_identity();
    } catch (const std::exception& e) {
      host::log("yougame: sign-in failed: %s", e.what());
    }
    g_identity_fetching = false;
  }).detach();
}

Identity identity() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_identity_asked) {
      // The page may not have been ready, the player signed in since, or the connect code did not
      // come: ask again, now and then.
      if ((!g_identity.ok || g_identity.code.empty()) && GetTickCount64() - g_identity_at > 5000) refresh_identity();
      return g_identity;
    }
  }
  refresh_identity();
  Identity pending;
  pending.error = "Signing in to YouGame...";
  return pending;
}

Identity wait_identity(int ms) {
  static const uint64_t until = GetTickCount64() + (uint64_t)ms;   // one budget for the whole process
  Identity id = identity();
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_identity_asked) return g_identity;
    }
    if (GetTickCount64() >= until) return id;
    Sleep(20);
  }
}

void relay_key(const char* key) {
  if (g_stdio) write_line(std::string("yg {\"key\":\"") + key + "\"}");
}

void pause_event(bool pausable, bool paused, const char* reason) {
  if (!g_stdio) return;
  json event = {{"type", "pause"}, {"pausable", pausable}, {"paused", paused}};
  if (reason) event["reason"] = reason;
  write_line("yg " + json{{"event", event}}.dump());
}

bool update_safe_point(bool idle_scene, bool online_busy, bool ranked_set, bool online_screen) {
  if (g_update_notice.visible()) return true;
  if (g_update_notice.pending().empty()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool retiring = g_retired.valid() &&
      g_retired.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
  const update::SafePoint state{idle_scene,
      online_busy || g_update_searches.load() != 0,
      bool(g_room) || g_slippi_match || retiring, ranked_set,
      g_room && g_room->friends, online_screen};
  if (!g_update_notice.begin(state)) return false;
  host::log("update: safe point, handing back to YouGame");
  return true;
}
bool update_handoff_visible() { return g_update_notice.visible(); }
void update_notice_presented() {
  const auto line = g_update_notice.presented(now_ms());
  if (line.empty()) return;
  write_line(line); // host::log alone goes to stderr in app stdio mode, NOT the app's stdout.
  host::log("%s", line.c_str());
  host::set_replay_viewing(false); // update acceptance always exits 0, including a finished replay
  host::request_user_exit();
}

void blend_slippi_stopped(uint64_t epoch, const std::string& reason) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_search_generation.current(epoch)) return;
  g_blend_reason = blend::slippi_stop_reason(reason);
  if (g_status == blend::kSearchBoth || g_status == blend::kSearchYouGameOnly) {
    g_status = blend::kSearchYouGameOnly;
    g_status_since = now_ms();
  }
}

std::string status_line() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_status = friends::expire_joined(g_status, g_status_since, now_ms());
  return g_update_notice.available() ? update::kMessage : g_status;
}

std::string slippi_link_status(const std::string& status) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (status == blend::kSearchYouGameOnly && !g_blend_reason.empty()) return status + "\n" + g_blend_reason;
  return status == slippi_linking::kSearching
      ? slippi_linking::presentation.line(slippi_linking::Screen::Search) : status;
}

nlohmann::json request_slippi_link(const nlohmann::json& request, int timeout, const std::function<bool()>& cancelled) {
  return bridge(request, timeout, cancelled);
}

std::string pick_friend_room(const std::function<bool()>& cancelled, const std::string& held) {
  host::log("friends: picker requested");
  try {
    auto code = held.empty() ? friends::resolve_room({},
        [&](const json& q, int ms) { return bridge(q, ms, cancelled); }, cancelled)
        : friends::picker_room(bridge(friends::picker(held),friends::kPickerTimeout,cancelled));
    if (cancelled() || (!held.empty() && code != held)) throw std::runtime_error("Cancelled");
    host::log("friends: invite sent");
    return code;
  } catch (const std::exception& e) {
    if (std::string(e.what()) == friends::kVersion) log_friend_version();
    else host::log("%s", cancelled() || std::string(e.what()) == "Cancelled" ? "friends: cancelled" :
                          "friends: invite failed: picker failed");
    throw;
  }
}

void with_native_udp(const std::function<void()>& create) {
  // g_mutex: no transport selection (a search starting, an identity refresh) lands in between.
  std::lock_guard<std::mutex> lock(g_mutex);
  const int was = yg_enet_selected();
  yg_enet_select(0);
  try {
    create();
  } catch (...) {
    yg_enet_select(was);
    throw;
  }
  yg_enet_select(was);
}

void slippi_link(bool linked, const std::string& tag) {
  const std::string line = blend::slippi_link_line(linked, tag);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    static std::string sent;   // the last line the app has: only a change goes out again
    if (line == sent) return;
    sent = line;
  }
  host::log("yougame: Slippi %s", linked ? "linked" : "not linked");
  if (g_stdio) write_line(line);
}

uint64_t begin_search() {
  const auto epoch = close_room(Leave::Quit);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_search_generation.current(epoch)) g_blend_reason.clear();
  return epoch;
}

bool guard_search(uint64_t epoch, const std::function<void()>& commit) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_search_generation.current(epoch)) return false;
  commit();
  return true;
}

bool prepare_slippi_match(const std::function<bool()>& cancelled, uint64_t epoch) {
  // A blend reserved/retired its room before launching the helper. Do not close a newer room.
  const bool blend = epoch != 0;
  if (!epoch) epoch = close_room(Leave::Quit);
  std::shared_future<void> retired;
  { std::lock_guard<std::mutex> lock(g_mutex); retired = g_retired; }
  // A blend has no published YouGame room; older room teardown owns only its old links.
  // Its completion must not consume the Slippi opponent's connection window either.
  while (!blend && retired.valid() && retired.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
    if (cancelled()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_search_generation.current(epoch) || cancelled()) return false;
  select_match_transport(true);
  yg_enet_enable(0);
#ifdef YOUGAME_AGENT_TEST_HOOK
  g_pending_input = false;
  g_pending_input_roster = {};
#endif
  return true;
}

Match find_match(const Search& search, const std::function<bool()>& cancel_requested) {
  // Reserve before blocking, or use the generation reserved before the helper was launched.
  if (cancel_requested()) { Match out; out.error = "Cancelled"; return out; }
  const uint64_t epoch = search.epoch ? search.epoch : begin_search();
  const auto cancelled = [&] { return !g_search_generation.current(epoch) || cancel_requested(); };
  struct SearchScope {
    search_policy::BusyRetry retry;
    explicit SearchScope(const std::function<bool()>& stop) { ++g_update_searches; g_search_stopped = stop; g_ticket_retry = &retry; }
    ~SearchScope() { g_search_stopped = {}; g_ticket_retry = nullptr; --g_update_searches; }
  } scope(cancelled);
  if (cancelled()) return fail("Cancelled");
  try {
  struct FriendSearch {
    explicit FriendSearch(bool active) {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (search_stopped()) return;
      friends::events.take_version();  // a notice from an earlier search cannot refuse this invite
      g_friend_search = active; g_friend_failed = false;
    }
    ~FriendSearch() { g_friend_search = false; }
  } friend_search(!search.friend_room.empty());
  // Blended Unranked: the queue reports to the coordinator and asks it before taking a room.
  const bool blend = static_cast<bool>(search.claim);
  struct BlendSearch {
    explicit BlendSearch(bool active) { g_blend_search = active; }
    ~BlendSearch() { g_blend_search = false; }
  } blend_search(blend);
  const std::string compat = compatibility(search.mode);
  if (cancelled()) return fail("Cancelled");
  if (compat.empty()) return fail("Could not verify this mod's content. Restart the game before playing online.");
#ifdef YOUGAME_AGENT_TEST_HOOK
  struct PendingInput {
    uint64_t epoch;
    void clear() const {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (!g_search_generation.current(epoch) || search_stopped()) return;
      g_pending_input = false;
      g_pending_input_roster = {};
    }
    void publish(const Room& room, const json& participants) const {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (!g_search_generation.current(epoch) || search_stopped()) return;
      g_pending_input_roster = {room.mode, room.ranked, participants, room.me};
      g_pending_input = true;
    }
    ~PendingInput() { clear(); }
  } pending_input{epoch};
#endif
  std::shared_future<void> retired;
  { std::lock_guard<std::mutex> lock(g_mutex); retired = g_retired; }
  while (retired.valid() && retired.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
    if (cancelled()) return fail("Cancelled");
  if (cancelled()) return fail("Cancelled");
  Identity me;
  { std::lock_guard<std::mutex> lock(g_mutex); me = g_identity; }
  if (!me.ok) me = fetch_identity(cancelled);
  if (cancelled()) return fail("Cancelled");
  if (!me.ok) return fail(search.mode == 0 && me.error == "Sign in to YouGame to play online"
                            ? "Sign in to YouGame to play Ranked." : me.error);
  { std::lock_guard<std::mutex> lock(g_mutex);
    if (cancelled()) { Match out; out.error = "Cancelled"; return out; }
    select_match_transport(false);
  }
#ifndef _WIN32
  if (g_broker && !g_direct) return fail("YouGame broker requires direct links v1 (1200-byte datagrams)");
#endif
  const bool team_code = search.mode == 3 && !search.direct_code.empty();
  std::string team_room;
  if (team_code && !friends::team_room_code(search.direct_code, &team_room)) return fail(friends::kTeamCodeError);
  const bool friends_mode = !team_code && (!search.friend_room.empty());
  const bool direct = search.mode == 2;
  const int size = search.mode == 3 ? 4 : 2;
  const json lobby = {{"version", 1}, {"minPlayers", size}, {"maxLocalPlayers", 1}, {"compatibility", compat}};
  json request = {{"type", "mpTicket"}, {"players", size}, {"mode", size == 4 ? "teams" : "singles"}, {"lobby", lobby}, {"ranked", search.mode == 0}};
  if (search.mode == 0) {
    ranked::set_contract(request);
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_rank_opponent.clear(); g_rank_score.clear();
      g_rank_results = {}; g_rank_result_visible = false;
      g_rank_read.recovery_at = 0;
      if (!g_rank_read.enabled) g_rank_read.enter(); // the agent harness bypasses the CSS
    }
    ranked_header();
    // The CSS may already have the read in flight. The ticket must follow it, including when
    // a hook enters Ranked without visiting the menu/CSS first.
    for (;;) {
      { std::lock_guard<std::mutex> lock(g_mutex); if (!g_rank_read.in_flight) break; }
      if (cancelled()) return fail("Cancelled");
      Sleep(20);
    }
  }
  // Unranked keeps its two players together for the next game, as Slippi's unranked does.
  if (search.mode == 1) request["afterMatch"] = "stay";
  if (!friends_mode && direct) request["direct"] = search.direct_code;
  if (team_code) request = friends::team_ticket(compat, team_room);

  // Clear failed/cancelled setup without clearing a newer search's room.
  struct FriendsSetup {
    uint64_t epoch;
    bool keep = false;
    ~FriendsSetup() {
      if (keep) return;
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_search_generation.current(epoch) && !search_stopped()) friends::events.current_room({});
    }
  } friends_setup{epoch};
  // The base listing's Unranked says which pools it searches (yougame_blend.h kSearch*).
  const std::string searching = search.status.empty() ? std::string("searching") : search.status;
  bool repeated_link_failure;
  { std::lock_guard<std::mutex> lock(g_mutex); repeated_link_failure = g_direct_link_failures.repeated(); }
  set_status(team_code ? friends::kTeamWaiting : friends_mode ? friends::kWaiting :
             repeated_link_failure ? search_policy::kLinkRetry : searching);
  json ticket;
  try {
    if (team_code) {
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_search_generation.current(epoch) || cancelled()) throw std::runtime_error("Cancelled");
        friends::events.current_room(team_room);
      }
      ticket = bridge(request, 30000, cancelled);
    } else if (friends_mode) {
      ticket = friends::request_ticket(compat, search.friend_room, request,
          [&](const json& q, int ms) {
            if (q.value("type", "") == "mpFriend") host::log("friends: picker requested");
            auto answer = bridge(q, ms, cancelled);
            if (q.value("type", "") == "mpFriend") host::log("friends: invite sent");
            return answer;
          }, cancelled,
          [&](const std::string& code) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_search_generation.current(epoch) || cancelled()) throw std::runtime_error("Cancelled");
            friends::events.current_room(code);
          });
    } else {
      // The blend cancels this side when Slippi's is first: the ticket wait must not hold it up.
      ticket = bridge(request, 30000, cancelled);
    }
  } catch (const update::Required&) {
    return fail(update::kMessage, false, true);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  if (search.mode == 0 && !ranked::set_contract_matches(ticket))
    return fail("YouGame did not return the Ranked best-of-three rules. Please try again later.");
  // A ranked ticket carries the player's standing on this game's ladder (the ticket route's
  // `rating` and ranks.ts `rank`): the Ranked header shows it from the first search on.
  if (!cancelled() && search.mode == 0 && ticket.is_object() && ticket.count("rank")) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (cancelled()) { Match out; out.error = "Cancelled"; return out; }
    if (g_rank.standing(ticket["rank"], ticket.count("rating") ? ticket["rating"] : json()))
      host::log("yougame: ranked standing: %s", g_rank.header().c_str());
  }
  const std::string url = ticket.value("url", "");
  std::string pass = ticket.value("ticket", "");
  std::string room_code = ticket.count("room") && ticket["room"].is_string() ? ticket["room"].get<std::string>() : "";
  if ((friends_mode || team_code) && room_code.empty()) room_code = friends::room(request);
  std::string error;
  const auto decline = [&] {
    if (!g_search_generation.current(epoch)) return;
    Socket late;
    // A Slippi commit cancels normal I/O; this bounded cleanup owns only the assigned room.
    const auto stale = [&] { return !g_search_generation.current(epoch); };
    search_policy::decline_room(
        [&] { return late.open_session(url, pass, room_code, &error, 1000, stale); },
        [&] { late.send({{"t", "leave"}}); }); // Leave::Quit's casual protocol; no result
  };

  if (cancelled()) {
    if (blend && !room_code.empty()) decline();
    return fail("Cancelled");
  }
  if ((!g_broker && url.empty()) || pass.empty()) return fail("YouGame did not return a ticket");
  if ((friends_mode || team_code) && (room_code != friends::room(request))) return fail("YouGame returned a different friends room");
  // The reopen loop retries every 250 ms; renewals go to the site, so at most one per 1.5 s.
  uint64_t last_renew = 0;
  bool update_refused = false;
  const auto renew_ticket = [&](uint64_t until) {
    const auto now = now_ms();
    if (now >= until || cancelled() || (last_renew && now - last_renew < 1500)) return false;
    last_renew = now;
    return transport::renew_room_ticket(pass, room_code, [&] {
      try { return bridge(request, (int)std::min<uint64_t>(3000, until - now), cancelled); }
      catch (const update::Required&) { update_refused = true; throw; }
    }, [](const char* message) { host::log("%s", message); });
  };

  // Unranked: wait in the queue until the matchmaker names a room.
  const bool queued = room_code.empty();
  if (queued) {
    Socket queue;
    if (!queue.open_session(url, pass, "", &error, 10000, cancelled)) return fail(error);
    for (;;) {
      if (cancelled()) {
        if (blend && g_search_generation.current(epoch)) {
          queue.send({{"t", "cancel"}});
          json late;
          while (queue.next(late, 0)) {
            if (late.value("t", "") == "matched" && late.count("room") && late["room"].is_string()) {
              room_code = late["room"].get<std::string>();
              decline();
              break;
            }
          }
        }
        return fail("Cancelled");
      }
      json m;
      if (!queue.next(m, 250)) {
        if (queue.closed()) {
          if (!queue.recoverable()) return fail("Lost the connection to YouGame matchmaking");
          const uint64_t until = now_ms() + 15000;
          bool reopened = false;
          transport::TicketRetry retry;
          while (!cancelled() && !update_refused && now_ms() < until) {
            if (retry.attempt([&] { return !update_refused && now_ms() < until && queue.open_session(url, pass, "", &error, (int)std::min<uint64_t>(3000, until - now_ms()), cancelled); },
                              [&] { if (queue.closed()) renew_ticket(until); })) { reopened = true; break; }
            Sleep(250);
          }
          if (update_refused) return fail(update::kMessage, false, true);
          if (!reopened) return fail("Lost the connection to YouGame matchmaking");
        }
        continue;
      }
      if (cancelled()) {
        // next() may already have consumed the assignment when Slippi won. Release that
        // room through the ordinary decline path before discarding the queue message.
        if (blend && m.value("t", "") == "matched" && m.count("room") && m["room"].is_string()) {
          room_code = m["room"].get<std::string>();
          decline();
        }
        return fail("Cancelled");
      }
      const auto& refused = update::refusal(m);
      if (update::required(refused)) {
        g_update_notice.refuse();
        host::log("yougame: update required (current %s)", update::current(refused).c_str());
        return fail(update::kMessage, false, true);
      }
      const std::string t = m.value("t", "");
      if (!cancelled() && t == "searching" && search.on_searching && m.count("waiting") && m["waiting"].is_number_integer())
        search.on_searching((int)std::clamp<int64_t>(m["waiting"].get<int64_t>(), 0, 1000000));
      if (t == "matched" && m.count("room") && m["room"].is_string()) {
        room_code = m["room"].get<std::string>();
        if (blend && !search.claim()) { decline(); return fail("Cancelled"); }
        break;
      }
      if (t == "error") return fail(m.value("error", "Matchmaking failed"));
    }
  } else {
    if (blend && !search.claim()) { decline(); return fail("Cancelled"); }   // ticket assignment
    if (friends_mode) host::log("friends: waiting");
    set_status(team_code ? friends::kTeamWaiting : friends_mode ? friends::kWaiting : "waiting for " + search.direct_code);
  }

  auto room = std::make_unique<Room>();
  room->friends = friends_mode;
  room->size = size;
  room->mode = search.mode;
  room->ranked = search.mode == 0;
  room->socket = std::make_unique<Socket>();
  room->url = url; room->ticket = pass; room->code = room_code; room->ticket_request = request;
#ifdef YOUGAME_AGENT_TEST_HOOK
  pending_input.publish(*room, json::array());
#endif
  if (!room->socket->open_session(url, pass, room_code, &error, 10000, cancelled)) return fail(error, !direct);
  if (cancelled()) { room->socket->send({{"t", "leave"}}); return fail("Cancelled"); }

  const bool links_mode = g_direct;
  const auto requested_port = udp::port(links_mode ? nullptr : std::getenv("YOUGAME_UDP_PORT"));
  uint16_t bound_port = search.udp_port;
  udp::Ingress ingress(&udp::own_udp_drops);
  SOCKET udp = INVALID_SOCKET;
  struct Closer { SOCKET& s; ~Closer() { if (s != INVALID_SOCKET) closesocket(s); } } closer{udp};
  std::string external, lan_addr;
#ifndef _WIN32
  if (g_broker && !links_mode) return fail("YouGame broker requires direct links v1 (1200-byte datagrams)");
#endif
  if (!links_mode) {
    if (!requested_port.valid) {
      host::log("%s", udp::port_error(requested_port, "invalid decimal port").c_str());
      return fail("The own UDP port is unavailable");
    }
    udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET) {
      if (requested_port.set) host::log("%s", udp::port_error(requested_port, "socket creation failed").c_str());
      return fail("Could not open the network port");
    }
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    if (!udp::bind_port(requested_port, search.udp_port, [&](uint16_t p) {
      bind_addr.sin_port = htons(p); // zero-initialized INADDR_ANY: all interfaces
      return bind(udp, (sockaddr*)&bind_addr, sizeof bind_addr) == 0;
    }, [](const char* message) { host::log("%s", message); }))
      return fail("The network port is in use");
    SocketLength length = sizeof bind_addr;
    if (getsockname(udp, (sockaddr*)&bind_addr, &length) != 0 ||
        (requested_port.set && ntohs(bind_addr.sin_port) != requested_port.value)) {
      if (requested_port.set) host::log("%s", udp::port_error(requested_port, "bound port check failed").c_str());
      return fail("The own UDP port is unavailable");
    }
    bound_port = ntohs(bind_addr.sin_port);
    if (!stun(udp, external, cancelled)) host::log("yougame: STUN unavailable");
    lan_addr = udp::lan_candidate(std::getenv("YOUGAME_HOST_LAN_IP"), bound_port, lan_ip,
        [](const char* message) { host::log("%s", message); });
  }
  if (cancelled()) return fail("Cancelled");
  // Seat 0 names the match: Slippi's match id slot and replays get this, never the room's code.
  const std::string my_match_id = "yg-" + hex64(random64());

  // Seats come from the room: the room numbers the players 0..size-1 (participants), seat n plays
  // in-game port n+1 and seat 0 decides. A peer's addresses are keyed by its connection, as the room
  // reports the sender, never by a seat the peer claims.
  json participants = json::array();
  std::map<std::string, json> cands;          // connection id -> the addresses it sent me
  std::map<std::string, uint64_t> tokens;     // connection id -> the token I gave it
  std::map<std::string, uint64_t> sent_at;    // connection id -> when I last sent it mine
  uint64_t last_ping = now_ms(), full_since = 0;
  // Only base-listing plain Unranked can meet a blending client.
  const uint64_t arrive_by = search_policy::no_show_deadline(host::gameplay::vanilla_only(), queued,
      search.mode, friends_mode || team_code, now_ms());
  bool arrived = false;
  int my_seat = -1;
  auto seat_of = [&](const std::string& conn) {
    return transport::connection_seat(participants, conn);
  };
  for (;;) {
    if (cancelled()) return fail("Cancelled");
    if (friends_mode && friends::events.take_version()) return fail(friends::kVersion);
    json m;
    const bool received = room->socket->next(m, 200);
    if (cancelled()) return fail("Cancelled");
    if (received) {
      const auto& refused = update::refusal(m);
      if (update::required(refused)) {
        g_update_notice.refuse();
        host::log("yougame: update required (current %s)", update::current(refused).c_str());
        return fail(update::kMessage, false, true);
      }
      const std::string t = m.value("t", "");
      if (t == "welcome") room->me = m.value("me", "");
      if (m.count("participants") && m["participants"].is_array()) participants = m["participants"];
      read_roster(*room, m);
#ifdef YOUGAME_AGENT_TEST_HOOK
      pending_input.publish(*room, room->participants);
#endif
      if (friends_mode && friends::version_reason(m) && (t == "error" || t == "notice" || t == "refused"))
        return fail(friends::kVersion);
      if (t == "error") return fail(m.value("error", m.value("message", "The room refused this player")), !direct);
      if (t == "leave" || t == "away") {
        // A player left before the match: forget what they sent (a newcomer gets a fresh exchange).
        const std::string who = transport::leave_id(m, room->socket->brokered());
        cands.erase(who);
        sent_at.erase(who);
      }
      if (t == "msg" && m.count("d") && m["d"].is_object()) {
        const json& d = m["d"];
        const std::string from = m.value("from", "");
        if (d.value("kind", "") == "mu-cand" && d.value("v", 0) == 2 && seat_of(from) >= 0) {
          if (d.value("compat", "") != compat) return fail("Your opponent is on a different Melee Unlocked version or mod build");
          // A player we had not heard from: answer at once. Ours may have gone out before they
          // joined, and once we hold every address we stop sending.
          if (!cands.count(from)) sent_at.erase(from);
          cands[from] = links_mode ? transport::candidate(true, d.value("compat", ""), d.value("code", ""), d.value("match", ""), "", "", "") : d;
        }
      }
    } else if (room->socket->closed()) {
      if (!room->socket->recoverable()) return fail("Lost the connection to the YouGame room", !direct);
      const uint64_t until = now_ms() + 15000;
      bool reopened = false;
      transport::TicketRetry retry;
      while (!cancelled() && !update_refused && now_ms() < until) {
        if (retry.attempt([&] { return !update_refused && now_ms() < until && room->socket->open_session(url, pass, room_code, &error, (int)std::min<uint64_t>(3000, until - now_ms()), cancelled); },
                          [&] { if (room->socket->closed()) renew_ticket(until); })) { reopened = true; room->ticket = pass; break; }
        Sleep(250);
      }
      if (update_refused) return fail(update::kMessage, false, true);
      if (!reopened) return fail("Lost the connection to the YouGame room", !direct);
      participants = json::array(); cands.clear(); sent_at.clear(); tokens.clear();
      room->me.clear(); room->participants = json::array(); full_since = 0;
#ifdef YOUGAME_AGENT_TEST_HOOK
      pending_input.publish(*room, room->participants);
#endif
      continue;
    }
    my_seat = seat_of(room->me);
    const bool full = (int)participants.size() == size && my_seat >= 0;
    if (full) arrived = true;
    else if (search_policy::no_show_expired(arrive_by, arrived, now_ms())) {
      host::log("yougame: unranked: opponent never joined the room after 30 s; searching again");
      room->socket->send({{"t", "leave"}});
      return fail("Your opponent did not arrive", true);
    }
    if (!full) full_since = 0;
    else if (!full_since) { full_since = now_ms(); set_status("connecting"); }
    // My addresses, point to point, each peer with a token of its own.
    if (full) {
      for (auto& p : participants) {
        const std::string conn = p.value("connectionId", "");
        if (conn == room->me || p.value("localIndex", 0) != 0) continue;
        if (sent_at.count(conn) && now_ms() - sent_at[conn] < 1000) continue;
        if (!links_mode && !tokens.count(conn)) tokens[conn] = random64() | 1;
        room->socket->send({{"t", "msg"}, {"to", conn},
          {"d", transport::candidate(links_mode, compat, me.code, my_match_id, lan_addr, external,
                                     links_mode ? "" : hex64(tokens[conn]))}});
        sent_at[conn] = now_ms();
      }
    }
    if (now_ms() - last_ping > 5000) {
      room->socket->send({{"t", "ping"}, {"at", now_ms()}});
      last_ping = now_ms();
    }
    if (!full) continue;
    bool all = true;
    for (auto& p : participants) {
      const std::string conn = p.value("connectionId", "");
      if (conn != room->me && (!cands.count(conn) || !sent_at.count(conn))) all = false;
    }
    if (all) break;
    // A seated opponent who never answers (crashed, stuck): give up and, in Unranked, search again.
    if (now_ms() - full_since > 15000) return fail("Your opponent did not answer", !direct);
  }

  // Punch to every other seat.
  std::vector<PunchPeer> peers;
  std::string match_id = my_seat == 0 ? my_match_id : "";
  for (auto& p : participants) {
    const std::string conn = p.value("connectionId", "");
    if (conn == room->me) continue;
    const json& c = cands[conn];
    PunchPeer peer;
    peer.port = p.value("slot", 0) + 1;
    if (!links_mode) peer.mine = tokens[conn];
    if (!links_mode) peer.theirs = std::strtoull(c.value("token", "").c_str(), nullptr, 16);
    if (!links_mode) for (const char* key : {"lan", "ext"}) {
      sockaddr_in a;
      if (parse_addr(c.value(key, ""), a) && std::none_of(peer.candidates.begin(), peer.candidates.end(), [&](auto& x) { return same_addr(x, a); })) {
        peer.candidates.push_back(a);
        peer.kinds.push_back(key);
      }
    }
    if (peer.port == 1) match_id = c.value("match", "");
    peers.push_back(peer);
  }
  if (links_mode) {
    try {
      // Idempotent once per run. Mapping stays alive across matches; only the host owns ring I/O.
      if (!yg_enet_channel_ready()) {
        json h = bridge({{"type", "directAttach"}}, 10000, cancelled);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (cancelled()) throw std::runtime_error("Cancelled");
#ifdef _WIN32
        if (h.value("version", 0) != 1 || yg_enet_attach_handles(h.value("section", uintptr_t(0)),
            h.value("toApp", uintptr_t(0)), h.value("toGame", uintptr_t(0))))
          throw std::runtime_error("Could not attach direct links");
#else
        transport::PosixAttachment p;
        if (!transport::posix_attachment(h, p) || yg_enet_attach_posix(p.section, p.wake, p.wait, p.size))
          throw std::runtime_error("Could not attach YouGame POSIX direct links");
#endif
      }
      std::vector<std::string> remotes;
      for (const auto& p : participants) if (p.value("connectionId", "") != room->me) remotes.push_back(p.value("connectionId", ""));
      if (!transport::open_links(room->links, room->socket->connection(), remotes,
          [&](const json& q, int ms) { return bridge(q, ms, cancelled); }, direct_state, now_ms, [] { Sleep(10); }, cancelled))
        throw std::runtime_error("Direct link failed");
      size_t index = 0;
      for (const auto& p : participants) {
        if (p.value("connectionId", "") == room->me) continue;
        const unsigned link = room->links.ids[index++];
        for (auto& peer : peers) if (peer.port == p.value("slot", -1) + 1) {
          peer.chosen.sin_family = AF_INET;
          peer.chosen.sin_addr.s_addr = htonl(0x0aff0000u | link);
          peer.chosen.sin_port = htons(7777);
        }
      }
      for (unsigned link : room->links.ids) {
        ENetAddress a;
        if (yg_enet_link_address(link, &a)) throw std::runtime_error("Invalid direct address");
      }
      { std::lock_guard<std::mutex> lock(g_mutex);
        if (cancelled()) throw std::runtime_error("Cancelled");
        yg_enet_enable(1);
      }
    } catch (const update::Required&) { throw; }
    catch (...) {
#ifndef _WIN32
      try { throw; }
      catch (const std::exception& e) { host::log("yougame: direct links failed: %s", e.what()); }
      catch (...) { host::log("yougame: direct links failed"); }
#endif
      if (cancelled()) return fail("Cancelled");
      // Count only peers whose link failed, never a teammate who connected.
      // Even a partial successful link resets the previous failure history.
      json failed_participants = json::array();
      bool linked = false;
      size_t peer_index = 0;
      for (const auto& p : participants) {
        if (p.value("connectionId", "") == room->me) continue;
        const bool up = peer_index < room->links.ids.size() && direct_state(room->links.ids[peer_index]) == 2;
        linked |= up;
        if (!up) failed_participants.push_back(p);
        ++peer_index;
      }
      bool repeated;
      { std::lock_guard<std::mutex> lock(g_mutex);
        if (cancelled()) { Match out; out.error = "Cancelled"; return out; }
        if (linked) g_direct_link_failures.successful();
        repeated = g_direct_link_failures.failed(failed_participants, room->me);
      }
      return fail(repeated ? search_policy::kLinkRetry : search_policy::kLinkFailed,
                  transport::retry_link_failure(search.mode));
    }
  } else if (!punch(udp, my_seat + 1, peers, ingress, cancelled)) {
    if (cancelled()) return fail("Cancelled");
    host::log("yougame: connect failed (own UDP): my STUN %s, my LAN %s", external.empty() ? "none" : "ok", lan_addr.empty() ? "none" : "ok");
    // Unranked searches again, as Slippi does when a found opponent cannot be reached.
    return fail("Could not connect to your opponent directly", !direct);
  }

  if (cancelled()) return fail("Cancelled");
  { std::lock_guard<std::mutex> lock(g_mutex);
    if (!cancelled()) g_direct_link_failures.successful();
  }
  host::log("yougame: transport %s", links_mode ? "YouGame direct links" : "own UDP");
  Match out;
  out.ok = true;
  out.udp_port = links_mode ? 0 : bound_port;
  out.match_id = match_id.rfind("yg-", 0) == 0 && match_id.size() <= 32 ? match_id : my_match_id;
  out.local_index = my_seat;
  out.host = my_seat == 0;
  room->port_ids = transport::port_participants(participants, size);
  for (auto& p : participants) {
    int seat = p.value("slot", -1);
    if (seat < 0 || seat >= size) continue;
    const std::string conn = p.value("connectionId", "");
    Player player;
    player.port = seat + 1;
    player.local = seat == my_seat;
    player.id = p.value("id", "");
    player.name = player.local ? me.name : p.value("name", "Player");
    // In Direct the room admits only the account behind the code we typed: that code is the
    // opponent's. Elsewhere the code is what the opponent sent, shown as theirs.
    player.code = player.local ? me.code : direct && !friends_mode ? search.direct_code : cands[conn].value("code", "");
    for (auto& peer : peers) if (peer.port == player.port) player.address = addr_string(peer.chosen);
    out.players.push_back(player);
  }
  room->recovery.configure(room_code, room->me, participants);
  if (room->ranked) room->set.configure(my_seat, room->port_ids);
  std::sort(out.players.begin(), out.players.end(), [](const Player& a, const Player& b) { return a.port < b.port; });
  std::string names;
  for (auto& p : out.players) if (!p.local) names += (names.empty() ? "" : ", ") + p.name + (p.code.empty() ? "" : " (" + p.code + ")");
  if (friends_mode) { host::log("friends: friend joined"); set_status(friends::kJoined); }
  else set_status("playing " + slippi::display::opponent_label(slippi::display::Provider::YouGame, names));

  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_search_generation.current(epoch) || g_room || cancelled()) {
    out.ok = false;
    out.error = "Cancelled";
    return out;
  }
  Room* raw = room.get();
  if (raw->ranked) {
    ++g_ranked_rooms;
    g_rank_local = my_seat;
    const auto local = ranked::roster_rank(participants, my_seat);
    if (local.known()) g_rank.update(local);
    const auto opponent = ranked::roster_rank(participants, 1 - my_seat);
    g_rank_opponent = opponent.known() ? opponent.header().substr(9) : "Unknown";
    g_rank_score = raw->set.score_text();
    g_rank_results = {};
    g_rank_read.recovery_at = 0;
    g_rank_result_visible = false;
  }
  g_room = std::move(room);
#ifdef YOUGAME_AGENT_TEST_HOOK
  g_pending_input = false;
  g_pending_input_roster = {};
#endif
  friends_setup.keep = true;
  raw->pump = std::thread(pump_room, raw);
  return out;
  } catch (const update::Required&) {
    return fail(update::kMessage, false, true);
  }
}

#ifdef YOUGAME_AGENT_TEST_HOOK
agent_test::InputState agent_test::live_input_state(int live_mode) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t session = g_search_generation.value() + 1;
  if (!g_room) {
    if (!g_pending_input) return pre_room_input_state(session, live_mode);
    const int mode = live_mode == -1 ? g_pending_input_roster.mode : live_mode;
    return room_input_state(session, mode, g_pending_input_roster.ranked,
                            g_pending_input_roster.participants, g_pending_input_roster.me, true);
  }
  // A room remains online even during a transition in the game's session flags.
  const int mode = live_mode == -1 ? g_room->mode : live_mode;
  if (g_room->stop || g_room->socket->closed())
    return {session, mode, g_room->ranked || ranked_mode(mode)};
  return room_input_state(session, mode, g_room->ranked, g_room->participants, g_room->me);
}
#endif

int seat_team(int player_idx) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_room ? g_room->seat_team(player_idx) : -1;
}

void game_started(int64_t rng, int stage) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_room) return;
  // Frame 1 is retried while waiting for peers; emit once per game, including later games of a set.
  if (g_room->ranked && !g_room->in_game)
    host::log("%s", lcancel::policy::ranked_start_log(lcancel::automatic_enabled()));
  g_room->in_game = true;
  // Called on every frame 1 the game sends while it waits for the opponent: the set counts a game once.
  if (g_room->ranked) { g_room->set.start_game(ranked::game_key(rng, stage)); return; }
  if (g_room->socket->brokered()) { g_room->games.start_game(g_room->match_id); return; }
  // Only the room's host may start a match; everyone reports its result.
  g_room->pending_begin = g_room->me == g_room->host_connection;
  g_room->begin_at = 0;
  // A result still waiting from a game the room never started belongs to no match: drop it, or it
  // would be filed under this game's match.
  g_room->pending_winner = -2;
  g_room->pending_team_result = nullptr;
}

void game_ended(const GameEnd& end) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_room || !g_room->in_game) return;   // one report per game, however many places say it ended
  if (ranked::no_contest_waits_for_report(end.end_method, end.has_placements, g_room->ranked)) {
    host::log("yougame: ranked No Contest: waiting for finalized desync/disconnect report");
    return;
  }
  if (ranked::time_out_waits_for_report(end.end_method, end.has_placements, g_room->size)) {
    host::log("yougame: time-out: the game's report names the winner");
    return;
  }
  g_room->in_game = false;
  if (g_room->ranked) {
    // Only the finalized report can distinguish a replay from a disconnect forfeit.
    ranked::Game game = ranked::game_outcome(end.end_method, end.lras, end.winner,
                                            end.has_placements ? end.placements : nullptr);
    game.key = ranked::game_key(end.rng, end.stage);   // an id both players' games share
    game.index = (uint32_t)g_room->set.games_started();
    game.detail = ranked::game_detail(end.end_method, end.lras, end.stage, end.characters, end.frames);
    g_room->set.report_game(game);
    host::log("yougame: ranked game %u (%s): %s%s", game.index, game.key.c_str(),
              game.winner >= 0 ? ("port " + std::to_string(game.winner + 1) + " won").c_str() : "no winner",
              game.forfeit ? "; a No Contest: the set goes to the other player" : "");
    return;
  }
  if (!g_room->socket->brokered()) g_room->pending_begin = false;
  if (g_room->size == 4) {
    int teams[4];
    transport::outcome_teams(end.teams, g_room->port_teams.data(), g_room->ranked, teams);
    const int winner = ranked::team_winner(end.end_method, end.lras,
                                           end.has_placements ? end.placements : nullptr, teams);
    const json result = transport::room_team_outcome(winner, g_room->port_ids, teams,
                                                     g_room->port_teams.data(), g_room->ranked);
    if (g_room->socket->brokered()) g_room->games.report_team_result(result);
    else g_room->pending_team_result = result;
    return;
  }
  const int winner = ranked::casual_winner(end.end_method, end.lras, end.winner,
                                          end.has_placements ? end.placements : nullptr, g_room->size);
  if (g_room->socket->brokered()) g_room->games.report(winner);
  else g_room->pending_winner = winner;
}

void ranked_set_complete(uint8_t end_mode) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_room && g_room->ranked) g_room->set.complete(end_mode);
}

void ranked_match_status(uint8_t status) {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Slippi's status 40 explicitly means this client abandoned the set. Setup and start
  // statuses are telemetry; neither changes the game's rules or completes a set.
  if (g_room && g_room->ranked && status == 40) g_room->set.quit();
}

void ranked_game_setup() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_room && g_room->ranked) g_room->set.game_setup();
}

bool ranked_hold() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_room && g_room->ranked && (g_room->set.awaiting_receipt() || g_room->set.over());
}

bool ranked_set_over() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_room && g_room->ranked && g_room->set.release_pair();
}

int ranked_games_received() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_room && g_room->ranked ? g_room->set.games_received() : 0;
}

bool ranked_room_retired() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_ranked_rooms > 0 && !g_room &&
         (!g_retired.valid() || g_retired.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
}

void ranked_screen_enter() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_rank_read.enter(g_room && g_room->ranked && g_room->set.started());
}

std::string ranked_header() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_slippi_match) return g_rank.header();
  const auto now = now_ms();
  const bool live_set = g_room && g_room->ranked && g_room->set.started();
  const bool recovery = !live_set && g_rank_results.state() == ranked::ResultsPanel::State::Pending && g_rank_read.recovery_due(now);
  if (g_identity.ok && !live_set && (recovery || !g_rank_read.recovery_at) && g_rank_read.begin(now)) {
    const uint64_t asked = g_rank.version();
    const auto match = g_rank_results.match();
    std::thread([asked, recovery, match] {
      json result;
      bool failed = false, unsupported = false;
      try {
        result = bridge({{"type", "mpRank"}, {"mode", "singles"}}, 10000);
      } catch (const std::exception& e) {
        failed = true;
        unsupported = ranked::unknown_request(e.what());
        // Bridge errors may contain untrusted text. The throttle applies to 429 and all failures.
        host::log("yougame: mpRank unavailable; keeping cached standing and backing off");
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      const bool applied = g_rank_read.answer(g_rank, result, asked, now_ms(), failed, unsupported);
      if (recovery) {
        ranked::RankCache unknown;
        g_rank_results.recovered_standing(match, applied ? g_rank : unknown, g_rank_read.recovery_tries >= 2 || unsupported);
        if (g_rank_results.match() == match && g_rank_results.state() != ranked::ResultsPanel::State::Pending)
          g_rank_read.recovery_at = 0;
      }
    }).detach();
  }
  return g_rank.header();
}

std::array<std::string, 6> ranked_lines() {
  const auto header = ranked_header();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_room && g_room->ranked) {
    g_rank_score = g_room->set.score_text();
    if (g_room->set.over()) g_rank_result_visible = true;
  }
  return {header, g_rank_opponent.empty() ? "Opponent: Unknown" : "Opponent: " + g_rank_opponent,
          // Line 3 stays empty: the splash that read it follows the setup screen, so "Stage
          // striking comes next" was always wrong there (build 25, 11-01).
          g_rank_score, "",
          g_rank_result_visible ? g_rank_results.rating_text() : "",
          g_rank_result_visible ? g_rank_results.change_text() : ""};
}
uint8_t opponent_rank() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_room && g_room->ranked ? ranked::roster_rank_byte(g_room->participants, 1 - g_rank_local) : 0xFF;
}

std::vector<uint8_t> rank_record_response() {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto opponent = g_room && g_room->ranked ? ranked::roster_rank(g_room->participants, 1 - g_rank_local) : ranked::RankCache();
  return ranked::rank_record_response(g_rank.record(), opponent.record(), g_rank_results.record());
}

std::vector<uint8_t> rank_response() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto reply = g_rank.response();
  const auto status = g_rank_results.record().status;
  if (status == MU_YG_RESULT_RECORDING) reply[1] = 0;
  if (status == MU_YG_RESULT_ERROR) reply[1] = 2;
  return reply;
}
uint8_t rank_visibility() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_rank.visibility();
}
uint8_t local_rank() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_rank.rank();
}

void end_session(Leave why) {
  close_room(why);
  { std::lock_guard<std::mutex> lock(g_mutex); select_match_transport(false); }
  set_status("offline");
}

}  // namespace yougame
