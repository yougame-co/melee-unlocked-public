// Room relay framing and pacing (yougame_room_relay.h): no sockets, a fake clock.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_room_relay.h"
#include <cstdio>
#include <cstdlib>

using namespace yougame::room_relay;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
  // base64 both ways, strict
  for (size_t n = 0; n < 40; ++n) {
    std::string raw;
    for (size_t i = 0; i < n; ++i) raw += char(i * 37 + 11);
    std::string back;
    if (!n) { CHECK(!unb64(b64(raw.data(), 0), back)); continue; }
    CHECK(unb64(b64(raw.data(), raw.size()), back) && back == raw);
  }
  std::string out;
  CHECK(b64("hi", 2) == "aGk=" && unb64("aGk=", out) && out == "hi");
  CHECK(!unb64("aGk", out) && !unb64("a=k=", out) && !unb64("aG k", out) && !unb64("aGk=aGk=", out, 3));
  CHECK(!unb64(std::string(1604, 'A'), out));   // over one datagram

  // links: the other seats, ascending, from kLink0
  auto links = links_for(2, {4, 1, 2, 3, 3});
  CHECK(links.size() == 3 && links[1] == kLink0 && links[3] == kLink0 + 1 && links[4] == kLink0 + 2 && !links.count(2));

  // frames: one msg per flush when it fits, split under 12 KiB when not, never `to`
  std::deque<Datagram> q;
  for (int i = 0; i < 3; ++i) q.push_back({1, std::string(100, char('a' + i))});
  auto f = frames(2, q, 4);
  CHECK(f.size() == 1 && q.empty() && !f[0].count("to") && f[0]["t"] == "msg");
  CHECK(f[0]["d"]["k"] == kKind && f[0]["d"]["s"] == 2 && f[0]["d"]["p"].size() == 3 && f[0]["d"]["p"][0][0] == 1);
  for (int i = 0; i < 40; ++i) q.push_back({i % 2 ? 1 : 3, std::string(1200, 'x')});
  f = frames(2, q, 10);
  CHECK(f.size() >= 5 && q.empty());
  size_t total = 0;
  for (const auto& m : f) { CHECK(m.dump().size() <= kMaxFrame); total += m["d"]["p"].size(); }
  CHECK(total == 40);
  for (int i = 0; i < 40; ++i) q.push_back({1, std::string(1200, 'x')});
  f = frames(2, q, 1);
  CHECK(f.size() == 1 && !q.empty());   // the rest waits for the next flush

  // entries: only what is addressed to me, from a seat that is not me
  json m = {{"t", "msg"}, {"from", "c1"}, {"d", {{"k", kKind}, {"s", 1}, {"p", json::array({
      json::array({2, b64("for2", 4)}), json::array({3, b64("for3", 4)}), json::array({2, "%%%"}), "junk"})}}}};
  int from = 0;
  std::vector<std::string> got;
  CHECK(entries_for(m, 2, from, got) && from == 1 && got.size() == 1 && got[0] == "for2");
  CHECK(entries_for(m, 3, from, got) && got.size() == 1 && got[0] == "for3");
  CHECK(!entries_for(m, 1, from, got));   // my own echo
  CHECK(!is_frame(json{{"t", "msg"}, {"d", {{"kind", "mu-cand"}}}}));

  // Relay: queue -> one msg per flush; receive -> the sender seat's link; pacing drops the excess
  uint64_t now = 1000;
  std::vector<json> wire;
  std::vector<std::pair<unsigned, std::string>> delivered;
  Relay r(2, links_for(2, {1, 2, 3}), [&](const json& j) { wire.push_back(j); return true; },
          [&](unsigned link, const std::string& b) { delivered.push_back({link, b}); }, [&] { return now; });
  r.queue(kLink0, "to-1", 4);
  r.queue(kLink0 + 1, "to-3", 4);
  r.queue(kLink0 + 7, "nobody", 6);
  r.flush();
  CHECK(wire.size() == 1 && wire[0]["d"]["s"] == 2 && wire[0]["d"]["p"].size() == 2);
  CHECK(wire[0]["d"]["p"][0][0] == 1 && wire[0]["d"]["p"][1][0] == 3);
  CHECK(r.receive(m) && delivered.size() == 1 && delivered[0].first == kLink0 && delivered[0].second == "for2");
  CHECK(!r.receive(json{{"t", "msg"}, {"d", {{"kind", "mu-cand"}}}}) && delivered.size() == 1);
  json from4 = m; from4["d"]["s"] = 4;   // a seat this match has no link for: consumed, dropped
  CHECK(r.receive(from4) && delivered.size() == 1);
  // 60 msg/s: a burst of flushes in the same millisecond sends only the burst
  wire.clear();
  for (int i = 0; i < 20; ++i) { r.queue(kLink0, "x", 1); r.flush(); }
  CHECK(wire.size() <= (size_t)kMsgBurst);
  // a second later the rate is back
  now += 1000; wire.clear();
  for (int i = 0; i < 4; ++i) { now += kFlushMs; r.queue(kLink0, "y", 1); r.flush(); }
  CHECK(wire.size() == 4);
  // the queue keeps the newest when a flush can't keep up
  for (size_t i = 0; i < kMaxQueued + 10; ++i) r.queue(kLink0, "z", 1);
  CHECK(r.dropped() >= 10);
  // stop: nothing more goes out
  r.stop();
  wire.clear(); r.queue(kLink0, "late", 4); now += 1000; r.flush();
  CHECK(wire.empty());
  std::puts("yougame_room_relay_test: ok");
}
