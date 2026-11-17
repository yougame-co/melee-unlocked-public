// Gecko codes in network matches: the pure parts (gecko_online.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gecko_online.h"

#include <algorithm>
#include <cstdio>

namespace gecko_online {
namespace {

uint64_t fnv(uint64_t h, uint8_t b) { return (h ^ b) * 0x100000001B3ull; }
std::string hex16(uint64_t h) {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
  return buf;
}
bool is_id(const std::string& s) {
  return s.size() == 16 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The known codes, by code_id of their lines. Filled from codes checked by hand: what each line
// writes, and whether a player gains anything the other does not see. Unlisted codes are Unknown.
const Known kKnown[] = {
    {"", "", "", Kind::Unknown},   // placeholder: entries are added as codes are checked
};

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}
std::vector<uint8_t> head(uint8_t kind) { return {'M', 'U', 'G', 1, kind}; }

}  // namespace

std::string code_id(const Lines& lines) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (const auto& l : lines)
    for (uint32_t w : {l.first, l.second})
      for (int s = 24; s >= 0; s -= 8) h = fnv(h, (uint8_t)(w >> s));
  return hex16(h);
}

const char* kind_label(Kind k) {
  switch (k) {
    case Kind::Display: return "display only";
    case Kind::Info: return "shows extra information";
    case Kind::Gameplay: return "changes gameplay";
    default: return "unknown code";
  }
}

const Known* known(const Lines& lines) {
  const std::string id = code_id(lines);
  for (const Known& k : kKnown)
    if (k.id[0] && id == k.id) return &k;
  return nullptr;
}

std::vector<std::string> agreed(const std::vector<std::string>& mine, const std::vector<std::string>& they_accepted,
                                const std::vector<std::string>& theirs, const std::vector<std::string>& i_accepted) {
  std::vector<std::string> out;
  const auto has = [](const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); };
  for (const auto& id : mine) if (has(they_accepted, id)) out.push_back(id);
  for (const auto& id : theirs) if (has(i_accepted, id)) out.push_back(id);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string set_fingerprint(const std::vector<std::string>& ids) {
  if (ids.empty()) return "none";
  std::vector<std::string> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  uint64_t h = 0xCBF29CE484222325ull;
  for (const auto& id : sorted) { for (char c : id) h = fnv(h, (uint8_t)c); h = fnv(h, ','); }
  return hex16(h);
}

Match match_policy(bool online, bool direct_or_arena, bool peer_is_mu, bool ranked) {
  if (!online) return Match::Offline;
  if (ranked) return Match::None;
  if (direct_or_arena && peer_is_mu) return Match::Consent;
  return Match::SafeOnly;
}

bool runs_without_consent(Match m, const Lines& lines) {
  if (m == Match::Offline) return true;
  if (m == Match::None) return false;
  const Known* k = known(lines);
  return k && k->kind == Kind::Display;   // SafeOnly and Consent: display codes need no answer
}

std::vector<uint8_t> encode_offer(const std::vector<Offer>& codes) {
  std::vector<uint8_t> v = head(0);
  const size_t n = std::min(codes.size(), kMaxCodes);
  v.push_back((uint8_t)n);
  for (size_t i = 0; i < n; ++i) {
    const auto& c = codes[i];
    const size_t lines = std::min(c.lines.size(), kMaxLines);
    v.push_back((uint8_t)lines);
    for (size_t j = 0; j < lines; ++j) { put32(v, c.lines[j].first); put32(v, c.lines[j].second); }
    const std::string name = c.sent_name.substr(0, 64);
    v.push_back((uint8_t)name.size());
    v.insert(v.end(), name.begin(), name.end());
  }
  return v;
}

std::vector<uint8_t> encode_answer(const std::vector<std::string>& accepted_ids) {
  std::vector<uint8_t> v = head(1);
  std::vector<std::string> ids;
  for (const auto& id : accepted_ids) if (is_id(id) && ids.size() < kMaxCodes) ids.push_back(id);
  v.push_back((uint8_t)ids.size());
  for (const auto& id : ids) v.insert(v.end(), id.begin(), id.end());
  return v;
}

std::vector<uint8_t> encode_final(const std::string& fingerprint);
std::vector<uint8_t> encode_lock(const std::string& fingerprint) {
  std::vector<uint8_t> v = encode_final(fingerprint);
  v[4] = 3;
  return v;
}

std::vector<uint8_t> encode_final(const std::string& fingerprint) {
  std::vector<uint8_t> v = head(2);
  const std::string fp = fingerprint == "none" ? std::string(16, '0') : fingerprint;
  v.insert(v.end(), fp.begin(), fp.end());
  return v;
}

bool decode(const uint8_t* d, size_t size, Message* out) {
  *out = Message{};
  size_t at = 0;
  const auto take = [&](size_t n) { if (size - at < n) return false; at += n; return true; };
  if (!d || size < 5 || d[0] != 'M' || d[1] != 'U' || d[2] != 'G' || d[3] != 1) return false;
  const uint8_t kind = d[4];
  at = 5;
  if (kind == 0) {
    if (!take(1)) return false;
    const size_t n = d[at - 1];
    if (n > kMaxCodes) return false;
    for (size_t i = 0; i < n; ++i) {
      if (!take(1)) return false;
      const size_t lines = d[at - 1];
      if (lines == 0 || lines > kMaxLines || !take(lines * 8)) return false;
      Offer o;
      const uint8_t* p = d + at - lines * 8;
      for (size_t j = 0; j < lines; ++j, p += 8)
        o.lines.push_back({(uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3],
                           (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7]});
      if (!take(1)) return false;
      const size_t name_len = d[at - 1];
      if (name_len > 64 || !take(name_len)) return false;
      o.sent_name.assign((const char*)d + at - name_len, name_len);
      o.id = code_id(o.lines);
      out->offer.push_back(std::move(o));
    }
  } else if (kind == 1) {
    if (!take(1)) return false;
    const size_t n = d[at - 1];
    if (n > kMaxCodes || !take(n * 16)) return false;
    for (size_t i = 0; i < n; ++i) {
      std::string id((const char*)d + at - (n - i) * 16, 16);
      if (!is_id(id)) return false;
      out->ids.push_back(id);
    }
  } else if (kind == 2 || kind == 3) {
    if (!take(16)) return false;
    std::string fp((const char*)d + at - 16, 16);
    if (fp == std::string(16, '0')) fp = "none";
    else if (!is_id(fp)) return false;
    out->fingerprint = fp;
  } else {
    return false;
  }
  if (at != size) return false;
  out->kind = kind;
  return true;
}

}  // namespace gecko_online
