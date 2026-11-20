// SPDX-License-Identifier: GPL-2.0-or-later
// A line from the YouGame app that the game could not handle, made safe for the log. The app's lines
// carry room tickets, sign-in pushes and other credentials, so the head that is logged is redacted
// over the WHOLE line first (a value cut at the length limit cannot slip through), then cut:
//   - a JSON string value whose key names a credential (token, ticket, key, secret, auth, session,
//     password, cookie, code, sig...) becomes "<redacted>";
//   - any run of 16 or more token characters (letters, digits, - _ . + / = ~ %) becomes <redacted>:
//     JWTs, hex and base64 secrets, ids; ordinary words and JSON keys are shorter;
//   - control bytes become '?'.
// What stays: the command word, JSON punctuation and keys, short values. Enough to see which line it was.
#pragma once
#include <cctype>
#include <cstddef>
#include <string>

namespace yougame::app_line_log {

inline bool secret_key(const std::string& key) {
  std::string k;
  for (char c : key) k += (char)std::tolower((unsigned char)c);
  static const char* const words[] = {"token", "ticket", "secret", "key", "auth", "session", "pass", "cookie",
                                      "code", "sig", "jwt", "bearer", "cred", "nonce", "otp", "pin"};
  for (const char* w : words)
    if (k.find(w) != std::string::npos) return true;
  return false;
}

inline bool token_char(char c) {
  return std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '+' || c == '/' || c == '=' ||
         c == '~' || c == '%';
}

// Pass 1: JSON string values under a credential key. Scans quoted strings (with escapes) in order; a
// string followed by ':' is a key, and the next string right after that colon is its value.
inline std::string redact_values(const std::string& s) {
  std::string out;
  bool value_is_secret = false, after_colon = false;
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c != '"') {
      out += c;
      if (c == ':') after_colon = true;
      else if (!std::isspace((unsigned char)c)) { after_colon = false; value_is_secret = false; }
      ++i;
      continue;
    }
    size_t j = i + 1;
    std::string text;
    while (j < s.size() && s[j] != '"') {
      if (s[j] == '\\' && j + 1 < s.size()) { text += s[j]; ++j; }
      text += s[j++];
    }
    const bool closed = j < s.size();
    const size_t end = closed ? j + 1 : j;
    size_t k = end;
    while (k < s.size() && std::isspace((unsigned char)s[k])) ++k;
    const bool is_key = k < s.size() && s[k] == ':';
    if (!is_key && after_colon && value_is_secret) out += closed ? "\"<redacted>\"" : "\"<redacted>";
    else out.append(s, i, end - i);
    value_is_secret = is_key && secret_key(text);
    after_colon = false;
    i = end;
  }
  return out;
}

// Pass 2 and the cut: long token runs, control bytes, at most `max` characters (then "...").
inline std::string head(const std::string& line, size_t max = 80) {
  const std::string s = redact_values(line);
  std::string out;
  for (size_t i = 0; i < s.size();) {
    if (token_char(s[i])) {
      size_t j = i;
      while (j < s.size() && token_char(s[j])) ++j;
      if (j - i >= 16) out += "<redacted>";
      else out.append(s, i, j - i);
      i = j;
      continue;
    }
    const unsigned char c = (unsigned char)s[i++];
    out += c < 0x20 || c == 0x7f ? '?' : (char)c;
  }
  if (out.size() > max) {
    out.resize(max);
    out += "...";
  }
  return out;
}

}  // namespace yougame::app_line_log
