// SPDX-License-Identifier: GPL-2.0-or-later
// A bad line from the YouGame app is logged as a redacted head (yougame_app_line_log.h): never a
// credential, at most 80 characters, enough left to see which line it was.
#include "yougame_app_line_log.h"
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

using yougame::app_line_log::head;

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main() {
  // A room ticket answer: the ticket value goes, the shape stays.
  std::string h = head(R"(yg {"re":12,"ok":true,"result":{"ticket":"abc.def","room":"r1"}})");
  CHECK(has(h, "\"ticket\":\"<redacted>\"") && !has(h, "abc.def") && has(h, "\"room\":\"r1\"") && h.rfind("yg {\"re\":12", 0) == 0);
  // Credential-ish keys of every kind, spaced or not.
  CHECK(!has(head(R"({"accessToken" : "short1"})"), "short1"));
  CHECK(!has(head(R"({"push":{"type":"signin","session":"s2"}})"), "s2\""));
  CHECK(!has(head(R"({"api_key":"k3","password":"p4","code":"c5"})"), "k3") &&
        !has(head(R"({"api_key":"k3","password":"p4","code":"c5"})"), "p4") &&
        !has(head(R"({"api_key":"k3","password":"p4","code":"c5"})"), "c5"));
  // An escaped quote inside a secret does not end it early.
  CHECK(!has(head(R"({"token":"a\"b"})"), "b\""));
  // Long token runs anywhere, under any key or none: JWT, hex, base64.
  const std::string jwt = std::string("ey") + "J" + "hbGciOiJIUzI1NiJ9" + "." + std::string("ey") + "J" + "zdWIiOiIxIn0" + "." + "c2lnbmF0dXJl";
  CHECK(!has(head("yg {\"push\":{\"type\":\"x\",\"v\":\"" + jwt + "\"}}"), "eyJ"));
  CHECK(!has(head("embed something 0123456789abcdef0123"), "0123456789abcdef"));
  // Short ordinary words stay; a secret past the cut is still redacted, not half shown.
  CHECK(head("embed fullscreen 1") == "embed fullscreen 1");
  const std::string long_line = "yg {\"push\":{\"type\":\"notice\",\"kind\":\"x\",\"text\":\"" + std::string(60, ' ') +
                                "\"},\"token\":\"" + std::string(10, 'Z') + "\"}";
  h = head(long_line);
  CHECK(h.size() == 83 && h.substr(80) == "..." && !has(h, "ZZZZ"));
  CHECK(!has(head(long_line, 400), "ZZZZ") && has(head(long_line, 400), "<redacted>"));
  // Control bytes never reach the log as themselves.
  CHECK(head(std::string("a\x01" "b\x1b" "c")) == "a?b?c");
  // A json parse error's "last read" carries the line's bytes: redacted the same way.
  CHECK(!has(head("[json.exception.parse_error.101] parse error at line 1, column 40: last read: '\"" + jwt + "'"), "eyJ"));
  std::puts("yougame_app_line_log_test: ok");
  return 0;
}
